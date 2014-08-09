#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifndef VERSION
#define VERSION "unknown"
#endif

#if __STDC_VERSION__ >= 201112L
#define NORETURN _Noreturn
#else
#define NORETURN
#endif

#define NELEM(x) \
    ((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))

/* an 8MB window */
#define WINDOW (8 * 1024 * 1024)

/* global variables that control program behaviour */
static int g_verbose = 0;
static bool g_force_no_thrash = false;

/* macro that only prints when verbosity is enabled (easier than messing
 * with varargs and vprintf) */
#define TRACE(...) \
    do { \
        if (g_verbose > 0) fprintf(stderr, __VA_ARGS__); \
    } while (0)

/* perror() but with a self-supplied errno */
static void perrorv(const char *msg, int err) {
    /* we don't multithread, so strerror() should be fine */
    fprintf(stderr, "%s: %s\n", msg, strerror(err));
}

/* simplified version of posix_fadvise */
static void fadvise(int fd, unsigned int advice) {
    int err = posix_fadvise(fd, 0, 0, advice);
    if (err != 0) {
        perrorv("posix_fadvise", err);
    }
}

static bool spliceall(int infd, int outfd, size_t len) {
    while (len) {
        ssize_t written = splice(infd, NULL, outfd, NULL, len,
                SPLICE_F_MORE | SPLICE_F_MOVE);
        if (written <= 0) {
            return false;
        }

        len -= written;
    }

    return true;
}

NORETURN static void usage() {
    puts("Usage: utee [OPTION]... [FILE]...\n"
         "\n  -v\tbe verbose"
         "\n  -c\tforce pagecache cleansing (even if write performance suffers a little)");
    exit(EXIT_FAILURE);
}

static mode_t mode(int fd) {
    struct stat st;

    if (fstat(fd, &st) == -1) {
        exit(EXIT_FAILURE);
    }

    return st.st_mode;
}

/* return a string representation of the fd type */
static const char *typestr(mode_t m) {
    if (S_ISFIFO(m)) { return "pipe"; }
    else if (S_ISREG(m)) { return "file"; }
    else if (S_ISDIR(m)) { return "dir"; }
    else if (S_ISBLK(m)) { return "special block file (device)"; }
    else if (S_ISCHR(m)) { return "tty"; }
    else if (S_ISSOCK(m)) { return "socket"; }
    return "unknown";
}

/* asynchronously write the range to disk */
static void writeout(int fd, size_t offset, size_t len) {
    /* this won't block, but will induce the kernel to perform a
     * writeout of the previous window */
    sync_file_range(fd, offset, len, SYNC_FILE_RANGE_WRITE);
}

static void pipesize(int fd, int size) {
    if (fcntl(fd, F_SETPIPE_SZ, size) == -1) {
        perror("fcntl(F_SETPIPE_SZ)");
    }
}

/* write the range and force it out of the page cache */
static void discard(int fd, size_t offset, size_t len) {
    /* contrary to the former call this will block, force write
     * out the old range, then tell the OS we don't need it
     * anymore */
    sync_file_range(fd, offset, len,
            SYNC_FILE_RANGE_WAIT_BEFORE |
            SYNC_FILE_RANGE_WRITE |
            SYNC_FILE_RANGE_WAIT_AFTER);
    posix_fadvise(fd, offset, len, POSIX_FADV_DONTNEED);
}

/* tell the OS to queue the window we just wrote (idx) for writing, and
 * force it to write the window before that (idx - 1) and discard it from
 * the page cache */
static void swapwindow(int fd, size_t idx, size_t window) {
    writeout(fd, window * idx, window);
    if (idx) {
        discard(fd, window * (idx - 1), window);
    }
}

/* multiplex an input pipe to a bunch of output pipes */
static ssize_t muxpipe(int in, const int *out, size_t nout) {
    ssize_t min = SSIZE_MAX;

    size_t i = 0;
    while (i < nout) {
        ssize_t teed = tee(in, out[i], (size_t) INT_MAX, SPLICE_F_NONBLOCK);
        if (teed == 0) {
            return 0;
        }

        if (teed == -1) {
            if (errno == EAGAIN) {
                usleep(1000);
                continue;
            }
            perror("tee()");
            return -1;
        }

        if (teed < min) {
            min = teed;
        }

        ++i;
    }

    return (min == SSIZE_MAX) ? 0 : min;
}

/* drain fds to fds with splice */
static ssize_t drain(const int *in, const int *out, size_t nfds, size_t len) {
    for (size_t i = 0; i < nfds; ++i) {
        if (spliceall(in[i], out[i], len) == -1) {
            return -1;
        }
    }

    return len;
}

typedef union {
    struct { int out, in; } fd;
    int o[2];
} pipe_t;

static pipe_t xpipe() {
    pipe_t p;
    if (pipe(p.o) < 0) {
        perror("pipe()");

        memset(&p, 0x0, sizeof(p));
        return p;
    }

    pipesize(p.fd.out, 0x100000);
    return p;
}

/**
 * the tee to rule all tees
 *
 *        ,-> outp2 (output to pipes)
 * tee()  |-> outp1
 *        |-> p2w | p2r -> out3 (output to file, through a pipe)
 *        |-> p1w | p1r -> out2
 * p --------------------> out1 (output to one file can be done directly)
 *        splice()
 */
static ssize_t utee(int in, int *out, size_t nout) {
    bool ok = false;

    int origin = 0;
    int originpipe[2];

    bool copyin = !S_ISFIFO(mode(in));
    if (copyin) {
        TRACE("input is not a pipe, using an intermediate\n");
        /* we have to create an intermediate pipe to act as the origin */
        if (pipe(originpipe) < 0) {
            perror("pipe()");
            return -1;
        }

        origin = originpipe[0];
    }
    else {
        TRACE("tee'ing directly from the input pipe\n");
        /* if the input is already a pipe, we can directly use it as the
         * origin pipe */
        origin = in;
    }
    pipesize(origin, 0x100000);

    /* possibly slightly overallocate, doesn't matter a lot */
    int *teeto = malloc((nout + 1) * sizeof(int));
    int *splicefrom = malloc((nout + 1) * sizeof(int));
    int *spliceto = malloc((nout + 1) * sizeof(int));

    size_t ntee = 0;
    size_t nsplice = 0;

    /* first, fill up the final output array (spliceto) */
    for (size_t i = 0; i < nout; ++i) {
        /* anything that's not a pipe, goes in the splice output array */
        if (!S_ISFIFO(mode(out[i]))) {
            spliceto[nsplice++] = out[i];
        }
    }

    /* we always splice directly from the origin */
    splicefrom[0] = origin;

    /* add as much intermediate pipes as necessary to be able to feed the
     * files in the spliceto array (files can't be teed to, so we need to
     * tee to an intermediate pipe and then splice from it to a file). This
     * loop starts at idx 1 because we already added the origin. */
    for (size_t i = 1; i < nsplice; ++i) {
        pipe_t p = xpipe();
        if (p.fd.in == 0) goto error;
        teeto[ntee++] = p.fd.in;
        splicefrom[i] = p.fd.out;
    }

    /* loop again over the output fds, but this time only look for pipes,
     * these pipes are output pipes themselves, they won't be spliced into
     * another fd */
    for (size_t i = 0; i < nout; ++i) {
        if (S_ISFIFO(mode(out[i]))) {
            teeto[ntee++] = out[i];
            pipesize(out[i], 0x100000);
        }
    }

    ssize_t written = 0;
    size_t wfilled = 0;
    size_t widx = 0;

    /* start shuttling data */
    while (1) {
        /* if the input wasn't a pipe, we have to splice its data into the
         * pipe we created */
        if (copyin) {
            ssize_t rcvd = splice(in, NULL, originpipe[1], NULL, (size_t) INT_MAX,
                    SPLICE_F_MORE | SPLICE_F_MOVE);
            if (rcvd == -1) {
                perror("input splice()");
                goto error;
            }
            if (rcvd == 0) {
                break; /* we're done */
            }
            TRACE("push -> %zd bytes\n", rcvd);
        }

        ssize_t teed = muxpipe(origin, teeto, ntee);
        if (teed == -1) {
            goto error;
        }
        if (teed == 0) {
            break; /* we're done */
        }
        TRACE("mux 1 -> %zu, %zd bytes\n", ntee, teed);

        ssize_t drained = drain(splicefrom, spliceto, nsplice, (size_t) teed);
        if (drained == -1) {
            goto error;
        }
        TRACE("drain %zu -> %zu, %zd bytes\n", nsplice, nsplice, drained);

        /* do our best to not thrash the page cache, we won't be reading the
         * file anyway, we don't call it on every iteration (save CPU), just
         * when a window has filled up */
        if (wfilled >= WINDOW) {
            swapwindow(spliceto[0], widx, WINDOW);

            if (g_force_no_thrash) {
                for (size_t i = 1; i < nsplice; ++i) {
                    swapwindow(spliceto[i], widx, WINDOW);
                }
            }

            wfilled = 0;
            widx++;
        }

        written += teed;
        wfilled += teed;
    }

    /* discard the last slice of data */
    if (widx) {
        discard(spliceto[0], WINDOW * (widx - 1), WINDOW + wfilled);
        if (g_force_no_thrash) {
            for (size_t i = 1; i < nsplice; ++i) {
                discard(spliceto[i], WINDOW * (widx - 1), WINDOW + wfilled);
            }
        }
    }

    ok = true;

error:
    /* close all pipes that we created ourselves */
    if (copyin) {
        close(originpipe[0]);
        close(originpipe[1]);
    }
    for (size_t i = 1; i < nsplice; ++i) {
        close(teeto[i - 1]);
        close(splicefrom[i]);
    }

    free(splicefrom);
    free(spliceto);
    free(teeto);

    return ok ? written : -1;
}

static int parseopts(int argc, char *argv[]) {
    int option = 0;

    while ((option = getopt(argc, argv, "vc")) != -1) {
        switch (option) {
            case 'v': g_verbose = 1; break;
            case 'c': g_force_no_thrash = true; break;
            default: return -1;
        }
    }

    if (argc <= optind) {
        return -1;
    }

    return optind;
}

int main(int argc, char *argv[]) {
    int idx = parseopts(argc, argv);
    if (idx == -1) {
        usage();
    }

    TRACE("Welcome to utee version " VERSION "\n");
    if (fcntl(STDOUT_FILENO, F_GETFL) & O_APPEND) {
        fputs("can't output to an append-mode file, use regular tee\n", stderr);
        exit(EXIT_FAILURE);
    }

    int nfiles = argc - idx;
    int *out = calloc(nfiles + 1, sizeof(int));

    /* the first one is standard output */
    out[0] = STDOUT_FILENO;
    for (int i = 1; i < nfiles + 1; ++i) {
        int fd = open(argv[idx + i - 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("couldn't create file");
            exit(EXIT_FAILURE);
        }
        out[i] = fd;
    }

    mode_t inmode = mode(STDIN_FILENO);

    TRACE("STDIN is a %s!\n", typestr(inmode));
    TRACE("STDOUT is a %s!\n", typestr(mode(STDOUT_FILENO)));

    if (S_ISREG(inmode)) {
        /* technically it would be best to supply POSIX_FADV_NOREUSE, but
         * since that's a no-op on Linux we'll at least try to make linux
         * perform more readahead. */
        fadvise(STDIN_FILENO, POSIX_FADV_SEQUENTIAL);
    }

    ssize_t written = utee(STDIN_FILENO, out, nfiles + 1);

    if (written != -1) {
        TRACE("wrote %zd bytes\n", written);
    }

    if (S_ISREG(inmode)) {
        /* restore the advice for the infd */
        fadvise(STDIN_FILENO, POSIX_FADV_NORMAL);
    }

    for (int i = 1; i < nfiles + 1; ++i) {
        if (close(out[i]) == -1) {
            perror("close()");
            exit(EXIT_FAILURE);
        }
    }

    free(out);
    exit(written == -1 ? EXIT_FAILURE : EXIT_SUCCESS);
}

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

/* fully generalized tee */
static ssize_t ctee(int in, int out, int file) {
    int inpipe[2];
    int outpipe[2];

    /* create the kernel buffers */
    if (pipe(inpipe) < 0) {
        perror("pipe()");
        return -1;
    }

    if (pipe(outpipe) < 0) {
        perror("pipe()");
        goto parterror;
    }

    ssize_t written = 0;
    size_t wfilled = 0;
    size_t windowidx = 0;
    bool outisfile = S_ISREG(mode(out));
    while (1) {
        /* copy the input to the kernel buffer, passing SIZE_MAX or even
         * SSIZE_MAX doesn't (always) work. The call returns "Invalid
         * argument" */
        ssize_t rcvd = splice(in, NULL, inpipe[1], NULL, (size_t) INT_MAX,
                SPLICE_F_MORE | SPLICE_F_MOVE);
        if (rcvd == -1) {
            perror("input splice()");
            goto error;
        }

        if (rcvd == 0) {
            /* reached the end of the input file */
            break;
        }

        TRACE("recvd %zd bytes\n", rcvd);

        /* "copy" to temporary buffer */
        ssize_t teed = tee(inpipe[0], outpipe[1], (size_t) rcvd, SPLICE_F_NONBLOCK);
        if (teed == -1) {
            /* TODO:  this is definitely wrong in this case */
            if (errno == EAGAIN) {
                usleep(1000);
                continue;
            }
            perror("tee()");
            goto error;
        }

        if (teed == 0) {
            /* we're done! */
            break;
        }

        TRACE("teed %zd bytes\n", teed);

        /* stream len bytes to `out` and `file` */
        if (!spliceall(outpipe[0], file, teed)) {
            perror("file splice()");
            goto error;
        }

        if (!spliceall(inpipe[0], out, teed)) {
            perror("out splice()");
            goto error;
        }

        /* don't thrash the page cache, we won't be reading the file anyway,
         * we don't call it on every iteration (save CPU) quite
         * CPU-intensive to call; wait until a window has filled up */
        if (wfilled >= WINDOW) {
            swapwindow(file, windowidx, WINDOW);

            /* technically, the out fd could also be a file (example: `utee
             * file1 > file2`), in that case also clear the page cache for
             * the out fd */
            if (outisfile && g_force_no_thrash) {
                swapwindow(out, windowidx, WINDOW);
            }

            wfilled = 0;
            windowidx++;
        }

        written += rcvd;
        wfilled += rcvd;
    }

    /* discard the last slice of data */
    if (windowidx) {
        discard(file, WINDOW * (windowidx - 1), WINDOW + wfilled);
        if (outisfile && g_force_no_thrash) {
            discard(out, WINDOW * (windowidx - 1), WINDOW + wfilled);
        }
    }

    return written;

error:
    close(outpipe[0]);
    close(outpipe[1]);
parterror:
    close(inpipe[0]);
    close(inpipe[1]);

    return -1;
}

static ssize_t ttee(int pin, int pout, int file) {
    ssize_t written = 0;
    do {
        /* this will work if both stdin and stdout are pipes */
        ssize_t len = tee(pin, pout, SSIZE_MAX, SPLICE_F_NONBLOCK);

        if (len == -1) {
            if (errno == EAGAIN) {
                usleep(1000);
                continue;
            }
            perror("tee()");
            return -1;
        }
        else if (len == 0) {
            /* we're done! */
            break;
        }

        /* send the output to a file (consumes it) */
        if (!spliceall(pin, file, len)) {
            perror("splice():");
            return -1;
        }

        written += len;
    } while(1);

    return written;
}

static const char *parseopts(int argc, char *argv[]) {
    int option = 0;

    while ((option = getopt(argc, argv, "vc")) != -1) {
        switch (option) {
            case 'v': g_verbose = 1; break;
            case 'c': g_force_no_thrash = true; break;
            default: return NULL;
        }
    }

    if (argc <= optind) {
        return NULL;
    }

    return argv[optind];
}

int main(int argc, char *argv[]) {
    const char *file = parseopts(argc, argv);
    if (!file) {
        usage();
    }

    TRACE("Welcome to utee version " VERSION "\n");

    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("couldn't create file");
        exit(EXIT_FAILURE);
    }

    mode_t inmode = mode(STDIN_FILENO);
    mode_t outmode = mode(STDOUT_FILENO);

    TRACE("STDIN is a %s!\n", typestr(inmode));
    TRACE("STDOUT is a %s!\n", typestr(outmode));

    if (S_ISREG(inmode)) {
        /* technically it would be best to supply POSIX_FADV_NOREUSE, but
         * since that's a no-op on Linux we'll at least try to make linux
         * perform more readahead. */
        fadvise(STDIN_FILENO, POSIX_FADV_SEQUENTIAL);
    }

    int status = EXIT_SUCCESS;
    ssize_t written = -1;
    /* detect which kind of file descript stdin and stdout are, some
     * possibilities are: pipe, file, tty, ... */
    if (S_ISFIFO(inmode) && S_ISFIFO(outmode)) {
        /* both stdin and stdout are pipes (this happens in a shell when
         * doing for example "... | utee | ..."), which means we don't have
         * to create intermediate pipes */
        TRACE("input and output are pipes, taking a shortcut\n");
        written = ttee(STDIN_FILENO, STDOUT_FILENO, fd);
    }
    else {
        if (fcntl(STDOUT_FILENO, F_GETFL) & O_APPEND) {
            fputs("can't output to an append-mode file, use regular tee\n", stderr);
            status = EXIT_FAILURE;
            goto end;
        }

        /* either stdin or stdout is not a pipe, so we use intermediate
         * pipes to be able to use tee()/splice(), thus avoiding user-space
         * buffers */
        TRACE("input or output is not a pipe, creating intermediary pipes\n");
        written = ctee(STDIN_FILENO, STDOUT_FILENO, fd);
    }

    if (written == -1) {
        status = EXIT_FAILURE;
        goto end;
    }

    TRACE("wrote %zd bytes\n", written);

end:
    if (S_ISREG(inmode)) {
        /* restore the advice for the infd */
        fadvise(STDIN_FILENO, POSIX_FADV_NORMAL);
    }

    close(fd);
    exit(status);
}

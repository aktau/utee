#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

/* global variables that control program behaviour */
static int g_verbose = 0;

/* macro that only prints when verbosity is enabled (easier than messing
 * with varargs and vprintf) */
#define TRACE(format, ...) \
    do { \
        if (g_verbose > 0) fprintf(stderr, format, ##__VA_ARGS__); \
    } while (0)

static bool spliceall(int infd, int outfd, size_t len) {
    while (len) {
        ssize_t written = splice(infd, NULL, outfd, NULL, (size_t) len,
                SPLICE_F_MORE | SPLICE_F_MOVE);
        if (written <= 0) {
            return false;
        }

        len -= written;
    }

    return true;
}

_Noreturn static void usage() {
    puts("Usage: tee <filename>");
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

        written += rcvd;
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

    while ((option = getopt(argc, argv, "v")) != -1) {
        switch (option) {
            case 'v': g_verbose = 1; break;
            /* default: return false; */
            default: return NULL;
        }
    }

    return argv[optind];
}

int main(int argc, char *argv[]) {
    const char *file = parseopts(argc, argv);
    if (!file) {
        usage();
    }

    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("couldn't create file");
        exit(EXIT_FAILURE);
    }

    mode_t inmode = mode(STDIN_FILENO);
    mode_t outmode = mode(STDOUT_FILENO);

    TRACE("STDIN is a %s!\n", typestr(inmode));
    TRACE("STDOUT is a %s!\n", typestr(outmode));

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
    close(fd);
    exit(status);
}

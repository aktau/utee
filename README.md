microtee
========

Based on a [small implementation of
tee(1)](http://lwn.net/Articles/179434/) with the
[tee()](http://man7.org/linux/man-pages/man2/tee.2.html)/[splice()](http://man7.org/linux/man-pages/man2/splice.2.html)
system calls by Jens Axboe.

That toy example can only handle cases where both stdin and stdout are
pipes. This is because the `tee()` system call only accepts pipes as
input fds. For reference: `splice()` only needs one of the fds to be
pipes.

**utee** tries to alleviate this problem by falling back to an
intermediate pipe if it has to deal with input or ouput that's not a
pipe. An pipe is a kernel buffer, so no user-space buffering is done.

So, in the simple case where both stdin and stdout are already pipes,
**utee** does (pseudo-code):

```c
tee(stdin, stdout);
splice(stdin, outfile);
```

Otherwise, **utee** falls back to something like this:

```c
/* use intermediate pipes */
int inpipe[2], outpipe[2];
pipe(inpipe);
pipe(outpipe);

/* stream the input to an intermediate pipe and copy that pipe to
 * another intermediate pipe with tee() */
splice(stdin, pipefd[1]);
tee(inpipe[0], outpipe[1]);

/* copy to both stdout and the outfile */
splice(inpipe[0], stdout);
splice(outpipe[0], outfile);
```

Of course, there could be intermediate cases where either stdin or
stdout is a pipe, but I haven't implemented that yet.

It's just a small experiment of mine because I wanted to get to know
`tee()` and `splice()` better. I don't expect it will see any actual
production use. I haven't even looked at the original `tee(1)`
implementation to see if it actually uses `tee()`/`splice()`. It's
possible, but those system calls only exist on rather modern linux
(2.16.17) so perhaps not. Linus more or less predicted that there would
be few users of these system calls, even if they were really awesome.

Advantages:
- No user space buffers
  - This should be fast
  - Should be able to use the underlying devices' DMA engine
- Just a single tiny source file

Disadvantages:
- Not portable to other UNIXes until they implement `tee()`/`splice()`

Building
========

It could be as simple as:

```bash
$ gcc utee.c -o utee && ./utee echo.txt
```

Or you could use the Makefile:

```bash
$ make
# or if you want a debug version:
$ make debug
```

Requirements
============

These are minimum version, though I advise to use something more recent
as the initial implementations had bugs:

- Linux kernel >= 2.6.17
- Glibc >= 2.5

TODO
====

- Try `fcntl(fd, F_SETPIPE_SZ, ...)` and see if it can do something for
  utee.

Resources
=========

Things that talk about `tee()` and `splice()` or zero-copy thatt I found online.

- [Archive of things Linus has said about tee() and
  splice()](http://yarchive.net/comp/linux/splice.html)
- [Some more things Linus
  said](https://web.archive.org/web/20130521163124/http://kerneltrap.org/node/6505)
  may overlap slightly with the link above.
- [Zero-Copy with sendfile and
  splice](http://blog.superpat.com/2010/06/01/zero-copy-in-linux-with-sendfile-and-splice/)
- [scribd: splice, tee, vmsplice, zero-copy in
  Linux](http://www.scribd.com/doc/4006475/Splice-Tee-VMsplice-zero-copy-in-Linux)

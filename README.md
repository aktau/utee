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
pipe. A pipe is a kernel buffer, so no user-space buffering is done.

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
production use.

Advantages:
- No user space buffers
  - This should be fast
  - Should be able to use the underlying devices' DMA engine
- Just a single tiny source file

Disadvantages:
- Not portable to other UNIXes until they implement `tee()`/`splice()`

Comparison
==========

Inspecting the source of [GNU
tee](https://github.com/coreutils/coreutils/blob/master/src/tee.c)
reveals that it relies on a static buffer and a simple
`read()`/`write()` pair for its operation. Running it under strace
confirms it:

```bash
$ strace tee copy1.file < input.file > copy2.file
...
read(0,
"WK\277\363\327\323O\317_\375r\254\213/\202J\312\306+\244\275\333\363\374R{\371zz\374+>"...,
8192) = 8192
write(1,
"WK\277\363\327\323O\317_\375r\254\213/\202J\312\306+\244\275\333\363\374R{\371zz\374+>"...,
8192) = 8192
write(3,
"WK\277\363\327\323O\317_\375r\254\213/\202J\312\306+\244\275\333\363\374R{\371zz\374+>"...,
8192) = 8192
read(0,
"\302\303:qy>\332Q\305\375\207\215\4\37\307\203bc;\310\272\36kA\364PK\323=\3120\332"...,
8192) = 8192
write(1,
"\302\303:qy>\332Q\305\375\207\215\4\37\307\203bc;\310\272\36kA\364PK\323=\3120\332"...,
8192) = 8192
write(3,
"\302\303:qy>\332Q\305\375\207\215\4\37\307\203bc;\310\272\36kA\364PK\323=\3120\332"...,
8192) = 8192
...
```

Doing the same with **utee** gives:

```bash
$ strace utee copy1.file < input.file > copy2.file
tee(0x4, 0x7, 0x10000, 0x2)             = 65536
splice(0x6, 0, 0x3, 0, 0x10000, 0x5)    = 65536
splice(0x4, 0, 0x1, 0, 0x10000, 0x5)    = 65536
splice(0, 0, 0x5, 0, 0x7fffffff, 0x5)   = 65536
tee(0x4, 0x7, 0x10000, 0x2)             = 65536
splice(0x6, 0, 0x3, 0, 0x10000, 0x5)    = 65536
splice(0x4, 0, 0x1, 0, 0x10000, 0x5)    = 65536
splice(0, 0, 0x5, 0, 0x7fffffff, 0x5)   = 65536
```

Ghetto benchmarks show that this approach is about 8-10% faster than
`read()`/`write()`. Not as much as I expected. I first thought that
enlarging the (intermediate) pipes might help, but as we see the GNU tee
buffer is already 8 times smaller than the pipe buffer used by utee. So
that's unlikely to be a bottleneck. Perhaps it's limited by HDD write speed.

To test that hypothesis I tried redirecting to `/dev/null`:

```bash
$ time tee /dev/null < input.file > /dev/null
tee /dev/null < input.file > /dev/null  0.01s user 0.03s
system 95% cpu 0.046 total
$ time utee /dev/null < input.file > /dev/null
utee /dev/null < input.file > /dev/null  0.00s user 0.01s
system 75% cpu 0.011 total
```

So about 4x faster, with the HDD for the input file warmed up (I tried
multiple interleaved runs).

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

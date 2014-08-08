PREFIX := /usr/local

CFLAGS_DEBUG := -g3 \
	-O0 \
	-Werror \
	-DDEBUG

CFLAGS_RELEASE := -O2 \
	-DNDEBUG

# detect the compiler, perhaps enable some more features
CC_OUTPUT := $(shell $(CC) -v 2>&1 | grep -Ei 'clang|gcc')
# CC_TYPE :=
ifneq (,$(findstring clang,$(CC_OUTPUT)))
	CC_TYPE := clang
else ifneq (,$(findstring gcc,$(CC_OUTPUT)))
	CC_TYPE := gcc
else
	CC_TYPE := unknown
endif

ifeq ($(CC_TYPE),clang)
	# we're going to assume clang always supports C11 (clang is usually more
	# recent)
	STD := -std=c11
else ifeq ($(CC_TYPE),gcc)
	# gcc 4.7+ supports C11
	GCC_GTEQ_47 := $(shell expr `gcc -dumpversion | sed -e 's/\.\([0-9][0-9]\)/\1/g' -e 's/\.\([0-9]\)/0\1/g' -e 's/^[0-9]\{3,4\}$$/&00/'` \>= 40700)
	ifeq "$(GCC_GTEQ_47)" "1"
		STD := -std=c11
	endif
endif

STD ?= -std=c99
WARN := -Wall -Wextra -pedantic \
	-Wcast-align -Wcast-qual -Wpointer-arith \
	-Wredundant-decls -Wformat=2 \
	-Wunreachable-code -Wfloat-equal \
	-Wstrict-aliasing=2 -Wstrict-overflow=5 \
	-Wdisabled-optimization -Wshadow -Wmissing-braces
PROT := -D_FORTIFY_SOURCE=2 -fstack-protector
VERSION := $(shell git describe --always --dirty)
CFLAGS ?= $(STD) $(WARN) $(PROT) -DVERSION=\"$(VERSION)\"

# the default target is release
all: release

# when the target is debug,
# add CFLAGS_DEBUG to CFLAGS
debug: CFLAGS += $(CFLAGS_DEBUG)
debug: utee

# when the target is release,
# add CFLAGS_RELEASE to CFLAGS
release: CFLAGS += $(CFLAGS_RELEASE)
release: utee

utee: utee.c

install: playpen
	install -Dm755 $< $(DESTDIR)$(PREFIX)/bin/$<

clean:
	@rm utee || true

.PHONY: clean install

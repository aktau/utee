CFLAGS_DEBUG := -g3 \
	-O0 \
	-Werror \
	-DDEBUG

CFLAGS_RELEASE := -O2 \
	-DNDEBUG

STD := -std=c99
WARN := -Wall -Wextra \
	-Wcast-align -Wcast-qual -Wpointer-arith \
	-Wredundant-decls -Wformat=2 \
	-Wunreachable-code -Wfloat-equal \
	-Wstrict-aliasing=2 -Wstrict-overflow=5 \
	-Wdisabled-optimization -Wshadow -Wmissing-braces \
	-D_FORTIFY_SOURCE=2
CFLAGS ?= $(STD) $(WARN)

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
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm utee

.PHONY: clean

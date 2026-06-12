# Host-native unit tests: one executable per tests/unit/test_*.c, built with
# sanitizers and warnings. Each test lists the src/ objects it needs via a
# test-local <name>_SRC. src/hal/ and src/lib/ are not host-testable.

CC      ?= cc
SRCDIR  := ../src
CFLAGS  := -O1 -g -std=gnu11 -I$(SRCDIR) -Wall -Wextra -Wno-unused-parameter \
           -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address,undefined

TESTS := $(basename $(notdir $(wildcard unit/test_*.c)))
BINS  := $(addprefix unit/,$(TESTS))

# Per-test source dependencies: <name>_SRC is linked in, <name>_DEPS only
# triggers rebuilds (for sources a test #includes rather than links).
test_nal_SRC := $(SRCDIR)/fmt/nal.c
test_rtmp_chunk_SRC := $(SRCDIR)/fmt/flv.c $(SRCDIR)/fmt/amf.c $(SRCDIR)/fmt/bitbuf.c
test_rtmp_chunk_DEPS := $(SRCDIR)/rtmp.c

.PHONY: all clean
all: $(BINS)
	@fail=0; for t in $(BINS); do echo "== $$t =="; ./$$t || fail=1; done; \
	[ $$fail -eq 0 ] && echo "all unit tests passed" || { echo "UNIT TESTS FAILED"; exit 1; }

.SECONDEXPANSION:
unit/%: unit/%.c $$($$*_SRC) $$($$*_DEPS)
	$(CC) $(CFLAGS) -o $@ $< $($*_SRC) $(LDFLAGS)

clean:
	rm -f $(BINS)

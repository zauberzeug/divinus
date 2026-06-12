# Host-native test build for divinus. Compiles the vendor-free, hardware-free
# sources (src/fmt/, src/rtsp/, ...) with the host compiler plus sanitizers and
# warnings, links one executable per tests/unit/test_*.c, and runs them.
#
#   make            build + run every unit test
#   make clean
#
# src/hal/ (hardware-bound) and src/lib/ (vendored third-party) stay out of host
# tests. Each test lists the source objects it needs via test-local <name>_SRC.

CC      ?= cc
SRCDIR  := ../src
CFLAGS  := -O1 -g -std=gnu11 -I$(SRCDIR) -Wall -Wextra -Wno-unused-parameter \
           -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address,undefined

TESTS := $(basename $(notdir $(wildcard unit/test_*.c)))
BINS  := $(addprefix unit/,$(TESTS))

# Per-test source dependencies (besides the test file itself).
test_nal_SRC := $(SRCDIR)/fmt/nal.c

.PHONY: all clean
all: $(BINS)
	@fail=0; for t in $(BINS); do echo "== $$t =="; ./$$t || fail=1; done; \
	[ $$fail -eq 0 ] && echo "all unit tests passed" || { echo "UNIT TESTS FAILED"; exit 1; }

unit/%: unit/%.c
	$(CC) $(CFLAGS) -o $@ $< $($*_SRC) $(LDFLAGS)

clean:
	rm -f $(BINS)

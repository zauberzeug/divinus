# Host-native unit tests: one executable per tests/unit/test_*.c, built with
# sanitizers and warnings. Each test lists the src/ objects it needs via a
# test-local <name>_SRC. Vendor-bound code (src/hal/<vendor>/, src/lib/) is
# not host-testable; pure policy modules like src/hal/frc.c are.

CC      ?= cc
SRCDIR  := ../src
CFLAGS  := -O1 -g -std=gnu11 -I$(SRCDIR) -Wall -Wextra -Wno-unused-parameter \
           -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address,undefined

TESTS := $(basename $(notdir $(wildcard unit/test_*.c)))
BINS  := $(addprefix unit/,$(TESTS))

# Per-test source dependencies (besides the test file itself).
test_nal_SRC := $(SRCDIR)/fmt/nal.c
test_frc_SRC := $(SRCDIR)/hal/frc.c

.PHONY: all clean
all: $(BINS)
	@fail=0; for t in $(BINS); do echo "== $$t =="; ./$$t || fail=1; done; \
	[ $$fail -eq 0 ] && echo "all unit tests passed" || { echo "UNIT TESTS FAILED"; exit 1; }

.SECONDEXPANSION:
unit/%: unit/%.c $$($$*_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BINS)

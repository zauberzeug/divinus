# Host-native unit tests: one executable per tests/unit/test_*.c, built with
# sanitizers and warnings. Each test lists the src/ objects it needs via a
# test-local <name>_SRC. src/lib/ and most of src/hal/ are vendor-coupled and
# not host-testable; plain-C exceptions like hal/tools.c link fine.

CC      ?= cc
SRCDIR  := ../src
CFLAGS  := -O1 -g -std=gnu11 -I$(SRCDIR) -Wall -Wextra -Wno-unused-parameter \
           -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS := -fsanitize=address,undefined

TESTS := $(basename $(notdir $(wildcard unit/test_*.c)))
BINS  := $(addprefix unit/,$(TESTS))

# Per-test source dependencies (besides the test file itself).
test_nal_SRC := $(SRCDIR)/fmt/nal.c
test_http_headers_SRC := $(SRCDIR)/http_headers.c $(SRCDIR)/hal/tools.c

.PHONY: all clean
all: $(BINS)
	@fail=0; for t in $(BINS); do echo "== $$t =="; ./$$t || fail=1; done; \
	[ $$fail -eq 0 ] && echo "all unit tests passed" || { echo "UNIT TESTS FAILED"; exit 1; }

.SECONDEXPANSION:
unit/%: unit/%.c $$($$*_SRC)
	$(CC) $(CFLAGS) -o $@ $< $($*_SRC) $(LDFLAGS)

clean:
	rm -f $(BINS)

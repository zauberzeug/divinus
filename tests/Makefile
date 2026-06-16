# Host-native unit tests: one executable per tests/unit/test_*.c, built with
# sanitizers and warnings. Each test lists the src/ objects it needs via a
# test-local <name>_SRC. Vendor-bound code (src/hal/<vendor>/, src/lib/) is
# not host-testable; pure policy modules (src/hal/frc.c) and plain-C
# exceptions (hal/tools.c) link fine.

CC      ?= cc
SRCDIR  := ../src
# -pthread: sources such as http_post.c pull in libpthread (e.g.
# pthread_attr_setstacksize). System glibc >= 2.34 folds it into libc so the
# link succeeds without it, but toolchains with a split libpthread (the conda
# gcc used in CI) need it explicitly or the link fails with
# "DSO missing from command line".
CFLAGS  := -O1 -g -std=gnu11 -I$(SRCDIR) -Wall -Wextra -Wno-unused-parameter \
           -fsanitize=address,undefined -fno-omit-frame-pointer -pthread
LDFLAGS := -fsanitize=address,undefined -pthread

TESTS := $(basename $(notdir $(wildcard unit/test_*.c)))
BINS  := $(addprefix unit/,$(TESTS))

# Per-test source dependencies: <name>_SRC is linked in, <name>_DEPS only
# triggers rebuilds (for sources a test #includes rather than links).
test_nal_SRC := $(SRCDIR)/fmt/nal.c
test_rtmp_chunk_SRC := $(SRCDIR)/fmt/flv.c $(SRCDIR)/fmt/amf.c $(SRCDIR)/fmt/bitbuf.c
test_rtmp_chunk_DEPS := $(SRCDIR)/rtmp.c
test_http_headers_SRC := $(SRCDIR)/http_headers.c $(SRCDIR)/hal/tools.c
test_config_bounds_SRC := $(SRCDIR)/hal/config.c $(SRCDIR)/hal/tools.c \
                          $(SRCDIR)/stream_cfg.c
test_api_stream_SRC := $(SRCDIR)/stream_cfg.c $(SRCDIR)/hal/tools.c \
                       $(SRCDIR)/hal/config.c
test_http_post_SRC := $(SRCDIR)/http_post.c
test_mime_SRC := $(SRCDIR)/rtsp/mime.c
test_record_SRC := $(SRCDIR)/record.c $(SRCDIR)/fmt/mp4.c $(SRCDIR)/fmt/bitbuf.c \
                   $(SRCDIR)/fmt/moof.c $(SRCDIR)/fmt/moov.c $(SRCDIR)/fmt/nal.c
test_sock_send_SRC := $(SRCDIR)/sock_send.c
test_stream_rtp_SRC := $(SRCDIR)/stream.c $(SRCDIR)/fmt/rtppkt.c $(SRCDIR)/hal/tools.c \
                       $(SRCDIR)/hal/captime.c
test_rtp_split_SRC := $(SRCDIR)/fmt/rtppkt.c
test_rtppkt_SRC := $(SRCDIR)/fmt/rtppkt.c
test_frc_SRC := $(SRCDIR)/hal/frc.c
test_captime_SRC := $(SRCDIR)/hal/captime.c
test_rtcp_sr_SRC := $(SRCDIR)/hal/captime.c
test_gain_SRC := $(SRCDIR)/gain.c

.PHONY: all clean
all: $(BINS)
	@fail=0; for t in $(BINS); do echo "== $$t =="; ./$$t || fail=1; done; \
	[ $$fail -eq 0 ] && echo "all unit tests passed" || { echo "UNIT TESTS FAILED"; exit 1; }

.SECONDEXPANSION:
unit/%: unit/%.c $$($$*_SRC) $$($$*_DEPS)
	$(CC) $(CFLAGS) -o $@ $< $($*_SRC) $(LDFLAGS)

clean:
	rm -f $(BINS)

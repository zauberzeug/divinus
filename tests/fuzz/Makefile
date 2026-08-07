# libFuzzer harnesses for the RTSP and HTTP request parsers.
#
# Needs clang (libFuzzer ships with it); the host unit suite's gcc has no
# -fsanitize=fuzzer. Ubuntu runners and `apt install clang` both provide it.
# FUZZ_CC (not CC, which make predefines to cc) selects the compiler.
#
#   make -C tests/fuzz build          # compile both harnesses
#   make -C tests/fuzz smoke-http     # bounded HTTP campaign (blocking)
#   make -C tests/fuzz smoke-rtsp     # bounded RTSP campaign
#   make -C tests/fuzz smoke          # both
#   FUZZ_SECS=300 make -C tests/fuzz smoke   # longer local campaign
#
# NOTE: smoke-rtsp reproduces the #34 SETUP track-id OOB on the UNhardened
# parser in seconds; it runs clean only against the hardened rtsp.c
# (fix/rtsp-control-robust). See tests/README.md.

FUZZ_CC  ?= clang
SRCDIR   := ../../src
FUZZ_SECS ?= 30
FUZZFLAGS := -g -O1 -std=gnu11 -I$(SRCDIR) -fsanitize=fuzzer,address,undefined \
             -fno-omit-frame-pointer -pthread
RUNFLAGS  := -max_len=4096 -print_final_stats=1 -timeout=10

RTSP_SRC := $(SRCDIR)/hal/tools.c $(SRCDIR)/hal/captime.c $(SRCDIR)/http_headers.c
HTTP_SRC := $(SRCDIR)/http_headers.c $(SRCDIR)/hal/tools.c

.PHONY: build smoke smoke-rtsp smoke-http clean

build: fuzz_rtsp fuzz_http

fuzz_rtsp: fuzz_rtsp.c $(SRCDIR)/rtsp/rtsp.c
	$(FUZZ_CC) $(FUZZFLAGS) $< $(RTSP_SRC) -lm -o $@

fuzz_http: fuzz_http.c
	$(FUZZ_CC) $(FUZZFLAGS) $< $(HTTP_SRC) -lm -o $@

# ASAN_OPTIONS: leaks off (parser keeps some intentional process-lifetime state);
# a fresh corpus copy keeps the checked-in seeds pristine.
smoke-http: fuzz_http
	@rm -rf .work_http && cp -r corpus_http .work_http
	ASAN_OPTIONS=detect_leaks=0 ./fuzz_http -max_total_time=$(FUZZ_SECS) $(RUNFLAGS) .work_http

smoke-rtsp: fuzz_rtsp
	@rm -rf .work_rtsp && cp -r corpus_rtsp .work_rtsp
	ASAN_OPTIONS=detect_leaks=0 ./fuzz_rtsp -max_total_time=$(FUZZ_SECS) $(RUNFLAGS) .work_rtsp

smoke: smoke-http smoke-rtsp

clean:
	rm -f fuzz_rtsp fuzz_http
	rm -rf .work_http .work_rtsp crash-* oom-* timeout-* leak-*

# Standalone unit tests for the LPC profiler aggregator.
#
# These tests build against src/lpc_profile_agg.c directly, with the
# LPC_PROFILE_AGG_STANDALONE_TEST macro defined so the module's allocator
# and hash hooks fall back to libc instead of the driver's xalloc + hashmem32.
# Nothing here depends on the rest of the driver build — invoke as:
#
#     make -C src/test test-agg
#
# ASan+UBSan are on by default so use-after-free, off-by-one resize, and
# uninitialised-memory bugs surface immediately.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -O1
INCLUDE  = -I..

.PHONY: all test-agg clean

all: test-agg

test_lpc_profile_agg: test_lpc_profile_agg.c ../lpc_profile_agg.c ../lpc_profile_agg.h
	$(CC) $(CFLAGS) $(INCLUDE) test_lpc_profile_agg.c -o $@

test-agg: test_lpc_profile_agg
	./test_lpc_profile_agg

clean:
	rm -f test_lpc_profile_agg

# Standalone unit tests for the LPC profiler aggregator.
# Run as:  make -C src/test test-agg
# ASan + UBSan are on by default.

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

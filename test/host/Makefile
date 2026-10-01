# Host tests for the parts of the firmware that are plain C.
#   make -C test/host
CFLAGS ?= -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer
MAIN := ../../main

test_ogg_sniff: test_ogg_sniff.c $(MAIN)/ogg_sniff.c $(MAIN)/ogg_sniff.h
	$(CC) $(CFLAGS) -I$(MAIN) -o $@ test_ogg_sniff.c $(MAIN)/ogg_sniff.c

test_mixer: test_mixer.c $(MAIN)/mixer.c $(MAIN)/mixer.h
	$(CC) $(CFLAGS) -I$(MAIN) -o $@ test_mixer.c $(MAIN)/mixer.c -lm

# Days of simulated audio: optimised, without the sanitizers.
test_drift: test_drift.c $(MAIN)/drift.c $(MAIN)/drift.h
	$(CC) -O2 -g -Wall -Wextra -I$(MAIN) -o $@ test_drift.c $(MAIN)/drift.c -lm

.PHONY: test clean
test: test_ogg_sniff test_drift test_mixer
	./test_ogg_sniff
	./test_drift
	./test_mixer

clean:
	rm -f test_ogg_sniff test_drift test_mixer

.DEFAULT_GOAL := test

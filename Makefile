CC     ?= gcc
CFLAGS ?= -O3 -march=native -Wall

all: kl cm

kl: src/kl.c
	$(CC) $(CFLAGS) -o $@ $<

cm: src/cm.c
	$(CC) $(CFLAGS) -o $@ $< -lm

# Memory-safety builds. Both bugs this codec has shipped were found here,
# not by the round-trip tests.
kl-san: src/kl.c
	$(CC) -O1 -g -fsanitize=address,undefined -o $@ $<

test: kl kl-san
	./tests/run_tests.sh

# Every corpus at every level under both builds. Most of an hour.
test-full: kl kl-san
	FULL=1 ./tests/run_tests.sh

clean:
	rm -f kl cm kl-san

.PHONY: all test test-full clean

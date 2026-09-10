# Host test harness for the Rockpocket Stick engine.
#
#   make        build
#   make test   build and run
#
# Needs nothing but a C compiler: apps/stick.c has no Rockbox dependencies.

APPS    := ../../apps
CC      ?= gcc
CFLAGS  ?= -O2 -g -std=gnu99 -Wall -Wextra -Wno-unused-parameter -I$(APPS)
LDFLAGS ?= -lm

SRC := stick_test.c $(APPS)/stick.c
BIN := stick_test

all: $(BIN)

$(BIN): $(SRC) $(APPS)/stick.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

test: $(BIN)
	./$(BIN)

# Replay a captured gesture trace: make replay TRACE=traces/foo.trace
replay: $(BIN)
	./$(BIN) --trace $(TRACE)

clean:
	rm -f $(BIN) $(BIN).exe

.PHONY: all test replay clean

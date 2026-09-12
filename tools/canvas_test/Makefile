# Host test harness for the Rockpocket Canvas engine.
#
#   make        build
#   make test   build and run
#   make dump   build, run, and write PPM samples to look at
#
# Needs nothing but a C compiler: apps/canvas.c has no Rockbox dependencies.

APPS    := ../../apps
CC      ?= gcc
CFLAGS  ?= -O2 -g -std=gnu99 -Wall -Wextra -Wno-unused-parameter -I$(APPS)
LDFLAGS ?=

SRC := canvas_test.c $(APPS)/canvas.c
BIN := canvas_test

# The glue needs stand-ins for lcd.h, config.h and core_alloc.h; they live
# in stubs/ and are deliberately hostile (the fake buflib moves the block
# on every call, exactly as the real one may).
GSRC := glue_test.c $(APPS)/canvas_glue.c $(APPS)/canvas.c
GBIN := glue_test
GCFLAGS := $(CFLAGS) -Istubs

all: $(BIN) $(GBIN)

$(BIN): $(SRC) $(APPS)/canvas.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(GBIN): $(GSRC) $(APPS)/canvas.h $(APPS)/canvas_glue.h stubs/lcd.h
	$(CC) $(GCFLAGS) -o $@ $(GSRC) $(LDFLAGS)

test: $(BIN) $(GBIN)
	./$(BIN)
	./$(GBIN)

dump: $(BIN)
	./$(BIN) --dump

clean:
	rm -f $(BIN) $(BIN).exe $(GBIN) $(GBIN).exe *.ppm *.png

.PHONY: all test dump clean

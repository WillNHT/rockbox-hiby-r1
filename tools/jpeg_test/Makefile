# Host harness for the JPEG decoder used for covers and backdrops.
#
#   make        build
#   make test   decode the seeds, then 2000 mutations of each, under
#               the sanitizers
#   make fuzz   the same with 50000 mutations each (minutes)
#
# Builds apps/recorder/jpeg_load.c as is; stubs/ stands in for the
# Rockbox headers it includes.

RECORDER := ../../apps/recorder
CC       ?= gcc
# AddressSanitizer for reads and writes past a buffer, and the UBSan
# checks for what takes a device down: divide by zero (#195), bounds and
# null. Not shifts or signed overflow: the decoder relies on both wrapping
# the way the CPUs we run on do, and a garbled coefficient is harmless.
SAN      ?= -fsanitize=address -fsanitize=integer-divide-by-zero,bounds,null,vla-bound \
            -fno-sanitize-recover=all
CFLAGS   ?= -O1 -g -std=gnu99 -Wall -Wno-pointer-sign -fno-omit-frame-pointer
CFLAGS   += $(SAN) -Istubs -I$(RECORDER)
FUZZ     ?= 2000

SRC := jpeg_test.c $(RECORDER)/jpeg_load.c
BIN := jpeg_test

all: $(BIN)

$(BIN): $(SRC) $(RECORDER)/jpeg_load.h $(RECORDER)/jpeg_common.h $(wildcard stubs/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(SAN)

test: $(BIN)
	./$(BIN) --fuzz $(FUZZ) seeds/*.jpg bad/*.jpg

fuzz:
	$(MAKE) test FUZZ=50000

clean:
	rm -f $(BIN) $(BIN).exe mutation.jpg

.PHONY: all test fuzz clean

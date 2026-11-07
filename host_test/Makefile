# Host build for picoruby-midi unit tests (parser, transport registry, routes,
# BLE-MIDI codec).
# No FreeRTOS / ESP-IDF — just gcc/clang + libc.
#
#     make            # build + run
#     make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c99 -I..
LDFLAGS ?=

SRC     = ../src/midi_parser.c test_midi_parser.c
BIN     = test_midi_parser

ROUTE_SRC = ../src/midi_transport_registry.c ../src/midi_route.c test_midi_route.c
ROUTE_BIN = test_midi_route

BLE_SRC = ../src/ble_midi_codec.c test_ble_midi_codec.c
BLE_BIN = test_ble_midi_codec

all: run

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(ROUTE_BIN): $(ROUTE_SRC)
	$(CC) $(CFLAGS) -o $@ $(ROUTE_SRC) $(LDFLAGS)

$(BLE_BIN): $(BLE_SRC)
	$(CC) $(CFLAGS) -o $@ $(BLE_SRC) $(LDFLAGS)

run: $(BIN) $(ROUTE_BIN) $(BLE_BIN)
	./$(BIN)
	./$(ROUTE_BIN)
	./$(BLE_BIN)

clean:
	rm -f $(BIN) $(ROUTE_BIN) $(BLE_BIN)

.PHONY: all run clean

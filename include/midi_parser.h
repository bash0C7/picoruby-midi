/*
 * PicoRuby MIDI - OS-free MIDI byte parser
 *
 * One parser instance per source. Holds the running-status raw parser,
 * SysEx accumulator, and external clock BPM tracker. No OS / FreeRTOS /
 * ESP-IDF dependencies.
 */

#ifndef MIDI_PARSER_DEFINED_H_
#define MIDI_PARSER_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>
#include "midi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_EXTERNAL_CLOCK_SAMPLES     48
#define MIDI_EXTERNAL_CLOCK_MIN_SAMPLES 24

typedef struct midi_parser midi_parser_t;

/* Allocate and initialize a parser. `source` is the value put into
 * out->source on every emitted event. Returns NULL on alloc failure. */
midi_parser_t *midi_parser_new(midi_event_source_t source);

/* Free a parser, including any in-progress SysEx buffer. */
void midi_parser_free(midi_parser_t *p);

/* Reset all parser state (running status, SysEx, BPM tracker). */
void midi_parser_reset(midi_parser_t *p);

/* Feed one raw MIDI byte (e.g. from a UART/DIN stream).
 *
 * `now_us` is the current monotonic time in microseconds. It is consulted
 * only when a 0xF8 clock byte is processed so that the external BPM
 * tracker can compute an instantaneous tempo estimate.
 *
 * Returns true when a complete event has been decoded into *out. When
 * out->type == MIDI_EVENT_SYSEX, the caller takes ownership of
 * out->sysex_data (malloc'd) and is responsible for free()'ing it.
 */
bool midi_parser_feed_raw(midi_parser_t *p, uint8_t byte, uint64_t now_us,
                          midi_event_t *out);

/* Feed one USB-MIDI 4-byte packet (low nibble of the cable+CIN byte goes
 * into `cin`; the three following MIDI bytes go into b1/b2/b3).
 *
 * `now_us` semantics are identical to midi_parser_feed_raw().
 */
bool midi_parser_feed_usb(midi_parser_t *p, uint8_t cin,
                          uint8_t b1, uint8_t b2, uint8_t b3,
                          uint64_t now_us, midi_event_t *out);

/* Latest external BPM estimate. 0.0 until enough samples are collected. */
float midi_parser_get_external_bpm(const midi_parser_t *p);

/* Where the external clock is in the song. Start (position 0), Continue
 * and Stop run and halt it, Song Position Pointer moves it, and each clock
 * while it runs takes the next position. */
typedef struct {
    bool    running;    /* between Start / Continue and Stop */
    int32_t tick;       /* position of the last clock, in clocks (24 a
                         * quarter note); -1 until a clock comes after
                         * Start / Continue */
    int64_t at_us;      /* when that clock came, on the now_us clock,
                         * fitted over the recent clocks */
    float   period_us;  /* microseconds between clocks; 0.0 until known */
} midi_parser_clock_t;

/* The song position of the external clock (any thread). false until a
 * clock has come. */
bool midi_parser_get_external_clock(const midi_parser_t *p, midi_parser_clock_t *out);

/* Reset only the external BPM tracker (e.g. on MIDI Start). */
void midi_parser_reset_external_clock(midi_parser_t *p);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_PARSER_DEFINED_H_ */

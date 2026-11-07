/*
 * PicoRuby MIDI - timed packet ring (ESP32)
 *
 * A ring of 4-byte USB-MIDI packets, each with the time it may be read. A
 * transport whose input arrives in bursts (BLE-MIDI: per connection event)
 * puts its packets in with the times they should be passed on
 * (ble_midi_sync_release) and reads them as its packet source; the ring has
 * the input task woken when the next one is due (MIDI_Input_kick), so the
 * spacing does not depend on the input task's polling period.
 *
 *   one producer : midi_timed_ring_push ... midi_timed_ring_commit
 *   one consumer : midi_timed_ring_bytes_due / midi_timed_ring_read
 *
 * Times are esp_timer_get_time() microseconds and must not go backward.
 */

#ifndef MIDI_TIMED_RING_DEFINED_H_
#define MIDI_TIMED_RING_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct midi_timed_ring midi_timed_ring_t;

/* `packets` is rounded up to a power of two. The buffers may be in PSRAM.
 * NULL when out of memory. */
midi_timed_ring_t *midi_timed_ring_new(uint32_t packets, const char *name);

/* Not while either side may still use it. NULL is fine. */
void midi_timed_ring_free(midi_timed_ring_t *r);

/* Producer: false (dropped) when full. Visible after commit. */
bool midi_timed_ring_push(midi_timed_ring_t *r, const uint8_t pkt[4], int64_t at_us);
/* Producer: after a burst of pushes. Wakes the input task for what is due
 * now and sets the timer for the next one. */
void midi_timed_ring_commit(midi_timed_ring_t *r);
/* Drop everything (either side; e.g. on a disconnect) */
void midi_timed_ring_clear(midi_timed_ring_t *r);

/* Consumer: bytes (4 per packet) due now / read up to maxlen of them */
int midi_timed_ring_bytes_due(midi_timed_ring_t *r);
int midi_timed_ring_read(midi_timed_ring_t *r, uint8_t *buf, size_t maxlen);

/* Packets in the ring, due or not */
uint32_t midi_timed_ring_count(midi_timed_ring_t *r);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_TIMED_RING_DEFINED_H_ */

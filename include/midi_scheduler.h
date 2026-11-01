/*
 * PicoRuby MIDI - OS-free Note Scheduler
 *
 * Tracks scheduled note_off events triggered by MIDI_Note_trigger() and
 * fires them when their off-time is reached. The core has no FreeRTOS /
 * esp_timer dependency: the port (or a Ruby loop) drives it by calling
 * midi_scheduler_tick(now_us) periodically and supplies a send callback
 * for emitting note_on / note_off bytes.
 */

#ifndef MIDI_SCHEDULER_DEFINED_H_
#define MIDI_SCHEDULER_DEFINED_H_

#include <stdint.h>
#include "midi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Send callback. Emits a single 3-byte channel-voice MIDI message to the
 * transport(s) selected by `transport_mask` (MIDI_TRANSPORT_*). The CIN
 * is provided for ports that prefer the USB-MIDI 4-byte packet format
 * (0x09 = note_on, 0x08 = note_off). The status byte already has the
 * channel low-nibble baked in.
 */
typedef void (*midi_scheduler_send_fn)(midi_tmask_t transport_mask,
                                       uint8_t cin,
                                       uint8_t status,
                                       uint8_t data1,
                                       uint8_t data2);

/* Reset all slots. Idempotent. */
void midi_scheduler_core_init(void);

/* Reset all slots without emitting note_offs. */
void midi_scheduler_core_deinit(void);

/* Register the send callback. Pass NULL to disable note emission. */
void midi_scheduler_set_send_callback(midi_scheduler_send_fn cb);

/* Drive scheduled note_offs. Caller passes the current monotonic time
 * in microseconds. Any active slot whose off-time has been reached
 * fires its note_off via the registered send callback and the slot is
 * freed.
 */
void midi_scheduler_tick(uint64_t now_us);

/* Send note_on immediately and schedule the matching note_off at
 * now_us + duration_ms * 1000.
 *
 * Returns 0 on success. Returns -1 when all slots are full; the
 * note_on still went out, but no automatic note_off is scheduled.
 */
int midi_scheduler_trigger(midi_tmask_t transport_mask,
                           uint8_t channel, uint8_t note, uint8_t velocity,
                           uint32_t duration_ms, uint64_t now_us);

/* Fire note_off for every active slot. */
void midi_scheduler_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_SCHEDULER_DEFINED_H_ */

/*
 * PicoRuby MIDI - Constants and Clock Timer API
 */

#ifndef MIDI_DEFINED_H_
#define MIDI_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>
#include "midi_transport.h"   /* midi_tmask_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The MIDI clock (platform-specific)
 *
 * One per process: a send-only transport in the registry and a route
 * source (midi_route.h). From MIDI_Clock_init() on, a timer ticks 24 times
 * per quarter note and sends Timing Clock (F8) along the routes from
 * MIDI_Clock_bit() while there are any. Start / Continue / Stop go the same
 * way and leave the F8s running.
 */

/* Start the timer (once; later calls return 0). Call it before routing
 * from the clock in C. Returns 0, or -1 when the timer cannot be made. */
int MIDI_Clock_init(void);

/* Stop and delete the timer */
void MIDI_Clock_deinit(void);

/* The clock's bit in the transport registry (0 if the registry was full) */
midi_tmask_t MIDI_Clock_bit(void);

/* The tempo (clamped to 20..300) */
void MIDI_Clock_set_bpm(float bpm);
float MIDI_Clock_get_bpm(void);

/* Run at `bpm` instead of the tempo while it is above 0 (an application
 * following an external clock); 0 goes back to the tempo. */
void MIDI_Clock_set_follow_bpm(float bpm);

/* What the clock runs at: the follow BPM, else the tempo */
float MIDI_Clock_current_bpm(void);

/* Send Start (FA; the next F8 is the downbeat) / Continue (FB) / Stop (FC)
 * along the clock's routes. Start and Continue start the timer if needed. */
void MIDI_Clock_start(void);
void MIDI_Clock_continue(void);
void MIDI_Clock_stop(void);

/* Back to stopped without sending anything (the application has just sent
 * Stop everywhere at the end of a script) */
void MIDI_Clock_reset(void);

/* Between Start / Continue and Stop */
bool MIDI_Clock_is_running(void);

/* Ticks since MIDI_Clock_init() (wraps), and microseconds until the next */
uint32_t MIDI_Clock_count(void);
uint32_t MIDI_Clock_us_to_next(void);

/* Where the clock is in the song, like MIDI_Input_get_external_clock_from
 * for the clock this process sends: the position of its last tick since
 * Start (24 a quarter note; the ticks a Stop holds do not count after
 * Continue), microseconds since that tick and between ticks. false while
 * stopped or before the first tick after Start. */
bool MIDI_Clock_position(int32_t *tick, int32_t *age_us, float *period_us);

/* Set a callback for each tick (called from the timer task) */
typedef void (*midi_clock_callback_t)(void);
void MIDI_Clock_set_callback(midi_clock_callback_t callback);

/*
 * Cooperative-stop hook
 *
 * The input task periodically consults the registered stop-check function
 * (if any) and exits when it returns true. Application-supplied so
 * picoruby-midi stays free of application-layer references (e.g.
 * picoruby-esp32's script-stop flag).
 */
typedef bool (*midi_stop_check_fn)(void);

/* Register the stop-requested predicate. NULL disables. */
void MIDI_set_stop_check(midi_stop_check_fn fn);

/*
 * MIDI Input Background Task API
 */

/* MIDI event types */
typedef enum {
    MIDI_EVENT_NONE = 0,
    MIDI_EVENT_NOTE_ON,
    MIDI_EVENT_NOTE_OFF,
    MIDI_EVENT_CONTROL_CHANGE,
    MIDI_EVENT_PROGRAM_CHANGE,
    MIDI_EVENT_PITCH_BEND,
    MIDI_EVENT_POLY_AFTERTOUCH,
    MIDI_EVENT_CHANNEL_PRESSURE,
    MIDI_EVENT_CLOCK,
    MIDI_EVENT_START,
    MIDI_EVENT_STOP,
    MIDI_EVENT_CONTINUE,
    MIDI_EVENT_ACTIVE_SENSING,
    MIDI_EVENT_SYSTEM_RESET,
    MIDI_EVENT_SYSEX
} midi_event_type_t;

/* MIDI event source */
typedef enum {
    MIDI_SOURCE_NONE = 0,
    MIDI_SOURCE_USB = 1,
    MIDI_SOURCE_SAM2695 = 2
} midi_event_source_t;

/* Maximum SysEx message length (including F0/F7). Longer messages are truncated. */
#define MIDI_SYSEX_MAX_LEN 512

/* MIDI event structure */
typedef struct {
    midi_event_type_t type;
    midi_event_source_t source;  /* Source of the event (USB or SAM2695) */
    uint8_t channel;
    uint8_t data1;   /* note, cc number, program, pressure */
    uint8_t data2;   /* velocity, cc value */
    int16_t value;   /* pitch bend value (-8192 to 8191) */
    uint16_t sysex_len;      /* SysEx payload length (0 if not SysEx) */
    uint8_t *sysex_data;     /* malloc'd SysEx payload incl. F0/F7, NULL if not SysEx.
                                Consumer owns the buffer and must free() after use. */
} midi_event_t;

/* Initialize input task */
int MIDI_Input_init(void);

/* Deinitialize input task */
void MIDI_Input_deinit(void);

/* Start background processing task */
int MIDI_Input_start(void);

/* Whether parsed events go to the queues MIDI::Input pops from. The
 * bindings turn it on in MIDI::Input._start_task and off in _stop_task;
 * without a reader (routes only) events are not queued. */
void MIDI_Input_set_queueing(bool on);

/* Start the input task for MIDI.route. Like MIDI_Input_start(), but also
 * marks the task as wanted when no source is connected yet, so the USB host
 * driver starts it once a device appears. */
int MIDI_Input_start_routing(void);

/* Make sure something reads route source `src_bit` (what MIDI.route does from
 * Ruby, for C callers such as the MIDI Devices screen): starts the UART's
 * input when it is up but not reading, then the input task. Other sources
 * push their packets themselves. Returns MIDI_Input_start_routing()'s result,
 * or 0. */
int MIDI_route_start_source(midi_tmask_t src_bit);

/* Have the input task read transport `bit` like the USB host (0x01): its
 * read_bytes gives 4-byte USB-MIDI packets, forwarded along the routes from
 * it. Also declares it a route source. Read only while it is connected and a
 * route, filter or watch starts from it (else its packets stay in its buffer).
 * For transports beyond the built-ins, e.g. a second device on a USB hub. */
void MIDI_Input_add_packet_source(midi_tmask_t bit);

/* Measure the external clock (BPM, MIDI_Input_get_external_bpm_from) of
 * packet source `bit` too, like the built-ins. A few sources at most (the
 * rest are ignored). Measured only while the input task reads it. */
void MIDI_Input_track_clock(midi_tmask_t bit);

/* Have the input task make a pass now instead of at its next poll (any
 * task or esp_timer callback; not from an ISR). For packet sources that
 * know when their input is there (midi_timed_ring.h). */
void MIDI_Input_kick(void);

/* Stop background processing task */
void MIDI_Input_stop(void);

/* Check if input task is running */
bool MIDI_Input_is_running(void);

/* Check if input task was ever started (for auto-restart on reconnect) */
bool MIDI_Input_was_started(void);

/* Get number of events available (from USB queue) */
int MIDI_Input_events_available(void);

/* Get number of events available from specific source */
int MIDI_Input_events_available_usb(void);
int MIDI_Input_events_available_sam(void);

/* Pop event from queue (returns false if empty) - legacy, uses USB queue */
bool MIDI_Input_pop_event(midi_event_t *event);

/* Pop event from specific source queue */
bool MIDI_Input_pop_event_usb(midi_event_t *event);
bool MIDI_Input_pop_event_sam(midi_event_t *event);

/* Get external BPM calculated from incoming MIDI clock (USB-MIDI) */
float MIDI_Input_get_external_bpm_usb(void);

/* Get external BPM calculated from incoming MIDI clock (SAM2695/MIDI-DIN) */
float MIDI_Input_get_external_bpm_sam(void);

/* Get external BPM calculated from incoming MIDI clock (legacy - returns USB BPM) */
float MIDI_Input_get_external_bpm(void);

/* Reset external clock tracking for USB-MIDI */
void MIDI_Input_reset_external_clock_usb(void);

/* Reset external clock tracking for SAM2695 */
void MIDI_Input_reset_external_clock_sam(void);

/* Reset external clock tracking (legacy - resets both) */
void MIDI_Input_reset_external_clock(void);

/* External BPM of the route source `src_bit` (USB host 0x01, UART 0x02, USB
 * device 0x04; 0.0 for any other source or until enough clocks arrived).
 * Tracked by the input task while it reads that source. */
float MIDI_Input_get_external_bpm_from(midi_tmask_t src_bit);
void MIDI_Input_reset_external_clock_from(midi_tmask_t src_bit);

/* Song position of the external clock on `src_bit` (midi_parser_get_external_clock):
 * the position of the last clock (24 a quarter note), how long ago it came
 * (us) and the period between clocks (us; 0.0 until known). false while
 * that clock is stopped or no clock has come since Start / Continue. */
bool MIDI_Input_get_external_clock_from(midi_tmask_t src_bit, int32_t *tick,
                                        int32_t *age_us, float *period_us);

/*
 * Filter queue (MIDI.filter / MIDI.on)
 *
 * The port's sink for the route table's filters (midi_route.h): packets a
 * filter diverts wait here for Ruby. Created on first use. Each entry is a
 * midi_filter_event_t; its fields are returned through the out
 * parameters so this header does not need midi_route.h.
 */
int  MIDI_Filter_init(void);
/* Pop one event. false when empty. */
bool MIDI_Filter_pop(uint32_t *dst, midi_tmask_t *src, uint8_t *cin,
                     uint8_t *b1, uint8_t *b2, uint8_t *b3);
/* Block up to timeout_ms until an event is queued. true if one is. */
bool MIDI_Filter_wait(uint32_t timeout_ms);
/* Drop everything queued (a script's filters are gone, so are its events). */
void MIDI_Filter_flush(void);

/*
 * Note Scheduler API
 *
 * Allows triggering notes with automatic note-off after specified duration.
 * Designed for simultaneous multi-note triggering (e.g., multi-touch pads).
 */

/* Transport type bitmask.
 * NOTE: Ruby-side transport_id doubles as this mask, so each transport
 * must own a distinct bit. These three are the built-ins with their
 * historical values, reserved in the transport registry (see
 * midi_transport.h); other transport gems take bits from
 * MIDI_TRANSPORT_FIRST_DYNAMIC_BIT up with MIDI_transport_register().
 * MIDI_TRANSPORT_ALL deliberately covers only the physical outputs
 * (legacy fallback for unknown transports); the USB device transport and
 * registered transports are always addressed explicitly. */
#define MIDI_TRANSPORT_USB         0x01  /* USB-MIDI host (USB-A) */
#define MIDI_TRANSPORT_SAM2695     0x02  /* UART / SAM2695 synth */
#define MIDI_TRANSPORT_ALL         0x03
#define MIDI_TRANSPORT_USB_DEVICE  0x04  /* USB-MIDI device (USB-C, Tab5) */

/* Maximum number of scheduled notes */
#define MIDI_MAX_SCHEDULED_NOTES 32

/* Initialize note scheduler */
int MIDI_Note_scheduler_init(void);

/* Deinitialize note scheduler */
void MIDI_Note_scheduler_deinit(void);

/* Trigger a note (sends note_on immediately, schedules note_off after duration)
 *
 * @param transport_mask  Which transports to send to (MIDI_TRANSPORT_*)
 * @param channel         MIDI channel (0-15)
 * @param note            Note number (0-127)
 * @param velocity        Velocity (0-127)
 * @param duration_ms     Duration in milliseconds before note_off
 * @return 0 on success, -1 on error (scheduler full)
 */
int MIDI_Note_trigger(midi_tmask_t transport_mask, uint8_t channel, uint8_t note,
                      uint8_t velocity, uint32_t duration_ms);

/* Cancel all scheduled notes and send all-notes-off */
void MIDI_Note_scheduler_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_DEFINED_H_ */

/*
 * PicoRuby MIDI - Routing (MIDI Thru between transports)
 *
 * A route forwards what arrives on one transport straight to others, in C,
 * as the input task reads it -- no Ruby in the path, so a keyboard on the
 * USB host port can play a synth with the latency of the input task rather
 * than that of a script's polling loop.
 *
 * Transports are named by their bit in the transport registry (see
 * midi_transport.h). Packets are USB-MIDI packets (CIN + 3 bytes), and
 * forwarding goes through MIDI_transport_send().
 *
 * Sources are the transports the input task reads (USB host, UART, USB
 * device -- the last only while a route starts from it) plus
 * any transport that declares itself one with MIDI_route_set_source() and
 * pushes its own packets through MIDI_route_targets() -- e.g. an on-screen
 * keyboard, which produces MIDI on the UI task rather than receiving it.
 *
 * OS-free. The table is small and fixed; writers (Ruby, via the bindings)
 * and the reader (the input task) are not locked against each other, so a
 * packet that races a change may go by the old or the new table.
 */

#ifndef MIDI_ROUTE_DEFINED_H_
#define MIDI_ROUTE_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>
#include "midi_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_ROUTE_MAX          32
#define MIDI_ROUTE_ALL_CHANNELS (-1)
/* A route's "channel" that passes only what synchronises: timing clock,
 * start / continue / stop and song position pointer. */
#define MIDI_ROUTE_CLOCK_ONLY   (-2)
/* What MIDI_route_get() gives for a route that passes several of the above
 * (see MIDI_route_get_selection). */
#define MIDI_ROUTE_SEVERAL      (-3)

/*
 * A route's selection: what it passes, as a set. Bits 0..15 are the
 * channels' voice messages, MIDI_ROUTE_SEL_CLOCK the clock (as for
 * MIDI_ROUTE_CLOCK_ONLY), and MIDI_ROUTE_SEL_ALL everything -- every
 * channel plus all system messages, SysEx included. ALL is on its own;
 * the others combine (channels 1 and 10 plus the clock, say).
 */
#define MIDI_ROUTE_SEL_CHANNELS 0xFFFFu
#define MIDI_ROUTE_SEL_CLOCK    (1u << 16)
#define MIDI_ROUTE_SEL_ALL      (1u << 17)

/* The selection of a channel argument (0..15, MIDI_ROUTE_ALL_CHANNELS or
 * MIDI_ROUTE_CLOCK_ONLY); 0 for anything else. */
uint32_t MIDI_route_selection(int channel);

/* The channel argument a selection stands for, or MIDI_ROUTE_SEVERAL when
 * it holds more than one channel / the clock. */
int MIDI_route_channel(uint32_t sel);

/* Forward everything from transport `src_bit` to the transports in
 * `dst_mask`. channel is 0..15 to pass only that channel's voice messages,
 * MIDI_ROUTE_ALL_CHANNELS to pass every channel plus system messages
 * (clock, start / stop, SysEx), or MIDI_ROUTE_CLOCK_ONLY for the clock alone.
 * A route to the same source and channel as an existing one adds its
 * destinations to it.
 * Returns 0, or -1 when the arguments are invalid (src_bit not a single
 * bit, an empty destination, a destination equal to the source) or the
 * table is full. */
int MIDI_route_add(midi_tmask_t src_bit, midi_tmask_t dst_mask, int channel);

/* MIDI_route_add with a selection (MIDI_ROUTE_SEL_*): one route passing
 * every channel / the clock in it. A route with the same source and
 * selection takes the destinations. Returns 0, or -1 as MIDI_route_add
 * (also for an empty selection, or ALL combined with anything). */
int MIDI_route_add_selection(midi_tmask_t src_bit, midi_tmask_t dst_mask, uint32_t sel);

/* Remove the destinations in `dst_mask` from every route out of `src_bit`
 * (all of them for dst_mask MIDI_TMASK_ALL); routes left without a
 * destination are dropped. Returns how many routes were dropped. */
int MIDI_route_remove(midi_tmask_t src_bit, midi_tmask_t dst_mask);

/* Drop every route and every filter, and the script's watches (below). */
void MIDI_route_clear(void);

/* Whether any route, filter or watch is set (the input task keeps running
 * while one is). */
bool MIDI_route_active(void);

/* The transports a packet from `src_bit` would be forwarded to (0 for
 * none). For a source that sends on its own and wants to fold the routes
 * into one MIDI_transport_send() with destinations of its own. */
midi_tmask_t MIDI_route_targets(midi_tmask_t src_bit, uint8_t cin, uint8_t b1);

/* Whether any route, filter or watch starts from `src_bit`. */
bool MIDI_route_from(midi_tmask_t src_bit);

/* Built-in route sources: the transports the input task reads (USB host
 * 0x01, UART 0x02, USB device 0x04). */
#define MIDI_ROUTE_INPUT_SOURCES 0x07

/* Declare (on) or retract (off) `bit` as a route source besides the
 * built-in ones. Such a source forwards its own packets (see
 * MIDI_route_targets / MIDI_route_packet); the input task never reads it. */
void MIDI_route_set_source(midi_tmask_t bit, bool on);

/* Every transport a route may start from: MIDI_ROUTE_INPUT_SOURCES plus the
 * declared ones. */
midi_tmask_t MIDI_route_sources(void);

/* Forward one packet received on `src_bit` along the matching routes.
 * cin is the USB-MIDI Code Index Number (low nibble used), b1..b3 the
 * message bytes. Filters apply (see below). */
void MIDI_route_packet(midi_tmask_t src_bit, uint8_t cin,
                       uint8_t b1, uint8_t b2, uint8_t b3);

/*
 * Watches: sources to read with nothing routed from them, for what reading
 * them measures -- the external clock tempo (MIDI.external_bpm). Without a
 * route, filter or watch the input task does not read the USB device or the
 * UART at all. Two owners, so neither undoes the other: the application (the
 * Tempo screen) and the script (sync_from:); MIDI_route_clear() drops the
 * script's only. Callers start reading with MIDI_route_start_source().
 */
#define MIDI_WATCH_APP    0
#define MIDI_WATCH_SCRIPT 1

void         MIDI_route_set_watch(int owner, midi_tmask_t src_mask);
midi_tmask_t MIDI_route_watched(void);

/* Read route `i` (0..MIDI_ROUTE_MAX-1). Returns false for an unused slot.
 * channel is MIDI_route_channel() of its selection. */
bool MIDI_route_get(int i, midi_tmask_t *src_bit, midi_tmask_t *dst_mask, int *channel);
bool MIDI_route_get_selection(int i, midi_tmask_t *src_bit, midi_tmask_t *dst_mask, uint32_t *sel);

/*
 * Filters (MIDI.filter / MIDI.on)
 *
 * A filter diverts what a route would forward from `src_bit` to some of its
 * destinations into a queue that Ruby drains (MIDI._run_filters), so a block
 * can rewrite, multiply or drop each message before Ruby sends it on. Only
 * the packets that match the filter's types and channel are diverted; the
 * rest -- and every destination without a filter -- still go straight out
 * in C.
 *
 * MIDI_ROUTE_DST_RUBY is a destination of its own: a filter to it receives
 * packets from `src_bit` with no route needed (what MIDI.on listens with).
 * It never names a transport, so it costs no registry bit: it is the bit
 * just above them, which is why filter destinations are uint32_t.
 *
 * The queue itself is the port's (a FreeRTOS queue on ESP32): it hands a
 * sink to MIDI_route_set_filter_sink(). Without a sink nothing is diverted.
 */
#define MIDI_ROUTE_DST_RUBY       (1u << MIDI_TRANSPORT_REGISTRY_SIZE)
/* Every destination a filter can have */
#define MIDI_ROUTE_DST_ALL        ((uint32_t)MIDI_TMASK_ALL | MIDI_ROUTE_DST_RUBY)
#define MIDI_FILTER_MAX           16

/* What happens to a packet the queue has no room for */
#define MIDI_FILTER_OVERFLOW_BYPASS 0   /* sent on unfiltered */
#define MIDI_FILTER_OVERFLOW_DROP   1   /* dropped */

/* One diverted packet: from `src`, for the destinations in `dst` (transport
 * bits and/or MIDI_ROUTE_DST_RUBY). */
typedef struct {
    uint32_t dst;
    midi_tmask_t src;
    uint8_t cin;
    uint8_t b1, b2, b3;
} midi_filter_event_t;

/* Queues one event; false when the queue is full. Called from whatever task
 * forwards the packet (the input task, the UI task for UI::Keys). */
typedef bool (*midi_filter_sink_t)(const midi_filter_event_t *ev);

void MIDI_route_set_filter_sink(midi_filter_sink_t sink);

/* Divert packets from `src_bit` to each destination in `dst_mask` whose CIN
 * bit is set in `types` (bit n = CIN n) and, for channel messages, whose
 * channel is `channel` (or any, for MIDI_ROUTE_ALL_CHANNELS). Setting it
 * again for the same source and destination replaces it.
 * Returns 0, or -1 for invalid arguments or a full table. */
int MIDI_route_set_filter(midi_tmask_t src_bit, uint32_t dst_mask, uint16_t types,
                          int channel, uint8_t overflow);

/* Remove the filters from `src_bit` to the destinations in `dst_mask`
 * (MIDI_ROUTE_DST_ALL: all of them). Returns how many were removed. */
int MIDI_route_clear_filter(midi_tmask_t src_bit, uint32_t dst_mask);

/* Destinations that have a filter from `src_bit` (for the UI). */
uint32_t MIDI_route_filtered(midi_tmask_t src_bit);

/* Packets the queue had no room for, since start-up. */
uint32_t MIDI_route_filter_overflows(void);

/* For a source that sends on its own (e.g. UI::Keys): forward one packet to
 * `dst_mask` (normally MIDI_route_targets() of it) with the filters from
 * `src_bit` applied -- diverted destinations and Ruby listeners get it
 * through the queue, the rest directly. */
void MIDI_route_dispatch(midi_tmask_t src_bit, midi_tmask_t dst_mask, uint8_t cin,
                         uint8_t b1, uint8_t b2, uint8_t b3);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_ROUTE_DEFINED_H_ */

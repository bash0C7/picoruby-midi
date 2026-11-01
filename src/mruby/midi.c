/*
 * PicoRuby MIDI - mruby bindings
 *
 * Feature parity with the mrubyc binding in src/mrubyc/midi.c. Methods
 * defined here are wired up only when the gem is built against the
 * mruby VM (PICORB_VM_MRUBY); on mrubyc builds the mrubyc/ binding is
 * used instead.
 */

#include <stdlib.h>
#include <string.h>

#include <mruby.h>
#include <mruby/presym.h>
#include <mruby/class.h>
#include <mruby/variable.h>
#include <mruby/hash.h>
#include <mruby/array.h>
#include <mruby/string.h>

#include "../../include/midi.h"
#include "../../include/midi_route.h"
#include "../../include/midi_transport.h"

/* ========================================================================
 * MIDI::Clock (class methods: the one clock of the process)
 * ======================================================================== */

/* Counts are cut to 30 bits so they stay Integers on every VM */
#define CLOCK_COUNT_MASK 0x3FFFFFFF

static mrb_float
clock_bpm_arg(mrb_state *mrb)
{
    mrb_value v;
    mrb_get_args(mrb, "o", &v);
    if (mrb_float_p(v)) return mrb_float(v);
    if (mrb_integer_p(v)) return (mrb_float)mrb_integer(v);
    mrb_raise(mrb, E_TYPE_ERROR, "BPM must be a number");
    return 0;
}

/* MIDI::Clock._bit -> its transport bit (starts the timer) */
static mrb_value
mrb_midi_clock_bit(mrb_state *mrb, mrb_value self)
{
    if (MIDI_Clock_init() != 0) return mrb_fixnum_value(0);
    return mrb_fixnum_value(MIDI_Clock_bit());
}

static mrb_value
mrb_midi_clock_set_bpm(mrb_state *mrb, mrb_value self)
{
    MIDI_Clock_set_bpm((float)clock_bpm_arg(mrb));
    return mrb_nil_value();
}

static mrb_value
mrb_midi_clock_get_bpm(mrb_state *mrb, mrb_value self)
{
    return mrb_float_value(mrb, MIDI_Clock_get_bpm());
}

static mrb_value
mrb_midi_clock_current_bpm(mrb_state *mrb, mrb_value self)
{
    return mrb_float_value(mrb, MIDI_Clock_current_bpm());
}

static mrb_value
mrb_midi_clock_start(mrb_state *mrb, mrb_value self)
{
    MIDI_Clock_start();
    return mrb_nil_value();
}

static mrb_value
mrb_midi_clock_continue(mrb_state *mrb, mrb_value self)
{
    MIDI_Clock_continue();
    return mrb_nil_value();
}

static mrb_value
mrb_midi_clock_stop(mrb_state *mrb, mrb_value self)
{
    MIDI_Clock_stop();
    return mrb_nil_value();
}

static mrb_value
mrb_midi_clock_running(mrb_state *mrb, mrb_value self)
{
    return mrb_bool_value(MIDI_Clock_is_running());
}

/* MIDI::Clock._count -> ticks so far (30 bits, wraps) */
static mrb_value
mrb_midi_clock_count(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value((mrb_int)(MIDI_Clock_count() & CLOCK_COUNT_MASK));
}

/* MIDI::Clock._us_to_next -> microseconds until the next tick */
static mrb_value
mrb_midi_clock_us_to_next(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value((mrb_int)MIDI_Clock_us_to_next());
}

/* ========================================================================
 * MIDI::Input - task control + queue accessors
 * ======================================================================== */

static mrb_value
mrb_midi_input_start_task(mrb_state *mrb, mrb_value self)
{
    MIDI_Input_set_queueing(true);
    int ret = MIDI_Input_start();
    return mrb_fixnum_value(ret);
}

static mrb_value
mrb_midi_input_stop_task(mrb_state *mrb, mrb_value self)
{
    MIDI_Input_set_queueing(false);
    MIDI_Input_stop();
    return mrb_nil_value();
}

/* MIDI._route_add(src_bit, dst_mask, channel) -> 0 / -1 */
static mrb_value
mrb_midi_route_add(mrb_state *mrb, mrb_value self)
{
    mrb_int src, dst, channel;
    mrb_get_args(mrb, "iii", &src, &dst, &channel);
    return mrb_fixnum_value(MIDI_route_add((midi_tmask_t)src, (midi_tmask_t)dst, (int)channel));
}

/* MIDI._route_add_sel(src_bit, dst_mask, selection) -> 0 / -1 */
static mrb_value
mrb_midi_route_add_sel(mrb_state *mrb, mrb_value self)
{
    mrb_int src, dst, sel;
    mrb_get_args(mrb, "iii", &src, &dst, &sel);
    return mrb_fixnum_value(MIDI_route_add_selection((midi_tmask_t)src, (midi_tmask_t)dst, (uint32_t)sel));
}

/* MIDI._route_remove(src_bit, dst_mask) -> number of routes dropped */
static mrb_value
mrb_midi_route_remove(mrb_state *mrb, mrb_value self)
{
    mrb_int src, dst;
    mrb_get_args(mrb, "ii", &src, &dst);
    return mrb_fixnum_value(MIDI_route_remove((midi_tmask_t)src, (midi_tmask_t)dst));
}

/* MIDI._route_clear */
static mrb_value
mrb_midi_route_clear(mrb_state *mrb, mrb_value self)
{
    MIDI_route_clear();
    return mrb_nil_value();
}

/* MIDI._route_start -> 0 / -1 */
static mrb_value
mrb_midi_route_start(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value(MIDI_Input_start_routing());
}

/* MIDI._route_sources -> mask of the transports a route may start from */
static mrb_value
mrb_midi_route_sources(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value(MIDI_route_sources());
}

/* MIDI._route_targets_of(src_bit) -> transports any route from src_bit forwards to */
static mrb_value
mrb_midi_route_targets_of(mrb_state *mrb, mrb_value self)
{
    mrb_int src;
    mrb_get_args(mrb, "i", &src);
    midi_tmask_t s, d, all = 0;
    for (int i = 0; i < MIDI_ROUTE_MAX; i++) {
        if (MIDI_route_get(i, &s, &d, NULL) && s == (midi_tmask_t)src) all |= d;
    }
    return mrb_fixnum_value(all);
}

/* MIDI._route_get(i) -> [src_bit, dst_mask, channel, selection] or nil */
static mrb_value
mrb_midi_route_get(mrb_state *mrb, mrb_value self)
{
    mrb_int i;
    mrb_get_args(mrb, "i", &i);
    midi_tmask_t src, dst;
    uint32_t sel;
    if (!MIDI_route_get_selection((int)i, &src, &dst, &sel)) return mrb_nil_value();
    mrb_value a = mrb_ary_new_capa(mrb, 4);
    mrb_ary_push(mrb, a, mrb_fixnum_value(src));
    mrb_ary_push(mrb, a, mrb_fixnum_value(dst));
    mrb_ary_push(mrb, a, mrb_fixnum_value(MIDI_route_channel(sel)));
    mrb_ary_push(mrb, a, mrb_fixnum_value((mrb_int)sel));
    return a;
}

/* MIDI._route_filtered(src_bit) -> destinations with a filter (MIDI::DST_RUBY = Ruby) */
static mrb_value
mrb_midi_route_filtered(mrb_state *mrb, mrb_value self)
{
    mrb_int src;
    mrb_get_args(mrb, "i", &src);
    return mrb_fixnum_value((mrb_int)MIDI_route_filtered((midi_tmask_t)src));
}

/* MIDI._transport_connected?(bit) -> whether that registered transport is up */
static mrb_value
mrb_midi_transport_connected(mrb_state *mrb, mrb_value self)
{
    mrb_int bit;
    mrb_get_args(mrb, "i", &bit);
    const midi_transport_t *t = MIDI_transport_get((midi_tmask_t)bit);
    return mrb_bool_value(t && t->ops && t->ops->is_connected && t->ops->is_connected(t->ctx));
}

/* MIDI._route_watch(src_mask): the script's watches (read with nothing routed) */
static mrb_value
mrb_midi_route_watch(mrb_state *mrb, mrb_value self)
{
    mrb_int mask;
    mrb_get_args(mrb, "i", &mask);
    MIDI_route_set_watch(MIDI_WATCH_SCRIPT, (midi_tmask_t)mask);
    return mrb_nil_value();
}

/* MIDI._transport_send(mask, cin, b1, b2, b3) */
static mrb_value
mrb_midi_transport_send(mrb_state *mrb, mrb_value self)
{
    mrb_int mask, cin, b1, b2, b3;
    mrb_get_args(mrb, "iiiii", &mask, &cin, &b1, &b2, &b3);
    MIDI_transport_send((midi_tmask_t)mask, 0, (uint8_t)(cin & 0x0F),
                        (uint8_t)b1, (uint8_t)b2, (uint8_t)b3);
    return mrb_nil_value();
}

/* ========================================================================
 * Filters (MIDI.filter / MIDI.on)
 * ======================================================================== */

/* MIDI._filter_init -> 0 / -1 */
static mrb_value
mrb_midi_filter_init(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value(MIDI_Filter_init());
}

/* MIDI._filter_set(src_bit, dst_mask, types, channel, overflow) -> 0 / -1 */
static mrb_value
mrb_midi_filter_set(mrb_state *mrb, mrb_value self)
{
    mrb_int src, dst, types, channel, overflow;
    mrb_get_args(mrb, "iiiii", &src, &dst, &types, &channel, &overflow);
    return mrb_fixnum_value(MIDI_route_set_filter((midi_tmask_t)src, (uint32_t)dst,
                                                  (uint16_t)types, (int)channel,
                                                  (uint8_t)overflow));
}

/* MIDI._filter_clear(src_bit, dst_mask) -> number removed */
static mrb_value
mrb_midi_filter_clear(mrb_state *mrb, mrb_value self)
{
    mrb_int src, dst;
    mrb_get_args(mrb, "ii", &src, &dst);
    return mrb_fixnum_value(MIDI_route_clear_filter((midi_tmask_t)src, (uint32_t)dst));
}

/* MIDI._filter_pop -> [dst_mask, src_bit, cin, b1, b2, b3] or nil */
static mrb_value
mrb_midi_filter_pop(mrb_state *mrb, mrb_value self)
{
    uint32_t dst;
    midi_tmask_t src;
    uint8_t cin, b1, b2, b3;
    if (!MIDI_Filter_pop(&dst, &src, &cin, &b1, &b2, &b3)) {
        return mrb_nil_value();
    }
    mrb_value a = mrb_ary_new_capa(mrb, 6);
    mrb_ary_push(mrb, a, mrb_fixnum_value((mrb_int)dst));
    mrb_ary_push(mrb, a, mrb_fixnum_value(src));
    mrb_ary_push(mrb, a, mrb_fixnum_value(cin));
    mrb_ary_push(mrb, a, mrb_fixnum_value(b1));
    mrb_ary_push(mrb, a, mrb_fixnum_value(b2));
    mrb_ary_push(mrb, a, mrb_fixnum_value(b3));
    return a;
}

/* MIDI._filter_wait(timeout_ms) -> true when an event is waiting */
static mrb_value
mrb_midi_filter_wait(mrb_state *mrb, mrb_value self)
{
    mrb_int ms;
    mrb_get_args(mrb, "i", &ms);
    return mrb_bool_value(MIDI_Filter_wait(ms > 0 ? (uint32_t)ms : 0));
}

/* MIDI._filter_flush */
static mrb_value
mrb_midi_filter_flush(mrb_state *mrb, mrb_value self)
{
    MIDI_Filter_flush();
    return mrb_nil_value();
}

/* MIDI._filter_overflows -> packets the queue had no room for */
static mrb_value
mrb_midi_filter_overflows(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value((mrb_int)MIDI_route_filter_overflows());
}

/* MIDI._external_bpm_from(src_bit) -> Float */
static mrb_value
mrb_midi_external_bpm_from(mrb_state *mrb, mrb_value self)
{
    mrb_int src;
    mrb_get_args(mrb, "i", &src);
    return mrb_float_value(mrb, (mrb_float)MIDI_Input_get_external_bpm_from((midi_tmask_t)src));
}

/* MIDI._reset_external_clock_from(src_bit) */
static mrb_value
mrb_midi_reset_external_clock_from(mrb_state *mrb, mrb_value self)
{
    mrb_int src;
    mrb_get_args(mrb, "i", &src);
    MIDI_Input_reset_external_clock_from((midi_tmask_t)src);
    return mrb_nil_value();
}

static mrb_value
mrb_midi_input_task_running(mrb_state *mrb, mrb_value self)
{
    return mrb_bool_value(MIDI_Input_is_running());
}

static mrb_value
mrb_midi_input_events_available(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value(MIDI_Input_events_available());
}

static mrb_value
mrb_midi_input_events_available_usb(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value(MIDI_Input_events_available_usb());
}

static mrb_value
mrb_midi_input_events_available_sam(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value(MIDI_Input_events_available_sam());
}

/* ========================================================================
 * MIDI::Input - external clock BPM tracking
 * ======================================================================== */

static mrb_value
mrb_midi_input_external_bpm(mrb_state *mrb, mrb_value self)
{
    return mrb_float_value(mrb, (mrb_float)MIDI_Input_get_external_bpm());
}

static mrb_value
mrb_midi_input_external_bpm_usb(mrb_state *mrb, mrb_value self)
{
    return mrb_float_value(mrb, (mrb_float)MIDI_Input_get_external_bpm_usb());
}

static mrb_value
mrb_midi_input_external_bpm_sam(mrb_state *mrb, mrb_value self)
{
    return mrb_float_value(mrb, (mrb_float)MIDI_Input_get_external_bpm_sam());
}

static mrb_value
mrb_midi_input_reset_external_clock(mrb_state *mrb, mrb_value self)
{
    MIDI_Input_reset_external_clock();
    return mrb_nil_value();
}

static mrb_value
mrb_midi_input_reset_external_clock_usb(mrb_state *mrb, mrb_value self)
{
    MIDI_Input_reset_external_clock_usb();
    return mrb_nil_value();
}

static mrb_value
mrb_midi_input_reset_external_clock_sam(mrb_state *mrb, mrb_value self)
{
    MIDI_Input_reset_external_clock_sam();
    return mrb_nil_value();
}

/* ========================================================================
 * MIDI module-level: note scheduler / batch trigger
 * ======================================================================== */

/*
 * MIDI._trigger(transport_mask, channel, note, velocity, duration_ms)
 * Sends note_on immediately and schedules note_off after duration_ms.
 * Returns 0 on success, -1 on error.
 */
static mrb_value
mrb_midi_trigger(mrb_state *mrb, mrb_value self)
{
    mrb_int transport_mask, channel, note, velocity, duration_ms;
    mrb_get_args(mrb, "iiiii",
                 &transport_mask, &channel, &note, &velocity, &duration_ms);

    int ret = MIDI_Note_trigger((midi_tmask_t)transport_mask,
                                (uint8_t)channel,
                                (uint8_t)note,
                                (uint8_t)velocity,
                                (uint32_t)duration_ms);
    return mrb_fixnum_value(ret);
}

static mrb_value
mrb_midi_scheduler_clear(mrb_state *mrb, mrb_value self)
{
    MIDI_Note_scheduler_clear();
    return mrb_nil_value();
}

/*
 * MIDI._send_batch(events)
 * Trigger many note_ons in a tight C loop. Each entry must be a Hash with
 * at least :type => :note_on, :transport, :channel, :note. :velocity and
 * :duration_ms are optional (defaults 100 / 1000).
 */
static mrb_value
mrb_midi_send_batch(mrb_state *mrb, mrb_value self)
{
    mrb_value events;
    mrb_get_args(mrb, "A", &events);

    if (!mrb_array_p(events)) {
        return mrb_fixnum_value(0);
    }

    mrb_int num_events = RARRAY_LEN(events);
    mrb_int sent = 0;

    mrb_sym sym_type        = mrb_intern_lit(mrb, "type");
    mrb_sym sym_transport   = mrb_intern_lit(mrb, "transport");
    mrb_sym sym_channel     = mrb_intern_lit(mrb, "channel");
    mrb_sym sym_note        = mrb_intern_lit(mrb, "note");
    mrb_sym sym_velocity    = mrb_intern_lit(mrb, "velocity");
    mrb_sym sym_duration_ms = mrb_intern_lit(mrb, "duration_ms");
    mrb_sym sym_note_on     = mrb_intern_lit(mrb, "note_on");

    for (mrb_int i = 0; i < num_events; i++) {
        mrb_value entry = mrb_ary_ref(mrb, events, i);
        if (!mrb_hash_p(entry)) continue;

        mrb_value type     = mrb_hash_get(mrb, entry, mrb_symbol_value(sym_type));
        mrb_value tx       = mrb_hash_get(mrb, entry, mrb_symbol_value(sym_transport));
        mrb_value channel  = mrb_hash_get(mrb, entry, mrb_symbol_value(sym_channel));
        mrb_value note     = mrb_hash_get(mrb, entry, mrb_symbol_value(sym_note));
        mrb_value velocity = mrb_hash_get(mrb, entry, mrb_symbol_value(sym_velocity));
        mrb_value duration = mrb_hash_get(mrb, entry, mrb_symbol_value(sym_duration_ms));

        if (!mrb_symbol_p(type) || mrb_symbol(type) != sym_note_on) continue;
        if (!mrb_integer_p(tx) || !mrb_integer_p(channel) || !mrb_integer_p(note)) continue;

        uint8_t  vel_u  = mrb_integer_p(velocity) ? (uint8_t)mrb_integer(velocity) : 100;
        uint32_t dur_u  = mrb_integer_p(duration) ? (uint32_t)mrb_integer(duration) : 1000;

        MIDI_Note_trigger((uint8_t)mrb_integer(tx),
                          (uint8_t)mrb_integer(channel),
                          (uint8_t)mrb_integer(note),
                          vel_u, dur_u);
        sent++;
    }

    return mrb_fixnum_value(sent);
}

/* ========================================================================
 * MIDI::Input - event pop helpers
 * ======================================================================== */

/* Convert a midi_event_t into the Hash shape Ruby code expects. Mirrors
 * mrubyc/midi.c's midi_event_to_hash; consumer of event->sysex_data
 * frees it. */
static mrb_value
midi_event_to_hash(mrb_state *mrb, midi_event_t *event)
{
    const char *type_str;
    switch (event->type) {
        case MIDI_EVENT_NOTE_ON:          type_str = "note_on"; break;
        case MIDI_EVENT_NOTE_OFF:         type_str = "note_off"; break;
        case MIDI_EVENT_CONTROL_CHANGE:   type_str = "control_change"; break;
        case MIDI_EVENT_PROGRAM_CHANGE:   type_str = "program_change"; break;
        case MIDI_EVENT_PITCH_BEND:       type_str = "pitch_bend"; break;
        case MIDI_EVENT_POLY_AFTERTOUCH:  type_str = "poly_aftertouch"; break;
        case MIDI_EVENT_CHANNEL_PRESSURE: type_str = "channel_pressure"; break;
        case MIDI_EVENT_CLOCK:            type_str = "clock"; break;
        case MIDI_EVENT_START:            type_str = "start"; break;
        case MIDI_EVENT_STOP:             type_str = "stop"; break;
        case MIDI_EVENT_CONTINUE:         type_str = "continue"; break;
        case MIDI_EVENT_ACTIVE_SENSING:   type_str = "active_sensing"; break;
        case MIDI_EVENT_SYSTEM_RESET:     type_str = "system_reset"; break;
        case MIDI_EVENT_SYSEX:            type_str = "sysex"; break;
        default:                          type_str = "unknown"; break;
    }

    const char *source_str;
    switch (event->source) {
        case MIDI_SOURCE_USB:     source_str = "usb"; break;
        case MIDI_SOURCE_SAM2695: source_str = "sam2695"; break;
        default:                  source_str = "unknown"; break;
    }

    mrb_value hash = mrb_hash_new_capa(mrb, 6);
    mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "type")),
                 mrb_symbol_value(mrb_intern_cstr(mrb, type_str)));
    mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "source")),
                 mrb_symbol_value(mrb_intern_cstr(mrb, source_str)));
    mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "channel")),
                 mrb_fixnum_value(event->channel));

    switch (event->type) {
        case MIDI_EVENT_NOTE_ON:
        case MIDI_EVENT_NOTE_OFF:
            mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "note")),
                         mrb_fixnum_value(event->data1));
            mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "velocity")),
                         mrb_fixnum_value(event->data2));
            break;
        case MIDI_EVENT_CONTROL_CHANGE:
            mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "cc")),
                         mrb_fixnum_value(event->data1));
            mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "value")),
                         mrb_fixnum_value(event->data2));
            break;
        case MIDI_EVENT_PROGRAM_CHANGE:
            mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "program")),
                         mrb_fixnum_value(event->data1));
            break;
        case MIDI_EVENT_PITCH_BEND:
            mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "value")),
                         mrb_fixnum_value(event->value));
            break;
        case MIDI_EVENT_POLY_AFTERTOUCH:
            mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "note")),
                         mrb_fixnum_value(event->data1));
            mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "pressure")),
                         mrb_fixnum_value(event->data2));
            break;
        case MIDI_EVENT_CHANNEL_PRESSURE:
            mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "pressure")),
                         mrb_fixnum_value(event->data1));
            break;
        case MIDI_EVENT_SYSEX: {
            mrb_int len = (mrb_int)event->sysex_len;
            mrb_value arr = mrb_ary_new_capa(mrb, len);
            if (event->sysex_data) {
                for (mrb_int i = 0; i < len; i++) {
                    mrb_ary_set(mrb, arr, i, mrb_fixnum_value(event->sysex_data[i]));
                }
                free(event->sysex_data);
                event->sysex_data = NULL;
            }
            mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "data")), arr);
            if (event->value) {
                mrb_hash_set(mrb, hash, mrb_symbol_value(mrb_intern_lit(mrb, "truncated")),
                             mrb_true_value());
            }
            break;
        }
        default:
            break;
    }

    return hash;
}

static mrb_value
mrb_midi_input_pop_event(mrb_state *mrb, mrb_value self)
{
    midi_event_t event;
    if (!MIDI_Input_pop_event(&event)) {
        return mrb_nil_value();
    }
    return midi_event_to_hash(mrb, &event);
}

static mrb_value
mrb_midi_input_pop_event_usb(mrb_state *mrb, mrb_value self)
{
    midi_event_t event;
    if (!MIDI_Input_pop_event_usb(&event)) {
        return mrb_nil_value();
    }
    return midi_event_to_hash(mrb, &event);
}

static mrb_value
mrb_midi_input_pop_event_sam(mrb_state *mrb, mrb_value self)
{
    midi_event_t event;
    if (!MIDI_Input_pop_event_sam(&event)) {
        return mrb_nil_value();
    }
    return midi_event_to_hash(mrb, &event);
}

/* ========================================================================
 * Gem init / final
 * ======================================================================== */

void
mrb_picoruby_midi_gem_init(mrb_state *mrb)
{
    struct RClass *module_MIDI;
    struct RClass *class_Clock;
    struct RClass *class_Input;

    if (!mrb_class_defined_id(mrb, MRB_SYM(MIDI))) {
        module_MIDI = mrb_define_module_id(mrb, MRB_SYM(MIDI));
    } else {
        module_MIDI = mrb_module_get_id(mrb, MRB_SYM(MIDI));
    }

    /* Module-level methods (note scheduler / batch trigger). */
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_trigger),
                                  mrb_midi_trigger, MRB_ARGS_REQ(5));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_scheduler_clear),
                                  mrb_midi_scheduler_clear, MRB_ARGS_NONE());
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_send_batch),
                                  mrb_midi_send_batch, MRB_ARGS_REQ(1));

    /* Routing (MIDI.route) */
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_route_add),
                                  mrb_midi_route_add, MRB_ARGS_REQ(3));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_route_add_sel),
                                  mrb_midi_route_add_sel, MRB_ARGS_REQ(3));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_route_remove),
                                  mrb_midi_route_remove, MRB_ARGS_REQ(2));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_route_clear),
                                  mrb_midi_route_clear, MRB_ARGS_NONE());
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_route_start),
                                  mrb_midi_route_start, MRB_ARGS_NONE());
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_route_sources),
                                  mrb_midi_route_sources, MRB_ARGS_NONE());
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_route_targets_of),
                                  mrb_midi_route_targets_of, MRB_ARGS_REQ(1));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_transport_send),
                                  mrb_midi_transport_send, MRB_ARGS_REQ(5));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_route_watch),
                                  mrb_midi_route_watch, MRB_ARGS_REQ(1));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_route_get),
                                  mrb_midi_route_get, MRB_ARGS_REQ(1));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_route_filtered),
                                  mrb_midi_route_filtered, MRB_ARGS_REQ(1));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM_Q(_transport_connected),
                                  mrb_midi_transport_connected, MRB_ARGS_REQ(1));

    /* Filters (MIDI.filter / MIDI.on) */
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_filter_init),
                                  mrb_midi_filter_init, MRB_ARGS_NONE());
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_filter_set),
                                  mrb_midi_filter_set, MRB_ARGS_REQ(5));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_filter_clear),
                                  mrb_midi_filter_clear, MRB_ARGS_REQ(2));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_filter_pop),
                                  mrb_midi_filter_pop, MRB_ARGS_NONE());
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_filter_wait),
                                  mrb_midi_filter_wait, MRB_ARGS_REQ(1));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_filter_flush),
                                  mrb_midi_filter_flush, MRB_ARGS_NONE());
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_filter_overflows),
                                  mrb_midi_filter_overflows, MRB_ARGS_NONE());
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_external_bpm_from),
                                  mrb_midi_external_bpm_from, MRB_ARGS_REQ(1));
    mrb_define_module_function_id(mrb, module_MIDI, MRB_SYM(_reset_external_clock_from),
                                  mrb_midi_reset_external_clock_from, MRB_ARGS_REQ(1));

    /* MIDI::Clock */
    class_Clock = mrb_define_class_under_id(mrb, module_MIDI, MRB_SYM(Clock),
                                            mrb->object_class);
    mrb_define_class_method_id(mrb, class_Clock, MRB_SYM(_bit),
                               mrb_midi_clock_bit, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, class_Clock, MRB_SYM(_set_bpm),
                               mrb_midi_clock_set_bpm, MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, class_Clock, MRB_SYM(_bpm),
                               mrb_midi_clock_get_bpm, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, class_Clock, MRB_SYM(_current_bpm),
                               mrb_midi_clock_current_bpm, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, class_Clock, MRB_SYM(_start),
                               mrb_midi_clock_start, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, class_Clock, MRB_SYM(_continue),
                               mrb_midi_clock_continue, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, class_Clock, MRB_SYM(_stop),
                               mrb_midi_clock_stop, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, class_Clock, MRB_SYM_Q(_running),
                               mrb_midi_clock_running, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, class_Clock, MRB_SYM(_count),
                               mrb_midi_clock_count, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, class_Clock, MRB_SYM(_us_to_next),
                               mrb_midi_clock_us_to_next, MRB_ARGS_NONE());

    /* MIDI::Input */
    class_Input = mrb_define_class_under_id(mrb, module_MIDI, MRB_SYM(Input),
                                            mrb->object_class);
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_start_task),
                         mrb_midi_input_start_task, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_stop_task),
                         mrb_midi_input_stop_task, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM_Q(_task_running),
                         mrb_midi_input_task_running, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_events_available),
                         mrb_midi_input_events_available, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_events_available_usb),
                         mrb_midi_input_events_available_usb, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_events_available_sam),
                         mrb_midi_input_events_available_sam, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_pop_event),
                         mrb_midi_input_pop_event, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_pop_event_usb),
                         mrb_midi_input_pop_event_usb, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_pop_event_sam),
                         mrb_midi_input_pop_event_sam, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_external_bpm),
                         mrb_midi_input_external_bpm, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_external_bpm_usb),
                         mrb_midi_input_external_bpm_usb, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_external_bpm_sam),
                         mrb_midi_input_external_bpm_sam, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_reset_external_clock),
                         mrb_midi_input_reset_external_clock, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_reset_external_clock_usb),
                         mrb_midi_input_reset_external_clock_usb, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, class_Input, MRB_SYM(_reset_external_clock_sam),
                         mrb_midi_input_reset_external_clock_sam, MRB_ARGS_NONE());
}

void
mrb_picoruby_midi_gem_final(mrb_state *mrb)
{
    /* Nothing to tear down. The input task, note scheduler and clock timer
     * are process-wide C state that outlives any one VM: the host stops them
     * when a script ends (see picoruby-esp32's MIDI cleanup), and a VM that
     * is abandoned rather than closed never reaches this function anyway.
     * C-side users such as the Tombola sequencer keep calling
     * MIDI_Note_trigger() between scripts, so the scheduler must survive. */
}

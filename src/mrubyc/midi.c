/*
 * PicoRuby MIDI - mrubyc bindings
 */

#include <mrubyc.h>
#include <stdlib.h>

#include "../../include/midi.h"
#include "../../include/midi_route.h"
#include "../../include/midi_transport.h"

/*
 * MIDI::Clock (class methods: the one clock of the process)
 */

/* Counts are cut to 30 bits so they stay Integers on every VM */
#define CLOCK_COUNT_MASK 0x3FFFFFFF

/* MIDI::Clock._bit -> its transport bit (starts the timer) */
static void
c_midi_clock_bit(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (MIDI_Clock_init() != 0) {
        SET_INT_RETURN(0);
        return;
    }
    SET_INT_RETURN(MIDI_Clock_bit());
}

static void
c_midi_clock_set_bpm(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc < 1) {
        mrbc_raise(vm, MRBC_CLASS(ArgumentError), "wrong number of arguments");
        return;
    }
    if (v[1].tt == MRBC_TT_FLOAT) {
        MIDI_Clock_set_bpm((float)v[1].d);
    } else if (v[1].tt == MRBC_TT_INTEGER) {
        MIDI_Clock_set_bpm((float)v[1].i);
    } else {
        mrbc_raise(vm, MRBC_CLASS(TypeError), "BPM must be a number");
        return;
    }
    SET_NIL_RETURN();
}

static void
c_midi_clock_get_bpm(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_FLOAT_RETURN(MIDI_Clock_get_bpm());
}

static void
c_midi_clock_current_bpm(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_FLOAT_RETURN(MIDI_Clock_current_bpm());
}

static void
c_midi_clock_start(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_Clock_start();
    SET_NIL_RETURN();
}

static void
c_midi_clock_continue(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_Clock_continue();
    SET_NIL_RETURN();
}

static void
c_midi_clock_stop(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_Clock_stop();
    SET_NIL_RETURN();
}

static void
c_midi_clock_running(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_BOOL_RETURN(MIDI_Clock_is_running());
}

/* MIDI::Clock._count -> ticks so far (30 bits, wraps) */
static void
c_midi_clock_count(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN((mrbc_int_t)(MIDI_Clock_count() & CLOCK_COUNT_MASK));
}

/* MIDI::Clock._us_to_next -> microseconds until the next tick */
static void
c_midi_clock_us_to_next(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN((mrbc_int_t)MIDI_Clock_us_to_next());
}

/*
 * MIDI::Input._start_task
 */
static void
c_midi_input_start_task(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_Input_set_queueing(true);
    int ret = MIDI_Input_start();
    SET_INT_RETURN(ret);
}

/*
 * MIDI::Input._stop_task
 */
static void
c_midi_input_stop_task(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_Input_set_queueing(false);
    MIDI_Input_stop();
    SET_NIL_RETURN();
}

/*
 * MIDI::Input._task_running?
 */
static void
c_midi_input_task_running(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_BOOL_RETURN(MIDI_Input_is_running());
}

/*
 * MIDI::Input._events_available
 */
static void
c_midi_input_events_available(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN(MIDI_Input_events_available());
}

/*
 * MIDI::Input._events_available_usb
 */
static void
c_midi_input_events_available_usb(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN(MIDI_Input_events_available_usb());
}

/*
 * MIDI::Input._events_available_sam
 */
static void
c_midi_input_events_available_sam(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN(MIDI_Input_events_available_sam());
}

/*
 * MIDI::Input._external_bpm
 * Returns: Float BPM calculated from external MIDI clock (USB-MIDI), or 0.0 if not available
 */
static void
c_midi_input_external_bpm(mrbc_vm *vm, mrbc_value v[], int argc)
{
    float bpm = MIDI_Input_get_external_bpm();
    SET_FLOAT_RETURN(bpm);
}

/*
 * MIDI::Input._external_bpm_usb
 * Returns: Float BPM calculated from USB-MIDI clock, or 0.0 if not available
 */
static void
c_midi_input_external_bpm_usb(mrbc_vm *vm, mrbc_value v[], int argc)
{
    float bpm = MIDI_Input_get_external_bpm_usb();
    SET_FLOAT_RETURN(bpm);
}

/*
 * MIDI::Input._external_bpm_sam
 * Returns: Float BPM calculated from SAM2695 (MIDI-DIN) clock, or 0.0 if not available
 */
static void
c_midi_input_external_bpm_sam(mrbc_vm *vm, mrbc_value v[], int argc)
{
    float bpm = MIDI_Input_get_external_bpm_sam();
    SET_FLOAT_RETURN(bpm);
}

/*
 * MIDI::Input._reset_external_clock
 * Reset external clock tracking (call on MIDI Start)
 */
static void
c_midi_input_reset_external_clock(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_Input_reset_external_clock();
    SET_NIL_RETURN();
}

/*
 * MIDI::Input._reset_external_clock_usb
 * Reset USB-MIDI external clock tracking
 */
static void
c_midi_input_reset_external_clock_usb(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_Input_reset_external_clock_usb();
    SET_NIL_RETURN();
}

/*
 * MIDI::Input._reset_external_clock_sam
 * Reset SAM2695 (MIDI-DIN) external clock tracking
 */
static void
c_midi_input_reset_external_clock_sam(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_Input_reset_external_clock_sam();
    SET_NIL_RETURN();
}

/*
 * MIDI._trigger(transport_mask, channel, note, velocity, duration_ms)
 *
 * Trigger a note with automatic note_off after duration.
 * Sends note_on immediately and schedules note_off.
 *
 * @param transport_mask [Integer] MIDI_TRANSPORT_USB(1), MIDI_TRANSPORT_SAM2695(2), or both(3)
 * @param channel [Integer] MIDI channel (0-15)
 * @param note [Integer] Note number (0-127)
 * @param velocity [Integer] Velocity (0-127)
 * @param duration_ms [Integer] Duration in milliseconds
 * @return [Integer] 0 on success, -1 on error
 */
static void
c_midi_trigger(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc < 5) {
        SET_INT_RETURN(-1);
        return;
    }

    midi_tmask_t transport_mask = (midi_tmask_t)GET_INT_ARG(1);
    uint8_t channel = (uint8_t)GET_INT_ARG(2);
    uint8_t note = (uint8_t)GET_INT_ARG(3);
    uint8_t velocity = (uint8_t)GET_INT_ARG(4);
    uint32_t duration_ms = (uint32_t)GET_INT_ARG(5);

    int ret = MIDI_Note_trigger(transport_mask, channel, note, velocity, duration_ms);
    SET_INT_RETURN(ret);
}

/*
 * MIDI._scheduler_clear
 *
 * Clear all scheduled notes and send note_off for each.
 */
static void
c_midi_scheduler_clear(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_Note_scheduler_clear();
    SET_NIL_RETURN();
}

/*
 * MIDI._send_batch(events)
 *
 * Send multiple note_on events with minimal latency.
 * Events are sent sequentially with no Ruby overhead.
 * Note: Only note_on events are processed. note_off should be sent separately.
 *
 * @param events [Array] Array of hashes with keys:
 *   - :type => :note_on (Symbol)
 *   - :transport => 1 (USB), 2 (SAM2695), or 3 (both) (Integer)
 *   - :channel => 0-15 (Integer)
 *   - :note => 0-127 (Integer)
 *   - :velocity => 0-127 (Integer)
 * @return [Integer] Number of events sent
 */
static void
c_midi_send_batch(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc < 1 || v[1].tt != MRBC_TT_ARRAY) {
        SET_INT_RETURN(0);
        return;
    }

    mrbc_value events_array = v[1];
    int num_events = mrbc_array_size(&events_array);
    int sent_count = 0;

    for (int i = 0; i < num_events; i++) {
        mrbc_value event = mrbc_array_get(&events_array, i);

        if (event.tt != MRBC_TT_HASH) {
            continue;
        }

        /* Extract event fields from hash */
        mrbc_value type_val = mrbc_hash_get(&event, &mrbc_symbol_value(mrbc_str_to_symid("type")));
        mrbc_value transport_val = mrbc_hash_get(&event, &mrbc_symbol_value(mrbc_str_to_symid("transport")));
        mrbc_value channel_val = mrbc_hash_get(&event, &mrbc_symbol_value(mrbc_str_to_symid("channel")));
        mrbc_value note_val = mrbc_hash_get(&event, &mrbc_symbol_value(mrbc_str_to_symid("note")));
        mrbc_value velocity_val = mrbc_hash_get(&event, &mrbc_symbol_value(mrbc_str_to_symid("velocity")));
        mrbc_value duration_val = mrbc_hash_get(&event, &mrbc_symbol_value(mrbc_str_to_symid("duration_ms")));

        if (type_val.tt != MRBC_TT_SYMBOL || transport_val.tt != MRBC_TT_INTEGER ||
            channel_val.tt != MRBC_TT_INTEGER || note_val.tt != MRBC_TT_INTEGER) {
            continue;
        }

        uint8_t transport = (uint8_t)transport_val.i;
        uint8_t channel = (uint8_t)channel_val.i;
        uint8_t note = (uint8_t)note_val.i;

        /* Get type symbol string */
        const char *type_str = mrbc_symid_to_str(type_val.i);

        /* Only process note_on in batch */
        if (strcmp(type_str, "note_on") == 0) {
            uint8_t velocity = (velocity_val.tt == MRBC_TT_INTEGER) ? (uint8_t)velocity_val.i : 100;
            uint32_t duration_ms = (duration_val.tt == MRBC_TT_INTEGER) ? (uint32_t)duration_val.i : 1000;

            /* Log the event */
            // console_printf("[BATCH] ch=%d note=%d vel=%d dur=%ums\n",
            //               channel, note, velocity, duration_ms);

            /* Send note_on with automatic note_off after duration_ms */
            MIDI_Note_trigger(transport, channel, note, velocity, duration_ms);
            sent_count++;
        }
    }

    SET_INT_RETURN(sent_count);
}

/*
 * Helper function to convert midi_event_t to mruby hash
 */
static mrbc_value midi_event_to_hash(mrbc_vm *vm, midi_event_t *event)
{
    /* Convert event type to symbol */
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

    mrbc_value hash = mrbc_hash_new(vm, 6);

    /* :type */
    mrbc_value key_type = mrbc_symbol_value(mrbc_str_to_symid("type"));
    mrbc_value val_type = mrbc_symbol_value(mrbc_str_to_symid(type_str));
    mrbc_hash_set(&hash, &key_type, &val_type);

    /* :source */
    const char *source_str;
    switch (event->source) {
        case MIDI_SOURCE_USB:     source_str = "usb"; break;
        case MIDI_SOURCE_SAM2695: source_str = "sam2695"; break;
        default:                  source_str = "unknown"; break;
    }
    mrbc_value key_source = mrbc_symbol_value(mrbc_str_to_symid("source"));
    mrbc_value val_source = mrbc_symbol_value(mrbc_str_to_symid(source_str));
    mrbc_hash_set(&hash, &key_source, &val_source);

    /* :channel */
    mrbc_value key_channel = mrbc_symbol_value(mrbc_str_to_symid("channel"));
    mrbc_value val_channel = mrbc_integer_value(event->channel);
    mrbc_hash_set(&hash, &key_channel, &val_channel);

    /* Event-specific data */
    switch (event->type) {
        case MIDI_EVENT_NOTE_ON:
        case MIDI_EVENT_NOTE_OFF:
        {
            mrbc_value key_note = mrbc_symbol_value(mrbc_str_to_symid("note"));
            mrbc_value val_note = mrbc_integer_value(event->data1);
            mrbc_hash_set(&hash, &key_note, &val_note);

            mrbc_value key_vel = mrbc_symbol_value(mrbc_str_to_symid("velocity"));
            mrbc_value val_vel = mrbc_integer_value(event->data2);
            mrbc_hash_set(&hash, &key_vel, &val_vel);
            break;
        }
        case MIDI_EVENT_CONTROL_CHANGE:
        {
            mrbc_value key_cc = mrbc_symbol_value(mrbc_str_to_symid("cc"));
            mrbc_value val_cc = mrbc_integer_value(event->data1);
            mrbc_hash_set(&hash, &key_cc, &val_cc);

            mrbc_value key_val = mrbc_symbol_value(mrbc_str_to_symid("value"));
            mrbc_value val_val = mrbc_integer_value(event->data2);
            mrbc_hash_set(&hash, &key_val, &val_val);
            break;
        }
        case MIDI_EVENT_PROGRAM_CHANGE:
        {
            mrbc_value key_prog = mrbc_symbol_value(mrbc_str_to_symid("program"));
            mrbc_value val_prog = mrbc_integer_value(event->data1);
            mrbc_hash_set(&hash, &key_prog, &val_prog);
            break;
        }
        case MIDI_EVENT_PITCH_BEND:
        {
            mrbc_value key_val = mrbc_symbol_value(mrbc_str_to_symid("value"));
            mrbc_value val_val = mrbc_integer_value(event->value);
            mrbc_hash_set(&hash, &key_val, &val_val);
            break;
        }
        case MIDI_EVENT_POLY_AFTERTOUCH:
        {
            mrbc_value key_note = mrbc_symbol_value(mrbc_str_to_symid("note"));
            mrbc_value val_note = mrbc_integer_value(event->data1);
            mrbc_hash_set(&hash, &key_note, &val_note);

            mrbc_value key_pressure = mrbc_symbol_value(mrbc_str_to_symid("pressure"));
            mrbc_value val_pressure = mrbc_integer_value(event->data2);
            mrbc_hash_set(&hash, &key_pressure, &val_pressure);
            break;
        }
        case MIDI_EVENT_CHANNEL_PRESSURE:
        {
            mrbc_value key_pressure = mrbc_symbol_value(mrbc_str_to_symid("pressure"));
            mrbc_value val_pressure = mrbc_integer_value(event->data1);
            mrbc_hash_set(&hash, &key_pressure, &val_pressure);
            break;
        }
        case MIDI_EVENT_SYSEX:
        {
            /* Build :data as Array<Integer> from malloc'd buffer, then free it */
            uint16_t len = event->sysex_len;
            mrbc_value arr = mrbc_array_new(vm, len);
            if (event->sysex_data) {
                for (uint16_t i = 0; i < len; i++) {
                    mrbc_value byte_val = mrbc_integer_value(event->sysex_data[i]);
                    mrbc_array_set(&arr, i, &byte_val);
                }
                free(event->sysex_data);
                event->sysex_data = NULL;
            }
            mrbc_value key_data = mrbc_symbol_value(mrbc_str_to_symid("data"));
            mrbc_hash_set(&hash, &key_data, &arr);

            /* :truncated flag (true if message exceeded MIDI_SYSEX_MAX_LEN) */
            if (event->value) {
                mrbc_value key_trunc = mrbc_symbol_value(mrbc_str_to_symid("truncated"));
                mrbc_value val_trunc = mrbc_true_value();
                mrbc_hash_set(&hash, &key_trunc, &val_trunc);
            }
            break;
        }
        default:
            break;
    }

    return hash;
}

/*
 * MIDI::Input._pop_event
 * Returns: Hash with event data or nil
 */
static void
c_midi_input_pop_event(mrbc_vm *vm, mrbc_value v[], int argc)
{
    midi_event_t event;
    if (!MIDI_Input_pop_event(&event)) {
        SET_NIL_RETURN();
        return;
    }

    mrbc_value hash = midi_event_to_hash(vm, &event);
    SET_RETURN(hash);
}

/*
 * MIDI::Input._pop_event_usb
 * Returns: Hash with event data or nil (from USB queue)
 */
static void
c_midi_input_pop_event_usb(mrbc_vm *vm, mrbc_value v[], int argc)
{
    midi_event_t event;
    if (!MIDI_Input_pop_event_usb(&event)) {
        SET_NIL_RETURN();
        return;
    }

    mrbc_value hash = midi_event_to_hash(vm, &event);
    SET_RETURN(hash);
}

/*
 * MIDI::Input._pop_event_sam
 * Returns: Hash with event data or nil (from SAM2695 queue)
 */
static void
c_midi_input_pop_event_sam(mrbc_vm *vm, mrbc_value v[], int argc)
{
    midi_event_t event;
    if (!MIDI_Input_pop_event_sam(&event)) {
        SET_NIL_RETURN();
        return;
    }

    mrbc_value hash = midi_event_to_hash(vm, &event);
    SET_RETURN(hash);
}

/*
 * MIDI._route_add(src_bit, dst_mask, channel) -> 0 / -1
 * channel: 0..15, or -1 for every channel plus system messages.
 */
static void
c_midi_route_add(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc < 3) {
        SET_INT_RETURN(-1);
        return;
    }
    SET_INT_RETURN(MIDI_route_add((midi_tmask_t)GET_INT_ARG(1), (midi_tmask_t)GET_INT_ARG(2),
                                  (int)GET_INT_ARG(3)));
}

/*
 * MIDI._route_add_sel(src_bit, dst_mask, selection) -> 0 / -1
 * selection: MIDI_ROUTE_SEL_* (channel bits 0..15, clock, all).
 */
static void
c_midi_route_add_sel(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc < 3) {
        SET_INT_RETURN(-1);
        return;
    }
    SET_INT_RETURN(MIDI_route_add_selection((midi_tmask_t)GET_INT_ARG(1), (midi_tmask_t)GET_INT_ARG(2),
                                            (uint32_t)GET_INT_ARG(3)));
}

/*
 * MIDI._route_remove(src_bit, dst_mask) -> number of routes dropped
 */
static void
c_midi_route_remove(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc < 2) {
        SET_INT_RETURN(0);
        return;
    }
    SET_INT_RETURN(MIDI_route_remove((midi_tmask_t)GET_INT_ARG(1), (midi_tmask_t)GET_INT_ARG(2)));
}

/*
 * MIDI._route_clear
 */
static void
c_midi_route_clear(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_route_clear();
    SET_NIL_RETURN();
}

/*
 * MIDI._route_start -> 0 / -1 (input task for routing; -1 = no source yet)
 */
static void
c_midi_route_start(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN(MIDI_Input_start_routing());
}

/*
 * MIDI._route_sources -> mask of the transports a route may start from
 */
static void
c_midi_route_sources(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN(MIDI_route_sources());
}

/*
 * MIDI._route_targets_of(src_bit) -> transports any route from src_bit forwards to
 */
static void
c_midi_route_targets_of(mrbc_vm *vm, mrbc_value v[], int argc)
{
    midi_tmask_t s, d, all = 0;
    if (argc >= 1) {
        for (int i = 0; i < MIDI_ROUTE_MAX; i++) {
            if (MIDI_route_get(i, &s, &d, NULL) && s == (midi_tmask_t)GET_INT_ARG(1)) all |= d;
        }
    }
    SET_INT_RETURN(all);
}

/*
 * MIDI._route_get(i) -> [src_bit, dst_mask, channel, selection] or nil
 */
static void
c_midi_route_get(mrbc_vm *vm, mrbc_value v[], int argc)
{
    midi_tmask_t src, dst;
    uint32_t sel;
    if (argc < 1 || !MIDI_route_get_selection((int)GET_INT_ARG(1), &src, &dst, &sel)) {
        SET_NIL_RETURN();
        return;
    }
    int vals[4] = { src, dst, MIDI_route_channel(sel), (int)sel };
    mrbc_value arr = mrbc_array_new(vm, 4);
    for (int i = 0; i < 4; i++) {
        mrbc_value iv = mrbc_integer_value(vals[i]);
        mrbc_array_set(&arr, i, &iv);
    }
    SET_RETURN(arr);
}

/*
 * MIDI._route_filtered(src_bit) -> destinations with a filter (MIDI::DST_RUBY = Ruby)
 */
static void
c_midi_route_filtered(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN(argc >= 1 ? (mrbc_int_t)MIDI_route_filtered((midi_tmask_t)GET_INT_ARG(1)) : 0);
}

/*
 * MIDI._transport_connected?(bit)
 */
static void
c_midi_transport_connected(mrbc_vm *vm, mrbc_value v[], int argc)
{
    const midi_transport_t *t = argc >= 1 ? MIDI_transport_get((midi_tmask_t)GET_INT_ARG(1)) : NULL;
    if (t && t->ops && t->ops->is_connected && t->ops->is_connected(t->ctx)) {
        SET_TRUE_RETURN();
    } else {
        SET_FALSE_RETURN();
    }
}

/*
 * MIDI._route_watch(src_mask): the script's watches (read with nothing routed)
 */
static void
c_midi_route_watch(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc >= 1) MIDI_route_set_watch(MIDI_WATCH_SCRIPT, (midi_tmask_t)GET_INT_ARG(1));
    SET_NIL_RETURN();
}

/*
 * MIDI._transport_send(mask, cin, b1, b2, b3)
 */
static void
c_midi_transport_send(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc >= 5) {
        MIDI_transport_send((midi_tmask_t)GET_INT_ARG(1), 0, (uint8_t)(GET_INT_ARG(2) & 0x0F),
                            (uint8_t)GET_INT_ARG(3), (uint8_t)GET_INT_ARG(4),
                            (uint8_t)GET_INT_ARG(5));
    }
    SET_NIL_RETURN();
}

/*
 * Filters (MIDI.filter / MIDI.on). Same contracts as the mruby binding.
 */
static void
c_midi_filter_init(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN(MIDI_Filter_init());
}

static void
c_midi_filter_set(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc < 5) {
        SET_INT_RETURN(-1);
        return;
    }
    SET_INT_RETURN(MIDI_route_set_filter((midi_tmask_t)GET_INT_ARG(1), (uint32_t)GET_INT_ARG(2),
                                         (uint16_t)GET_INT_ARG(3), (int)GET_INT_ARG(4),
                                         (uint8_t)GET_INT_ARG(5)));
}

static void
c_midi_filter_clear(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc < 2) {
        SET_INT_RETURN(0);
        return;
    }
    SET_INT_RETURN(MIDI_route_clear_filter((midi_tmask_t)GET_INT_ARG(1), (uint32_t)GET_INT_ARG(2)));
}

static void
c_midi_filter_pop(mrbc_vm *vm, mrbc_value v[], int argc)
{
    uint32_t dst;
    midi_tmask_t src;
    uint8_t cin, b1, b2, b3;
    if (!MIDI_Filter_pop(&dst, &src, &cin, &b1, &b2, &b3)) {
        SET_NIL_RETURN();
        return;
    }
    int vals[6] = { (int)dst, src, cin, b1, b2, b3 };
    mrbc_value arr = mrbc_array_new(vm, 6);
    for (int i = 0; i < 6; i++) {
        mrbc_value iv = mrbc_integer_value(vals[i]);
        mrbc_array_set(&arr, i, &iv);
    }
    SET_RETURN(arr);
}

static void
c_midi_filter_wait(mrbc_vm *vm, mrbc_value v[], int argc)
{
    int ms = argc >= 1 ? (int)GET_INT_ARG(1) : 0;
    if (MIDI_Filter_wait(ms > 0 ? (uint32_t)ms : 0)) {
        SET_TRUE_RETURN();
    } else {
        SET_FALSE_RETURN();
    }
}

static void
c_midi_filter_flush(mrbc_vm *vm, mrbc_value v[], int argc)
{
    MIDI_Filter_flush();
    SET_NIL_RETURN();
}

static void
c_midi_filter_overflows(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN((int)MIDI_route_filter_overflows());
}

static void
c_midi_external_bpm_from(mrbc_vm *vm, mrbc_value v[], int argc)
{
    float bpm = argc >= 1 ? MIDI_Input_get_external_bpm_from((midi_tmask_t)GET_INT_ARG(1)) : 0.0f;
    SET_FLOAT_RETURN(bpm);
}

static void
c_midi_reset_external_clock_from(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc >= 1) MIDI_Input_reset_external_clock_from((midi_tmask_t)GET_INT_ARG(1));
    SET_NIL_RETURN();
}

/*
 * Gem initialization
 */
void
mrbc_midi_init(mrbc_vm *vm)
{
    /* Define MIDI module */
    mrbc_class *module_MIDI = mrbc_define_module(vm, "MIDI");

    /* Module-level methods for note scheduler */
    mrbc_define_method(vm, module_MIDI, "_trigger", c_midi_trigger);
    mrbc_define_method(vm, module_MIDI, "_scheduler_clear", c_midi_scheduler_clear);
    mrbc_define_method(vm, module_MIDI, "_send_batch", c_midi_send_batch);

    /* Routing (MIDI.route) */
    mrbc_define_method(vm, module_MIDI, "_route_add", c_midi_route_add);
    mrbc_define_method(vm, module_MIDI, "_route_add_sel", c_midi_route_add_sel);
    mrbc_define_method(vm, module_MIDI, "_route_remove", c_midi_route_remove);
    mrbc_define_method(vm, module_MIDI, "_route_clear", c_midi_route_clear);
    mrbc_define_method(vm, module_MIDI, "_route_start", c_midi_route_start);
    mrbc_define_method(vm, module_MIDI, "_route_sources", c_midi_route_sources);
    mrbc_define_method(vm, module_MIDI, "_route_targets_of", c_midi_route_targets_of);
    mrbc_define_method(vm, module_MIDI, "_transport_send", c_midi_transport_send);
    mrbc_define_method(vm, module_MIDI, "_route_watch", c_midi_route_watch);
    mrbc_define_method(vm, module_MIDI, "_route_get", c_midi_route_get);
    mrbc_define_method(vm, module_MIDI, "_route_filtered", c_midi_route_filtered);
    mrbc_define_method(vm, module_MIDI, "_transport_connected?", c_midi_transport_connected);

    /* Filters (MIDI.filter / MIDI.on) */
    mrbc_define_method(vm, module_MIDI, "_filter_init", c_midi_filter_init);
    mrbc_define_method(vm, module_MIDI, "_filter_set", c_midi_filter_set);
    mrbc_define_method(vm, module_MIDI, "_filter_clear", c_midi_filter_clear);
    mrbc_define_method(vm, module_MIDI, "_filter_pop", c_midi_filter_pop);
    mrbc_define_method(vm, module_MIDI, "_filter_wait", c_midi_filter_wait);
    mrbc_define_method(vm, module_MIDI, "_filter_flush", c_midi_filter_flush);
    mrbc_define_method(vm, module_MIDI, "_filter_overflows", c_midi_filter_overflows);
    mrbc_define_method(vm, module_MIDI, "_external_bpm_from", c_midi_external_bpm_from);
    mrbc_define_method(vm, module_MIDI, "_reset_external_clock_from", c_midi_reset_external_clock_from);

    /* Define Clock class under MIDI */
    mrbc_class *class_Clock = mrbc_define_class_under(vm, module_MIDI, "Clock", mrbc_class_object);

    /* The clock (mruby/c does not tell class methods from instance ones) */
    mrbc_define_method(vm, class_Clock, "_bit", c_midi_clock_bit);
    mrbc_define_method(vm, class_Clock, "_set_bpm", c_midi_clock_set_bpm);
    mrbc_define_method(vm, class_Clock, "_bpm", c_midi_clock_get_bpm);
    mrbc_define_method(vm, class_Clock, "_current_bpm", c_midi_clock_current_bpm);
    mrbc_define_method(vm, class_Clock, "_start", c_midi_clock_start);
    mrbc_define_method(vm, class_Clock, "_continue", c_midi_clock_continue);
    mrbc_define_method(vm, class_Clock, "_stop", c_midi_clock_stop);
    mrbc_define_method(vm, class_Clock, "_running?", c_midi_clock_running);
    mrbc_define_method(vm, class_Clock, "_count", c_midi_clock_count);
    mrbc_define_method(vm, class_Clock, "_us_to_next", c_midi_clock_us_to_next);

    /* Define Input class under MIDI */
    mrbc_class *class_Input = mrbc_define_class_under(vm, module_MIDI, "Input", mrbc_class_object);

    /* Input task methods */
    mrbc_define_method(vm, class_Input, "_start_task", c_midi_input_start_task);
    mrbc_define_method(vm, class_Input, "_stop_task", c_midi_input_stop_task);
    mrbc_define_method(vm, class_Input, "_task_running?", c_midi_input_task_running);
    mrbc_define_method(vm, class_Input, "_events_available", c_midi_input_events_available);
    mrbc_define_method(vm, class_Input, "_events_available_usb", c_midi_input_events_available_usb);
    mrbc_define_method(vm, class_Input, "_events_available_sam", c_midi_input_events_available_sam);
    mrbc_define_method(vm, class_Input, "_pop_event", c_midi_input_pop_event);
    mrbc_define_method(vm, class_Input, "_pop_event_usb", c_midi_input_pop_event_usb);
    mrbc_define_method(vm, class_Input, "_pop_event_sam", c_midi_input_pop_event_sam);
    mrbc_define_method(vm, class_Input, "_external_bpm", c_midi_input_external_bpm);
    mrbc_define_method(vm, class_Input, "_external_bpm_usb", c_midi_input_external_bpm_usb);
    mrbc_define_method(vm, class_Input, "_external_bpm_sam", c_midi_input_external_bpm_sam);
    mrbc_define_method(vm, class_Input, "_reset_external_clock", c_midi_input_reset_external_clock);
    mrbc_define_method(vm, class_Input, "_reset_external_clock_usb", c_midi_input_reset_external_clock_usb);
    mrbc_define_method(vm, class_Input, "_reset_external_clock_sam", c_midi_input_reset_external_clock_sam);
}

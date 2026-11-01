/*
 * PicoRuby MIDI - OS-free Note Scheduler core
 *
 * The slot table and the trigger/tick/clear logic live here. Time-source
 * and transport routing are injected from the port (esp_timer + USB_MIDI /
 * SAM2695 send_packet) or from a Ruby poll loop on RTOS-less builds.
 *
 * Concurrency: matches the previous in-port implementation, which let
 * the periodic timer task and the trigger caller share the slot table
 * without locks. The bool `active` flag is the only mutual reference
 * point; on a single-writer/single-reader pattern (timer fires note_off,
 * trigger claims a free slot) this works in practice on ESP32-class MCUs.
 */

#include <string.h>

#include "../include/midi_scheduler.h"

typedef struct {
    midi_tmask_t transport_mask;
    uint8_t channel;
    uint8_t note;
    uint64_t off_time_us;
    bool active;
} sched_slot_t;

static sched_slot_t g_slots[MIDI_MAX_SCHEDULED_NOTES];
static midi_scheduler_send_fn g_send_cb = NULL;

void midi_scheduler_core_init(void)
{
    memset(g_slots, 0, sizeof(g_slots));
}

void midi_scheduler_core_deinit(void)
{
    memset(g_slots, 0, sizeof(g_slots));
}

void midi_scheduler_set_send_callback(midi_scheduler_send_fn cb)
{
    g_send_cb = cb;
}

static void emit_note_off(midi_tmask_t transport_mask, uint8_t channel, uint8_t note)
{
    if (!g_send_cb) return;
    uint8_t status = 0x80 | (channel & 0x0F);
    g_send_cb(transport_mask, 0x08, status, note, 0);
}

static void emit_note_on(midi_tmask_t transport_mask, uint8_t channel,
                         uint8_t note, uint8_t velocity)
{
    if (!g_send_cb) return;
    uint8_t status = 0x90 | (channel & 0x0F);
    g_send_cb(transport_mask, 0x09, status, note, velocity);
}

void midi_scheduler_tick(uint64_t now_us)
{
    for (int i = 0; i < MIDI_MAX_SCHEDULED_NOTES; i++) {
        if (g_slots[i].active && now_us >= g_slots[i].off_time_us) {
            emit_note_off(g_slots[i].transport_mask,
                          g_slots[i].channel,
                          g_slots[i].note);
            g_slots[i].active = false;
        }
    }
}

int midi_scheduler_trigger(midi_tmask_t transport_mask,
                           uint8_t channel, uint8_t note, uint8_t velocity,
                           uint32_t duration_ms, uint64_t now_us)
{
    emit_note_on(transport_mask, channel, note, velocity);

    uint64_t off_time = now_us + (uint64_t)duration_ms * 1000ULL;
    for (int i = 0; i < MIDI_MAX_SCHEDULED_NOTES; i++) {
        if (!g_slots[i].active) {
            g_slots[i].transport_mask = transport_mask;
            g_slots[i].channel = channel;
            g_slots[i].note = note;
            g_slots[i].off_time_us = off_time;
            g_slots[i].active = true;
            return 0;
        }
    }
    return -1;
}

void midi_scheduler_clear(void)
{
    for (int i = 0; i < MIDI_MAX_SCHEDULED_NOTES; i++) {
        if (g_slots[i].active) {
            emit_note_off(g_slots[i].transport_mask,
                          g_slots[i].channel,
                          g_slots[i].note);
            g_slots[i].active = false;
        }
    }
}

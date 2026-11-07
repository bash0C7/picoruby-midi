/*
 * PicoRuby MIDI - timed packet ring (ESP32). See include/midi_timed_ring.h.
 *
 * head / tail / cursor are free-running counters (index = counter & mask):
 *   [tail, cursor)  due, the input task was woken for them
 *   [cursor, head)  waiting for their time; the timer is set for `cursor`
 * The consumer reads from tail whatever is due, past cursor too (cursor is
 * then pulled up). All of it under one spinlock; the esp_timer calls and the
 * wake-up happen outside.
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "midi_timed_ring.h"
#include "midi.h"

/* Due within this of its time: a timer wakes slightly late anyway */
#define DUE_SLACK_US 200
/* Shortest timer period (esp_timer's overhead) */
#define MIN_TIMER_US 100

struct midi_timed_ring {
    uint8_t (*pkt)[4];
    int64_t *at;
    uint32_t mask;
    uint32_t head, tail, cursor;
    bool armed;                 /* the timer is set (or about to be) for cursor */
    portMUX_TYPE mux;
    esp_timer_handle_t timer;
};

static inline uint32_t used(const midi_timed_ring_t *r, uint32_t from)
{
    return r->head - from;
}

/* Under the lock: move cursor over what is due. Returns whether it moved;
 * *next = time of the first one not due (0: none waiting) */
static bool advance(midi_timed_ring_t *r, int64_t now, int64_t *next)
{
    if ((int32_t)(r->cursor - r->tail) < 0) r->cursor = r->tail;
    uint32_t c0 = r->cursor;
    while (r->cursor != r->head && r->at[r->cursor & r->mask] <= now + DUE_SLACK_US) {
        r->cursor++;
    }
    *next = (r->cursor != r->head) ? r->at[r->cursor & r->mask] : 0;
    return r->cursor != c0;
}

/* Wake for what is due, set the timer for the rest. `from_timer`: the timer
 * fired (it is not armed any more whatever `armed` says). */
static void service(midi_timed_ring_t *r, bool from_timer)
{
    int64_t now = esp_timer_get_time();
    int64_t next = 0;
    bool kick, arm = false;
    portENTER_CRITICAL(&r->mux);
    if (r->armed && !from_timer) {
        /* the timer is set for an earlier packet; pushes only go later */
        portEXIT_CRITICAL(&r->mux);
        return;
    }
    kick = advance(r, now, &next);
    if (next != 0) {
        r->armed = true;
        arm = true;
    } else {
        r->armed = false;
    }
    portEXIT_CRITICAL(&r->mux);

    if (kick) MIDI_Input_kick();
    if (arm) {
        int64_t d = next - now;
        if (d < MIN_TIMER_US) d = MIN_TIMER_US;
        if (esp_timer_start_once(r->timer, (uint64_t)d) != ESP_OK) {
            /* already running (a clear raced us): it calls service anyway */
        }
    }
}

static void timer_cb(void *arg)
{
    service((midi_timed_ring_t *)arg, true);
}

midi_timed_ring_t *midi_timed_ring_new(uint32_t packets, const char *name)
{
    uint32_t n = 1;
    while (n < packets) n <<= 1;
    midi_timed_ring_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_8BIT);
    if (r == NULL) return NULL;
    r->pkt = heap_caps_calloc(n, 4, MALLOC_CAP_8BIT);
    r->at = heap_caps_calloc(n, sizeof(int64_t), MALLOC_CAP_8BIT);
    if (r->pkt == NULL || r->at == NULL) goto fail;
    r->mask = n - 1;
    portMUX_INITIALIZE(&r->mux);
    const esp_timer_create_args_t args = {
        .callback = timer_cb,
        .arg = r,
        .name = name ? name : "midi_ring",
    };
    if (esp_timer_create(&args, &r->timer) != ESP_OK) goto fail;
    return r;

fail:
    heap_caps_free(r->pkt);
    heap_caps_free(r->at);
    heap_caps_free(r);
    return NULL;
}

void midi_timed_ring_free(midi_timed_ring_t *r)
{
    if (r == NULL) return;
    esp_timer_stop(r->timer);
    esp_timer_delete(r->timer);
    heap_caps_free(r->pkt);
    heap_caps_free(r->at);
    heap_caps_free(r);
}

bool midi_timed_ring_push(midi_timed_ring_t *r, const uint8_t pkt[4], int64_t at_us)
{
    bool ok;
    portENTER_CRITICAL(&r->mux);
    ok = used(r, r->tail) <= r->mask;
    if (ok) {
        uint32_t i = r->head & r->mask;
        memcpy(r->pkt[i], pkt, 4);
        r->at[i] = at_us;
        r->head++;
    }
    portEXIT_CRITICAL(&r->mux);
    return ok;
}

void midi_timed_ring_commit(midi_timed_ring_t *r)
{
    service(r, false);
}

void midi_timed_ring_clear(midi_timed_ring_t *r)
{
    portENTER_CRITICAL(&r->mux);
    r->tail = r->cursor = r->head;
    r->armed = false;
    portEXIT_CRITICAL(&r->mux);
    /* What comes next may be due before the time it was set for. If it is
     * firing right now, it finds nothing (or sets itself again). */
    esp_timer_stop(r->timer);
}

int midi_timed_ring_bytes_due(midi_timed_ring_t *r)
{
    int64_t now = esp_timer_get_time();
    uint32_t n = 0;
    portENTER_CRITICAL(&r->mux);
    for (uint32_t c = r->tail; c != r->head && r->at[c & r->mask] <= now + DUE_SLACK_US; c++) {
        n++;
        if (n >= 64) break;         /* enough to say "read" */
    }
    portEXIT_CRITICAL(&r->mux);
    return (int)(n * 4);
}

int midi_timed_ring_read(midi_timed_ring_t *r, uint8_t *buf, size_t maxlen)
{
    int64_t now = esp_timer_get_time();
    size_t n = 0;
    portENTER_CRITICAL(&r->mux);
    while (n + 4 <= maxlen && r->tail != r->head
           && r->at[r->tail & r->mask] <= now + DUE_SLACK_US) {
        memcpy(buf + n, r->pkt[r->tail & r->mask], 4);
        r->tail++;
        n += 4;
    }
    portEXIT_CRITICAL(&r->mux);
    return (int)n;
}

uint32_t midi_timed_ring_count(midi_timed_ring_t *r)
{
    portENTER_CRITICAL(&r->mux);
    uint32_t n = used(r, r->tail);
    portEXIT_CRITICAL(&r->mux);
    return n;
}

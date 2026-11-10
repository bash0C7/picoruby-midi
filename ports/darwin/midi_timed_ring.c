#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dispatch/dispatch.h>

#include "../../include/midi_timed_ring.h"
#include "../../include/midi.h"
#include "midi_darwin.h"

#define DUE_SLACK_US 200
#define MIN_TIMER_US 100

struct midi_timed_ring {
    uint8_t (*pkt)[4];
    int64_t *at;
    uint32_t mask;
    uint32_t head, tail, cursor;
    bool armed;
    pthread_mutex_t mux;
    dispatch_source_t timer;
};

static inline uint32_t used(const midi_timed_ring_t *r, uint32_t from)
{
    return r->head - from;
}

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

static void service(midi_timed_ring_t *r, bool from_timer)
{
    int64_t now = midi_darwin_now_us();
    int64_t next = 0;
    bool kick, arm = false;
    pthread_mutex_lock(&r->mux);
    if (r->armed && !from_timer) {
        pthread_mutex_unlock(&r->mux);
        return;
    }
    kick = advance(r, now, &next);
    if (next != 0) {
        r->armed = true;
        arm = true;
    } else {
        r->armed = false;
    }
    pthread_mutex_unlock(&r->mux);

    if (kick) MIDI_Input_kick();
    if (arm) {
        int64_t d = next - now;
        if (d < MIN_TIMER_US) d = MIN_TIMER_US;
        dispatch_source_set_timer(r->timer, dispatch_time(DISPATCH_TIME_NOW, d * 1000),
                                  DISPATCH_TIME_FOREVER, 0);
    }
}

static dispatch_queue_t ring_queue(void)
{
    static dispatch_once_t once;
    static dispatch_queue_t q;
    dispatch_once(&once, ^{
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        q = dispatch_queue_create("picoruby.midi.ring", attr);
    });
    return q;
}

midi_timed_ring_t *midi_timed_ring_new(uint32_t packets, const char *name)
{
    (void)name;
    uint32_t n = 1;
    while (n < packets) n <<= 1;
    midi_timed_ring_t *r = calloc(1, sizeof(*r));
    if (r == NULL) return NULL;
    r->pkt = calloc(n, 4);
    r->at = calloc(n, sizeof(int64_t));
    if (r->pkt == NULL || r->at == NULL) goto fail;
    r->mask = n - 1;
    pthread_mutex_init(&r->mux, NULL);
    r->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, DISPATCH_TIMER_STRICT, ring_queue());
    if (r->timer == NULL) goto fail;
    dispatch_source_set_event_handler(r->timer, ^{ service(r, true); });
    dispatch_source_set_timer(r->timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
    dispatch_resume(r->timer);
    return r;

fail:
    free(r->pkt);
    free(r->at);
    free(r);
    return NULL;
}

void midi_timed_ring_free(midi_timed_ring_t *r)
{
    if (r == NULL) return;
    dispatch_source_cancel(r->timer);
    dispatch_sync(ring_queue(), ^{});
    dispatch_release(r->timer);
    pthread_mutex_destroy(&r->mux);
    free(r->pkt);
    free(r->at);
    free(r);
}

bool midi_timed_ring_push(midi_timed_ring_t *r, const uint8_t pkt[4], int64_t at_us)
{
    bool ok;
    pthread_mutex_lock(&r->mux);
    ok = used(r, r->tail) <= r->mask;
    if (ok) {
        uint32_t i = r->head & r->mask;
        memcpy(r->pkt[i], pkt, 4);
        r->at[i] = at_us;
        r->head++;
    }
    pthread_mutex_unlock(&r->mux);
    return ok;
}

void midi_timed_ring_commit(midi_timed_ring_t *r)
{
    service(r, false);
}

void midi_timed_ring_clear(midi_timed_ring_t *r)
{
    pthread_mutex_lock(&r->mux);
    r->tail = r->cursor = r->head;
    r->armed = false;
    pthread_mutex_unlock(&r->mux);
    dispatch_source_set_timer(r->timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
}

int midi_timed_ring_bytes_due(midi_timed_ring_t *r)
{
    int64_t now = midi_darwin_now_us();
    uint32_t n = 0;
    pthread_mutex_lock(&r->mux);
    for (uint32_t c = r->tail; c != r->head && r->at[c & r->mask] <= now + DUE_SLACK_US; c++) {
        n++;
        if (n >= 64) break;
    }
    pthread_mutex_unlock(&r->mux);
    return (int)(n * 4);
}

int midi_timed_ring_read(midi_timed_ring_t *r, uint8_t *buf, size_t maxlen)
{
    int64_t now = midi_darwin_now_us();
    size_t n = 0;
    pthread_mutex_lock(&r->mux);
    while (n + 4 <= maxlen && r->tail != r->head
           && r->at[r->tail & r->mask] <= now + DUE_SLACK_US) {
        memcpy(buf + n, r->pkt[r->tail & r->mask], 4);
        r->tail++;
        n += 4;
    }
    pthread_mutex_unlock(&r->mux);
    return (int)n;
}

uint32_t midi_timed_ring_count(midi_timed_ring_t *r)
{
    pthread_mutex_lock(&r->mux);
    uint32_t n = used(r, r->tail);
    pthread_mutex_unlock(&r->mux);
    return n;
}

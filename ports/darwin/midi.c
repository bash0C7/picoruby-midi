#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <dispatch/dispatch.h>

#include "../../include/midi.h"
#include "../../include/midi_parser.h"
#include "../../include/midi_scheduler.h"
#include "../../include/midi_clock_gen.h"
#include "../../include/midi_transport.h"
#include "../../include/midi_route.h"
#include "midi_darwin.h"

#define CIN_SINGLE_BYTE 0x0F
#define MIDI_STATUS_TIMING_CLOCK 0xF8
#define MIDI_STATUS_START        0xFA
#define MIDI_STATUS_CONTINUE     0xFB
#define MIDI_STATUS_STOP         0xFC

int64_t midi_darwin_now_us(void)
{
    return (int64_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000);
}

static int g_timer_queue_key;

static dispatch_queue_t timer_queue(void)
{
    static dispatch_once_t once;
    static dispatch_queue_t q;
    dispatch_once(&once, ^{
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
            DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        q = dispatch_queue_create("picoruby.midi.timer", attr);
        dispatch_queue_set_specific(q, &g_timer_queue_key, &g_timer_queue_key, NULL);
    });
    return q;
}

static void on_timer_queue(dispatch_block_t block)
{
    dispatch_queue_t q = timer_queue();
    if (dispatch_get_specific(&g_timer_queue_key) == &g_timer_queue_key) {
        block();
    } else {
        dispatch_sync(q, block);
    }
}

static midi_stop_check_fn g_stop_check_fn = NULL;

void MIDI_set_stop_check(midi_stop_check_fn fn)
{
    g_stop_check_fn = fn;
}

static inline bool midi_stop_requested(void)
{
    return g_stop_check_fn ? g_stop_check_fn() : false;
}

static dispatch_source_t g_clock_timer = NULL;
static midi_tmask_t g_clock_bit;
static float g_clock_follow_bpm;
static uint64_t g_clock_period_us;
static volatile uint32_t g_clock_next_period_us;
static volatile bool g_clock_running;
static volatile uint32_t g_clock_count;
static volatile uint32_t g_clock_last_tick_us;
static midi_clock_callback_t g_clock_user_cb;

static int clock_tx_send_packet(void *ctx, uint8_t cable, uint8_t cin,
                                uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)ctx; (void)cable; (void)cin; (void)b1; (void)b2; (void)b3;
    return 0;
}

static bool clock_tx_is_connected(void *ctx)
{
    (void)ctx;
    return true;
}

static const midi_transport_ops_t g_clock_tx_ops = {
    .send_packet     = clock_tx_send_packet,
    .read_bytes      = NULL,
    .bytes_available = NULL,
    .is_connected    = clock_tx_is_connected,
    .transport_id    = MIDI_TRANSPORT_ID_NONE,
};

static const midi_transport_t g_clock_transport = {
    .ops = &g_clock_tx_ops,
    .ctx = NULL,
};

__attribute__((constructor))
static void register_clock_transport(void)
{
    g_clock_bit = MIDI_transport_register(&g_clock_transport);
    if (g_clock_bit) MIDI_route_set_source(g_clock_bit, true);
}

static void clock_send(uint8_t status)
{
    midi_tmask_t bit = g_clock_bit;
    if (!bit) return;
    midi_tmask_t dst = MIDI_route_targets(bit, CIN_SINGLE_BYTE, status);
    MIDI_route_dispatch(bit, dst, CIN_SINGLE_BYTE, status, 0, 0);
}

static void clock_arm_after(uint64_t first_us, uint64_t period_us)
{
    dispatch_source_set_timer(g_clock_timer,
                              dispatch_time(DISPATCH_TIME_NOW, (int64_t)first_us * 1000),
                              period_us * 1000, 0);
}

static void clock_arm(uint64_t period_us)
{
    clock_arm_after(period_us, period_us);
}

static void clock_tick(void)
{
    g_clock_last_tick_us = (uint32_t)midi_darwin_now_us();
    g_clock_count++;
    uint32_t next = __atomic_exchange_n(&g_clock_next_period_us, 0, __ATOMIC_RELAXED);
    if (next) {
        g_clock_period_us = next;
        clock_arm(next);
    }
    if (MIDI_route_from(g_clock_bit)) clock_send(MIDI_STATUS_TIMING_CLOCK);
    midi_clock_callback_t cb = g_clock_user_cb;
    if (cb) cb();
}

static float clock_current_bpm(void)
{
    float follow = g_clock_follow_bpm;
    return follow > 0.0f ? follow : midi_clock_gen_get_bpm();
}

static uint64_t clock_period_us(float bpm)
{
    if (bpm < 20.0f) bpm = 20.0f;
    if (bpm > 300.0f) bpm = 300.0f;
    return (uint64_t)(60000000.0f / (bpm * 24.0f));
}

static void clock_retime(void)
{
    if (g_clock_timer == NULL) return;
    uint32_t period = (uint32_t)clock_period_us(clock_current_bpm());
    __atomic_store_n(&g_clock_next_period_us,
                     period == g_clock_period_us ? 0 : period, __ATOMIC_RELAXED);
}

int MIDI_Clock_init(void)
{
    if (g_clock_timer != NULL) return 0;
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0,
                                                 DISPATCH_TIMER_STRICT, timer_queue());
    if (t == NULL) return -1;
    dispatch_source_set_event_handler(t, ^{ clock_tick(); });
    g_clock_period_us = clock_period_us(clock_current_bpm());
    g_clock_next_period_us = 0;
    g_clock_last_tick_us = (uint32_t)midi_darwin_now_us();
    g_clock_timer = t;
    clock_arm(g_clock_period_us);
    dispatch_resume(t);
    return 0;
}

void MIDI_Clock_deinit(void)
{
    on_timer_queue(^{
        if (g_clock_timer != NULL) {
            dispatch_source_cancel(g_clock_timer);
            dispatch_release(g_clock_timer);
            g_clock_timer = NULL;
        }
        g_clock_period_us = 0;
        g_clock_next_period_us = 0;
        g_clock_running = false;
    });
}

midi_tmask_t MIDI_Clock_bit(void)
{
    return g_clock_bit;
}

void MIDI_Clock_set_bpm(float bpm)
{
    midi_clock_gen_set_bpm(bpm);
    clock_retime();
}

float MIDI_Clock_get_bpm(void)
{
    return midi_clock_gen_get_bpm();
}

void MIDI_Clock_set_follow_bpm(float bpm)
{
    g_clock_follow_bpm = bpm > 0.0f ? bpm : 0.0f;
    clock_retime();
}

float MIDI_Clock_current_bpm(void)
{
    return clock_current_bpm();
}

void MIDI_Clock_start(void)
{
    if (MIDI_Clock_init() != 0) return;
    g_clock_running = true;
    clock_send(MIDI_STATUS_START);
    on_timer_queue(^{
        uint32_t next = __atomic_exchange_n(&g_clock_next_period_us, 0, __ATOMIC_RELAXED);
        if (next) g_clock_period_us = next;
        /* The clock right after Start is the first beat: receivers start
         * on it, so it goes now rather than a period later */
        clock_arm_after(0, g_clock_period_us);
        g_clock_last_tick_us = (uint32_t)midi_darwin_now_us();
    });
}

void MIDI_Clock_continue(void)
{
    if (MIDI_Clock_init() != 0) return;
    g_clock_running = true;
    clock_send(MIDI_STATUS_CONTINUE);
}

void MIDI_Clock_stop(void)
{
    g_clock_running = false;
    clock_send(MIDI_STATUS_STOP);
}

void MIDI_Clock_reset(void)
{
    g_clock_running = false;
}

bool MIDI_Clock_is_running(void)
{
    return g_clock_running;
}

uint32_t MIDI_Clock_count(void)
{
    return g_clock_count;
}

uint32_t MIDI_Clock_us_to_next(void)
{
    if (g_clock_timer == NULL) return 0;
    uint32_t elapsed = (uint32_t)midi_darwin_now_us() - g_clock_last_tick_us;
    uint32_t period = (uint32_t)g_clock_period_us;
    return elapsed >= period ? 0 : period - elapsed;
}

void MIDI_Clock_set_callback(midi_clock_callback_t callback)
{
    g_clock_user_cb = callback;
}

#ifndef MIDI_INPUT_POLL_MS
#define MIDI_INPUT_POLL_MS 5
#endif

static pthread_mutex_t g_input_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_input_thread;
static bool g_input_thread_joinable = false;
static dispatch_semaphore_t g_input_wake = NULL;
static volatile bool g_input_running = false;
static volatile bool g_input_was_started = false;
static volatile midi_tmask_t g_packet_sources = 0;

#define MIDI_CLOCK_TRACKED_MAX 4
static struct {
    volatile midi_tmask_t bit;
    midi_parser_t *parser;
} g_tracked[MIDI_CLOCK_TRACKED_MAX];

void MIDI_Input_add_packet_source(midi_tmask_t bit)
{
    if (bit == 0 || (bit & (bit - 1)) != 0) return;
    g_packet_sources = g_packet_sources | bit;
    MIDI_route_set_source(bit, true);
}

void MIDI_Input_kick(void)
{
    dispatch_semaphore_t w = g_input_wake;
    if (w && g_input_running) dispatch_semaphore_signal(w);
}

void MIDI_Input_track_clock(midi_tmask_t bit)
{
    if (bit == 0 || (bit & (bit - 1)) != 0) return;
    for (int i = 0; i < MIDI_CLOCK_TRACKED_MAX; i++) {
        if (g_tracked[i].bit == bit) return;
    }
    for (int i = 0; i < MIDI_CLOCK_TRACKED_MAX; i++) {
        if (g_tracked[i].bit == 0) {
            g_tracked[i].parser = midi_parser_new(MIDI_SOURCE_USB);
            if (g_tracked[i].parser) g_tracked[i].bit = bit;
            return;
        }
    }
}

static midi_parser_t *tracked_parser(midi_tmask_t bit)
{
    for (int i = 0; i < MIDI_CLOCK_TRACKED_MAX; i++) {
        if (g_tracked[i].bit == bit) return g_tracked[i].parser;
    }
    return NULL;
}

static midi_tmask_t packet_sources_wanted(void)
{
    midi_tmask_t want = 0;
    midi_tmask_t sources = g_packet_sources;
    for (midi_tmask_t bit = 1; bit; bit <<= 1) {
        if (!(sources & bit)) continue;
        const midi_transport_t *t = MIDI_transport_get(bit);
        if (t && t->ops && t->ops->is_connected && t->ops->read_bytes && t->ops->bytes_available
            && t->ops->is_connected(t->ctx) && MIDI_route_from(bit)) {
            want |= bit;
        }
    }
    return want;
}

static void reset_tracked_parsers(void)
{
    for (int i = 0; i < MIDI_CLOCK_TRACKED_MAX; i++) {
        if (g_tracked[i].parser) midi_parser_reset(g_tracked[i].parser);
    }
}

static void *midi_input_thread(void *arg)
{
    (void)arg;
    uint8_t buffer[256];
    int64_t sent[sizeof(buffer) / 4];
    midi_event_t event;
    pthread_setname_np("midi_input");

    while (g_input_running) {
        if (midi_stop_requested()) break;
        midi_tmask_t packet_sources = packet_sources_wanted();
        for (midi_tmask_t bit = 1; bit; bit <<= 1) {
            if (!(packet_sources & bit)) continue;
            const midi_transport_t *t = MIDI_transport_get(bit);
            if (!t) continue;
            while (t->ops->bytes_available(t->ctx) >= 4) {
                bool timed = t->ops->read_sent != NULL;
                int read_len = timed ? t->ops->read_sent(t->ctx, buffer, sent, sizeof(buffer))
                                     : t->ops->read_bytes(t->ctx, buffer, sizeof(buffer));
                if (read_len <= 0) break;
                midi_parser_t *clock = tracked_parser(bit);
                int64_t now = midi_darwin_now_us();
                for (int i = 0; i + 3 < read_len; i += 4) {
                    MIDI_route_packet(bit, buffer[i] & 0x0F,
                                      buffer[i + 1], buffer[i + 2], buffer[i + 3]);
                    if (clock && midi_parser_feed_usb(clock, buffer[i] & 0x0F,
                                                      buffer[i + 1], buffer[i + 2], buffer[i + 3],
                                                      (uint64_t)(timed ? sent[i / 4] : now), &event)
                        && event.sysex_data) {
                        free(event.sysex_data);
                    }
                }
            }
        }

        dispatch_semaphore_wait(g_input_wake,
                                dispatch_time(DISPATCH_TIME_NOW, (int64_t)MIDI_INPUT_POLL_MS * 1000000));
    }

    g_input_running = false;
    reset_tracked_parsers();
    return NULL;
}

int MIDI_Input_init(void)
{
    pthread_mutex_lock(&g_input_lock);
    if (g_input_wake == NULL) g_input_wake = dispatch_semaphore_create(0);
    pthread_mutex_unlock(&g_input_lock);
    return g_input_wake ? 0 : -1;
}

static void join_finished_thread(void)
{
    if (g_input_thread_joinable && !g_input_running) {
        pthread_join(g_input_thread, NULL);
        g_input_thread_joinable = false;
    }
}

int MIDI_Input_start(void)
{
    if (MIDI_Input_init() != 0) return -1;
    pthread_mutex_lock(&g_input_lock);
    if (g_input_running) {
        pthread_mutex_unlock(&g_input_lock);
        return 0;
    }
    join_finished_thread();
    if (!packet_sources_wanted()) {
        pthread_mutex_unlock(&g_input_lock);
        return -1;
    }
    reset_tracked_parsers();
    g_input_running = true;
    if (pthread_create(&g_input_thread, NULL, midi_input_thread, NULL) != 0) {
        g_input_running = false;
        pthread_mutex_unlock(&g_input_lock);
        return -1;
    }
    g_input_thread_joinable = true;
    g_input_was_started = true;
    pthread_mutex_unlock(&g_input_lock);
    return 0;
}

void MIDI_Input_deinit(void)
{
    MIDI_Input_stop();
}

bool MIDI_Input_was_started(void)
{
    return g_input_was_started;
}

void MIDI_Input_set_queueing(bool on)
{
    (void)on;
}

int MIDI_Input_start_routing(void)
{
    g_input_was_started = true;
    return MIDI_Input_start();
}

int MIDI_route_start_source(midi_tmask_t src_bit)
{
    if ((src_bit & (MIDI_ROUTE_INPUT_SOURCES | g_packet_sources)) == 0) return 0;
    return MIDI_Input_start_routing();
}

void MIDI_Input_stop(void)
{
    if (!g_input_running) return;
    if (MIDI_route_active()) return;
    pthread_mutex_lock(&g_input_lock);
    g_input_running = false;
    if (g_input_wake) dispatch_semaphore_signal(g_input_wake);
    if (g_input_thread_joinable) {
        pthread_join(g_input_thread, NULL);
        g_input_thread_joinable = false;
    }
    pthread_mutex_unlock(&g_input_lock);
}

bool MIDI_Input_is_running(void)
{
    return g_input_running;
}

int MIDI_Input_events_available(void) { return 0; }
int MIDI_Input_events_available_usb(void) { return 0; }
int MIDI_Input_events_available_sam(void) { return 0; }

bool MIDI_Input_pop_event(midi_event_t *event) { (void)event; return false; }
bool MIDI_Input_pop_event_usb(midi_event_t *event) { (void)event; return false; }
bool MIDI_Input_pop_event_sam(midi_event_t *event) { (void)event; return false; }

float MIDI_Input_get_external_bpm_usb(void) { return 0.0f; }
float MIDI_Input_get_external_bpm_sam(void) { return 0.0f; }
float MIDI_Input_get_external_bpm(void) { return 0.0f; }

void MIDI_Input_reset_external_clock_usb(void) {}
void MIDI_Input_reset_external_clock_sam(void) {}
void MIDI_Input_reset_external_clock(void) {}

float MIDI_Input_get_external_bpm_from(midi_tmask_t src_bit)
{
    return midi_parser_get_external_bpm(tracked_parser(src_bit));
}

void MIDI_Input_reset_external_clock_from(midi_tmask_t src_bit)
{
    midi_parser_t *p = tracked_parser(src_bit);
    if (p) midi_parser_reset_external_clock(p);
}

bool MIDI_Input_get_external_clock_from(midi_tmask_t src_bit, int32_t *tick,
                                        int32_t *age_us, float *period_us)
{
    midi_parser_clock_t c;
    if (!midi_parser_get_external_clock(tracked_parser(src_bit), &c) || !c.running || c.tick < 0) {
        return false;
    }
    *tick = c.tick;
    *age_us = (int32_t)(midi_darwin_now_us() - c.at_us);
    *period_us = c.period_us;
    return true;
}

#ifndef MIDI_FILTER_QUEUE_SIZE
#define MIDI_FILTER_QUEUE_SIZE 64
#endif

static pthread_mutex_t g_filter_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_filter_ready = PTHREAD_COND_INITIALIZER;
static midi_filter_event_t *g_filter_queue = NULL;
static uint32_t g_filter_head, g_filter_tail;

static bool filter_sink(const midi_filter_event_t *ev)
{
    pthread_mutex_lock(&g_filter_lock);
    bool ok = g_filter_head - g_filter_tail < MIDI_FILTER_QUEUE_SIZE;
    if (ok) {
        g_filter_queue[g_filter_head % MIDI_FILTER_QUEUE_SIZE] = *ev;
        g_filter_head++;
        pthread_cond_signal(&g_filter_ready);
    }
    pthread_mutex_unlock(&g_filter_lock);
    return ok;
}

int MIDI_Filter_init(void)
{
    pthread_mutex_lock(&g_filter_lock);
    if (g_filter_queue == NULL) {
        g_filter_queue = calloc(MIDI_FILTER_QUEUE_SIZE, sizeof(midi_filter_event_t));
    }
    bool ok = g_filter_queue != NULL;
    pthread_mutex_unlock(&g_filter_lock);
    if (!ok) return -1;
    MIDI_route_set_filter_sink(filter_sink);
    return 0;
}

bool MIDI_Filter_pop(uint32_t *dst, midi_tmask_t *src, uint8_t *cin,
                     uint8_t *b1, uint8_t *b2, uint8_t *b3)
{
    pthread_mutex_lock(&g_filter_lock);
    if (g_filter_queue == NULL || g_filter_head == g_filter_tail) {
        pthread_mutex_unlock(&g_filter_lock);
        return false;
    }
    midi_filter_event_t ev = g_filter_queue[g_filter_tail % MIDI_FILTER_QUEUE_SIZE];
    g_filter_tail++;
    pthread_mutex_unlock(&g_filter_lock);
    *dst = ev.dst; *src = ev.src; *cin = ev.cin;
    *b1 = ev.b1; *b2 = ev.b2; *b3 = ev.b3;
    return true;
}

bool MIDI_Filter_wait(uint32_t timeout_ms)
{
    struct timespec rel = {
        .tv_sec = (time_t)(timeout_ms / 1000),
        .tv_nsec = (long)(timeout_ms % 1000) * 1000000L,
    };
    pthread_mutex_lock(&g_filter_lock);
    if (g_filter_queue == NULL) {
        pthread_mutex_unlock(&g_filter_lock);
        nanosleep(&rel, NULL);
        return false;
    }
    if (g_filter_head == g_filter_tail && timeout_ms > 0) {
        pthread_cond_timedwait_relative_np(&g_filter_ready, &g_filter_lock, &rel);
    }
    bool any = g_filter_head != g_filter_tail;
    pthread_mutex_unlock(&g_filter_lock);
    return any;
}

void MIDI_Filter_flush(void)
{
    pthread_mutex_lock(&g_filter_lock);
    g_filter_tail = g_filter_head;
    pthread_mutex_unlock(&g_filter_lock);
}

#define NOTE_SCHEDULER_TICK_US 1000

static dispatch_source_t g_note_scheduler_timer = NULL;

static void scheduler_send_packet(midi_tmask_t transport_mask, uint8_t cin,
                                  uint8_t status, uint8_t data1, uint8_t data2)
{
    MIDI_transport_send(transport_mask, 0, cin, status, data1, data2);
}

int MIDI_Note_scheduler_init(void)
{
    if (g_note_scheduler_timer != NULL) return 0;
    midi_scheduler_core_init();
    midi_scheduler_set_send_callback(scheduler_send_packet);
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, timer_queue());
    if (t == NULL) return -1;
    dispatch_source_set_event_handler(t, ^{ midi_scheduler_tick((uint64_t)midi_darwin_now_us()); });
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, NOTE_SCHEDULER_TICK_US * 1000),
                              NOTE_SCHEDULER_TICK_US * 1000, 100 * 1000);
    g_note_scheduler_timer = t;
    dispatch_resume(t);
    return 0;
}

void MIDI_Note_scheduler_deinit(void)
{
    if (g_note_scheduler_timer != NULL) {
        dispatch_source_cancel(g_note_scheduler_timer);
        dispatch_release(g_note_scheduler_timer);
        g_note_scheduler_timer = NULL;
    }
    on_timer_queue(^{ midi_scheduler_core_deinit(); });
}

int MIDI_Note_trigger(midi_tmask_t transport_mask, uint8_t channel, uint8_t note,
                      uint8_t velocity, uint32_t duration_ms)
{
    if (g_note_scheduler_timer == NULL && MIDI_Note_scheduler_init() != 0) return -1;
    __block int ret = 0;
    on_timer_queue(^{
        ret = midi_scheduler_trigger(transport_mask, channel, note, velocity,
                                     duration_ms, (uint64_t)midi_darwin_now_us());
    });
    return ret;
}

void MIDI_Note_scheduler_clear(void)
{
    on_timer_queue(^{ midi_scheduler_clear(); });
}

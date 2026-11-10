#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/midi.h"
#include "../include/midi_route.h"
#include "../include/midi_timed_ring.h"
#include "../include/midi_transport.h"
#include "midi_darwin.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static midi_timed_ring_t *g_rx;
static bool g_connected = true;

static int src_send(void *ctx, uint8_t cable, uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)ctx; (void)cable; (void)cin; (void)b1; (void)b2; (void)b3;
    return 0;
}
static int src_read(void *ctx, uint8_t *buf, size_t maxlen) { (void)ctx; return midi_timed_ring_read(g_rx, buf, maxlen); }
static int src_avail(void *ctx) { (void)ctx; return midi_timed_ring_bytes_due(g_rx); }
static bool src_connected(void *ctx) { (void)ctx; return g_connected; }

static const midi_transport_ops_t src_ops = {
    .send_packet = src_send, .read_bytes = src_read, .bytes_available = src_avail,
    .is_connected = src_connected, .transport_id = MIDI_TRANSPORT_ID_BLE,
};
static const midi_transport_t src_transport = { .ops = &src_ops, .ctx = NULL };

static volatile int g_sent;
static uint8_t g_last[4];

static int sink_send(void *ctx, uint8_t cable, uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)ctx; (void)cable;
    g_last[0] = cin; g_last[1] = b1; g_last[2] = b2; g_last[3] = b3;
    g_sent++;
    return 0;
}
static bool sink_connected(void *ctx) { (void)ctx; return true; }

static const midi_transport_ops_t sink_ops = {
    .send_packet = sink_send, .is_connected = sink_connected, .transport_id = MIDI_TRANSPORT_ID_NONE,
};
static const midi_transport_t sink_transport = { .ops = &sink_ops, .ctx = NULL };

static void test_packet_source_to_filter(void)
{
    g_rx = midi_timed_ring_new(16, "test_rx");
    CHECK(g_rx != NULL, "ring");
    midi_tmask_t bit = MIDI_transport_register(&src_transport);
    CHECK(bit != 0, "registered");
    MIDI_Input_add_packet_source(bit);

    CHECK(MIDI_Filter_init() == 0, "filter queue");
    CHECK(MIDI_route_set_filter(bit, MIDI_ROUTE_DST_RUBY, 0xFFFF, MIDI_ROUTE_ALL_CHANNELS,
                                MIDI_FILTER_OVERFLOW_DROP) == 0, "filter");
    CHECK(MIDI_route_start_source(bit) == 0, "input thread started");
    CHECK(MIDI_Input_is_running(), "input thread running");

    int64_t t0 = midi_darwin_now_us();
    const uint8_t on[4] = { 0x09, 0x90, 60, 100 };
    CHECK(midi_timed_ring_push(g_rx, on, t0 + 30000), "push");
    midi_timed_ring_commit(g_rx);

    CHECK(!MIDI_Filter_wait(10), "not due after 10 ms");
    bool got = MIDI_Filter_wait(200);
    int64_t waited = midi_darwin_now_us() - t0;
    CHECK(got, "due within 200 ms");
    CHECK(waited >= 29000 && waited < 60000, "released at its time (%lld us)", (long long)waited);

    uint32_t dst; midi_tmask_t src; uint8_t cin, b1, b2, b3;
    CHECK(MIDI_Filter_pop(&dst, &src, &cin, &b1, &b2, &b3), "pop");
    CHECK(src == bit && (dst & MIDI_ROUTE_DST_RUBY) && cin == 0x9 && b1 == 0x90 && b2 == 60 && b3 == 100,
          "note on from the source (src %u dst %u cin %u %02X %u %u)", src, dst, cin, b1, b2, b3);
    CHECK(!MIDI_Filter_pop(&dst, &src, &cin, &b1, &b2, &b3), "queue empty");

    g_connected = false;
    usleep(30000);
    CHECK(!MIDI_Input_is_running(), "input thread ends without a connected source");
    g_connected = true;
    CHECK(MIDI_route_start_source(bit) == 0, "input thread restarts");
    MIDI_route_clear_filter(bit, MIDI_ROUTE_DST_ALL);
    MIDI_Input_stop();
    CHECK(!MIDI_Input_is_running(), "stopped once nothing reads the source");
}

static void test_clock(void)
{
    MIDI_Clock_set_bpm(300.0f);
    CHECK(MIDI_Clock_init() == 0, "clock");
    uint32_t c0 = MIDI_Clock_count();
    usleep(200000);
    uint32_t ticks = MIDI_Clock_count() - c0;
    CHECK(ticks >= 20 && ticks <= 26, "24 ticks per quarter at 300 BPM in 200 ms (%u)", ticks);
    CHECK(MIDI_Clock_us_to_next() <= 8334, "next tick within a period");
    MIDI_Clock_deinit();
}

static void test_note_scheduler(void)
{
    midi_tmask_t bit = MIDI_transport_register(&sink_transport);
    CHECK(bit != 0, "sink registered");
    g_sent = 0;
    CHECK(MIDI_Note_trigger(bit, 0, 64, 90, 20) == 0, "trigger");
    CHECK(g_sent == 1 && g_last[0] == 0x9 && g_last[1] == 0x90 && g_last[2] == 64, "note on at once");
    usleep(60000);
    CHECK(g_sent == 2 && g_last[0] == 0x8 && g_last[1] == 0x80 && g_last[2] == 64, "note off after 20 ms (%d sent)", g_sent);
    MIDI_Note_scheduler_deinit();
}

int main(void)
{
    test_packet_source_to_filter();
    test_clock();
    test_note_scheduler();
    if (failures) {
        printf("test_darwin_port: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_darwin_port: ok\n");
    return 0;
}

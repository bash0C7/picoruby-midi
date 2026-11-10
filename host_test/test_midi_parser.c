/*
 * Host-build unit tests for the OS-free MIDI byte parser.
 *
 * Builds on any POSIX host (no FreeRTOS / ESP-IDF needed). See the
 * accompanying Makefile for the canonical invocation:
 *
 *     make -C host_test
 *
 * Each TEST() block asserts a property; failures cause the binary to
 * exit non-zero so this can be wired into CI.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/midi_parser.h"

static int g_test_count = 0;
static int g_pass_count = 0;

#define TEST(name) \
    static void name(void); \
    static void run_##name(void) { \
        printf("  %-50s ", #name); fflush(stdout); \
        g_test_count++; \
        name(); \
        g_pass_count++; \
        printf("ok\n"); \
    } \
    static void name(void)

#define EXPECT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "\n    FAIL %s:%d  %s=%lld  %s=%lld\n", \
                __FILE__, __LINE__, #a, _a, #b, _b); \
        exit(1); \
    } \
} while (0)

#define EXPECT_TRUE(x) do { \
    if (!(x)) { \
        fprintf(stderr, "\n    FAIL %s:%d  expected true: %s\n", \
                __FILE__, __LINE__, #x); \
        exit(1); \
    } \
} while (0)

/* Convenience: feed `n` bytes and assert the n-th byte yields a
 * complete event with `expected_type`. */
static void feed_bytes_expect(midi_parser_t *p, const uint8_t *bytes, size_t n,
                              midi_event_type_t expected_type, midi_event_t *out)
{
    bool ready = false;
    for (size_t i = 0; i < n; i++) {
        ready = midi_parser_feed_raw(p, bytes[i], 0, out);
    }
    EXPECT_TRUE(ready);
    EXPECT_EQ(out->type, expected_type);
}

/* ---------- Lifecycle ---------- */

TEST(test_alloc_and_free)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    EXPECT_TRUE(p != NULL);
    midi_parser_free(p);
    midi_parser_free(NULL);  /* must not crash */
}

/* ---------- Channel voice (raw stream) ---------- */

TEST(test_note_on)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    uint8_t bytes[] = { 0x91, 0x3C, 0x64 };  /* note on ch1 C4 vel100 */
    feed_bytes_expect(p, bytes, 3, MIDI_EVENT_NOTE_ON, &ev);
    EXPECT_EQ(ev.channel, 1);
    EXPECT_EQ(ev.data1, 0x3C);
    EXPECT_EQ(ev.data2, 0x64);
    EXPECT_EQ(ev.source, MIDI_SOURCE_USB);
    midi_parser_free(p);
}

TEST(test_note_on_velocity_zero_emits_note_off)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    uint8_t bytes[] = { 0x90, 0x3C, 0x00 };  /* note on vel=0 */
    feed_bytes_expect(p, bytes, 3, MIDI_EVENT_NOTE_OFF, &ev);
    midi_parser_free(p);
}

TEST(test_note_off)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    uint8_t bytes[] = { 0x82, 0x3C, 0x40 };  /* note off ch2 */
    feed_bytes_expect(p, bytes, 3, MIDI_EVENT_NOTE_OFF, &ev);
    EXPECT_EQ(ev.channel, 2);
    midi_parser_free(p);
}

TEST(test_control_change)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    uint8_t bytes[] = { 0xB0, 0x07, 0x7F };  /* CC ch0 volume=127 */
    feed_bytes_expect(p, bytes, 3, MIDI_EVENT_CONTROL_CHANGE, &ev);
    EXPECT_EQ(ev.data1, 7);
    EXPECT_EQ(ev.data2, 127);
    midi_parser_free(p);
}

TEST(test_program_change)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    uint8_t bytes[] = { 0xC5, 0x18 };  /* PC ch5 program 24 */
    feed_bytes_expect(p, bytes, 2, MIDI_EVENT_PROGRAM_CHANGE, &ev);
    EXPECT_EQ(ev.channel, 5);
    EXPECT_EQ(ev.data1, 24);
    midi_parser_free(p);
}

TEST(test_pitch_bend_center)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    uint8_t bytes[] = { 0xE0, 0x00, 0x40 };  /* center: 0x2000 */
    feed_bytes_expect(p, bytes, 3, MIDI_EVENT_PITCH_BEND, &ev);
    EXPECT_EQ(ev.value, 0);  /* (0x40<<7 | 0x00) - 8192 == 0 */
    midi_parser_free(p);
}

TEST(test_pitch_bend_full_range)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;

    /* Max bend: 0x3FFF -> +8191 */
    uint8_t up[] = { 0xE0, 0x7F, 0x7F };
    feed_bytes_expect(p, up, 3, MIDI_EVENT_PITCH_BEND, &ev);
    EXPECT_EQ(ev.value, 8191);

    /* Min bend: 0x0000 -> -8192 */
    midi_parser_reset(p);
    uint8_t dn[] = { 0xE0, 0x00, 0x00 };
    feed_bytes_expect(p, dn, 3, MIDI_EVENT_PITCH_BEND, &ev);
    EXPECT_EQ(ev.value, -8192);

    midi_parser_free(p);
}

TEST(test_poly_aftertouch)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    uint8_t bytes[] = { 0xA1, 0x3C, 0x55 };
    feed_bytes_expect(p, bytes, 3, MIDI_EVENT_POLY_AFTERTOUCH, &ev);
    EXPECT_EQ(ev.channel, 1);
    EXPECT_EQ(ev.data1, 0x3C);
    EXPECT_EQ(ev.data2, 0x55);
    midi_parser_free(p);
}

TEST(test_channel_pressure)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    uint8_t bytes[] = { 0xD3, 0x60 };
    feed_bytes_expect(p, bytes, 2, MIDI_EVENT_CHANNEL_PRESSURE, &ev);
    EXPECT_EQ(ev.channel, 3);
    EXPECT_EQ(ev.data1, 0x60);
    midi_parser_free(p);
}

/* ---------- Running status ---------- */

TEST(test_running_status)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;

    /* First note on with status byte */
    uint8_t first[] = { 0x90, 0x3C, 0x64 };
    feed_bytes_expect(p, first, 3, MIDI_EVENT_NOTE_ON, &ev);

    /* Subsequent two-byte runs reuse the latched status */
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0x3E, 0, &ev));
    EXPECT_TRUE(midi_parser_feed_raw(p, 0x6E, 0, &ev));
    EXPECT_EQ(ev.type, MIDI_EVENT_NOTE_ON);
    EXPECT_EQ(ev.data1, 0x3E);
    EXPECT_EQ(ev.data2, 0x6E);

    EXPECT_TRUE(!midi_parser_feed_raw(p, 0x40, 0, &ev));
    EXPECT_TRUE(midi_parser_feed_raw(p, 0x70, 0, &ev));
    EXPECT_EQ(ev.data1, 0x40);

    midi_parser_free(p);
}

/* ---------- Real-time messages ---------- */

TEST(test_realtime_start_stop_continue)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;

    EXPECT_TRUE(midi_parser_feed_raw(p, 0xFA, 0, &ev));
    EXPECT_EQ(ev.type, MIDI_EVENT_START);

    EXPECT_TRUE(midi_parser_feed_raw(p, 0xFC, 0, &ev));
    EXPECT_EQ(ev.type, MIDI_EVENT_STOP);

    EXPECT_TRUE(midi_parser_feed_raw(p, 0xFB, 0, &ev));
    EXPECT_EQ(ev.type, MIDI_EVENT_CONTINUE);

    EXPECT_TRUE(midi_parser_feed_raw(p, 0xFF, 0, &ev));
    EXPECT_EQ(ev.type, MIDI_EVENT_SYSTEM_RESET);

    midi_parser_free(p);
}

TEST(test_realtime_clock_not_emitted_as_event)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    /* Clock byte updates the BPM tracker but does not produce an event */
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0xF8, 1000, &ev));
    midi_parser_free(p);
}

TEST(test_realtime_active_sensing_dropped)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0xFE, 0, &ev));
    midi_parser_free(p);
}

TEST(test_realtime_interleaves_with_running_status)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;

    /* Start a note message */
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0x90, 0, &ev));
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0x3C, 0, &ev));

    /* Real-time clock byte sneaks in mid-message — must not break it */
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0xF8, 0, &ev));

    /* Final velocity byte completes the original note on */
    EXPECT_TRUE(midi_parser_feed_raw(p, 0x64, 0, &ev));
    EXPECT_EQ(ev.type, MIDI_EVENT_NOTE_ON);
    EXPECT_EQ(ev.data1, 0x3C);
    EXPECT_EQ(ev.data2, 0x64);

    midi_parser_free(p);
}

/* ---------- SysEx (raw stream) ---------- */

TEST(test_sysex_short)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    uint8_t bytes[] = { 0xF0, 0x7E, 0x00, 0x09, 0x01, 0xF7 };
    bool ready = false;
    for (size_t i = 0; i < sizeof(bytes); i++) {
        ready = midi_parser_feed_raw(p, bytes[i], 0, &ev);
    }
    EXPECT_TRUE(ready);
    EXPECT_EQ(ev.type, MIDI_EVENT_SYSEX);
    EXPECT_EQ(ev.sysex_len, 6);
    EXPECT_TRUE(ev.sysex_data != NULL);
    EXPECT_EQ(ev.sysex_data[0], 0xF0);
    EXPECT_EQ(ev.sysex_data[5], 0xF7);
    EXPECT_EQ(ev.value, 0);  /* not truncated */
    free(ev.sysex_data);
    midi_parser_free(p);
}

TEST(test_sysex_truncation_flag)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;

    midi_parser_feed_raw(p, 0xF0, 0, &ev);
    /* push more than MIDI_SYSEX_MAX_LEN payload bytes */
    for (int i = 0; i < MIDI_SYSEX_MAX_LEN + 10; i++) {
        midi_parser_feed_raw(p, 0x55, 0, &ev);
    }
    bool ready = midi_parser_feed_raw(p, 0xF7, 0, &ev);

    EXPECT_TRUE(ready);
    EXPECT_EQ(ev.type, MIDI_EVENT_SYSEX);
    EXPECT_EQ(ev.value, 1);  /* truncated */
    EXPECT_EQ(ev.sysex_len, MIDI_SYSEX_MAX_LEN);
    free(ev.sysex_data);
    midi_parser_free(p);
}

/* ---------- USB-MIDI packet path ---------- */

TEST(test_usb_note_on)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    bool ready = midi_parser_feed_usb(p, 0x09, 0x91, 0x3C, 0x64, 0, &ev);
    EXPECT_TRUE(ready);
    EXPECT_EQ(ev.type, MIDI_EVENT_NOTE_ON);
    EXPECT_EQ(ev.channel, 1);
    EXPECT_EQ(ev.data1, 0x3C);
    EXPECT_EQ(ev.data2, 0x64);
    midi_parser_free(p);
}

TEST(test_usb_note_on_velocity_zero_to_off)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    bool ready = midi_parser_feed_usb(p, 0x09, 0x90, 0x3C, 0x00, 0, &ev);
    EXPECT_TRUE(ready);
    EXPECT_EQ(ev.type, MIDI_EVENT_NOTE_OFF);
    midi_parser_free(p);
}

TEST(test_usb_pitch_bend)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    bool ready = midi_parser_feed_usb(p, 0x0E, 0xE0, 0x00, 0x40, 0, &ev);
    EXPECT_TRUE(ready);
    EXPECT_EQ(ev.type, MIDI_EVENT_PITCH_BEND);
    EXPECT_EQ(ev.value, 0);
    midi_parser_free(p);
}

TEST(test_usb_realtime_via_cin0F)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    EXPECT_TRUE(midi_parser_feed_usb(p, 0x0F, 0xFA, 0, 0, 0, &ev));
    EXPECT_EQ(ev.type, MIDI_EVENT_START);
    EXPECT_TRUE(midi_parser_feed_usb(p, 0x0F, 0xFC, 0, 0, 0, &ev));
    EXPECT_EQ(ev.type, MIDI_EVENT_STOP);
    midi_parser_free(p);
}

TEST(test_usb_sysex_split_across_packets)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;

    /* CIN 0x04: SysEx start + 3 bytes (no terminator yet) */
    EXPECT_TRUE(!midi_parser_feed_usb(p, 0x04, 0xF0, 0x7E, 0x00, 0, &ev));
    /* CIN 0x07: SysEx end with last 3 bytes */
    EXPECT_TRUE(midi_parser_feed_usb(p, 0x07, 0x09, 0x01, 0xF7, 0, &ev));
    EXPECT_EQ(ev.type, MIDI_EVENT_SYSEX);
    EXPECT_EQ(ev.sysex_len, 6);
    EXPECT_EQ(ev.sysex_data[0], 0xF0);
    EXPECT_EQ(ev.sysex_data[5], 0xF7);
    free(ev.sysex_data);
    midi_parser_free(p);
}

/* ---------- External BPM tracking ---------- */

TEST(test_external_bpm_120)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    /* 120 BPM at 24 PPQ = 500ms/quarter / 24 = 20833 us between clocks */
    const uint64_t period_us = 20833;
    for (int i = 0; i < 48; i++) {
        midi_parser_feed_raw(p, 0xF8, (uint64_t)i * period_us, &ev);
    }
    float bpm = midi_parser_get_external_bpm(p);
    EXPECT_TRUE(bpm > 119.0f && bpm < 121.0f);
    midi_parser_free(p);
}

/* As on the device: the input task stamps the clocks when it wakes, once
 * every 10 ms. Past the first window the ring buffer wraps. */
TEST(test_external_bpm_quantized)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    const double period_us = 60000000.0 / (120.0 * 24.0);
    float lo = 1000.0f, hi = 0.0f;
    for (int i = 0; i < 24 * 40; i++) {
        uint64_t arrive = (uint64_t)(i * period_us) + 300;
        uint64_t stamped = (arrive / 10000 + 1) * 10000;
        midi_parser_feed_raw(p, 0xF8, stamped, &ev);
        if (i >= 48) {
            float bpm = midi_parser_get_external_bpm(p);
            if (bpm < lo) lo = bpm;
            if (bpm > hi) hi = bpm;
        }
    }
    EXPECT_TRUE(lo > 119.5f && hi < 120.5f);
    midi_parser_free(p);
}

TEST(test_external_bpm_reset)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    for (int i = 0; i < 48; i++) {
        midi_parser_feed_raw(p, 0xF8, (uint64_t)i * 20833, &ev);
    }
    EXPECT_TRUE(midi_parser_get_external_bpm(p) > 100.0f);

    midi_parser_reset_external_clock(p);
    EXPECT_EQ((int)(midi_parser_get_external_bpm(p) * 1000.0f), 0);

    midi_parser_free(p);
}

/* ---------- External clock position ---------- */

static midi_parser_clock_t clock_of(midi_parser_t *p)
{
    midi_parser_clock_t c;
    EXPECT_TRUE(midi_parser_get_external_clock(p, &c));
    return c;
}

TEST(test_external_clock_position)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    midi_parser_clock_t c;
    EXPECT_TRUE(!midi_parser_get_external_clock(p, &c));

    midi_parser_feed_raw(p, 0xF8, 1000, &ev);
    c = clock_of(p);
    EXPECT_TRUE(!c.running);
    EXPECT_EQ(c.tick, -1);

    EXPECT_TRUE(midi_parser_feed_raw(p, 0xFA, 2000, &ev));
    EXPECT_EQ(ev.type, MIDI_EVENT_START);
    c = clock_of(p);
    EXPECT_TRUE(c.running);
    EXPECT_EQ(c.tick, -1);

    midi_parser_feed_raw(p, 0xF8, 3000, &ev);
    midi_parser_feed_raw(p, 0xF8, 4000, &ev);
    c = clock_of(p);
    EXPECT_EQ(c.tick, 1);
    EXPECT_EQ(c.at_us, 4000);

    midi_parser_feed_raw(p, 0xFC, 5000, &ev);
    midi_parser_feed_raw(p, 0xF8, 6000, &ev);
    c = clock_of(p);
    EXPECT_TRUE(!c.running);
    EXPECT_EQ(c.tick, 1);

    midi_parser_feed_raw(p, 0xFB, 7000, &ev);
    midi_parser_feed_raw(p, 0xF8, 8000, &ev);
    EXPECT_EQ(clock_of(p).tick, 2);
    midi_parser_free(p);
}

TEST(test_external_clock_song_position_pointer)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    /* 0x81 sixteenths = (1 << 7) | 1: 129 * 6 clocks */
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0xF2, 0, &ev));
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0x01, 0, &ev));
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0x01, 0, &ev));
    /* No running status after System Common */
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0x05, 0, &ev));
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0x00, 0, &ev));
    midi_parser_feed_raw(p, 0xFB, 0, &ev);
    midi_parser_feed_raw(p, 0xF8, 100, &ev);
    EXPECT_EQ(clock_of(p).tick, 129 * 6);

    midi_parser_feed_raw(p, 0xFC, 200, &ev);
    EXPECT_TRUE(!midi_parser_feed_usb(p, 0x03, 0xF2, 0x10, 0x00, 300, &ev));
    midi_parser_feed_usb(p, 0x0F, 0xFB, 0, 0, 400, &ev);
    midi_parser_feed_usb(p, 0x0F, 0xF8, 0, 0, 500, &ev);
    EXPECT_EQ(clock_of(p).tick, 16 * 6);

    midi_parser_feed_usb(p, 0x0F, 0xFA, 0, 0, 600, &ev);
    midi_parser_feed_usb(p, 0x0F, 0xF8, 0, 0, 700, &ev);
    EXPECT_EQ(clock_of(p).tick, 0);
    midi_parser_free(p);
}

/* Stamps off by up to +-1 ms (a 1 ms timestamp): the time of the position
 * is the newest clock's stamp, the period stays near the real one, and the
 * period carries over a Start. */
TEST(test_external_clock_time_and_period)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    const double period_us = 60000000.0 / (132.0 * 24.0);
    const int jitter[] = { 0, 900, -700, 400, -1000, 600, -200, 1000 };
    midi_parser_feed_raw(p, 0xFA, 0, &ev);
    for (int i = 0; i < 24 * 8; i++) {
        int64_t stamp = 1000000 + (int64_t)(i * period_us) + jitter[i % 8];
        midi_parser_feed_raw(p, 0xF8, (uint64_t)stamp, &ev);
        if (i < 48) continue;
        midi_parser_clock_t c = clock_of(p);
        EXPECT_EQ(c.tick, i);
        EXPECT_EQ(c.at_us, stamp);
        EXPECT_TRUE(c.period_us > period_us - 20 && c.period_us < period_us + 20);
    }

    midi_parser_feed_raw(p, 0xFA, 9000000, &ev);
    midi_parser_feed_raw(p, 0xF8, 9000100, &ev);
    midi_parser_clock_t c = clock_of(p);
    EXPECT_EQ(c.tick, 0);
    EXPECT_EQ(c.at_us, 9000100);
    EXPECT_TRUE(c.period_us > period_us - 20 && c.period_us < period_us + 20);

    midi_parser_reset_external_clock(p);
    EXPECT_EQ((int)clock_of(p).period_us, 0);
    midi_parser_reset(p);
    EXPECT_TRUE(!midi_parser_get_external_clock(p, &c));
    midi_parser_free(p);
}

/* ---------- Reset ---------- */

TEST(test_reset_clears_running_status)
{
    midi_parser_t *p = midi_parser_new(MIDI_SOURCE_USB);
    midi_event_t ev;
    uint8_t first[] = { 0x90, 0x3C, 0x64 };
    feed_bytes_expect(p, first, 3, MIDI_EVENT_NOTE_ON, &ev);

    midi_parser_reset(p);

    /* After reset, a stand-alone data byte must NOT produce an event
     * (running status was cleared) */
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0x3E, 0, &ev));
    EXPECT_TRUE(!midi_parser_feed_raw(p, 0x64, 0, &ev));

    midi_parser_free(p);
}

/* ---------- Test runner ---------- */

int main(void)
{
    printf("midi_parser host tests\n");
    run_test_alloc_and_free();
    run_test_note_on();
    run_test_note_on_velocity_zero_emits_note_off();
    run_test_note_off();
    run_test_control_change();
    run_test_program_change();
    run_test_pitch_bend_center();
    run_test_pitch_bend_full_range();
    run_test_poly_aftertouch();
    run_test_channel_pressure();
    run_test_running_status();
    run_test_realtime_start_stop_continue();
    run_test_realtime_clock_not_emitted_as_event();
    run_test_realtime_active_sensing_dropped();
    run_test_realtime_interleaves_with_running_status();
    run_test_sysex_short();
    run_test_sysex_truncation_flag();
    run_test_usb_note_on();
    run_test_usb_note_on_velocity_zero_to_off();
    run_test_usb_pitch_bend();
    run_test_usb_realtime_via_cin0F();
    run_test_usb_sysex_split_across_packets();
    run_test_external_bpm_120();
    run_test_external_bpm_quantized();
    run_test_external_bpm_reset();
    run_test_external_clock_position();
    run_test_external_clock_song_position_pointer();
    run_test_external_clock_time_and_period();
    run_test_reset_clears_running_status();

    printf("\n%d/%d passed\n", g_pass_count, g_test_count);
    return g_pass_count == g_test_count ? 0 : 1;
}

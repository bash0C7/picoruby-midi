/*
 * Host-build unit tests for the BLE-MIDI codec (src/ble_midi_codec.c):
 *
 *     make -C host_test
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/ble_midi_codec.h"

#define EXPECT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "FAIL %s:%d  %s=%lld  %s=%lld\n", \
                __FILE__, __LINE__, #a, _a, #b, _b); \
        exit(1); \
    } \
} while (0)

#define MAXP 512
static uint8_t  g_pkts[MAXP][4];
static uint16_t g_ts[MAXP];
static int      g_n;

static void collect(void *arg, const uint8_t pkt[4], uint16_t ts)
{
    (void)arg;
    if (g_n >= MAXP) { fprintf(stderr, "too many packets\n"); exit(1); }
    memcpy(g_pkts[g_n], pkt, 4);
    g_ts[g_n] = ts;
    g_n++;
}

static void expect_pkt(int i, uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
    if (i >= g_n) { fprintf(stderr, "FAIL: packet %d missing (have %d)\n", i, g_n); exit(1); }
    EXPECT_EQ(g_pkts[i][0], cin);
    EXPECT_EQ(g_pkts[i][1], b1);
    EXPECT_EQ(g_pkts[i][2], b2);
    EXPECT_EQ(g_pkts[i][3], b3);
}

static ble_midi_decoder_t g_dec;

static int decode(const uint8_t *buf, size_t len)
{
    return ble_midi_decode(&g_dec, buf, len, collect, NULL);
}

static void test_decode_basic(void)
{
    ble_midi_decoder_reset(&g_dec); g_n = 0;
    /* header hi=1, ts lo=0x10, note on C4 */
    const uint8_t p[] = { 0x81, 0x90, 0x90, 60, 100 };
    EXPECT_EQ(decode(p, sizeof(p)), 0);
    EXPECT_EQ(g_n, 1);
    expect_pkt(0, 0x9, 0x90, 60, 100);
    EXPECT_EQ(g_ts[0], (1 << 7) | 0x10);

    /* two messages, program change (2 bytes), CC */
    g_n = 0;
    const uint8_t q[] = { 0x80, 0x81, 0xC3, 5, 0x82, 0xB0, 7, 99 };
    EXPECT_EQ(decode(q, sizeof(q)), 0);
    EXPECT_EQ(g_n, 2);
    expect_pkt(0, 0xC, 0xC3, 5, 0);
    expect_pkt(1, 0xB, 0xB0, 7, 99);
    EXPECT_EQ(g_ts[1], 2);
}

static void test_decode_running_status(void)
{
    ble_midi_decoder_reset(&g_dec); g_n = 0;
    /* ts status d d | ts d d (running, with ts) | d d (running, no ts) */
    const uint8_t p[] = { 0x80, 0x80, 0x90, 60, 100, 0x81, 62, 100, 64, 90 };
    EXPECT_EQ(decode(p, sizeof(p)), 0);
    EXPECT_EQ(g_n, 3);
    expect_pkt(0, 0x9, 0x90, 60, 100);
    expect_pkt(1, 0x9, 0x90, 62, 100);
    expect_pkt(2, 0x9, 0x90, 64, 90);
    EXPECT_EQ(g_ts[2], 1);

    /* running status carries into the next packet */
    g_n = 0;
    const uint8_t q[] = { 0x80, 0x85, 65, 0 };
    EXPECT_EQ(decode(q, sizeof(q)), 0);
    EXPECT_EQ(g_n, 1);
    expect_pkt(0, 0x9, 0x90, 65, 0);
}

static void test_decode_timestamp_wrap(void)
{
    ble_midi_decoder_reset(&g_dec); g_n = 0;
    /* hi = 0x3F, lo 0x7E then lo 0x01: wraps to hi 0 (13 bits) */
    const uint8_t p[] = { 0xBF, 0xFE, 0xF8, 0x81, 0xF8 };
    EXPECT_EQ(decode(p, sizeof(p)), 0);
    EXPECT_EQ(g_n, 2);
    EXPECT_EQ(g_ts[0], (0x3F << 7) | 0x7E);
    EXPECT_EQ(g_ts[1], 1);
}

static void test_decode_realtime_inside(void)
{
    ble_midi_decoder_reset(&g_dec); g_n = 0;
    /* note on with a clock between status and data: ts 90 ts F8 3C 64 */
    const uint8_t p[] = { 0x80, 0x80, 0x90, 0x81, 0xF8, 60, 100 };
    EXPECT_EQ(decode(p, sizeof(p)), 0);
    EXPECT_EQ(g_n, 2);
    expect_pkt(0, 0xF, 0xF8, 0, 0);
    expect_pkt(1, 0x9, 0x90, 60, 100);

    /* cut off by another status: dropped, the new one decodes */
    g_n = 0;
    const uint8_t q[] = { 0x80, 0x80, 0x90, 60, 0x81, 0xB0, 1, 2 };
    EXPECT_EQ(decode(q, sizeof(q)), -1);
    EXPECT_EQ(g_n, 1);
    expect_pkt(0, 0xB, 0xB0, 1, 2);
}

static void test_decode_sysex(void)
{
    ble_midi_decoder_reset(&g_dec); g_n = 0;
    /* one packet: F0 7E 7F 06 01 F7 */
    const uint8_t p[] = { 0x80, 0x80, 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0x81, 0xF7 };
    EXPECT_EQ(decode(p, sizeof(p)), 0);
    EXPECT_EQ(g_n, 2);
    expect_pkt(0, 0x4, 0xF0, 0x7E, 0x7F);
    expect_pkt(1, 0x7, 0x06, 0x01, 0xF7);

    /* spans packets, clock in the middle */
    g_n = 0;
    const uint8_t a[] = { 0x80, 0x80, 0xF0, 0x01, 0x02 };
    const uint8_t b[] = { 0x80, 0x03, 0x81, 0xF8, 0x04, 0x05 };
    const uint8_t c[] = { 0x80, 0x06, 0x82, 0xF7 };
    EXPECT_EQ(decode(a, sizeof(a)), 0);
    EXPECT_EQ(g_n, 1);   /* the first three bytes go out at once */
    EXPECT_EQ(decode(b, sizeof(b)), 0);
    EXPECT_EQ(decode(c, sizeof(c)), 0);
    EXPECT_EQ(g_n, 4);
    expect_pkt(0, 0x4, 0xF0, 0x01, 0x02);
    expect_pkt(1, 0xF, 0xF8, 0, 0);
    expect_pkt(2, 0x4, 0x03, 0x04, 0x05);
    expect_pkt(3, 0x6, 0x06, 0xF7, 0);

    /* only F0 F7 */
    g_n = 0;
    const uint8_t e[] = { 0x80, 0x80, 0xF0, 0x80, 0xF7 };
    EXPECT_EQ(decode(e, sizeof(e)), 0);
    EXPECT_EQ(g_n, 1);
    expect_pkt(0, 0x6, 0xF0, 0xF7, 0);
}

static void test_decode_errors(void)
{
    ble_midi_decoder_reset(&g_dec); g_n = 0;
    const uint8_t bad_header[] = { 0x00, 0x80, 0x90, 1, 2 };
    EXPECT_EQ(decode(bad_header, sizeof(bad_header)), -1);
    EXPECT_EQ(g_n, 0);
    /* truncated note on, then a good one in the next packet */
    const uint8_t cut[] = { 0x80, 0x80, 0x90, 60 };
    EXPECT_EQ(decode(cut, sizeof(cut)), -1);
    EXPECT_EQ(g_n, 0);
    const uint8_t ok[] = { 0x80, 0x80, 0x80, 60, 0 };
    EXPECT_EQ(decode(ok, sizeof(ok)), 0);
    EXPECT_EQ(g_n, 1);
    expect_pkt(0, 0x8, 0x80, 60, 0);
    /* data with no running status */
    ble_midi_decoder_reset(&g_dec); g_n = 0;
    const uint8_t orphan[] = { 0x80, 0x80, 60, 1 };
    EXPECT_EQ(decode(orphan, sizeof(orphan)), -1);
    EXPECT_EQ(g_n, 0);
    /* system common: song position, tune request */
    g_n = 0;
    const uint8_t sc[] = { 0x80, 0x80, 0xF2, 0x10, 0x20, 0x80, 0xF6 };
    EXPECT_EQ(decode(sc, sizeof(sc)), 0);
    EXPECT_EQ(g_n, 2);
    expect_pkt(0, 0x3, 0xF2, 0x10, 0x20);
    expect_pkt(1, 0x5, 0xF6, 0, 0);
}

/* --- encoder --- */

static uint8_t g_out[64][256];
static size_t  g_out_len[64];
static int     g_outs;

static void flush_to_list(ble_midi_encoder_t *e)
{
    memcpy(g_out[g_outs], e->buf, e->len);
    g_out_len[g_outs] = e->len;
    g_outs++;
}

/* Encode `pkts` with times `ts` into packets of `cap`, then decode them all */
static void roundtrip(const uint8_t (*pkts)[4], const uint16_t *ts, int n, size_t cap)
{
    static uint8_t buf[256];
    ble_midi_encoder_t e;
    memset(&e, 0, sizeof(e));
    g_outs = 0;
    ble_midi_encoder_begin(&e, buf, cap, ts[0]);
    for (int i = 0; i < n; i++) {
        uint8_t done = 0;
        int guard = 0;
        while (!ble_midi_encode(&e, pkts[i], ts[i], &done)) {
            flush_to_list(&e);
            ble_midi_encoder_begin(&e, buf, cap, ts[i]);
            if (++guard > 8) { fprintf(stderr, "FAIL: no progress\n"); exit(1); }
        }
    }
    if (ble_midi_encoder_pending(&e)) flush_to_list(&e);

    for (int k = 0; k < g_outs; k++) {
        if (g_out_len[k] > cap) { fprintf(stderr, "FAIL: packet over cap\n"); exit(1); }
    }
    ble_midi_decoder_reset(&g_dec); g_n = 0;
    for (int k = 0; k < g_outs; k++) {
        EXPECT_EQ(decode(g_out[k], g_out_len[k]), 0);
    }
}

static void test_encode_simple(void)
{
    const uint8_t p[][4] = {
        { 0x09, 0x90, 60, 100 },
        { 0x09, 0x90, 64, 100 },
        { 0x0C, 0xC0, 3, 0 },
        { 0x0F, 0xF8, 0, 0 },
    };
    const uint16_t ts[] = { 100, 100, 101, 105 };
    roundtrip(p, ts, 4, 20);
    EXPECT_EQ(g_outs, 1);
    /* header, ts 90 3C 64, ts 90 40 64, ts C0 03, ts F8 */
    EXPECT_EQ(g_out_len[0], 1 + 4 + 4 + 3 + 2);
    EXPECT_EQ(g_out[0][0], 0x80);          /* 100 >> 7 = 0 */
    EXPECT_EQ(g_out[0][1], 0x80 | 100);
    EXPECT_EQ(g_n, 4);
    for (int i = 0; i < 4; i++) {
        expect_pkt(i, p[i][0], p[i][1], p[i][2], p[i][3]);
        EXPECT_EQ(g_ts[i], ts[i]);
    }
}

static void test_encode_split_and_wrap(void)
{
    /* 10 notes in 20-byte packets (4 bytes each + header: 4 per packet) */
    uint8_t p[10][4];
    uint16_t ts[10];
    for (int i = 0; i < 10; i++) {
        p[i][0] = 0x09; p[i][1] = 0x91; p[i][2] = (uint8_t)(40 + i); p[i][3] = 1;
        ts[i] = (uint16_t)((8180 + i * 3) & BLE_MIDI_TS_MASK);  /* crosses 8191 -> 0 */
    }
    roundtrip((const uint8_t (*)[4])p, ts, 10, 20);
    EXPECT_EQ(g_outs, 3);
    EXPECT_EQ(g_n, 10);
    for (int i = 0; i < 10; i++) {
        expect_pkt(i, 0x09, 0x91, 40 + i, 1);
        EXPECT_EQ(g_ts[i], ts[i]);
    }
}

static void test_encode_time_jumps(void)
{
    /* the second note is 300 ms later: high bits move by 2, needs a new packet */
    const uint8_t p[][4] = { { 0x09, 0x90, 60, 1 }, { 0x08, 0x80, 60, 0 } };
    const uint16_t ts[] = { 10, 310 };
    roundtrip(p, ts, 2, 100);
    EXPECT_EQ(g_outs, 2);
    EXPECT_EQ(g_n, 2);
    EXPECT_EQ(g_ts[0], 10);
    EXPECT_EQ(g_ts[1], 310);

    /* a time going backwards is written as the previous one */
    const uint16_t back[] = { 500, 490 };
    roundtrip(p, back, 2, 100);
    EXPECT_EQ(g_outs, 1);
    EXPECT_EQ(g_ts[1], 500);
}

static void test_encode_sysex(void)
{
    /* F0 + 40 data bytes + F7 as USB-MIDI packets */
    uint8_t sx[42];
    sx[0] = 0xF0;
    for (int i = 1; i <= 40; i++) sx[i] = (uint8_t)i;
    sx[41] = 0xF7;
    uint8_t p[16][4];
    uint16_t ts[16];
    int n = 0;
    for (int i = 0; i < 42; i += 3) {
        int left = 42 - i;
        memset(p[n], 0, 4);
        if (left > 3) {
            p[n][0] = 0x4;
            memcpy(p[n] + 1, sx + i, 3);
        } else {
            p[n][0] = (uint8_t)(0x4 + left);
            memcpy(p[n] + 1, sx + i, left);
        }
        ts[n] = 7;
        n++;
    }
    for (size_t cap = 5; cap <= 60; cap += 11) {
        roundtrip((const uint8_t (*)[4])p, ts, n, cap);
        EXPECT_EQ(g_n, n);
        for (int i = 0; i < n; i++) expect_pkt(i, p[i][0], p[i][1], p[i][2], p[i][3]);
    }

    /* a clock inside the SysEx, and a note after it */
    const uint8_t q[][4] = {
        { 0x4, 0xF0, 0x01, 0x02 },
        { 0xF, 0xF8, 0, 0 },
        { 0x6, 0x03, 0xF7, 0 },
        { 0x9, 0x90, 1, 2 },
    };
    const uint16_t qt[] = { 1, 2, 3, 4 };
    roundtrip(q, qt, 4, 20);
    EXPECT_EQ(g_n, 4);
    expect_pkt(0, 0x4, 0xF0, 0x01, 0x02);
    expect_pkt(1, 0xF, 0xF8, 0, 0);
    expect_pkt(2, 0x6, 0x03, 0xF7, 0);
    expect_pkt(3, 0x9, 0x90, 1, 2);
}

static void test_cin_helpers(void)
{
    EXPECT_EQ(ble_midi_status_cin(0x93), 0x9);
    EXPECT_EQ(ble_midi_status_cin(0xF2), 0x3);
    EXPECT_EQ(ble_midi_status_cin(0xF1), 0x2);
    EXPECT_EQ(ble_midi_status_cin(0xF6), 0x5);
    EXPECT_EQ(ble_midi_status_cin(0xFA), 0xF);
    EXPECT_EQ(ble_midi_cin_length(0x0), 0);
    EXPECT_EQ(ble_midi_cin_length(0xC), 2);
    EXPECT_EQ(ble_midi_cin_length(0x5), 1);
    /* the UUIDs, little-endian */
    EXPECT_EQ(BLE_MIDI_SERVICE_UUID_LE[15], 0x03);
    EXPECT_EQ(BLE_MIDI_SERVICE_UUID_LE[0], 0x00);
    EXPECT_EQ(BLE_MIDI_CHAR_UUID_LE[15], 0x77);
    EXPECT_EQ(BLE_MIDI_CHAR_UUID_LE[0], 0xF3);
}

int main(void)
{
    test_decode_basic();
    test_decode_running_status();
    test_decode_timestamp_wrap();
    test_decode_realtime_inside();
    test_decode_sysex();
    test_decode_errors();
    test_encode_simple();
    test_encode_split_and_wrap();
    test_encode_time_jumps();
    test_encode_sysex();
    test_cin_helpers();
    printf("all BLE-MIDI codec tests passed\n");
    return 0;
}

/*
 * PicoRuby MIDI - BLE-MIDI packet codec (see include/ble_midi_codec.h)
 *
 * Plain C, no OS: compiled into libmruby with the rest of picoruby-midi, and
 * on the host by host_test/Makefile.
 */

#include "../include/ble_midi_codec.h"

#include <string.h>

/* 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, little-endian */
const uint8_t BLE_MIDI_SERVICE_UUID_LE[16] = {
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
    0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03,
};

/* 7772E5DB-3868-4112-A1A9-F2669D106BF3, little-endian */
const uint8_t BLE_MIDI_CHAR_UUID_LE[16] = {
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1,
    0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77,
};

/* ---------------------------------------------------------------------
 * USB-MIDI helpers
 * ------------------------------------------------------------------- */

uint8_t ble_midi_cin_length(uint8_t cin)
{
    switch (cin & 0x0F) {
    case 0x2: return 2;            /* system common, 2 bytes */
    case 0x3: return 3;            /* system common, 3 bytes */
    case 0x4: return 3;            /* SysEx starts / continues */
    case 0x5: return 1;            /* 1-byte common or SysEx end with 1 byte */
    case 0x6: return 2;            /* SysEx ends with 2 bytes */
    case 0x7: return 3;            /* SysEx ends with 3 bytes */
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xE:
        return 3;
    case 0xC: case 0xD:
        return 2;
    case 0xF: return 1;            /* single byte (real-time) */
    default:  return 0;            /* 0x0 / 0x1: reserved */
    }
}

/* Length of a message by its status byte (0: SysEx / undefined) */
static uint8_t status_length(uint8_t status)
{
    if (status < 0xF0) {
        uint8_t hi = status & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
    }
    switch (status) {
    case 0xF1: case 0xF3: return 2;   /* MTC quarter frame, song select */
    case 0xF2:            return 3;   /* song position */
    case 0xF6:            return 1;   /* tune request */
    case 0xF4: case 0xF5: return 1;   /* undefined: pass on alone */
    default:
        return (status >= 0xF8) ? 1 : 0;
    }
}

uint8_t ble_midi_status_cin(uint8_t status)
{
    if (status < 0xF0) return status >> 4;
    if (status >= 0xF8) return 0x0F;
    switch (status_length(status)) {
    case 1: return 0x05;
    case 2: return 0x02;
    case 3: return 0x03;
    default: return 0x0F;
    }
}

/* ---------------------------------------------------------------------
 * Decoder
 * ------------------------------------------------------------------- */

void ble_midi_decoder_reset(ble_midi_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
}

static void emit(ble_midi_packet_fn out, void *arg, uint8_t cin,
                 const uint8_t *bytes, uint8_t n, uint16_t ts)
{
    uint8_t pkt[4] = { (uint8_t)(cin & 0x0F), 0, 0, 0 };
    for (uint8_t i = 0; i < n && i < 3; i++) pkt[1 + i] = bytes[i];
    out(arg, pkt, ts);
}

/* SysEx bytes go out three at a time as soon as they are there, so that a
 * real-time message inside the SysEx keeps its place */
static void sysex_byte(ble_midi_decoder_t *d, uint8_t b, ble_midi_packet_fn out, void *arg)
{
    d->sx[d->sx_len++] = b;
    if (d->sx_len == 3) {
        emit(out, arg, 0x4, d->sx, 3, d->ts);
        d->sx_len = 0;
    }
}

static void sysex_end(ble_midi_decoder_t *d, ble_midi_packet_fn out, void *arg)
{
    d->sx[d->sx_len++] = 0xF7;
    emit(out, arg, (uint8_t)(0x4 + d->sx_len), d->sx, d->sx_len, d->ts);
    d->sx_len = 0;
    d->in_sysex = false;
}

/* A SysEx cut off by another status: drop what was collected */
static void sysex_abort(ble_midi_decoder_t *d)
{
    d->sx_len = 0;
    d->in_sysex = false;
    d->errors++;
}

static void start_message(ble_midi_decoder_t *d, uint8_t status)
{
    d->msg[0] = status;
    d->msg_len = 1;
    d->msg_need = status_length(status);
}

static void finish_if_complete(ble_midi_decoder_t *d, ble_midi_packet_fn out, void *arg)
{
    if (d->msg_len > 0 && d->msg_len == d->msg_need) {
        emit(out, arg, ble_midi_status_cin(d->msg[0]), d->msg, d->msg_len, d->ts);
        d->msg_len = 0;
    }
}

int ble_midi_decode(ble_midi_decoder_t *d, const uint8_t *buf, size_t len,
                    ble_midi_packet_fn out, void *arg)
{
    uint32_t errors_before = d->errors;

    if (len < 1 || (buf[0] & 0xC0) != 0x80) {
        d->errors++;
        return -1;
    }

    uint8_t hi = buf[0] & 0x3F;
    int last_lo = -1;
    bool after_ts = false;    /* the previous byte was a timestamp */
    size_t i = 1;

    /* A message other than SysEx never spans packets */
    if (d->msg_len > 0) {
        d->msg_len = 0;
        d->errors++;
    }

    for (; i < len; i++) {
        uint8_t b = buf[i];

        if (b & 0x80) {
            if (!after_ts) {
                /* timestamp */
                uint8_t lo = b & 0x7F;
                if (last_lo >= 0 && lo < last_lo) hi = (hi + 1) & 0x3F;
                last_lo = lo;
                d->ts = (uint16_t)(((uint16_t)hi << 7) | lo) & BLE_MIDI_TS_MASK;
                /* A message in progress may only be interrupted by a
                 * real-time one (decided by the status that follows) */
                after_ts = true;
                continue;
            }
            /* status after a timestamp */
            after_ts = false;
            if (b >= 0xF8) {
                uint8_t rt = b;
                emit(out, arg, 0x0F, &rt, 1, d->ts);   /* may sit inside anything */
                continue;
            }
            if (d->msg_len > 0) {       /* cut off before its data was complete */
                d->msg_len = 0;
                d->errors++;
            }
            if (b == 0xF7) {
                if (d->in_sysex) sysex_end(d, out, arg);
                else d->errors++;
                continue;
            }
            if (d->in_sysex) sysex_abort(d);
            if (b == 0xF0) {
                d->in_sysex = true;
                d->sx_len = 0;
                sysex_byte(d, 0xF0, out, arg);
                d->running = 0;
                continue;
            }
            if (b < 0xF0) d->running = b;
            else d->running = 0;        /* system common clears running status */
            start_message(d, b);
            finish_if_complete(d, out, arg);
            continue;
        }

        /* data byte */
        after_ts = false;
        if (d->in_sysex) {
            sysex_byte(d, b, out, arg);
            continue;
        }
        if (d->msg_len == 0) {
            /* running status: after a timestamp, or with none at all (the
             * previous message's time) */
            if (d->running == 0) {
                d->errors++;
                continue;
            }
            start_message(d, d->running);
        }
        d->msg[d->msg_len++] = b;
        finish_if_complete(d, out, arg);
    }

    if (d->msg_len > 0) {       /* incomplete at the end of the packet */
        d->msg_len = 0;
        d->errors++;
    }
    return d->errors == errors_before ? 0 : -1;
}

/* ---------------------------------------------------------------------
 * Encoder
 * ------------------------------------------------------------------- */

void ble_midi_encoder_begin(ble_midi_encoder_t *e, uint8_t *buf, size_t cap, uint16_t ts)
{
    e->buf = buf;
    e->cap = cap;
    e->hi = (uint8_t)((ts >> 7) & 0x3F);
    e->lo = 0;
    e->any_ts = false;
    buf[0] = (uint8_t)(0x80 | e->hi);
    e->len = 1;
}

/* The timestamp byte for `ts`, if the receiver can follow it from what is in
 * the packet: same high bits and not earlier, or the next high bits with a
 * smaller low (the wrap the receiver detects). A time that went backwards is
 * written as the previous one. Returns -1 when only a new packet can carry it. */
static int ts_byte(ble_midi_encoder_t *e, uint16_t ts, uint8_t *hi_out)
{
    uint8_t hi = (uint8_t)((ts >> 7) & 0x3F);
    uint8_t lo = (uint8_t)(ts & 0x7F);
    if (!e->any_ts) {
        if (hi == e->hi) { *hi_out = hi; return 0x80 | lo; }
        /* the header was written for another time: only lo can move */
        return -1;
    }
    if (hi == e->hi && lo >= e->lo) { *hi_out = hi; return 0x80 | lo; }
    if (hi == ((e->hi + 1) & 0x3F) && lo < e->lo) { *hi_out = hi; return 0x80 | lo; }
    /* earlier than what is already in the packet (or same high, smaller low
     * would read as a wrap): keep the last time */
    uint16_t last = (uint16_t)(((uint16_t)e->hi << 7) | e->lo);
    uint16_t diff = (uint16_t)((ts - last) & BLE_MIDI_TS_MASK);
    if (diff > (BLE_MIDI_TS_MASK / 2)) { *hi_out = e->hi; return 0x80 | e->lo; }
    return -1;
}

static void put_ts(ble_midi_encoder_t *e, int tsb, uint8_t hi)
{
    e->buf[e->len++] = (uint8_t)tsb;
    e->hi = hi;
    e->lo = (uint8_t)(tsb & 0x7F);
    e->any_ts = true;
}

bool ble_midi_encode(ble_midi_encoder_t *e, const uint8_t pkt[4], uint16_t ts, uint8_t *done)
{
    uint8_t cin = pkt[0] & 0x0F;
    uint8_t n = ble_midi_cin_length(cin);
    if (n == 0) return true;
    const uint8_t *bytes = pkt + 1;
    bool sysex_pkt = (cin == 0x4) || (cin == 0x6) || (cin == 0x7)
                  || (cin == 0x5 && bytes[0] == 0xF7);

    if (!sysex_pkt) {
        uint8_t hi;
        int tsb = ts_byte(e, ts, &hi);
        if (tsb < 0) {
            if (e->len > 1) return false;
            ble_midi_encoder_begin(e, e->buf, e->cap, ts);   /* empty: retime the header */
            tsb = ts_byte(e, ts, &hi);
        }
        if (e->len + 1 + n > e->cap) return false;
        put_ts(e, tsb, hi);
        for (uint8_t i = 0; i < n; i++) e->buf[e->len++] = bytes[i];
        /* any status but real-time ends a SysEx (the receiver drops it) */
        if (bytes[0] < 0xF8) e->sysex_open = false;
        *done = n;
        return true;
    }

    /* SysEx: byte by byte; F0 / F7 need a timestamp in front */
    while (*done < n) {
        uint8_t b = bytes[*done];
        if (b == 0xF0 || b == 0xF7) {
            uint8_t hi;
            int tsb = ts_byte(e, ts, &hi);
            if (tsb < 0) {
                if (e->len <= 1) {
                    ble_midi_encoder_begin(e, e->buf, e->cap, ts);
                    tsb = ts_byte(e, ts, &hi);
                } else {
                    return false;
                }
            }
            if (e->len + 2 > e->cap) return false;
            put_ts(e, tsb, hi);
            e->buf[e->len++] = b;
            e->sysex_open = (b == 0xF0);
        } else {
            if (e->len + 1 > e->cap) return false;
            e->buf[e->len++] = b;
        }
        (*done)++;
    }
    return true;
}

/* ---------------------------------------------------------------------
 * Receive timing
 * ------------------------------------------------------------------- */

void ble_midi_sync_reset(ble_midi_sync_t *s)
{
    memset(s, 0, sizeof(*s));
}

int32_t ble_midi_sync_auto_budget(uint16_t interval_1250us)
{
    int32_t b = (int32_t)interval_1250us * 1250 + BLE_MIDI_SYNC_MARGIN_US;
    if (b < BLE_MIDI_SYNC_BUDGET_MIN_US) b = BLE_MIDI_SYNC_BUDGET_MIN_US;
    if (b > BLE_MIDI_SYNC_BUDGET_MAX_US) b = BLE_MIDI_SYNC_BUDGET_MAX_US;
    return b;
}

int64_t ble_midi_sync_release(ble_midi_sync_t *s, uint16_t ts,
                              int64_t now_us, int32_t budget_us)
{
    if (budget_us <= 0) return now_us;
    int64_t now_ms = now_us / 1000;
    ts &= BLE_MIDI_TS_MASK;

    if (!s->started || now_ms - s->last_ms > BLE_MIDI_SYNC_IDLE_MS) {
        uint32_t late = s->late;
        ble_midi_sync_reset(s);
        s->late = late;
        s->started = true;
        s->sender_ms = ts;
        s->offset_us = now_us - (int64_t)ts * 1000;
        s->relax_at_ms = now_ms;
    } else {
        /* The 8192 ms candidate nearest the sender time we expect now */
        int64_t expected = (now_us - s->offset_us) / 1000;
        int64_t cand = (expected & ~(int64_t)BLE_MIDI_TS_MASK) | ts;
        if (cand - expected > (BLE_MIDI_TS_MASK + 1) / 2) cand -= BLE_MIDI_TS_MASK + 1;
        else if (expected - cand > (BLE_MIDI_TS_MASK + 1) / 2) cand += BLE_MIDI_TS_MASK + 1;
        s->sender_ms = cand;

        while (now_ms - s->relax_at_ms >= BLE_MIDI_SYNC_RELAX_MS) {
            s->offset_us += 1000;
            s->relax_at_ms += BLE_MIDI_SYNC_RELAX_MS;
        }
        int64_t d = now_us - cand * 1000;
        if (d < s->offset_us) {
            s->offset_us = d;
            s->relax_at_ms = now_ms;
        }
    }
    s->last_ms = now_ms;

    int64_t rel = s->sender_ms * 1000 + s->offset_us + budget_us;
    if (rel < now_us) {
        rel = now_us;
        s->late++;
    }
    if (rel > now_us + budget_us) rel = now_us + budget_us;
    if (rel < s->last_release_us) rel = s->last_release_us;
    s->last_release_us = rel;
    return rel;
}

int64_t ble_midi_sync_sent(const ble_midi_sync_t *s)
{
    return s->sender_ms * 1000 + s->offset_us;
}

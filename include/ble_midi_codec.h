/*
 * PicoRuby MIDI - BLE-MIDI packet codec
 *
 * MIDI over Bluetooth Low Energy (MMA / AMEI BLE-MIDI 1.0) carries MIDI in
 * the value of one GATT characteristic:
 *
 *     [header][ts][status][data...][ts][status][data...] ...
 *     header = 1 0 t12..t7      upper 6 bits of a 13-bit millisecond clock
 *     ts     = 1 t6..t0         lower 7 bits, before every message
 *
 * Running status may drop the status byte (and the timestamp) of a message;
 * real-time messages may sit inside other messages; SysEx alone may span
 * packets (a continuation packet goes on with data right after its header).
 *
 * This codec converts between that and 4-byte USB-MIDI event packets
 * ([cable << 4 | CIN][b1][b2][b3]), the unit every transport of picoruby-midi
 * speaks. It is plain C with no OS or BLE stack behind it (host tests:
 * host_test/test_ble_midi_codec.c), shared by picoruby-ble_midi_device and
 * picoruby-ble_midi_host.
 */

#ifndef BLE_MIDI_CODEC_DEFINED_H_
#define BLE_MIDI_CODEC_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GATT identifiers (128-bit, as strings for logs / docs) */
#define BLE_MIDI_SERVICE_UUID_STR "03B80E5A-EDE8-4B33-A751-6CE34EC4C700"
#define BLE_MIDI_CHAR_UUID_STR    "7772E5DB-3868-4112-A1A9-F2669D106BF3"

/* The same UUIDs in little-endian byte order (what BLE stacks take) */
extern const uint8_t BLE_MIDI_SERVICE_UUID_LE[16];
extern const uint8_t BLE_MIDI_CHAR_UUID_LE[16];

/* Timestamps are 13 bits of milliseconds */
#define BLE_MIDI_TS_MASK 0x1FFF

/* ---------------------------------------------------------------------
 * Decoder: BLE-MIDI packets -> USB-MIDI packets
 * ------------------------------------------------------------------- */

/* Receives each USB-MIDI packet decoded, with the 13-bit timestamp the
 * sender gave its message. */
typedef void (*ble_midi_packet_fn)(void *arg, const uint8_t pkt[4], uint16_t ts);

typedef struct {
    uint8_t running;    /* running status (kept across packets, read leniently) */
    uint8_t msg[3];     /* message being put together */
    uint8_t msg_len;
    uint8_t msg_need;
    bool    in_sysex;   /* inside a SysEx that may go on in the next packet */
    uint8_t sx[3];      /* SysEx bytes not yet in a USB-MIDI packet */
    uint8_t sx_len;
    uint16_t ts;        /* timestamp of the SysEx / last message */
    uint32_t errors;    /* malformed input seen (bytes dropped) */
} ble_midi_decoder_t;

void ble_midi_decoder_reset(ble_midi_decoder_t *d);

/* Decode one BLE-MIDI packet (one characteristic value). Calls `out` for
 * every complete message, in order. Returns 0, or -1 when something was
 * malformed (whatever could be read is still delivered; d->errors counts). */
int ble_midi_decode(ble_midi_decoder_t *d, const uint8_t *buf, size_t len,
                    ble_midi_packet_fn out, void *arg);

/* ---------------------------------------------------------------------
 * Encoder: USB-MIDI packets -> BLE-MIDI packets
 *
 *     ble_midi_encoder_begin(&e, buf, mtu - 3, now);
 *     for each USB-MIDI packet p (with the time it was sent, ts):
 *         uint8_t done = 0;
 *         while (!ble_midi_encode(&e, p, ts, &done)) {
 *             send(buf, e.len);                     // full: send, go on
 *             ble_midi_encoder_begin(&e, buf, cap, ts);
 *         }
 *     if (ble_midi_encoder_pending(&e)) send(buf, e.len);
 *
 * Messages are written whole except SysEx, which is split wherever the
 * packet fills up (`done` remembers how far a USB-MIDI packet got). Running
 * status is never used.
 * ------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    uint8_t  hi;          /* timestamp high bits as the receiver tracks them */
    uint8_t  lo;          /* last low 7 bits written */
    bool     any_ts;      /* a timestamp byte is in this packet */
    bool     sysex_open;  /* SysEx started and not ended (carries to the next packet) */
} ble_midi_encoder_t;

/* Start a packet in `buf` (cap = ATT MTU - 3, at least 5). The header takes
 * the high bits of `ts`. A SysEx left open continues in it. */
void ble_midi_encoder_begin(ble_midi_encoder_t *e, uint8_t *buf, size_t cap, uint16_t ts);

/* Add one USB-MIDI packet. Returns true once all of it is in the packet,
 * false when it does not fit (send the packet, begin a new one, call again
 * with the same `done`). Reserved CINs (0, 1) are skipped (true). */
bool ble_midi_encode(ble_midi_encoder_t *e, const uint8_t pkt[4], uint16_t ts, uint8_t *done);

/* Whether the packet holds anything beyond its header */
static inline bool ble_midi_encoder_pending(const ble_midi_encoder_t *e)
{
    return e->len > 1;
}

/* ---------------------------------------------------------------------
 * USB-MIDI helpers
 * ------------------------------------------------------------------- */

/* MIDI bytes a USB-MIDI packet with this CIN carries (0 for reserved) */
uint8_t ble_midi_cin_length(uint8_t cin);

/* CIN for a complete (non-SysEx) message starting with `status` */
uint8_t ble_midi_status_cin(uint8_t status);

#ifdef __cplusplus
}
#endif

#endif /* BLE_MIDI_CODEC_DEFINED_H_ */

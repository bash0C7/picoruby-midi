/*
 * PicoRuby MIDI - Transport interface
 *
 * Abstract MIDI byte-stream transport. Each concrete transport gem
 * (USB-MIDI host, USB-MIDI device, UART/serial MIDI, BLE-MIDI, ...)
 * implements this op-table and presents itself as a midi_transport_t
 * that the protocol layer can use uniformly.
 *
 * The interface is OS-free: implementations may sit on top of FreeRTOS
 * queues, ESP-IDF drivers, TinyUSB, or pure ring buffers.
 */

#ifndef MIDI_TRANSPORT_DEFINED_H_
#define MIDI_TRANSPORT_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable transport identifiers. The protocol layer uses these to route
 * by category without string-matching Ruby class names (replacing the
 * old _get_transport_mask logic). */
#define MIDI_TRANSPORT_ID_NONE      0
#define MIDI_TRANSPORT_ID_USB       1   /* USB-MIDI Host or Device */
#define MIDI_TRANSPORT_ID_SERIAL    2   /* UART / DIN MIDI */
#define MIDI_TRANSPORT_ID_BLE       3   /* BLE-MIDI */

typedef struct midi_transport_ops {
    /* Send one 4-byte USB-MIDI packet.
     * Returns 0 on success, negative on error. Required.
     */
    int  (*send_packet)(void *ctx, uint8_t cable, uint8_t cin,
                        uint8_t b1, uint8_t b2, uint8_t b3);

    /* Read raw bytes/packets from the transport's RX buffer into `buf`.
     * Returns the number of bytes read, 0 if none available, or
     * negative on error. NULL if the transport is send-only.
     */
    int  (*read_bytes)(void *ctx, uint8_t *buf, size_t maxlen);

    /* Bytes currently available to read. NULL if send-only. */
    int  (*bytes_available)(void *ctx);

    /* Whether the transport is currently usable (e.g. USB enumerated,
     * UART driver installed, BLE link up). Required.
     */
    bool (*is_connected)(void *ctx);

    /* Stable category identifier; one of MIDI_TRANSPORT_ID_*. */
    uint8_t transport_id;

    /* Like read_bytes, and puts the local time each packet was sent into
     * sent_us (one per 4 bytes), for a transport that knows it (BLE-MIDI
     * timestamps). The external clock is measured on these times instead
     * of when the packets are read. NULL if it does not. Darwin port. */
    int  (*read_sent)(void *ctx, uint8_t *buf, int64_t *sent_us, size_t maxlen);
} midi_transport_ops_t;

typedef struct midi_transport {
    const midi_transport_ops_t *ops;
    void *ctx;
} midi_transport_t;

/*
 * Transport registry
 *
 * Routing by transport_mask (the note scheduler, MIDI_Note_trigger(),
 * application cleanup) goes through this table instead of naming each
 * transport, so a new transport gem only has to register itself; the
 * protocol layer never includes its headers.
 *
 * One bit of the midi_tmask_t mask per transport, so at most
 * MIDI_TRANSPORT_REGISTRY_SIZE transports. The low bits are reserved for
 * the original built-ins and keep their historical values (0x01 USB host,
 * 0x02 UART, 0x04 USB device -- see MIDI_TRANSPORT_* in midi.h), so masks
 * that scripts or the UI hard-coded keep working. Everything else gets a
 * bit from MIDI_TRANSPORT_FIRST_DYNAMIC_BIT upwards.
 *
 * The table is written once at start-up and only read afterwards; entries
 * are never freed while a scheduled note may still reference their bit.
 */
#define MIDI_TRANSPORT_REGISTRY_SIZE      16
#define MIDI_TRANSPORT_FIRST_DYNAMIC_BIT  0x08

/* A set of transports, one bit each (a single bit names one). Widen it
 * together with MIDI_TRANSPORT_REGISTRY_SIZE. */
typedef uint16_t midi_tmask_t;
#define MIDI_TMASK_ALL ((midi_tmask_t)0xFFFFu)

/* Register `t` under a fixed bit (one of the reserved built-in bits).
 * Returns 0 on success, -1 if `bit` is not a single bit or is taken by a
 * different transport. Registering the same transport twice is a no-op. */
int MIDI_transport_register_at(midi_tmask_t bit, const midi_transport_t *t);

/* Register `t` under the next free dynamic bit and return that bit, or 0
 * when the table is full. Registering the same transport again returns
 * the bit it already has. */
midi_tmask_t MIDI_transport_register(const midi_transport_t *t);

/* Remove whatever is registered under `bit`. */
void MIDI_transport_unregister(midi_tmask_t bit);

/* The transport registered under `bit`, or NULL. */
const midi_transport_t *MIDI_transport_get(midi_tmask_t bit);

/* Send one USB-MIDI packet to every registered transport whose bit is set
 * in `mask`. Unregistered bits are ignored. */
void MIDI_transport_send(midi_tmask_t mask, uint8_t cable, uint8_t cin,
                         uint8_t b1, uint8_t b2, uint8_t b3);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_TRANSPORT_DEFINED_H_ */

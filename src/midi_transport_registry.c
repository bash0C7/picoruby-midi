/*
 * PicoRuby MIDI - Transport registry (OS-free)
 *
 * Maps each bit of a transport_mask to a midi_transport_t. See
 * include/midi_transport.h for the contract.
 */

#include <stddef.h>
#include "../include/midi_transport.h"

static const midi_transport_t *volatile g_registry[MIDI_TRANSPORT_REGISTRY_SIZE];

/* Index of a single-bit mask, or -1 when `bit` is zero or has several bits. */
static int bit_index(midi_tmask_t bit)
{
    if (bit == 0 || (bit & (bit - 1)) != 0) return -1;
    int i = 0;
    while ((bit >>= 1) != 0) i++;
    return i;
}

static int find(const midi_transport_t *t)
{
    for (int i = 0; i < MIDI_TRANSPORT_REGISTRY_SIZE; i++) {
        if (g_registry[i] == t) return i;
    }
    return -1;
}

int MIDI_transport_register_at(midi_tmask_t bit, const midi_transport_t *t)
{
    int i = bit_index(bit);
    if (i < 0 || t == NULL) return -1;
    if (g_registry[i] != NULL && g_registry[i] != t) return -1;
    g_registry[i] = t;
    return 0;
}

midi_tmask_t MIDI_transport_register(const midi_transport_t *t)
{
    if (t == NULL) return 0;
    int i = find(t);
    if (i >= 0) return (midi_tmask_t)(1u << i);
    for (i = bit_index(MIDI_TRANSPORT_FIRST_DYNAMIC_BIT);
         i < MIDI_TRANSPORT_REGISTRY_SIZE; i++) {
        if (g_registry[i] == NULL) {
            g_registry[i] = t;
            return (midi_tmask_t)(1u << i);
        }
    }
    return 0;
}

void MIDI_transport_unregister(midi_tmask_t bit)
{
    int i = bit_index(bit);
    if (i >= 0) g_registry[i] = NULL;
}

const midi_transport_t *MIDI_transport_get(midi_tmask_t bit)
{
    int i = bit_index(bit);
    return (i >= 0) ? g_registry[i] : NULL;
}

void MIDI_transport_send(midi_tmask_t mask, uint8_t cable, uint8_t cin,
                         uint8_t b1, uint8_t b2, uint8_t b3)
{
    for (int i = 0; i < MIDI_TRANSPORT_REGISTRY_SIZE; i++) {
        if ((mask & (1u << i)) == 0) continue;
        const midi_transport_t *t = g_registry[i];
        if (t != NULL && t->ops != NULL && t->ops->send_packet != NULL) {
            t->ops->send_packet(t->ctx, cable, cin, b1, b2, b3);
        }
    }
}

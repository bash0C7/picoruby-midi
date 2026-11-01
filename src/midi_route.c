/*
 * PicoRuby MIDI - Routing table (OS-free)
 *
 * See include/midi_route.h.
 */

#include <stddef.h>
#include "../include/midi_route.h"
#include "../include/midi_transport.h"

typedef struct {
    volatile midi_tmask_t src;   /* single transport bit; 0 = slot unused */
    volatile midi_tmask_t dst;   /* transport mask */
    volatile uint32_t sel;   /* MIDI_ROUTE_SEL_* */
} midi_route_t;

typedef struct {
    volatile midi_tmask_t src; /* single transport bit; 0 = slot unused */
    volatile uint8_t overflow; /* MIDI_FILTER_OVERFLOW_* */
    volatile int8_t channel;   /* 0..15, or MIDI_ROUTE_ALL_CHANNELS */
    volatile uint32_t dst;     /* one destination bit (or MIDI_ROUTE_DST_RUBY) */
    volatile uint16_t types;   /* bit n = CIN n */
} midi_filter_t;

static midi_route_t g_routes[MIDI_ROUTE_MAX];
static midi_filter_t g_filters[MIDI_FILTER_MAX];
static volatile midi_tmask_t g_extra_sources;
static midi_filter_sink_t g_filter_sink;
static volatile uint32_t g_filter_overflows;
static volatile midi_tmask_t g_watch[2];   /* MIDI_WATCH_APP / MIDI_WATCH_SCRIPT */
static volatile midi_tmask_t g_virtual;          /* The virtual port's bit, 0: none */
static volatile uint16_t g_virtual_listen;       /* CINs queued for its input */

static bool single_bit(midi_tmask_t b)
{
    return b != 0 && (b & (b - 1)) == 0;
}

uint32_t MIDI_route_selection(int channel)
{
    if (channel == MIDI_ROUTE_ALL_CHANNELS) return MIDI_ROUTE_SEL_ALL;
    if (channel == MIDI_ROUTE_CLOCK_ONLY) return MIDI_ROUTE_SEL_CLOCK;
    if (channel >= 0 && channel <= 15) return 1u << channel;
    return 0;
}

int MIDI_route_channel(uint32_t sel)
{
    if (sel & MIDI_ROUTE_SEL_ALL) return MIDI_ROUTE_ALL_CHANNELS;
    if (sel == MIDI_ROUTE_SEL_CLOCK) return MIDI_ROUTE_CLOCK_ONLY;
    for (int ch = 0; ch < 16; ch++) {
        if (sel == (1u << ch)) return ch;
    }
    return MIDI_ROUTE_SEVERAL;
}

int MIDI_route_add(midi_tmask_t src_bit, midi_tmask_t dst_mask, int channel)
{
    uint32_t sel = MIDI_route_selection(channel);
    if (sel == 0) return -1;
    return MIDI_route_add_selection(src_bit, dst_mask, sel);
}

int MIDI_route_add_selection(midi_tmask_t src_bit, midi_tmask_t dst_mask, uint32_t sel)
{
    if (!single_bit(src_bit)) return -1;
    dst_mask &= (midi_tmask_t)~src_bit;       /* no echo back to the source */
    if (dst_mask == 0) return -1;
    if (sel == 0 || (sel & ~(MIDI_ROUTE_SEL_CHANNELS | MIDI_ROUTE_SEL_CLOCK | MIDI_ROUTE_SEL_ALL))) return -1;
    if ((sel & MIDI_ROUTE_SEL_ALL) && sel != MIDI_ROUTE_SEL_ALL) return -1;

    for (int i = 0; i < MIDI_ROUTE_MAX; i++) {
        if (g_routes[i].src == src_bit && g_routes[i].sel == sel) {
            g_routes[i].dst |= dst_mask;
            return 0;
        }
    }
    for (int i = 0; i < MIDI_ROUTE_MAX; i++) {
        if (g_routes[i].src == 0) {
            g_routes[i].dst = dst_mask;
            g_routes[i].sel = sel;
            g_routes[i].src = src_bit;        /* last: publishes the slot */
            return 0;
        }
    }
    return -1;
}

int MIDI_route_remove(midi_tmask_t src_bit, midi_tmask_t dst_mask)
{
    int dropped = 0;
    for (int i = 0; i < MIDI_ROUTE_MAX; i++) {
        if (g_routes[i].src != src_bit) continue;
        midi_tmask_t left = g_routes[i].dst & (midi_tmask_t)~dst_mask;
        if (left == 0) {
            g_routes[i].src = 0;              /* first: unpublishes the slot */
            g_routes[i].dst = 0;
            dropped++;
        } else {
            g_routes[i].dst = left;
        }
    }
    return dropped;
}

void MIDI_route_clear(void)
{
    for (int i = 0; i < MIDI_ROUTE_MAX; i++) {
        g_routes[i].src = 0;
        g_routes[i].dst = 0;
    }
    for (int i = 0; i < MIDI_FILTER_MAX; i++) {
        g_filters[i].src = 0;
    }
    g_watch[MIDI_WATCH_SCRIPT] = 0;
    g_virtual_listen = 0;   /* Its listeners were the script's */
}

void MIDI_route_set_watch(int owner, midi_tmask_t src_mask)
{
    if (owner != MIDI_WATCH_APP && owner != MIDI_WATCH_SCRIPT) return;
    g_watch[owner] = src_mask;
}

midi_tmask_t MIDI_route_watched(void)
{
    return (midi_tmask_t)(g_watch[MIDI_WATCH_APP] | g_watch[MIDI_WATCH_SCRIPT]);
}

bool MIDI_route_active(void)
{
    for (int i = 0; i < MIDI_ROUTE_MAX; i++) {
        if (g_routes[i].src != 0) return true;
    }
    for (int i = 0; i < MIDI_FILTER_MAX; i++) {
        if (g_filters[i].src != 0) return true;
    }
    return MIDI_route_watched() != 0;
}

bool MIDI_route_from(midi_tmask_t src_bit)
{
    for (int i = 0; i < MIDI_ROUTE_MAX; i++) {
        if (g_routes[i].src == src_bit) return true;
    }
    for (int i = 0; i < MIDI_FILTER_MAX; i++) {
        if (g_filters[i].src == src_bit) return true;
    }
    return (MIDI_route_watched() & src_bit) != 0;
}

bool MIDI_route_get_selection(int i, midi_tmask_t *src_bit, midi_tmask_t *dst_mask, uint32_t *sel)
{
    if (i < 0 || i >= MIDI_ROUTE_MAX) return false;
    midi_tmask_t src = g_routes[i].src;
    if (src == 0) return false;
    if (src_bit) *src_bit = src;
    if (dst_mask) *dst_mask = g_routes[i].dst;
    if (sel) *sel = g_routes[i].sel;
    return true;
}

bool MIDI_route_get(int i, midi_tmask_t *src_bit, midi_tmask_t *dst_mask, int *channel)
{
    uint32_t sel;
    if (!MIDI_route_get_selection(i, src_bit, dst_mask, &sel)) return false;
    if (channel) *channel = MIDI_route_channel(sel);
    return true;
}

/* The channel a packet belongs to, or -1 for system messages (and SysEx,
 * whose continuation packets carry no status byte). */
static int packet_channel(uint8_t cin, uint8_t b1)
{
    switch (cin & 0x0F) {
    case 0x8: case 0x9: case 0xA: case 0xB:
    case 0xC: case 0xD: case 0xE:
        return b1 & 0x0F;
    default:
        return -1;
    }
}

/* Timing clock, start / continue / stop, song position pointer */
static bool packet_is_clock(uint8_t cin, uint8_t b1)
{
    switch (cin & 0x0F) {
    case 0xF:   /* single byte */
        return b1 == 0xF8 || b1 == 0xFA || b1 == 0xFB || b1 == 0xFC;
    case 0x3:   /* three-byte system common */
        return b1 == 0xF2;
    default:
        return false;
    }
}

midi_tmask_t MIDI_route_targets(midi_tmask_t src_bit, uint8_t cin, uint8_t b1)
{
    int ch = packet_channel(cin, b1);
    bool clock = packet_is_clock(cin, b1);
    midi_tmask_t dst = 0;
    for (int i = 0; i < MIDI_ROUTE_MAX; i++) {
        if (g_routes[i].src != src_bit) continue;
        uint32_t sel = g_routes[i].sel;
        if ((sel & MIDI_ROUTE_SEL_ALL) || (ch >= 0 && (sel & (1u << ch))) ||
            (clock && (sel & MIDI_ROUTE_SEL_CLOCK))) {
            dst |= g_routes[i].dst;
        }
    }
    return dst;
}

void MIDI_route_set_source(midi_tmask_t bit, bool on)
{
    if (!single_bit(bit)) return;
    if (on) {
        g_extra_sources |= bit;
    } else {
        g_extra_sources &= (midi_tmask_t)~bit;
    }
}

midi_tmask_t MIDI_route_sources(void)
{
    return (midi_tmask_t)(MIDI_ROUTE_INPUT_SOURCES | g_extra_sources);
}

/* --- Filters --------------------------------------------------------- */

void MIDI_route_set_filter_sink(midi_filter_sink_t sink)
{
    g_filter_sink = sink;
}

int MIDI_route_set_filter(midi_tmask_t src_bit, uint32_t dst_mask, uint16_t types,
                          int channel, uint8_t overflow)
{
    if (!single_bit(src_bit)) return -1;
    dst_mask &= ~(uint32_t)src_bit;
    dst_mask &= MIDI_ROUTE_DST_ALL;
    if (dst_mask == 0 || types == 0) return -1;
    if (channel < MIDI_ROUTE_ALL_CHANNELS || channel > 15) return -1;

    /* One slot per destination bit, so removing one leaves the others. Check
     * there is room for all of them first, so a failure changes nothing. */
    int need = 0;
    for (uint32_t bit = 1; bit <= MIDI_ROUTE_DST_RUBY; bit <<= 1) {
        if (!(dst_mask & bit)) continue;
        bool found = false;
        for (int i = 0; i < MIDI_FILTER_MAX; i++) {
            if (g_filters[i].src == src_bit && g_filters[i].dst == bit) found = true;
        }
        if (!found) need++;
    }
    int free_slots = 0;
    for (int i = 0; i < MIDI_FILTER_MAX; i++) {
        if (g_filters[i].src == 0) free_slots++;
    }
    if (need > free_slots) return -1;

    for (uint32_t bit = 1; bit <= MIDI_ROUTE_DST_RUBY; bit <<= 1) {
        if (!(dst_mask & bit)) continue;
        midi_filter_t *f = NULL;
        for (int i = 0; i < MIDI_FILTER_MAX && !f; i++) {
            if (g_filters[i].src == src_bit && g_filters[i].dst == bit) f = &g_filters[i];
        }
        bool fresh = (f == NULL);
        for (int i = 0; i < MIDI_FILTER_MAX && !f; i++) {
            if (g_filters[i].src == 0) f = &g_filters[i];
        }
        f->types = types;
        f->channel = (int8_t)channel;
        f->overflow = overflow;
        if (fresh) {
            f->dst = bit;
            f->src = src_bit;                 /* last: publishes the slot */
        }
    }
    return 0;
}

int MIDI_route_clear_filter(midi_tmask_t src_bit, uint32_t dst_mask)
{
    int removed = 0;
    for (int i = 0; i < MIDI_FILTER_MAX; i++) {
        if (g_filters[i].src == src_bit && (g_filters[i].dst & dst_mask)) {
            g_filters[i].src = 0;
            removed++;
        }
    }
    return removed;
}

uint32_t MIDI_route_filtered(midi_tmask_t src_bit)
{
    uint32_t dst = 0;
    for (int i = 0; i < MIDI_FILTER_MAX; i++) {
        if (g_filters[i].src == src_bit) dst |= g_filters[i].dst;
    }
    return dst;
}

uint32_t MIDI_route_filter_overflows(void)
{
    return g_filter_overflows;
}

void MIDI_route_set_virtual(midi_tmask_t bit)
{
    g_virtual = single_bit(bit) ? bit : 0;
}

midi_tmask_t MIDI_route_virtual(void)
{
    return g_virtual;
}

void MIDI_route_set_virtual_listen(uint16_t types)
{
    g_virtual_listen = types;
}

void MIDI_route_dispatch(midi_tmask_t src_bit, midi_tmask_t dst_mask, uint8_t cin,
                         uint8_t b1, uint8_t b2, uint8_t b3)
{
    cin &= 0x0F;
    uint32_t divert = 0;
    midi_tmask_t bypass = 0;   /* diverted destinations that take it unfiltered on overflow */

    if (g_filter_sink) {
        int ch = packet_channel(cin, b1);
        uint32_t candidates = (uint32_t)dst_mask | MIDI_ROUTE_DST_RUBY;
        for (int i = 0; i < MIDI_FILTER_MAX; i++) {
            if (g_filters[i].src != src_bit) continue;
            uint32_t d = g_filters[i].dst;
            if (!(d & candidates)) continue;
            if (!(g_filters[i].types & (1u << cin))) continue;
            int fc = g_filters[i].channel;
            if (fc != MIDI_ROUTE_ALL_CHANNELS && fc != ch) continue;
            divert |= d;
            if (g_filters[i].overflow == MIDI_FILTER_OVERFLOW_BYPASS) bypass |= (midi_tmask_t)d;
        }
    }

    midi_tmask_t direct = dst_mask & (midi_tmask_t)~divert;
    /* Into the virtual port is input for Ruby, never a send to its transport
     * (that would be output from it) */
    midi_tmask_t virt = g_virtual;
    uint32_t queued = divert;
    if (virt && (direct & virt)) {
        direct &= (midi_tmask_t)~virt;
        if (g_filter_sink && (g_virtual_listen & (1u << cin))) queued |= MIDI_ROUTE_DST_VIRTUAL;
    }
    bypass &= (midi_tmask_t)~virt;
    if (queued) {
        midi_filter_event_t ev = { queued, src_bit, cin, b1, b2, b3 };
        if (!g_filter_sink(&ev)) {
            g_filter_overflows++;
            direct |= bypass;
        }
    }
    /* One send per destination even when several routes name it. */
    if (direct != 0) {
        MIDI_transport_send(direct, 0, cin, b1, b2, b3);
    }
}

void MIDI_route_packet(midi_tmask_t src_bit, uint8_t cin,
                       uint8_t b1, uint8_t b2, uint8_t b3)
{
    MIDI_route_dispatch(src_bit, MIDI_route_targets(src_bit, cin, b1), cin, b1, b2, b3);
}

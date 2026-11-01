/*
 * Host-build unit tests for the transport registry and the route table
 * (src/midi_transport_registry.c, src/midi_route.c). No FreeRTOS needed:
 *
 *     make -C host_test
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/midi_transport.h"
#include "../include/midi_route.h"

#define EXPECT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "FAIL %s:%d  %s=%lld  %s=%lld\n", \
                __FILE__, __LINE__, #a, _a, #b, _b); \
        exit(1); \
    } \
} while (0)

/* Fake transports: count packets and remember the last one. */
#define FAKES MIDI_TRANSPORT_REGISTRY_SIZE
static int g_hits[FAKES];
static uint8_t g_last[FAKES][4];

static int fake_send(void *ctx, uint8_t cable, uint8_t cin,
                     uint8_t b1, uint8_t b2, uint8_t b3)
{
    int i = (int)(intptr_t)ctx;
    (void)cable;
    g_hits[i]++;
    g_last[i][0] = cin; g_last[i][1] = b1; g_last[i][2] = b2; g_last[i][3] = b3;
    return 0;
}

static const midi_transport_ops_t fake_ops = { .send_packet = fake_send };
static midi_transport_t g_t[FAKES];

static void reset_hits(void) { memset(g_hits, 0, sizeof(g_hits)); }

/* Fake filter queue */
#define SINK_CAP 4
static midi_filter_event_t g_sunk[SINK_CAP];
static int g_sunk_n;

static bool fake_sink(const midi_filter_event_t *ev)
{
    if (g_sunk_n >= SINK_CAP) return false;
    g_sunk[g_sunk_n++] = *ev;
    return true;
}

int main(void)
{
    for (int i = 0; i < FAKES; i++) {
        g_t[i].ops = &fake_ops;
        g_t[i].ctx = (void *)(intptr_t)i;
    }

    printf("registry\n");
    EXPECT_EQ(MIDI_transport_register_at(0x01, &g_t[0]), 0);   /* USB host */
    EXPECT_EQ(MIDI_transport_register_at(0x02, &g_t[1]), 0);   /* UART */
    EXPECT_EQ(MIDI_transport_register_at(0x04, &g_t[2]), 0);   /* USB device */
    EXPECT_EQ(MIDI_transport_register_at(0x01, &g_t[3]), -1);  /* taken */
    EXPECT_EQ(MIDI_transport_register_at(0x03, &g_t[3]), -1);  /* not one bit */
    EXPECT_EQ(MIDI_transport_register(&g_t[3]), 0x08);         /* e.g. AMY */
    EXPECT_EQ(MIDI_transport_register(&g_t[3]), 0x08);         /* idempotent */
    EXPECT_EQ(MIDI_transport_register(&g_t[4]), 0x10);
    MIDI_transport_send(0x03, 0, 0x9, 0x90, 60, 100);
    EXPECT_EQ(g_hits[0], 1); EXPECT_EQ(g_hits[1], 1); EXPECT_EQ(g_hits[2], 0);
    MIDI_transport_unregister(0x10);
    EXPECT_EQ((intptr_t)MIDI_transport_get(0x10), 0);
    reset_hits();

    printf("registry: 16 transports\n");
    for (int i = 4; i < FAKES; i++) {
        EXPECT_EQ(MIDI_transport_register(&g_t[i]), 1u << i);
    }
    EXPECT_EQ(MIDI_transport_register(&g_t[0] + 0), 0x01);    /* already there */
    {
        midi_transport_t extra = { &fake_ops, (void *)(intptr_t)0 };
        EXPECT_EQ(MIDI_transport_register(&extra), 0);        /* full */
    }
    MIDI_transport_send(0x8000 | 0x4000, 0, 0x9, 0x90, 60, 100);
    EXPECT_EQ(g_hits[15], 1); EXPECT_EQ(g_hits[14], 1); EXPECT_EQ(g_hits[13], 0);
    EXPECT_EQ(MIDI_route_add(0x8000, 0x01 | 0x4000, MIDI_ROUTE_ALL_CHANNELS), 0);
    MIDI_route_packet(0x8000, 0x9, 0x90, 60, 100);
    EXPECT_EQ(g_hits[0], 1); EXPECT_EQ(g_hits[14], 2); EXPECT_EQ(g_hits[15], 1);
    EXPECT_EQ(MIDI_route_targets(0x8000, 0x9, 0x90), 0x4001);
    EXPECT_EQ(MIDI_route_remove(0x8000, MIDI_TMASK_ALL), 1);
    EXPECT_EQ(MIDI_route_active(), 0);
    for (int i = 4; i < FAKES; i++) MIDI_transport_unregister((midi_tmask_t)(1u << i));
    reset_hits();

    printf("route: arguments\n");
    EXPECT_EQ(MIDI_route_active(), 0);
    EXPECT_EQ(MIDI_route_add(0x03, 0x08, -1), -1);   /* source not one bit */
    EXPECT_EQ(MIDI_route_add(0x01, 0x01, -1), -1);   /* only itself */
    EXPECT_EQ(MIDI_route_add(0x01, 0x08, 16), -1);   /* bad channel */

    printf("route: all channels\n");
    EXPECT_EQ(MIDI_route_add(0x01, 0x08, MIDI_ROUTE_ALL_CHANNELS), 0);
    EXPECT_EQ(MIDI_route_active(), 1);
    MIDI_route_packet(0x01, 0x9, 0x93, 60, 100);      /* ch 3 note on */
    EXPECT_EQ(g_hits[3], 1);
    EXPECT_EQ(g_last[3][0], 0x9); EXPECT_EQ(g_last[3][1], 0x93);
    EXPECT_EQ(g_last[3][2], 60);  EXPECT_EQ(g_last[3][3], 100);
    MIDI_route_packet(0x01, 0xF, 0xF8, 0, 0);         /* clock passes */
    EXPECT_EQ(g_hits[3], 2);
    MIDI_route_packet(0x02, 0x9, 0x90, 60, 100);      /* other source */
    EXPECT_EQ(g_hits[3], 2);
    EXPECT_EQ(g_hits[0], 0);                          /* never back to source */
    reset_hits();

    printf("route: one channel\n");
    EXPECT_EQ(MIDI_route_add(0x02, 0x02 | 0x04, 9), 0);  /* self bit dropped */
    MIDI_route_packet(0x02, 0x9, 0x99, 36, 127);      /* ch 9 */
    MIDI_route_packet(0x02, 0x9, 0x90, 36, 127);      /* ch 0: filtered */
    MIDI_route_packet(0x02, 0xF, 0xF8, 0, 0);         /* clock: filtered */
    EXPECT_EQ(g_hits[2], 1); EXPECT_EQ(g_hits[1], 0);
    reset_hits();

    printf("route: merge and dedupe\n");
    EXPECT_EQ(MIDI_route_add(0x01, 0x04, MIDI_ROUTE_ALL_CHANNELS), 0); /* merged */
    EXPECT_EQ(MIDI_route_add(0x01, 0x08, 0), 0);      /* overlaps the all-channel route */
    MIDI_route_packet(0x01, 0xB, 0xB0, 74, 64);
    EXPECT_EQ(g_hits[3], 1);                          /* once, not twice */
    EXPECT_EQ(g_hits[2], 1);
    reset_hits();

    printf("route: remove\n");
    EXPECT_EQ(MIDI_route_remove(0x01, 0x08), 1);      /* ch0 route had only 0x08 */
    MIDI_route_packet(0x01, 0xB, 0xB0, 74, 64);
    EXPECT_EQ(g_hits[3], 0); EXPECT_EQ(g_hits[2], 1);
    EXPECT_EQ(MIDI_route_remove(0x01, MIDI_TMASK_ALL), 1);
    EXPECT_EQ(MIDI_route_active(), 1);                /* the UART route is left */
    EXPECT_EQ(MIDI_route_from(0x01), 0);
    EXPECT_EQ(MIDI_route_from(0x02), 1);
    MIDI_route_clear();
    EXPECT_EQ(MIDI_route_active(), 0);

    printf("route: table full\n");
    for (int i = 0; i < MIDI_ROUTE_MAX; i++) {           /* a selection of its own each */
        uint32_t sel = (i < 16) ? (1u << i) : ((1u << (i - 16)) | MIDI_ROUTE_SEL_CLOCK);
        EXPECT_EQ(MIDI_route_add_selection(0x01, 0x08, sel), 0);
    }
    EXPECT_EQ(MIDI_route_add(0x01, 0x08, MIDI_ROUTE_ALL_CHANNELS), -1);
    EXPECT_EQ(MIDI_route_add(0x01, 0x04, 0), 0);      /* merging still works */
    MIDI_route_clear();

    printf("route: pushed sources\n");
    EXPECT_EQ(MIDI_route_sources(), MIDI_ROUTE_INPUT_SOURCES);
    MIDI_route_set_source(0x10, true);
    MIDI_route_set_source(0x18, true);                /* not one bit: ignored */
    EXPECT_EQ(MIDI_route_sources(), 0x17);
    EXPECT_EQ(MIDI_route_add(0x10, 0x08, MIDI_ROUTE_ALL_CHANNELS), 0);
    EXPECT_EQ(MIDI_route_add(0x10, 0x02, 9), 0);
    EXPECT_EQ(MIDI_route_targets(0x10, 0x9, 0x90), 0x08);
    EXPECT_EQ(MIDI_route_targets(0x10, 0x9, 0x99), 0x0A);
    EXPECT_EQ(MIDI_route_targets(0x01, 0x9, 0x99), 0);
    MIDI_route_set_source(0x10, false);
    EXPECT_EQ(MIDI_route_sources(), MIDI_ROUTE_INPUT_SOURCES);
    MIDI_route_clear();

    printf("filter: arguments\n");
    EXPECT_EQ(MIDI_route_set_filter(0x03, 0x08, 0x0300, -1, 0), -1);  /* source not one bit */
    EXPECT_EQ(MIDI_route_set_filter(0x01, 0x01, 0x0300, -1, 0), -1);  /* only itself */
    EXPECT_EQ(MIDI_route_set_filter(0x01, 0x08, 0, -1, 0), -1);       /* no types */
    EXPECT_EQ(MIDI_route_set_filter(0x01, 0x08, 0x0300, 16, 0), -1);  /* bad channel */

    printf("filter: no sink, nothing diverted\n");
    EXPECT_EQ(MIDI_route_add(0x01, 0x08 | 0x02, MIDI_ROUTE_ALL_CHANNELS), 0);
    EXPECT_EQ(MIDI_route_set_filter(0x01, 0x08, 0x0300, -1, MIDI_FILTER_OVERFLOW_BYPASS), 0);
    MIDI_route_packet(0x01, 0x9, 0x90, 60, 100);
    EXPECT_EQ(g_hits[3], 1); EXPECT_EQ(g_hits[1], 1);
    reset_hits();

    printf("filter: divert matching types only\n");
    MIDI_route_set_filter_sink(fake_sink);
    MIDI_route_packet(0x01, 0x9, 0x90, 60, 100);      /* note on: diverted for 0x08 */
    EXPECT_EQ(g_hits[3], 0); EXPECT_EQ(g_hits[1], 1);
    EXPECT_EQ(g_sunk_n, 1);
    EXPECT_EQ(g_sunk[0].src, 0x01); EXPECT_EQ(g_sunk[0].dst, 0x08);
    EXPECT_EQ(g_sunk[0].cin, 0x9);  EXPECT_EQ(g_sunk[0].b2, 60);
    MIDI_route_packet(0x01, 0xF, 0xF8, 0, 0);         /* clock: straight through */
    EXPECT_EQ(g_hits[3], 1); EXPECT_EQ(g_hits[1], 2);
    EXPECT_EQ(g_sunk_n, 1);
    EXPECT_EQ(MIDI_route_filtered(0x01), 0x08);
    reset_hits(); g_sunk_n = 0;

    printf("filter: channel\n");
    EXPECT_EQ(MIDI_route_set_filter(0x01, 0x08, 0x0300, 9, MIDI_FILTER_OVERFLOW_BYPASS), 0); /* replaces */
    MIDI_route_packet(0x01, 0x9, 0x90, 60, 100);      /* ch 0: direct */
    MIDI_route_packet(0x01, 0x9, 0x99, 36, 100);      /* ch 9: diverted */
    EXPECT_EQ(g_hits[3], 1); EXPECT_EQ(g_sunk_n, 1); EXPECT_EQ(g_sunk[0].b1, 0x99);
    reset_hits(); g_sunk_n = 0;

    printf("filter: Ruby listener needs no route\n");
    EXPECT_EQ(MIDI_route_set_filter(0x04, MIDI_ROUTE_DST_RUBY, 0xFFFF, -1, 0), 0);
    EXPECT_EQ(MIDI_route_from(0x04), 1);
    MIDI_route_packet(0x04, 0xB, 0xB0, 74, 1);
    EXPECT_EQ(g_sunk_n, 1); EXPECT_EQ(g_sunk[0].dst, MIDI_ROUTE_DST_RUBY);
    for (int i = 0; i < FAKES; i++) EXPECT_EQ(g_hits[i], 0);
    g_sunk_n = 0;

    printf("filter: listener and route filter share one event\n");
    EXPECT_EQ(MIDI_route_set_filter(0x01, MIDI_ROUTE_DST_RUBY, 0x0200, -1, 0), 0);
    MIDI_route_packet(0x01, 0x9, 0x99, 36, 100);
    EXPECT_EQ(g_sunk_n, 1); EXPECT_EQ(g_sunk[0].dst, 0x08 | MIDI_ROUTE_DST_RUBY);
    EXPECT_EQ(g_hits[1], 1);
    reset_hits(); g_sunk_n = 0;

    printf("filter: overflow bypass / drop\n");
    g_sunk_n = SINK_CAP;                              /* queue full */
    uint32_t before = MIDI_route_filter_overflows();
    MIDI_route_packet(0x01, 0x9, 0x99, 36, 100);      /* bypass: sent on unfiltered */
    EXPECT_EQ(g_hits[3], 1);
    EXPECT_EQ(MIDI_route_filter_overflows(), before + 1);
    EXPECT_EQ(MIDI_route_set_filter(0x01, 0x08, 0x0300, 9, MIDI_FILTER_OVERFLOW_DROP), 0);
    MIDI_route_packet(0x01, 0x9, 0x99, 36, 100);      /* drop */
    EXPECT_EQ(g_hits[3], 1);
    reset_hits(); g_sunk_n = 0;

    printf("filter: dispatch for pushed sources\n");
    MIDI_route_set_filter(0x10, 0x02, 0x0300, -1, 0);
    MIDI_route_dispatch(0x10, 0x02 | 0x08, 0x9, 0x90, 60, 100);
    EXPECT_EQ(g_hits[3], 1); EXPECT_EQ(g_hits[1], 0);
    EXPECT_EQ(g_sunk_n, 1); EXPECT_EQ(g_sunk[0].dst, 0x02);
    reset_hits(); g_sunk_n = 0;

    printf("filter: clear\n");
    EXPECT_EQ(MIDI_route_clear_filter(0x01, 0x08), 1);
    EXPECT_EQ(MIDI_route_filtered(0x01), MIDI_ROUTE_DST_RUBY);
    EXPECT_EQ(MIDI_route_clear_filter(0x01, MIDI_ROUTE_DST_ALL), 1);
    MIDI_route_packet(0x01, 0x9, 0x99, 36, 100);
    EXPECT_EQ(g_hits[3], 1); EXPECT_EQ(g_sunk_n, 0);
    MIDI_route_remove(0x01, MIDI_TMASK_ALL);
    EXPECT_EQ(MIDI_route_active(), 1);                /* filters from 0x04 / 0x10 */
    MIDI_route_clear();
    EXPECT_EQ(MIDI_route_active(), 0);
    EXPECT_EQ(MIDI_route_from(0x04), 0);
    reset_hits();

    printf("filter: table full changes nothing\n");
    for (int s = 0; s < 8; s++) {
        EXPECT_EQ(MIDI_route_set_filter((midi_tmask_t)(1 << s), MIDI_ROUTE_DST_RUBY, 0x0200, -1, 0), 0);
    }
    for (int s = 0; s < 7; s++) {
        EXPECT_EQ(MIDI_route_set_filter((midi_tmask_t)(1 << s), 0x8000, 0x0200, -1, 0), 0);
    }
    EXPECT_EQ(MIDI_route_set_filter(0x8000, 0x01, 0x0200, -1, 0), 0);  /* 16th slot */
    EXPECT_EQ(MIDI_route_set_filter(0x01, 0x02 | 0x04, 0x0200, -1, 0), -1);
    EXPECT_EQ(MIDI_route_filtered(0x01), 0x8000 | MIDI_ROUTE_DST_RUBY);  /* untouched */
    EXPECT_EQ(MIDI_route_set_filter(0x01, 0x8000, 0x0300, -1, 0), 0);    /* replacing still works */
    MIDI_route_clear();
    MIDI_route_set_filter_sink(NULL);

    printf("route: clock only\n");
    reset_hits();
    EXPECT_EQ(MIDI_route_add(0x01, 0x08, -3), -1);                  /* below CLOCK_ONLY */
    EXPECT_EQ(MIDI_route_add(0x04, 0x08, MIDI_ROUTE_CLOCK_ONLY), 0);
    MIDI_route_packet(0x04, 0xF, 0xF8, 0, 0);          /* clock */
    MIDI_route_packet(0x04, 0xF, 0xFA, 0, 0);          /* start */
    MIDI_route_packet(0x04, 0xF, 0xFC, 0, 0);          /* stop */
    MIDI_route_packet(0x04, 0x3, 0xF2, 0x10, 0x00);    /* song position */
    EXPECT_EQ(g_hits[3], 4);
    MIDI_route_packet(0x04, 0x9, 0x90, 60, 100);       /* note: not passed */
    MIDI_route_packet(0x04, 0xF, 0xFE, 0, 0);          /* active sensing: not passed */
    MIDI_route_packet(0x04, 0x4, 0xF0, 0x7E, 0x7F);    /* SysEx: not passed */
    EXPECT_EQ(g_hits[3], 4);
    EXPECT_EQ(MIDI_route_add(0x04, 0x02, 0), 0);       /* a channel route beside it */
    EXPECT_EQ(MIDI_route_targets(0x04, 0xF, 0xF8), 0x08);
    EXPECT_EQ(MIDI_route_targets(0x04, 0x9, 0x90), 0x02);
    MIDI_route_clear();
    reset_hits();

    printf("route: selections\n");
    reset_hits();
    EXPECT_EQ(MIDI_route_selection(MIDI_ROUTE_ALL_CHANNELS), MIDI_ROUTE_SEL_ALL);
    EXPECT_EQ(MIDI_route_selection(MIDI_ROUTE_CLOCK_ONLY), MIDI_ROUTE_SEL_CLOCK);
    EXPECT_EQ(MIDI_route_selection(9), 0x200);
    EXPECT_EQ(MIDI_route_selection(16), 0);
    EXPECT_EQ(MIDI_route_add_selection(0x01, 0x08, 0), -1);                       /* empty */
    EXPECT_EQ(MIDI_route_add_selection(0x01, 0x08, MIDI_ROUTE_SEL_ALL | 1), -1);  /* ALL is alone */
    EXPECT_EQ(MIDI_route_add_selection(0x01, 0x08, 1u << 18), -1);                /* unknown bit */
    uint32_t sel = 0x0001 | 0x0200 | MIDI_ROUTE_SEL_CLOCK;           /* channels 1, 10 + clock */
    EXPECT_EQ(MIDI_route_add_selection(0x01, 0x08, sel), 0);
    EXPECT_EQ(MIDI_route_add_selection(0x01, 0x02, sel), 0);          /* merged */
    {
        midi_tmask_t src = 0, dst = 0;
        uint32_t got = 0;
        int ch = 0;
        EXPECT_EQ(MIDI_route_get_selection(0, &src, &dst, &got), 1);
        EXPECT_EQ(dst, 0x0A);
        EXPECT_EQ(got, sel);
        EXPECT_EQ(MIDI_route_get(0, NULL, NULL, &ch), 1);
        EXPECT_EQ(ch, MIDI_ROUTE_SEVERAL);
        EXPECT_EQ(MIDI_route_get(1, NULL, NULL, NULL), 0);            /* one slot only */
    }
    EXPECT_EQ(MIDI_route_targets(0x01, 0x9, 0x90), 0x0A);  /* channel 1 */
    EXPECT_EQ(MIDI_route_targets(0x01, 0x9, 0x99), 0x0A);  /* channel 10 */
    EXPECT_EQ(MIDI_route_targets(0x01, 0x9, 0x91), 0);     /* channel 2 */
    EXPECT_EQ(MIDI_route_targets(0x01, 0xF, 0xF8), 0x0A);  /* clock */
    EXPECT_EQ(MIDI_route_targets(0x01, 0x4, 0xF0), 0);     /* SysEx */
    EXPECT_EQ(MIDI_route_channel(0x0200), 9);
    EXPECT_EQ(MIDI_route_channel(MIDI_ROUTE_SEL_ALL), MIDI_ROUTE_ALL_CHANNELS);
    EXPECT_EQ(MIDI_route_channel(MIDI_ROUTE_SEL_CLOCK), MIDI_ROUTE_CLOCK_ONLY);
    MIDI_route_clear();
    reset_hits();

    printf("watch\n");
    EXPECT_EQ(MIDI_route_active(), 0);
    MIDI_route_set_watch(MIDI_WATCH_APP, 0x04);
    MIDI_route_set_watch(MIDI_WATCH_SCRIPT, 0x02);
    MIDI_route_set_watch(5, 0x01);                    /* no such owner: ignored */
    EXPECT_EQ(MIDI_route_watched(), 0x06);
    EXPECT_EQ(MIDI_route_from(0x04), 1);
    EXPECT_EQ(MIDI_route_from(0x02), 1);
    EXPECT_EQ(MIDI_route_from(0x01), 0);
    EXPECT_EQ(MIDI_route_active(), 1);
    MIDI_route_packet(0x04, 0xF, 0xF8, 0, 0);         /* watched, not routed: sent nowhere */
    for (int i = 0; i < FAKES; i++) EXPECT_EQ(g_hits[i], 0);
    MIDI_route_clear();                               /* the script's go, the app's stay */
    EXPECT_EQ(MIDI_route_watched(), 0x04);
    MIDI_route_set_watch(MIDI_WATCH_APP, 0);
    EXPECT_EQ(MIDI_route_active(), 0);

    printf("virtual port\n");
    {
        /* Registered, so a send to its transport would show in g_hits[5] */
        EXPECT_EQ(MIDI_transport_register_at(0x20, &g_t[5]), 0);
        reset_hits(); g_sunk_n = 0;
        MIDI_route_set_filter_sink(fake_sink);
        MIDI_route_set_virtual(0x20);
        EXPECT_EQ(MIDI_route_virtual(), 0x20);
        MIDI_route_set_source(0x20, true);
        EXPECT_EQ(MIDI_route_add(0x20, 0x08, MIDI_ROUTE_ALL_CHANNELS), 0);   /* out: to 0x08 */
        EXPECT_EQ(MIDI_route_add(0x01, 0x20 | 0x02, MIDI_ROUTE_ALL_CHANNELS), 0);  /* in: from 0x01 */
        EXPECT_EQ(MIDI_route_add(0x20, 0x20, MIDI_ROUTE_ALL_CHANNELS), -1);  /* never to itself */
        MIDI_route_packet(0x20, 0x9, 0x90, 60, 100);                         /* the program's output */
        EXPECT_EQ(g_hits[3], 1); EXPECT_EQ(g_sunk_n, 0);
        MIDI_route_packet(0x01, 0x9, 0x90, 61, 100);    /* into the port: not listened to */
        EXPECT_EQ(g_hits[1], 1); EXPECT_EQ(g_hits[5], 0); EXPECT_EQ(g_sunk_n, 0);
        EXPECT_EQ(g_hits[3], 1);                        /* not forwarded through the port */
        MIDI_route_set_virtual_listen(0x0300);          /* note on / off */
        MIDI_route_packet(0x01, 0x9, 0x90, 62, 100);
        EXPECT_EQ(g_sunk_n, 1); EXPECT_EQ(g_sunk[0].dst, MIDI_ROUTE_DST_VIRTUAL);
        EXPECT_EQ(g_sunk[0].src, 0x01); EXPECT_EQ(g_sunk[0].b2, 62);
        EXPECT_EQ(g_hits[3], 1); EXPECT_EQ(g_hits[5], 0);
        MIDI_route_packet(0x01, 0xB, 0xB0, 1, 2);       /* a CC: not a listened type */
        EXPECT_EQ(g_sunk_n, 1);
        /* A filter on the route into the port: one event for both */
        EXPECT_EQ(MIDI_route_set_filter(0x01, 0x20, 0x0200, -1, 0), 0);
        MIDI_route_packet(0x01, 0x9, 0x90, 63, 100);
        EXPECT_EQ(g_sunk_n, 2); EXPECT_EQ(g_sunk[1].dst, 0x20);   /* diverted, not listened twice */
        g_sunk_n = SINK_CAP;                            /* full: bypass never reaches the port */
        MIDI_route_packet(0x01, 0x9, 0x90, 64, 100);
        EXPECT_EQ(g_hits[5], 0);
        MIDI_route_clear();
        EXPECT_EQ(MIDI_route_virtual(), 0x20);          /* the port stays, its listening goes */
        g_sunk_n = 0;
        EXPECT_EQ(MIDI_route_add(0x01, 0x20, MIDI_ROUTE_ALL_CHANNELS), 0);
        MIDI_route_packet(0x01, 0x9, 0x90, 65, 100);
        EXPECT_EQ(g_sunk_n, 0);
        MIDI_route_clear();
        MIDI_route_set_source(0x20, false);
        MIDI_route_set_virtual(0);
        MIDI_route_set_filter_sink(NULL);
        MIDI_transport_unregister(0x20);
    }

    printf("all route tests passed\n");
    return 0;
}

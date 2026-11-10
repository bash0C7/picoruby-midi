/*
 * PicoRuby MIDI - ESP32 High-Precision Clock Timer
 *
 * Uses esp_timer for accurate MIDI clock generation
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "../../include/midi.h"
#include "../../include/midi_parser.h"
#include "../../include/midi_scheduler.h"
#include "../../include/midi_clock_gen.h"
#include "../../include/midi_transport.h"
#include "../../include/midi_route.h"
#include "../../../picoruby-usb_midi_host/include/usb_midi_host.h"
#include "../../../picoruby-usb_midi_device/include/usb_midi_device.h"
#include "../../../picoruby-uart_midi/include/uart_midi.h"

/* Application-supplied cooperative-stop check. Defaults to NULL, meaning
 * "never stop on app request". The host application registers it via
 * MIDI_set_stop_check() at init time. */
static midi_stop_check_fn g_stop_check_fn = NULL;

void MIDI_set_stop_check(midi_stop_check_fn fn)
{
    g_stop_check_fn = fn;
}

static inline bool midi_stop_requested(void)
{
    return g_stop_check_fn ? g_stop_check_fn() : false;
}

static const char *TAG = "MIDI_CLOCK";

/* ========================================
 * Transport instances - wrap the existing per-gem APIs in the
 * picoruby-midi Transport interface. Static instances live here so the
 * port can route by transport_mask without picoruby-midi reaching back
 * into transport-specific symbols. Phase 5 of the standardization plan
 * will move ownership of these into each transport gem.
 * ======================================== */

static int usb_tx_send_packet(void *ctx, uint8_t cable, uint8_t cin,
                              uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)ctx;
    return USB_MIDI_HOST_send_packet(cable, cin, b1, b2, b3);
}

static int usb_tx_read_bytes(void *ctx, uint8_t *buf, size_t maxlen)
{
    (void)ctx;
    return USB_MIDI_HOST_read_packet(buf, maxlen);
}

static int usb_tx_bytes_available(void *ctx)
{
    (void)ctx;
    return USB_MIDI_HOST_bytes_available();
}

static bool usb_tx_is_connected(void *ctx)
{
    (void)ctx;
    return USB_MIDI_HOST_get_status() == USB_MIDI_HOST_CONNECTED;
}

static const midi_transport_ops_t g_usb_tx_ops = {
    .send_packet     = usb_tx_send_packet,
    .read_bytes      = usb_tx_read_bytes,
    .bytes_available = usb_tx_bytes_available,
    .is_connected    = usb_tx_is_connected,
    .transport_id    = MIDI_TRANSPORT_ID_USB,
};

/* Const so the transport struct lands in .rodata rather than .data and
 * does not perturb the internal-DRAM .bss layout (see CLAUDE.md). */
static const midi_transport_t g_usb_transport = {
    .ops = &g_usb_tx_ops,
    .ctx = NULL,
};

static int sam_tx_send_packet(void *ctx, uint8_t cable, uint8_t cin,
                              uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)ctx;
    return UART_MIDI_send_packet(cable, cin, b1, b2, b3);
}

static int sam_tx_read_bytes(void *ctx, uint8_t *buf, size_t maxlen)
{
    (void)ctx;
    return UART_MIDI_read_bytes(buf, maxlen);
}

static int sam_tx_bytes_available(void *ctx)
{
    (void)ctx;
    return UART_MIDI_bytes_available();
}

/* Up once the UART driver is installed: sending works from then on, and the
 * input task reads received bytes straight from the driver's RX buffer.
 * (This used to be UART_MIDI_Input_is_running(), so a UART only sending --
 * a synth on its TX -- read as disconnected, and MIDI_route_start_source()
 * never started reading one that was not reading yet.) */
static bool sam_tx_is_connected(void *ctx)
{
    (void)ctx;
    return UART_MIDI_get_status() == UART_MIDI_READY;
}

static const midi_transport_ops_t g_sam_tx_ops = {
    .send_packet     = sam_tx_send_packet,
    .read_bytes      = sam_tx_read_bytes,
    .bytes_available = sam_tx_bytes_available,
    .is_connected    = sam_tx_is_connected,
    .transport_id    = MIDI_TRANSPORT_ID_SERIAL,
};

/* Const for the same reason as g_usb_transport. */
static const midi_transport_t g_sam_transport = {
    .ops = &g_sam_tx_ops,
    .ctx = NULL,
};

/* USB-MIDI *device* transport (Tab5 USB-C, ESP32 appears as a MIDI
 * device to a host PC). On boards without CONFIG_USB_MIDI_BOARD_
 * M5STACK_TAB5 the USB_MIDI_DEVICE_* functions are link-time stubs
 * that report disconnected and fail sends, so this is safe everywhere. */
static int usbdev_tx_send_packet(void *ctx, uint8_t cable, uint8_t cin,
                                 uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)ctx;
    return USB_MIDI_DEVICE_send_packet(cable, cin, b1, b2, b3);
}

static int usbdev_tx_read_bytes(void *ctx, uint8_t *buf, size_t maxlen)
{
    (void)ctx;
    return USB_MIDI_DEVICE_read_packet(buf, maxlen);
}

static int usbdev_tx_bytes_available(void *ctx)
{
    (void)ctx;
    return USB_MIDI_DEVICE_bytes_available();
}

static bool usbdev_tx_is_connected(void *ctx)
{
    (void)ctx;
    return USB_MIDI_DEVICE_connected();
}

static const midi_transport_ops_t g_usbdev_tx_ops = {
    .send_packet     = usbdev_tx_send_packet,
    .read_bytes      = usbdev_tx_read_bytes,
    .bytes_available = usbdev_tx_bytes_available,
    .is_connected    = usbdev_tx_is_connected,
    .transport_id    = MIDI_TRANSPORT_ID_USB,
};

/* Const for the same reason as g_usb_transport. */
static const midi_transport_t g_usbdev_transport = {
    .ops = &g_usbdev_tx_ops,
    .ctx = NULL,
};

/* Put the three built-in transports into the registry under their
 * historical mask bits, before app_main and before any other transport
 * gem registers itself. Routing by mask (the note scheduler below,
 * application cleanup) then goes through MIDI_transport_send() and no
 * longer names a transport. */
__attribute__((constructor))
static void register_builtin_transports(void)
{
    MIDI_transport_register_at(MIDI_TRANSPORT_USB,        &g_usb_transport);
    MIDI_transport_register_at(MIDI_TRANSPORT_SAM2695,    &g_sam_transport);
    MIDI_transport_register_at(MIDI_TRANSPORT_USB_DEVICE, &g_usbdev_transport);
}

/* ========================================
 * The MIDI clock (one per process)
 *
 * A send-only transport of its own, and a route source that pushes its
 * output: MIDI.route(MIDI::Clock, dev) sends its clock to dev. An esp_timer
 * fires 24 times per quarter note from MIDI_Clock_init() on, and each tick
 * sends Timing Clock (F8) along the routes from the clock -- only while
 * there is one -- through MIDI_route_dispatch(), so filters and listeners on
 * it work too. Start / Continue / Stop go the same way; they do not stop the
 * F8s (receivers follow the tempo while stopped). The ticks are counted for
 * loops that run in step with the clock (MIDI.bpm_loop with output:).
 *
 * The tempo is the BPM set with MIDI_Clock_set_bpm(), or the one given to
 * MIDI_Clock_set_follow_bpm() while that is non-zero (an application
 * following an external clock).
 * ======================================== */

static esp_timer_handle_t g_clock_timer = NULL;
static midi_tmask_t g_clock_bit;
static float g_clock_follow_bpm;              /* 0: not following */
static uint64_t g_clock_period_us;            /* Period the timer runs at */
static volatile uint32_t g_clock_next_period_us; /* Period from the next tick on (0: none) */
static volatile bool g_clock_running;         /* Between Start and Stop */
static volatile uint32_t g_clock_count;       /* Ticks since init */
static volatile uint32_t g_clock_last_tick_us; /* Low 32 bits of the last tick's time */
static midi_clock_callback_t g_clock_user_cb;
static volatile uint32_t g_clock_song_start;  /* g_clock_count at Start, moved on by a Stop's ticks */
static volatile uint32_t g_clock_stopped_at;  /* g_clock_count at Stop */

static int clock_tx_send_packet(void *ctx, uint8_t cable, uint8_t cin,
                                uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)ctx; (void)cable; (void)cin; (void)b1; (void)b2; (void)b3;
    return 0;  /* Nothing is sent to the clock */
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

/* Const for the same reason as g_usb_transport. */
static const midi_transport_t g_clock_transport = {
    .ops = &g_clock_tx_ops,
    .ctx = NULL,
};

/* After the built-ins (constructor priority is not needed: the registry is
 * plain static data), so the clock takes the first dynamic bit. */
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
    midi_tmask_t dst = MIDI_route_targets(bit, USB_MIDI_HOST_CIN_SINGLE_BYTE, status);
    MIDI_route_dispatch(bit, dst, USB_MIDI_HOST_CIN_SINGLE_BYTE, status, 0, 0);
}

/* esp_timer task */
static void clock_timer_callback(void *arg)
{
    (void)arg;
    g_clock_last_tick_us = (uint32_t)esp_timer_get_time();
    g_clock_count++;
    /* A new tempo starts here, a whole period after this tick */
    uint32_t next = __atomic_exchange_n(&g_clock_next_period_us, 0, __ATOMIC_RELAXED);
    if (next) {
        g_clock_period_us = next;
        esp_timer_restart(g_clock_timer, next);
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

/* Run the timer at the current tempo's period, from the next tick on.
 * Restarting the timer here would start the period afresh from now and
 * drop the time since the last tick: in Sync mode the follow BPM changes
 * several times a second, and each change stretched one clock by up to a
 * whole period (a receiver saw the tempo drag and wobble). */
static void clock_retime(void)
{
    if (g_clock_timer == NULL) return;
    uint32_t period = (uint32_t)clock_period_us(clock_current_bpm());
    __atomic_store_n(&g_clock_next_period_us,
                     period == g_clock_period_us ? 0 : period, __ATOMIC_RELAXED);
}

int MIDI_Clock_init(void)
{
    if (g_clock_timer != NULL) {
        return 0;  /* Already initialized */
    }

    esp_timer_create_args_t timer_args = {
        .callback = clock_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "midi_clock",
        .skip_unhandled_events = true
    };

    esp_err_t ret = esp_timer_create(&timer_args, &g_clock_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create timer: %d", ret);
        g_clock_timer = NULL;
        return -1;
    }
    g_clock_period_us = clock_period_us(clock_current_bpm());
    g_clock_next_period_us = 0;
    g_clock_last_tick_us = (uint32_t)esp_timer_get_time();
    esp_timer_start_periodic(g_clock_timer, g_clock_period_us);

    ESP_LOGI(TAG, "Clock initialized, BPM=%.1f, period=%llu us",
             clock_current_bpm(), g_clock_period_us);

    return 0;
}

void MIDI_Clock_deinit(void)
{
    if (g_clock_timer != NULL) {
        esp_timer_stop(g_clock_timer);
        esp_timer_delete(g_clock_timer);
        g_clock_timer = NULL;
    }
    g_clock_period_us = 0;
    g_clock_next_period_us = 0;
    g_clock_running = false;
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
    /* The first clock after Start marks the downbeat: start the period afresh
     * (at the new tempo, if one is waiting for the next tick) */
    uint32_t next = __atomic_exchange_n(&g_clock_next_period_us, 0, __ATOMIC_RELAXED);
    if (next) g_clock_period_us = next;
    esp_timer_restart(g_clock_timer, g_clock_period_us);
    g_clock_last_tick_us = (uint32_t)esp_timer_get_time();
    g_clock_song_start = g_clock_count;
}

void MIDI_Clock_continue(void)
{
    if (MIDI_Clock_init() != 0) return;
    g_clock_song_start += g_clock_count - g_clock_stopped_at;
    g_clock_running = true;
    clock_send(MIDI_STATUS_CONTINUE);
}

void MIDI_Clock_stop(void)
{
    g_clock_running = false;
    g_clock_stopped_at = g_clock_count;
    clock_send(MIDI_STATUS_STOP);
}

bool MIDI_Clock_position(int32_t *tick, int32_t *age_us, float *period_us)
{
    if (g_clock_timer == NULL || !g_clock_running) return false;
    int32_t position = (int32_t)(g_clock_count - g_clock_song_start) - 1;
    if (position < 0) return false;
    *tick = position;
    *age_us = (int32_t)((uint32_t)esp_timer_get_time() - g_clock_last_tick_us);
    *period_us = (float)g_clock_period_us;
    return true;
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
    uint32_t elapsed = (uint32_t)esp_timer_get_time() - g_clock_last_tick_us;
    uint32_t period = (uint32_t)g_clock_period_us;
    return elapsed >= period ? 0 : period - elapsed;
}

void MIDI_Clock_set_callback(midi_clock_callback_t callback)
{
    g_clock_user_cb = callback;
}

/* ========================================
 * MIDI Input Background Task Implementation
 * ======================================== */

static const char *INPUT_TAG = "MIDI_INPUT";

/* Event queues - separate queues for USB and SAM2695 */
#define MIDI_EVENT_QUEUE_SIZE 256
static QueueHandle_t g_usb_event_queue = NULL;
static QueueHandle_t g_sam_event_queue = NULL;

/* The input task feeds two consumers: the event queues that MIDI::Input
 * pops from Ruby, and the routes (MIDI.route) it forwards to in C. It runs
 * while either wants it. Events are only queued while a MIDI::Input is
 * reading (set by the bindings); with routes alone nobody would drain the
 * queues. */
static volatile bool g_queue_events = false;

/* Above the PicoRuby VM task (3) on the same core, so routed MIDI is not
 * held up while a script computes. The task sleeps at least one tick every
 * pass (see MIDI_INPUT_POLL_MS), so this costs the VM next to nothing. */
#ifndef MIDI_INPUT_TASK_PRIORITY
#define MIDI_INPUT_TASK_PRIORITY 4
#endif

/* Pause between passes over the input sources. Rounded up to one tick (10 ms
 * at the usual 100 Hz), which is then also the worst-case extra latency of a
 * routed message. */
#ifndef MIDI_INPUT_POLL_MS
#define MIDI_INPUT_POLL_MS 5
#endif

/* Task handle */
static TaskHandle_t g_input_task = NULL;
/* MIDI_Input_kick: wakes the task's wait between passes. A semaphore, not a
 * task notification, so that it outlives the task (which ends and starts
 * again with the sources). */
static SemaphoreHandle_t g_input_wake = NULL;
static volatile bool g_input_running = false;
static volatile bool g_input_was_started = false;  /* Track if input was ever started (for auto-restart) */

/* Per-source MIDI parsers (allocated in MIDI_Input_init).
 * The parser layer is OS-free and lives in src/midi_parser.c.
 */
static midi_parser_t *g_usb_parser = NULL;
static midi_parser_t *g_sam_parser = NULL;
/* Only for the external clock of what the PC sends (the USB device is read
 * for routes and filters, never queued for MIDI::Input). */
static midi_parser_t *g_usbdev_parser = NULL;

/* A parsed event back as a USB-MIDI packet, for routing what arrived as
 * raw UART bytes. Returns false for what is not forwarded (SysEx). */
static bool event_to_packet(const midi_event_t *e, uint8_t *cin,
                            uint8_t *b1, uint8_t *b2, uint8_t *b3)
{
    uint8_t ch = e->channel & 0x0F;
    *b2 = 0;
    *b3 = 0;
    switch (e->type) {
    case MIDI_EVENT_NOTE_ON:         *cin = 0x9; *b1 = 0x90 | ch; *b2 = e->data1; *b3 = e->data2; return true;
    case MIDI_EVENT_NOTE_OFF:        *cin = 0x8; *b1 = 0x80 | ch; *b2 = e->data1; *b3 = e->data2; return true;
    case MIDI_EVENT_POLY_AFTERTOUCH: *cin = 0xA; *b1 = 0xA0 | ch; *b2 = e->data1; *b3 = e->data2; return true;
    case MIDI_EVENT_CONTROL_CHANGE:  *cin = 0xB; *b1 = 0xB0 | ch; *b2 = e->data1; *b3 = e->data2; return true;
    case MIDI_EVENT_PROGRAM_CHANGE:  *cin = 0xC; *b1 = 0xC0 | ch; *b2 = e->data1; return true;
    case MIDI_EVENT_CHANNEL_PRESSURE:*cin = 0xD; *b1 = 0xD0 | ch; *b2 = e->data1; return true;
    case MIDI_EVENT_PITCH_BEND: {
        int v = e->value + 8192;
        if (v < 0) v = 0;
        if (v > 16383) v = 16383;
        *cin = 0xE; *b1 = 0xE0 | ch; *b2 = v & 0x7F; *b3 = (v >> 7) & 0x7F;
        return true;
    }
    case MIDI_EVENT_CLOCK:          *cin = 0xF; *b1 = 0xF8; return true;
    case MIDI_EVENT_START:          *cin = 0xF; *b1 = 0xFA; return true;
    case MIDI_EVENT_CONTINUE:       *cin = 0xF; *b1 = 0xFB; return true;
    case MIDI_EVENT_STOP:           *cin = 0xF; *b1 = 0xFC; return true;
    case MIDI_EVENT_ACTIVE_SENSING: *cin = 0xF; *b1 = 0xFE; return true;
    case MIDI_EVENT_SYSTEM_RESET:   *cin = 0xF; *b1 = 0xFF; return true;
    default:                        return false;
    }
}

/* Hand a parsed event to MIDI::Input's queue, if one is reading, else
 * free it. */
static void queue_event(QueueHandle_t q, midi_event_t *event, const char *name)
{
    if (g_queue_events && xQueueSend(q, event, 0) == pdTRUE) {
        return;
    }
    if (g_queue_events) {
        ESP_LOGW(INPUT_TAG, "%s event queue full, dropping event", name);
    }
    if (event->sysex_data) {
        free(event->sysex_data);
        event->sysex_data = NULL;
    }
}

/* The USB-MIDI device (what the PC sends) is read only for routing, and only
 * while a route starts from it: otherwise its packets stay in the device's
 * ring buffer for USB_MIDI_DEVICE#read_available. Not tied to enumeration,
 * so a route set before the PC opens the port works once it does. */
static inline bool usbdev_input_wanted(void)
{
    return MIDI_route_from(MIDI_TRANSPORT_USB_DEVICE);
}

/* The UART is read when MIDI::Input started its input (as before), or when a
 * route or filter starts from it. Otherwise received bytes stay in the
 * driver's buffer for UART_MIDI#read_available. */
static inline bool sam_input_wanted(void)
{
    return g_sam_transport.ops->is_connected(g_sam_transport.ctx)
        && (UART_MIDI_Input_is_running() || MIDI_route_from(MIDI_TRANSPORT_SAM2695));
}

/* Further transports read like the USB host (MIDI_Input_add_packet_source):
 * e.g. the second and later devices on a USB hub. Read only while connected
 * and routed from, like the USB device. */
static volatile midi_tmask_t g_packet_sources = 0;

void MIDI_Input_add_packet_source(midi_tmask_t bit)
{
    if (bit == 0 || (bit & (bit - 1)) != 0) return;
    g_packet_sources = g_packet_sources | bit;
    MIDI_route_set_source(bit, true);
}

/* External clock tracking (BPM) for packet sources: a parser per source
 * asked for (MIDI_Input_track_clock), fed by the input task. */
#define MIDI_CLOCK_TRACKED_MAX 4
static struct {
    volatile midi_tmask_t bit;
    midi_parser_t *parser;
} g_tracked[MIDI_CLOCK_TRACKED_MAX];

void MIDI_Input_kick(void)
{
    SemaphoreHandle_t w = g_input_wake;
    if (w && g_input_running) xSemaphoreGive(w);
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

/* Background task that reads from USB-MIDI and SAM2695, forwards along the
 * routes and queues events */
static void midi_input_task(void *arg)
{
    /* Large buffer to drain RX buffer completely each iteration */
    uint8_t buffer[256];
    midi_event_t event;
    bool usb_midi_available = false;
    bool sam2695_available = false;
    bool usbdev_available = false;
    midi_tmask_t packet_sources = 0;

    ESP_LOGI(INPUT_TAG, "Input task started");

    while (g_input_running) {
        /* Check if the application has requested a stop */
        if (midi_stop_requested()) {
            ESP_LOGI(INPUT_TAG, "Stop requested, exiting input task");
            break;
        }

        /* Check available input sources via the Transport interface */
        usb_midi_available = g_usb_transport.ops->is_connected(g_usb_transport.ctx);
        sam2695_available  = sam_input_wanted();
        usbdev_available   = usbdev_input_wanted();
        packet_sources     = packet_sources_wanted();

        /* Exit if no input sources available */
        if (!usb_midi_available && !sam2695_available && !usbdev_available && !packet_sources) {
            ESP_LOGI(INPUT_TAG, "No input sources available, exiting input task");
            break;
        }

        /* Process USB-MIDI input */
        if (usb_midi_available) {
            int bytes_avail = g_usb_transport.ops->bytes_available(g_usb_transport.ctx);

            while (bytes_avail >= 4) {
                int read_len = g_usb_transport.ops->read_bytes(g_usb_transport.ctx,
                                                               buffer, sizeof(buffer));
                ESP_LOGD(INPUT_TAG, "Read %d bytes from USB MIDI", read_len);

                if (read_len <= 0) break;

                /* Parse packets */
                for (int i = 0; i + 3 < read_len; i += 4) {
                    uint8_t cin = buffer[i] & 0x0F;
                    uint8_t midi1 = buffer[i + 1];
                    uint8_t midi2 = buffer[i + 2];
                    uint8_t midi3 = buffer[i + 3];

                    ESP_LOGD(INPUT_TAG, "USB Packet[%d]: cin=0x%02X m1=0x%02X m2=0x%02X m3=0x%02X",
                             i/4, cin, midi1, midi2, midi3);

                    /* Routes first, and as the packet arrived (SysEx
                     * included): the lowest-latency path. */
                    MIDI_route_packet(MIDI_TRANSPORT_USB, cin, midi1, midi2, midi3);

                    if (midi_parser_feed_usb(g_usb_parser, cin, midi1, midi2, midi3,
                                             (uint64_t)esp_timer_get_time(), &event)) {
                        ESP_LOGD(INPUT_TAG, "USB event type=%d ch=%d d1=%d d2=%d",
                                 event.type, event.channel, event.data1, event.data2);
                        queue_event(g_usb_event_queue, &event, "USB");
                    }
                }

                bytes_avail = g_usb_transport.ops->bytes_available(g_usb_transport.ctx);
            }
        }

        /* Process SAM2695 input */
        if (sam2695_available) {
            int bytes_avail = g_sam_transport.ops->bytes_available(g_sam_transport.ctx);

            if (bytes_avail > 0) {
                int read_len = g_sam_transport.ops->read_bytes(g_sam_transport.ctx,
                                                               buffer, sizeof(buffer));
                ESP_LOGD(INPUT_TAG, "Read %d bytes from SAM2695", read_len);

                if (read_len > 0) {
                    /* Parse raw MIDI bytes */
                    for (int i = 0; i < read_len; i++) {
                        ESP_LOGD(INPUT_TAG, "SAM2695 byte[%d]: 0x%02X", i, buffer[i]);

                        if (midi_parser_feed_raw(g_sam_parser, buffer[i],
                                                 (uint64_t)esp_timer_get_time(), &event)) {
                            ESP_LOGD(INPUT_TAG, "SAM2695 event type=%d ch=%d d1=%d d2=%d",
                                     event.type, event.channel, event.data1, event.data2);
                            uint8_t rcin, rb1, rb2, rb3;
                            if (event_to_packet(&event, &rcin, &rb1, &rb2, &rb3)) {
                                MIDI_route_packet(MIDI_TRANSPORT_SAM2695, rcin, rb1, rb2, rb3);
                            }
                            queue_event(g_sam_event_queue, &event, "SAM");
                        }
                    }
                }
            }
        }

        /* Process USB-MIDI device input (routes only; see usbdev_input_wanted) */
        if (usbdev_available) {
            while (g_usbdev_transport.ops->bytes_available(g_usbdev_transport.ctx) >= 4) {
                int read_len = g_usbdev_transport.ops->read_bytes(g_usbdev_transport.ctx,
                                                                  buffer, sizeof(buffer));
                if (read_len <= 0) break;
                for (int i = 0; i + 3 < read_len; i += 4) {
                    MIDI_route_packet(MIDI_TRANSPORT_USB_DEVICE, buffer[i] & 0x0F,
                                      buffer[i + 1], buffer[i + 2], buffer[i + 3]);
                    if (midi_parser_feed_usb(g_usbdev_parser, buffer[i] & 0x0F,
                                             buffer[i + 1], buffer[i + 2], buffer[i + 3],
                                             (uint64_t)esp_timer_get_time(), &event)
                        && event.sysex_data) {
                        free(event.sysex_data);   /* parsed for the clock only */
                    }
                }
            }
        }

        /* Process the further packet sources (routes only) */
        for (midi_tmask_t bit = 1; packet_sources && bit; bit <<= 1) {
            if (!(packet_sources & bit)) continue;
            const midi_transport_t *t = MIDI_transport_get(bit);
            if (!t) continue;
            while (t->ops->bytes_available(t->ctx) >= 4) {
                int read_len = t->ops->read_bytes(t->ctx, buffer, sizeof(buffer));
                if (read_len <= 0) break;
                midi_parser_t *clock = tracked_parser(bit);
                for (int i = 0; i + 3 < read_len; i += 4) {
                    MIDI_route_packet(bit, buffer[i] & 0x0F,
                                      buffer[i + 1], buffer[i + 2], buffer[i + 3]);
                    if (clock && midi_parser_feed_usb(clock, buffer[i] & 0x0F,
                                                      buffer[i + 1], buffer[i + 2], buffer[i + 3],
                                                      (uint64_t)esp_timer_get_time(), &event)
                        && event.sysex_data) {
                        free(event.sysex_data);   /* parsed for the clock only */
                    }
                }
            }
        }

        /* Yield the CPU until the next pass. At least one tick: with the
         * usual 100 Hz tick, pdMS_TO_TICKS(5) is 0, and vTaskDelay(0) does
         * not block -- the task then spins. That went unnoticed while it ran
         * at priority 1, but above the Ruby VM it starved the VM (no
         * UI.process, no scripts) for as long as a USB device was plugged in. */
        TickType_t wait = pdMS_TO_TICKS(MIDI_INPUT_POLL_MS);
        if (wait == 0) wait = 1;
        if (g_input_wake) xSemaphoreTake(g_input_wake, wait);
        else vTaskDelay(wait);
    }

    /* Mark as not running before cleanup */
    g_input_running = false;

    /* Reset parsers (running status, SysEx, BPM tracker) */
    midi_parser_reset(g_usb_parser);
    midi_parser_reset(g_sam_parser);
    midi_parser_reset(g_usbdev_parser);

    ESP_LOGI(INPUT_TAG, "Input task stopped");
    vTaskDelete(NULL);
}

/*
 * Initialize input task
 */
int MIDI_Input_init(void)
{
    if (g_usb_event_queue != NULL && g_sam_event_queue != NULL
        && g_usb_parser != NULL && g_sam_parser != NULL && g_usbdev_parser != NULL) {
        return 0;  /* Already initialized */
    }

    /* Ensure DEBUG level logging is enabled for MIDI_INPUT */
    esp_log_level_set(INPUT_TAG, ESP_LOG_DEBUG);

    /* Create USB event queue */
    if (g_usb_event_queue == NULL) {
        g_usb_event_queue = xQueueCreate(MIDI_EVENT_QUEUE_SIZE, sizeof(midi_event_t));
        if (g_usb_event_queue == NULL) {
            ESP_LOGE(INPUT_TAG, "Failed to create USB event queue");
            return -1;
        }
    }

    /* Create SAM2695 event queue */
    if (g_sam_event_queue == NULL) {
        g_sam_event_queue = xQueueCreate(MIDI_EVENT_QUEUE_SIZE, sizeof(midi_event_t));
        if (g_sam_event_queue == NULL) {
            ESP_LOGE(INPUT_TAG, "Failed to create SAM event queue");
            vQueueDelete(g_usb_event_queue);
            g_usb_event_queue = NULL;
            return -1;
        }
    }

    /* Create parsers */
    if (g_usb_parser == NULL) {
        g_usb_parser = midi_parser_new(MIDI_SOURCE_USB);
        if (g_usb_parser == NULL) {
            ESP_LOGE(INPUT_TAG, "Failed to allocate USB parser");
            return -1;
        }
    }
    if (g_sam_parser == NULL) {
        g_sam_parser = midi_parser_new(MIDI_SOURCE_SAM2695);
        if (g_sam_parser == NULL) {
            ESP_LOGE(INPUT_TAG, "Failed to allocate SAM parser");
            midi_parser_free(g_usb_parser);
            g_usb_parser = NULL;
            return -1;
        }
    }
    if (g_usbdev_parser == NULL) {
        g_usbdev_parser = midi_parser_new(MIDI_SOURCE_USB);
        if (g_usbdev_parser == NULL) {
            ESP_LOGE(INPUT_TAG, "Failed to allocate USB device parser");
            return -1;
        }
    }

    ESP_LOGI(INPUT_TAG, "Input initialized (USB and SAM2695 queues + parsers)");
    return 0;
}

/*
 * Deinitialize input task
 */
void MIDI_Input_deinit(void)
{
    MIDI_Input_stop();

    if (g_usb_event_queue != NULL) {
        vQueueDelete(g_usb_event_queue);
        g_usb_event_queue = NULL;
    }

    if (g_sam_event_queue != NULL) {
        vQueueDelete(g_sam_event_queue);
        g_sam_event_queue = NULL;
    }

    if (g_usb_parser != NULL) {
        midi_parser_free(g_usb_parser);
        g_usb_parser = NULL;
    }
    if (g_sam_parser != NULL) {
        midi_parser_free(g_sam_parser);
        g_sam_parser = NULL;
    }
    if (g_usbdev_parser != NULL) {
        midi_parser_free(g_usbdev_parser);
        g_usbdev_parser = NULL;
    }
}

/*
 * Start background processing task
 */
int MIDI_Input_start(void)
{
    if (MIDI_Input_init() != 0) {
        return -1;
    }

    if (g_input_running) {
        return 0;  /* Already running */
    }

    /* Check if at least one input source is available via the Transport interface */
    bool usb_midi_available = g_usb_transport.ops->is_connected(g_usb_transport.ctx);
    bool sam2695_available  = sam_input_wanted();
    bool usbdev_available   = usbdev_input_wanted();
    midi_tmask_t packet_sources = packet_sources_wanted();

    ESP_LOGI(INPUT_TAG, "Starting input task - USB MIDI: %s, SAM2695: %s, USB device: %s, others: 0x%04X",
             usb_midi_available ? "available" : "not available",
             sam2695_available ? "available" : "not available",
             usbdev_available ? "routed" : "not routed",
             (unsigned)packet_sources);

    if (!usb_midi_available && !sam2695_available && !usbdev_available && !packet_sources) {
        ESP_LOGW(INPUT_TAG, "Cannot start input task: no input sources available");
        return -1;
    }

    /* Reset parsers (running status, SysEx, BPM tracker) */
    midi_parser_reset(g_usb_parser);
    midi_parser_reset(g_sam_parser);
    midi_parser_reset(g_usbdev_parser);

    if (g_input_wake == NULL) g_input_wake = xSemaphoreCreateBinary();

    g_input_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        midi_input_task,
        "midi_input",
        4096,  /* Increased stack size */
        NULL,
        MIDI_INPUT_TASK_PRIORITY,
        &g_input_task,
        1   /* Core 1, same as PicoRuby (consumer of events) */
    );

    if (ret != pdTRUE) {
        ESP_LOGE(INPUT_TAG, "Failed to create input task");
        g_input_running = false;
        return -1;
    }

    g_input_was_started = true;  /* Mark that input task was started (for auto-restart on reconnect) */
    ESP_LOGI(INPUT_TAG, "Input task started successfully");
    return 0;
}

/*
 * Check if input task was ever started (for auto-restart logic)
 */
bool MIDI_Input_was_started(void)
{
    return g_input_was_started;
}

/*
 * Drain a queue, freeing any SysEx payloads to avoid leaks.
 */
static void drain_event_queue(QueueHandle_t q)
{
    if (q == NULL) return;
    midi_event_t event;
    while (xQueueReceive(q, &event, 0) == pdTRUE) {
        if (event.sysex_data) {
            free(event.sysex_data);
        }
    }
}

/*
 * Stop background processing task
 */
void MIDI_Input_set_queueing(bool on)
{
    g_queue_events = on;
}

int MIDI_Input_start_routing(void)
{
    /* Mark the task as wanted even if no source is connected yet: the
     * USB host driver restarts it on (re)connect when this is set. */
    g_input_was_started = true;
    return MIDI_Input_start();
}

int MIDI_route_start_source(midi_tmask_t src_bit)
{
    if ((src_bit & (MIDI_ROUTE_INPUT_SOURCES | g_packet_sources)) == 0) return 0;
    /* The UART's own task keeps its RX buffer from overflowing; it needs the
     * driver installed (MIDIDevices.sam2695) and an RX pin. */
    if (src_bit == MIDI_TRANSPORT_SAM2695
        && UART_MIDI_get_status() == UART_MIDI_READY
        && !UART_MIDI_Input_is_running()) {
        UART_MIDI_Input_start();
    }
    return MIDI_Input_start_routing();
}

void MIDI_Input_stop(void)
{
    if (!g_input_running) {
        return;
    }

    /* Routes still need the task: only empty the queues MIDI::Input no
     * longer reads. */
    if (MIDI_route_active()) {
        drain_event_queue(g_usb_event_queue);
        drain_event_queue(g_sam_event_queue);
        return;
    }

    g_input_running = false;

    /* Wait for task to finish */
    vTaskDelay(pdMS_TO_TICKS(50));
    g_input_task = NULL;

    /* Drain queues (frees SysEx buffers), then reset to clear any stale state */
    drain_event_queue(g_usb_event_queue);
    drain_event_queue(g_sam_event_queue);
    if (g_usb_event_queue != NULL) xQueueReset(g_usb_event_queue);
    if (g_sam_event_queue != NULL) xQueueReset(g_sam_event_queue);

    /* Reset parsers so next run starts cleanly */
    midi_parser_reset(g_usb_parser);
    midi_parser_reset(g_sam_parser);

    ESP_LOGI(INPUT_TAG, "Input task stopped");
}

/*
 * Check if input task is running
 */
bool MIDI_Input_is_running(void)
{
    return g_input_running;
}

/*
 * Get number of events available (legacy - uses USB queue)
 */
int MIDI_Input_events_available(void)
{
    return MIDI_Input_events_available_usb();
}

/*
 * Get number of events available from USB queue
 */
int MIDI_Input_events_available_usb(void)
{
    if (g_usb_event_queue == NULL) {
        return 0;
    }
    return (int)uxQueueMessagesWaiting(g_usb_event_queue);
}

/*
 * Get number of events available from SAM2695 queue
 */
int MIDI_Input_events_available_sam(void)
{
    if (g_sam_event_queue == NULL) {
        return 0;
    }
    return (int)uxQueueMessagesWaiting(g_sam_event_queue);
}

/*
 * Pop event from queue (legacy - uses USB queue)
 */
bool MIDI_Input_pop_event(midi_event_t *event)
{
    return MIDI_Input_pop_event_usb(event);
}

/*
 * Pop event from USB queue
 */
bool MIDI_Input_pop_event_usb(midi_event_t *event)
{
    if (g_usb_event_queue == NULL || event == NULL) {
        return false;
    }
    return xQueueReceive(g_usb_event_queue, event, 0) == pdTRUE;
}

/*
 * Pop event from SAM2695 queue
 */
bool MIDI_Input_pop_event_sam(midi_event_t *event)
{
    if (g_sam_event_queue == NULL || event == NULL) {
        return false;
    }
    return xQueueReceive(g_sam_event_queue, event, 0) == pdTRUE;
}

/*
 * Get external BPM calculated from incoming MIDI clock (USB-MIDI)
 * Returns 0.0 if not enough data
 */
float MIDI_Input_get_external_bpm_usb(void)
{
    return midi_parser_get_external_bpm(g_usb_parser);
}

/*
 * Get external BPM calculated from incoming MIDI clock (SAM2695/MIDI-DIN)
 * Returns 0.0 if not enough data
 */
float MIDI_Input_get_external_bpm_sam(void)
{
    return midi_parser_get_external_bpm(g_sam_parser);
}

/*
 * Get external BPM calculated from incoming MIDI clock (legacy - returns USB BPM)
 * Returns 0.0 if not enough data
 */
float MIDI_Input_get_external_bpm(void)
{
    return midi_parser_get_external_bpm(g_usb_parser);
}

/*
 * Reset external clock tracking for USB-MIDI (call on MIDI Start)
 */
void MIDI_Input_reset_external_clock_usb(void)
{
    midi_parser_reset_external_clock(g_usb_parser);
}

/*
 * Reset external clock tracking for SAM2695 (call on MIDI Start)
 */
void MIDI_Input_reset_external_clock_sam(void)
{
    midi_parser_reset_external_clock(g_sam_parser);
}

/*
 * Reset external clock tracking (legacy - resets both)
 */
void MIDI_Input_reset_external_clock(void)
{
    MIDI_Input_reset_external_clock_usb();
    MIDI_Input_reset_external_clock_sam();
}

static midi_parser_t *parser_for(midi_tmask_t src_bit)
{
    switch (src_bit) {
    case MIDI_TRANSPORT_USB:        return g_usb_parser;
    case MIDI_TRANSPORT_SAM2695:    return g_sam_parser;
    case MIDI_TRANSPORT_USB_DEVICE: return g_usbdev_parser;
    default:                        return tracked_parser(src_bit);
    }
}

float MIDI_Input_get_external_bpm_from(midi_tmask_t src_bit)
{
    return midi_parser_get_external_bpm(parser_for(src_bit));  /* NULL-safe */
}

void MIDI_Input_reset_external_clock_from(midi_tmask_t src_bit)
{
    midi_parser_t *p = parser_for(src_bit);
    if (p) midi_parser_reset_external_clock(p);
}

bool MIDI_Input_get_external_clock_from(midi_tmask_t src_bit, int32_t *tick,
                                        int32_t *age_us, float *period_us)
{
    midi_parser_clock_t c;
    if (!midi_parser_get_external_clock(parser_for(src_bit), &c) || !c.running || c.tick < 0) {
        return false;
    }
    *tick = c.tick;
    *age_us = (int32_t)(esp_timer_get_time() - c.at_us);
    *period_us = c.period_us;
    return true;
}

/* ========================================
 * Filter queue (MIDI.filter / MIDI.on)
 * ======================================== */

/* Each entry is 12 bytes. Deep enough for a chord or a burst of CCs while the
 * script is between MIDI.sleep_ms / UI.process calls. */
#ifndef MIDI_FILTER_QUEUE_SIZE
#define MIDI_FILTER_QUEUE_SIZE 64
#endif

static QueueHandle_t g_filter_queue = NULL;

/* Runs on whatever task forwards the packet (input task, UI task): never
 * blocks. */
static bool filter_sink(const midi_filter_event_t *ev)
{
    return xQueueSend(g_filter_queue, ev, 0) == pdTRUE;
}

int MIDI_Filter_init(void)
{
    if (g_filter_queue != NULL) return 0;
    g_filter_queue = xQueueCreate(MIDI_FILTER_QUEUE_SIZE, sizeof(midi_filter_event_t));
    if (g_filter_queue == NULL) {
        ESP_LOGE(INPUT_TAG, "Failed to create filter queue");
        return -1;
    }
    MIDI_route_set_filter_sink(filter_sink);
    return 0;
}

bool MIDI_Filter_pop(uint32_t *dst, midi_tmask_t *src, uint8_t *cin,
                     uint8_t *b1, uint8_t *b2, uint8_t *b3)
{
    midi_filter_event_t ev;
    if (g_filter_queue == NULL || xQueueReceive(g_filter_queue, &ev, 0) != pdTRUE) {
        return false;
    }
    *dst = ev.dst; *src = ev.src; *cin = ev.cin;
    *b1 = ev.b1; *b2 = ev.b2; *b3 = ev.b3;
    return true;
}

bool MIDI_Filter_wait(uint32_t timeout_ms)
{
    midi_filter_event_t ev;
    if (g_filter_queue == NULL) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return false;
    }

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (ticks == 0 && timeout_ms > 0) ticks = 1;   /* < 1 tick at 100 Hz would not block */
    return xQueuePeek(g_filter_queue, &ev, ticks) == pdTRUE;
}

void MIDI_Filter_flush(void)
{
    if (g_filter_queue != NULL) xQueueReset(g_filter_queue);
}

/* ========================================
 * Note Scheduler Port - drives the OS-free scheduler core in
 * src/midi_scheduler.c via an esp_timer and routes its send callback
 * through the transport registry.
 * ======================================== */

static const char *SCHED_TAG = "NOTE_SCHED";

static esp_timer_handle_t g_note_scheduler_timer = NULL;
static volatile bool g_scheduler_running = false;

/* Timer tick interval (check every 1ms for responsive note-offs) */
#define NOTE_SCHEDULER_TICK_US 1000

/* Send callback registered with the scheduler core: routes a single
 * 3-byte channel-voice message to every registered transport selected
 * by transport_mask.
 */
static void scheduler_send_packet(midi_tmask_t transport_mask, uint8_t cin,
                                  uint8_t status, uint8_t data1, uint8_t data2)
{
    MIDI_transport_send(transport_mask, 0, cin, status, data1, data2);
}

static void note_scheduler_callback(void *arg)
{
    if (!g_scheduler_running) {
        return;
    }
    midi_scheduler_tick((uint64_t)esp_timer_get_time());
}

int MIDI_Note_scheduler_init(void)
{
    if (g_note_scheduler_timer != NULL) {
        return 0;  /* Already initialized */
    }

    midi_scheduler_core_init();
    midi_scheduler_set_send_callback(scheduler_send_packet);

    esp_timer_create_args_t timer_args = {
        .callback = note_scheduler_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "note_scheduler",
        .skip_unhandled_events = true
    };

    esp_err_t ret = esp_timer_create(&timer_args, &g_note_scheduler_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(SCHED_TAG, "Failed to create timer: %d", ret);
        return -1;
    }

    ret = esp_timer_start_periodic(g_note_scheduler_timer, NOTE_SCHEDULER_TICK_US);
    if (ret != ESP_OK) {
        ESP_LOGE(SCHED_TAG, "Failed to start timer: %d", ret);
        esp_timer_delete(g_note_scheduler_timer);
        g_note_scheduler_timer = NULL;
        return -1;
    }

    g_scheduler_running = true;
    ESP_LOGI(SCHED_TAG, "Note scheduler initialized");
    return 0;
}

void MIDI_Note_scheduler_deinit(void)
{
    g_scheduler_running = false;

    if (g_note_scheduler_timer != NULL) {
        esp_timer_stop(g_note_scheduler_timer);
        esp_timer_delete(g_note_scheduler_timer);
        g_note_scheduler_timer = NULL;
    }

    midi_scheduler_core_deinit();
    ESP_LOGI(SCHED_TAG, "Note scheduler deinitialized");
}

int MIDI_Note_trigger(midi_tmask_t transport_mask, uint8_t channel, uint8_t note,
                      uint8_t velocity, uint32_t duration_ms)
{
    /* Auto-initialize scheduler if needed */
    if (g_note_scheduler_timer == NULL) {
        if (MIDI_Note_scheduler_init() != 0) {
            return -1;
        }
    }

    int ret = midi_scheduler_trigger(transport_mask, channel, note, velocity,
                                     duration_ms,
                                     (uint64_t)esp_timer_get_time());
    if (ret != 0) {
        ESP_LOGW(SCHED_TAG,
                 "Scheduler full, note_off for note %d won't be automatic",
                 note);
    }
    return ret;
}

void MIDI_Note_scheduler_clear(void)
{
    midi_scheduler_clear();
    ESP_LOGI(SCHED_TAG, "Scheduler cleared");
}

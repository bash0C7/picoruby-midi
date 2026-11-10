# picoruby-midi

MIDI protocol layer for PicoRuby — parser, scheduler, clock, and a
transport-agnostic device wrapper. Pair with one of the transport gems
(`picoruby-usb_midi_host`, `picoruby-uart_midi`, `picoruby-sam2695`) to
actually send and receive MIDI bytes.

## Usage

### Send notes

```ruby
require 'midi'
require 'sam2695'

device = MIDI::Device.new(SAM2695.new(17, 18))
device.note_on(60, 100)
sleep 1
device.note_off(60)
```

### Trigger with auto-off (multitouch-friendly)

```ruby
device.trigger(60, 100, duration: 200)   # note_off auto-fires after 200ms
```

### Receive events

```ruby
MIDI.on(device, :note_on) { |m| puts "ON #{m.note} vel=#{m.velocity}" }
loop do
  MIDI.sleep_ms 20      # listeners run here, as messages arrive
end
```

`MIDI.on` is a filter with no destination (below), so it hears every
route source: the USB-MIDI host, the UART, the USB-MIDI device and
transports that push their own output. (`MIDI::Input` is deprecated.)

### Route one transport to another (MIDI Thru)

Forwarding happens in C, in the input task, as each message is read --
no Ruby in the path, so a keyboard keeps playing a synth whatever the
script is doing:

```ruby
MIDI.route(MIDIDevices.usb_midi_host, MIDIDevices.sam2695)              # everything
MIDI.route(MIDIDevices.usb_midi_host, MIDIDevices.amy, channel: 0)      # channel 1 only
MIDI.route(MIDIDevices.usb_midi_device, MIDIDevices.sam2695, channel: :clock)  # clock / start / stop / SPP only
MIDI.route(MIDIDevices.usb_midi_host, MIDIDevices.amy, channel: [0, 9, :clock]) # channels 1 and 10 plus the clock
MIDI.unroute(MIDIDevices.usb_midi_host)                                 # all routes from it
MIDI.unroute_all
```

Sources are the transports the input task reads (USB-MIDI host, UART),
plus transports that produce MIDI themselves and declare it with
`MIDI_route_set_source()` -- they look up their destinations with
`MIDI_route_targets()` and send on their own (an on-screen keyboard, for
example). Any transport in the registry can be a destination. A
`MIDI::Input` on the source still receives every message, routed or not.

### Filter a route in Ruby

A filter diverts what a route would forward to one destination into a
Ruby block. Only the message types it asks for leave C (clock and the
rest still go straight through), and other destinations are untouched:

```ruby
MIDI.route(kb, synth).filter(types: [:note_on, :note_off]) do |m|
  m.note += 12
  m                     # a Message is sent, an Array of them each sent, nil dropped
end
```

Blocks run in `MIDI.run_filters`, which `MIDI.sleep_ms`, the clock
loops and `process_all_inputs` call; while a filter exists, those sleep
on the filter queue and wake as soon as a message is queued. A full queue
sends messages on unfiltered (`overflow: :bypass`, default) or drops them
(`overflow: :drop`). `MIDI::NoteMap` keeps note offs in step with note ons
through a filter that changes notes differently each time.

### The MIDI clock

`MIDI::Clock` is the master clock: one per process, run in C by the port
(an esp_timer on ESP32), and a MIDI device that only sends. Route it to
where the clock should go:

```ruby
MIDI.route(MIDI::Clock, synth)   # Timing Clock (F8), 24 per quarter note
MIDI::Clock.bpm = 128
MIDI::Clock.start                # Start (FA) along the routes; .stop / .continue
```

The F8s run while something is routed from the clock, started or not;
Start / Stop are messages of their own. The script's loop does not move
the clock, so any loop works with it (`MIDI.start!`, `MIDI.bpm_loop`,
`loop do ... MIDI.sleep_ms ... end`, ...).

### BPM-synced loop

`MIDI.start!` runs the clock + dispatches MIDI Input events + invokes
the block at every subdivision tick:

```ruby
require 'midi'

MIDI.start!(bpm: 120, output: device, subdivisions: 4) do |clock|
  beat = clock.beat
  device.note_on(60 + (beat % 8), 100)
end
```

With a port and an `output:` that is a registered transport, the loop
routes `MIDI::Clock` to `output` while it runs (a route that was already
there stays) and the block follows the clock's ticks; otherwise the loop
sends the clock itself. A `bpm:` sets the clock's tempo; without one the
loop runs at the clock's current tempo.

External BPM source (e.g. an incoming clock from another device) can be
fed in by hooking `MIDI.on_bpm_change`:

```ruby
MIDI.on_bpm_change { |new_bpm| puts "BPM is now #{new_bpm}" }
```

## API

### Top-level

- `MIDI.start!(bpm:, output:, sync:, subdivisions:) { |clock| ... }` -
  Canonical BPM-synced loop entry point.
- `MIDI.on_bpm_change { |new_bpm| ... }` - Register a hook called when
  the live BPM changes (manual set or external sync).
- `MIDI.input(device)` / `MIDI.clock(device)` - Convenience builders.
- `MIDI.usb_host_device` - Returns a `MIDI::Device` wrapping the USB MIDI
  Host transport, or `nil` if nothing is connected.
- `MIDI.sam2695_device(tx_pin, rx_pin = -1)` - Returns a `MIDI::Device`
  wrapping a SAM2695 synth on the given UART pins, or `nil` if not ready.
- `MIDI.sleep_ms(ms)` - Sleep that yields to MIDI input dispatch so you
  don't drop incoming events.
- `MIDI.route(from, to, channel: nil)` - Forward messages from one transport
  (or `MIDI::Device`) to another in C. `channel:` 0..15 passes only that
  channel's voice messages; `:clock` only timing clock, start / continue /
  stop and song position; `nil` passes every channel plus clock / start /
  stop / SysEx. Returns `false` when the route table (8 entries) is full.
  Returns a `MIDI::Route` (`#filter`, `#unfilter`, `#unroute`).
- `MIDI.unroute(from, to = nil)` / `MIDI.unroute_all` - Remove routes (and
  their filters).
- `MIDI.filter(from, to = nil, types: nil, channel: nil, overflow: :bypass) { |m| ... }` -
  Run a block on what the route from `from` to `to` forwards; with no `to`, a
  listener. Returns the block, or `false` when a table is full.
- `MIDI.unfilter(from, to = nil, handler = nil)` - Remove filters (not listeners).
- `MIDI.on(from, type = :any, channel: nil) { |m| ... }` / `MIDI.off(from, type = nil, handler = nil)` -
  Listeners. `:any` is every channel message; realtime types only when named.
- `MIDI.external_bpm(from)` / `MIDI.reset_external_clock(from)` - Tempo of the
  clock arriving on a source (USB host, UART, USB device).
- `MIDI.external_clock(from)` - `[tick, age_us, period_us]`: where that clock
  is in the song (the position of its last clock, 24 a quarter note, moved by
  Start, Continue, Stop and Song Position Pointer), how long ago that clock
  came (when it was sent, for BLE-MIDI on Darwin) and the period between
  clocks; `nil` while it is stopped. To play in time with it.
- `MIDI.run_filters(max = 64)` - Run queued filter work (for loops of your own).
- `MIDI.filter_stats` - `{ overflows: }`.
- `MIDI.bpm_loop(bpm = nil, output:, ...)` - Legacy version of `start!`.
  Kept for now; prefer `start!`. Like `start!`, an omitted `bpm` follows
  `MIDI::Clock`, and `output:` routes the clock rather than sending it
  from Ruby when it can.

### `MIDI::Device`

Wraps any transport that implements `send_packet` / `transport_id` /
`connected?`.

- `note_on(note, velocity = 127, channel: 0)`
- `note_off(note, velocity = 0, channel: 0)`
- `trigger(note, velocity = 127, duration: 100, channel: 0)` - Sends
  `note_on` immediately and schedules `note_off` after `duration` ms
  via the gem's note scheduler.
- `trigger_batch([{note:, velocity:, duration:, channel:}, ...])`
- `control_change(cc, value, channel: 0)`
- `program_change(program, channel: 0)`
- `pitch_bend(value, channel: 0)` - 14-bit, range 0..16383, 8192 = center
- `poly_aftertouch(note, pressure, channel: 0)`
- `channel_pressure(pressure, channel: 0)`
- Convenience CC wrappers: `modulation`, `volume`, `pan`, `expression`,
  `sustain`
- `all_notes_off(channel: 0)` / `all_sound_off(channel: 0)` /
  `reset_all_controllers(channel: 0)`
- Realtime: `send_clock` / `send_start` / `send_stop` / `send_continue`
- `send_sysex(data, wrap: false)` - Send a SysEx message. `data` is an
  array of bytes; with `wrap: true` the leading `0xF0` and trailing `0xF7`
  are added automatically.

### `MIDI::Message`

What filters and listeners get: `type`, `channel`, `note` / `velocity`,
`cc` / `program`, `value` (pitch bend as -8192..8191), `source` (Symbol),
`device`, `raw` (`[cin, b1, b2, b3]`, data clamped to 0..127),
`with(note:, velocity:, ...)` for a changed copy.

### `MIDI::Input` (deprecated: use `MIDI.on`)

- `MIDI::Input.new(device, auto_process: true)`
- `on(event_type) { |event| ... }` where `event_type` is `:note_on`,
  `:note_off`, `:control_change`, `:program_change`, `:pitch_bend`,
  `:poly_aftertouch`, `:channel_pressure`, `:clock`, `:start`, `:stop`,
  `:continue`, `:system_reset`, `:sysex`, or `:any`
- `start` / `stop` - Start/stop the background read task
- `process(max_events = 64)` - Pop and dispatch queued events
- `external_bpm` - Current external BPM (from incoming clock)

### `MIDI::Clock`

The clock (class methods; one per process):

- `MIDI.route(MIDI::Clock, dev)` / `MIDI.unroute(MIDI::Clock, dev)` -
  Where the clock goes (`transport_id` is its registry bit; 0 without a port)
- `MIDI::Clock.bpm`, `MIDI::Clock.bpm = value` (20..300)
- `MIDI::Clock.start` / `.continue` / `.stop` / `.running?` - Start (FA),
  Continue (FB), Stop (FC) along its routes. The F8s go on either way.
- `MIDI::Clock.hardware?` - Whether the port runs it in C

The older handle (same clock):

- `MIDI::Clock.new(device = nil)` / `MIDI.clock(device)` - `start` routes the
  clock to `device` and sends Start (unless the clock is running already),
  `stop` sends Stop and removes that route
- `bpm`, `bpm=(new_bpm)` - The clock's tempo
- `start` / `stop` / `continue` / `running?`
- `receive_clock` / `receive_start` / `receive_stop` - Feed in external
  clock bytes for sync.
- `external_bpm` / `sync_to_external`

### `MIDI::Note`

- `MIDI::Note.number(MIDI::Note::C, 4)` -> 60
- `MIDI::Note.name(60)` -> `"C4"`
- Note-name constants: `MIDI::Note::C`, `Cs`/`Db`, `D`, `Ds`/`Eb`, `E`, `F`,
  `Fs`/`Gb`, `G`, `Gs`/`Ab`, `A`, `As`/`Bb`, `B` (0..11)

## Architecture

The protocol code is split into OS-free cores plus thin per-platform
ports:

- `src/midi_parser.c` - Byte/packet parser, SysEx accumulator
- `src/midi_scheduler.c` - Note off scheduler (driven by `tick(now_us)`)
- `src/midi_clock_gen.c` - Clock generator (driven by `tick(now_us)`;
  the ESP32 port keeps only its tempo and drives the clock from its timer)
- `src/midi_transport_registry.c` - Transport registry: one bit of the
  transport mask per transport
- `src/midi_route.c` - Route table for `MIDI.route`
- `ports/esp32/midi.c` - ESP32 port: the clock (an esp_timer; a transport
  and route source of its own), FreeRTOS input task
- `ports/darwin/midi.c` - macOS / iOS port: the clock and the note
  scheduler on a GCD timer, the input task on a pthread that waits while no
  source is connected and ends at `MIDI_Input_stop`. Darwin has no
  built-in transports (USB-MIDI host, UART, USB-MIDI device), so the input
  task reads only the packet sources other gems register
  (`MIDI_Input_add_packet_source`), and `MIDI::Input` receives nothing.
  `ports/darwin/midi_timed_ring.c` times its packets with
  `midi_darwin_now_us()` (`ports/darwin/include/midi_darwin.h`).
  `make -C host_test darwin` tests it on macOS.

Transports plug in via the `midi_transport_t` interface
(`include/midi_transport.h`); per-transport gems implement it and
register themselves with the input task.

## Notes

- Default PPQ is 24 (MIDI standard).
- The note scheduler runs from the same 1 ms timer as the clock, so
  `trigger` is non-blocking and multitouch-safe.
- mruby and mrubyc bindings are at feature parity.

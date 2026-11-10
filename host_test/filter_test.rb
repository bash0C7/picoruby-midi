# Host tests for the Ruby side of MIDI filters (mrblib/midi_filter.rb and
# MIDI.route in mrblib/midi_route.rb). The C bindings are stubbed: the
# stubs record what reaches C and let the test queue filter events.
#
#   ruby host_test/filter_test.rb
#   cat host_test/filter_stubs.rb mrblib/midi_filter.rb mrblib/midi_route.rb \
#       mrblib/midi_input.rb host_test/filter_test.rb | RBENV_VERSION=mruby-3.2.0 mruby
#
# (run from the gem directory; mruby has no `load`)

unless Object.const_defined?(:MIDI) && MIDI.const_defined?(:Message)
  dir = File.expand_path("..", File.dirname(__FILE__))
  load File.join(dir, "host_test/filter_stubs.rb")
  load File.join(dir, "mrblib/midi_filter.rb")
  load File.join(dir, "mrblib/midi_route.rb")
  load File.join(dir, "mrblib/midi_input.rb")
end

$passed = 0
$failed = 0

def check(name, expected, actual)
  if expected == actual
    $passed += 1
  else
    $failed += 1
    puts "FAIL #{name}: expected #{expected.inspect}, got #{actual.inspect}"
  end
end

def check_raises(name, pattern)
  yield
  $failed += 1
  puts "FAIL #{name}: nothing raised"
rescue ArgumentError => e
  if e.message.include?(pattern)
    $passed += 1
  else
    $failed += 1
    puts "FAIL #{name}: #{e.message}"
  end
end

def reset
  MIDI.unroute_all
  $stub = { routes: [], filters: {}, queue: [], sent: [], started: 0 }
end

class FakeTransport
  attr_reader :transport_id
  def initialize(bit); @transport_id = bit; end
end

HOST = FakeTransport.new(0x01)
UART = FakeTransport.new(0x02)
AMY  = FakeTransport.new(0x08)

# --- Message -----------------------------------------------------------------

m = MIDI::Message._from_packet(0x9, 0x93, 60, 100, :usb_host)
check "message type", :note_on, m.type
check "message channel", 3, m.channel
check "message note", 60, m.note
check "message velocity", 100, m.velocity
check "message source", :usb_host, m.source
check "message raw", [0x9, 0x93, 60, 100], m.raw

m.note += 12
check "message mutate", [0x9, 0x93, 72, 100], m.raw
c = m.with(note: 76, velocity: 10)
check "with copy", [0x9, 0x93, 76, 10], c.raw
check "with leaves original", 72, m.note

check "note on vel 0 is note off", :note_off, MIDI::Message._from_packet(0x9, 0x90, 60, 0).type
check "clamp", [0x9, 0x90, 127, 0], MIDI::Message.new(:note_on, 0, 140, -3).raw

pb = MIDI::Message._from_packet(0xE, 0xE1, 0x00, 0x40)
check "pitch bend center", 0, pb.value
pb.value = 8191
check "pitch bend max", [0xE, 0xE1, 0x7F, 0x7F], pb.raw
check "program value", 5, MIDI::Message._from_packet(0xC, 0xC0, 5, 0).value
check "cc", [7, 100], [MIDI::Message._from_packet(0xB, 0xB0, 7, 100).cc, MIDI::Message._from_packet(0xB, 0xB0, 7, 100).value]
check "clock", :clock, MIDI::Message._from_packet(0xF, 0xF8, 0, 0).type
check "clock raw", [0xF, 0xF8, 0, 0], MIDI::Message._from_packet(0xF, 0xF8, 0, 0).raw

# --- route returns a Route -----------------------------------------------------

reset
r = MIDI.route(HOST, AMY)
check "route object", MIDI::Route, r.class
check "route added", [[0x01, 0x08, -1]], $stub[:routes]
check "route started input", 1, $stub[:started]

reset
check "route clock only", MIDI::Route, MIDI.route(HOST, AMY, channel: :clock).class
check "clock only reaches C as -2", [[0x01, 0x08, -2]], $stub[:routes]
check_raises("bad channel symbol", "0..15 or :clock") { MIDI.route(HOST, AMY, channel: :notes) }
check_raises("bad channel number", "0..15 or :clock") { MIDI.route(HOST, AMY, channel: 16) }

reset
check "route several", MIDI::Route, MIDI.route(HOST, AMY, channel: [0, 9, :clock]).class
check "several reach C as a selection", [[0x01, 0x08, -3, 0x10201]], $stub[:routes]
check "several read back", [0, 9, :clock], MIDI._route_channel(-3, 0x10201)
check "one read back", 9, MIDI._route_channel(9, 0x200)
check "all read back", nil, MIDI._route_channel(-1, 0x20000)
check_raises("bad channel in a list", "0..15 or :clock") { MIDI.route(HOST, AMY, channel: [0, 16]) }
check_raises("empty channel list", "not be empty") { MIDI.route(HOST, AMY, channel: []) }

reset
r = MIDI.route(HOST, AMY)

# --- filter on a route ---------------------------------------------------------

h = r.filter(types: [:note_on, :note_off]) { |msg| msg.note += 12; msg }
check "filter returns block", true, h.is_a?(Proc)
f = $stub[:filters][[0x01, 0x08]]
check "C types (note on/off)", (1 << 8) | (1 << 9), f[0]
check "C channel", -1, f[1]
check "C overflow bypass", 0, f[2]

$stub[:queue] << [0x08, 0x01, 0x9, 0x90, 60, 100]
check "run_filters count", 1, MIDI.run_filters
check "transposed", [[0x08, 0x9, 0x90, 72, 100]], $stub[:sent]

# chain: a second filter sees the first one's output; Array fan-out; nil drops
$stub[:sent] = []
MIDI.filter(HOST, AMY, types: :note_on) { |msg| [msg, msg.with(note: msg.note + 7)] }
$stub[:queue] << [0x08, 0x01, 0x9, 0x90, 60, 100]
MIDI.run_filters
check "chain + fan-out", [[0x08, 0x9, 0x90, 72, 100], [0x08, 0x9, 0x90, 79, 100]], $stub[:sent]

$stub[:sent] = []
$stub[:queue] << [0x08, 0x01, 0x8, 0x80, 60, 0]     # note off: 2nd filter passes it through
MIDI.run_filters
check "non-matching type passes", [[0x08, 0x8, 0x80, 72, 0]], $stub[:sent]

# a filter on a route that was not there sets the route up
reset
MIDI.filter(HOST, UART, channel: 9, overflow: :drop) { |msg| nil }
check "filter adds route", [[0x01, 0x02, -1]], $stub[:routes]
f = $stub[:filters][[0x01, 0x02]]
check "C channel 9", 9, f[1]
check "C overflow drop", 1, f[2]
$stub[:queue] << [0x02, 0x01, 0x9, 0x99, 36, 100]
MIDI.run_filters
check "nil drops", [], $stub[:sent]

# exceptions drop the message and are reported once
reset
MIDI.filter(HOST, AMY) { |msg| raise "boom" }
$stub[:queue] << [0x08, 0x01, 0x9, 0x90, 60, 100]
$stub[:queue] << [0x08, 0x01, 0x9, 0x90, 61, 100]
MIDI.run_filters
check "error drops", [], $stub[:sent]

# a packet whose filter went away meanwhile is sent as is
reset
MIDI.route(HOST, AMY)
$stub[:queue] << [0x08, 0x01, 0x9, 0x90, 60, 100]
MIDI.run_filters
check "orphan packet sent", [[0x08, 0x9, 0x90, 60, 100]], $stub[:sent]

# --- unfilter ------------------------------------------------------------------

reset
h1 = MIDI.filter(HOST, AMY) { |msg| msg }
h2 = MIDI.filter(HOST, AMY, types: :clock) { |msg| msg }
check "union with clock", (0x7F << 8) | (1 << 0xF), $stub[:filters][[0x01, 0x08]][0]
check "unfilter one", 1, MIDI.unfilter(HOST, AMY, h2)
check "C types back", 0x7F << 8, $stub[:filters][[0x01, 0x08]][0]
check "unfilter rest", 1, MIDI.unfilter(HOST, AMY)
check "C filter gone", nil, $stub[:filters][[0x01, 0x08]]
check "route stays", [[0x01, 0x08, -1]], $stub[:routes]

# --- listeners (MIDI.on) -------------------------------------------------------

reset
got = []
MIDI._set_source_info(0x01, :keyboard, :dev)
MIDI.on(HOST, :note_on) { |msg| got << [msg.type, msg.note, msg.source, msg.device]; :ignored }
check "listener dst", true, $stub[:filters].key?([0x01, MIDI::DST_RUBY])
check "listener needs no route", [], $stub[:routes]
check "listener started input", 1, $stub[:started]
$stub[:queue] << [MIDI::DST_RUBY, 0x01, 0x9, 0x90, 64, 1]
MIDI.run_filters
check "listener got it", [[:note_on, 64, :keyboard, :dev]], got
check "listener sends nothing", [], $stub[:sent]

# one packet for a listener and a route filter at once
MIDI.filter(HOST, AMY) { |msg| msg.with(velocity: 1) }
$stub[:queue] << [MIDI::DST_RUBY | 0x08, 0x01, 0x9, 0x90, 65, 100]
MIDI.run_filters
check "shared event: listener", 65, got.last[1]
check "shared event: route", [[0x08, 0x9, 0x90, 65, 1]], $stub[:sent]

clk = 0
MIDI.on(HOST, :clock) { |msg| clk += 1 }
check "off by type", 1, MIDI.off(HOST, :clock)
check "off all", 1, MIDI.off(HOST)
check "listener C gone", false, $stub[:filters].key?([0x01, MIDI::DST_RUBY])
check "unfilter keeps nothing for Ruby", true, $stub[:filters].key?([0x01, 0x08])

# --- arguments -----------------------------------------------------------------

reset
check_raises("bad source", "source must be") { MIDI.filter(AMY, HOST) { |x| x } }
check_raises("bad type", "unknown message type") { MIDI.filter(HOST, AMY, types: :sysex) { |x| x } }
check_raises("bad overflow", "overflow must be") { MIDI.filter(HOST, AMY, overflow: :later) { |x| x } }
check_raises("bad channel", "channel must be") { MIDI.filter(HOST, AMY, channel: 16) { |x| x } }
check_raises("no block", "block is required") { MIDI.filter(HOST, AMY) }

# a full C table gives false and leaves Ruby unchanged
$stub[:full] = true
check "full table", false, MIDI.filter(HOST, AMY) { |x| x }
check "nothing kept", 0, $__midi_filters__.size
$stub[:full] = false

# --- unroute takes the filters with it ------------------------------------------

reset
MIDI.filter(HOST, AMY) { |x| x }
MIDI.on(HOST) { |x| x }
MIDI.unroute(HOST, AMY)
check "unroute drops filter", false, $stub[:filters].key?([0x01, 0x08])
check "unroute keeps listener", true, $stub[:filters].key?([0x01, MIDI::DST_RUBY])

# --- sync_from: reads the source --------------------------------------------------

reset
MIDI::ClockSource.new(UART)
check "sync_from watches", 0x02, $stub[:watch]
check "sync_from starts reading", 1, $stub[:started]
check "external bpm via source", 120.0, MIDI::ClockSource.new(HOST).external_bpm
check "watches add up", 0x03, $stub[:watch]
MIDI::ClockSource.new(AMY)                            # not a source: no watch
check "non-source not watched", 0x03, $stub[:watch]
check "external clock by source bit", [0x01, 12, 34, 20833.0], MIDI.external_clock(0x01)
check "external clock by transport", [0x02, 12, 34, 20833.0], MIDI.external_clock(UART)

# --- NoteMap ---------------------------------------------------------------------

map = MIDI::NoteMap.new
pick = [67, 72]
on = map.apply(MIDI::Message.new(:note_on, 2, 60, 100)) { |x| x.note = pick.shift; x }
check "notemap on", [0x9, 0x92, 67, 100], on.raw
off = map.apply(MIDI::Message.new(:note_off, 2, 60, 0)) { |x| raise "not for note off" }
check "notemap off follows on", [[0x8, 0x82, 67, 0]], off.map { |x| x.raw }
off2 = map.apply(MIDI::Message.new(:note_off, 2, 60, 0)) { |x| x }
check "unknown note off passes", [0x8, 0x82, 60, 0], off2.raw
chord = map.apply(MIDI::Message.new(:note_on, 0, 48, 90)) { |x| [x, x.with(note: 52)] }
check "notemap chord on", 2, chord.size
check "release_all", [[0x8, 0x80, 48, 0], [0x8, 0x80, 52, 0]], map.release_all.map { |x| x.raw }
dropped = map.apply(MIDI::Message.new(:note_on, 0, 50, 90)) { |x| nil }
check "dropped on", nil, dropped
check "its off sends nothing", [], map.apply(MIDI::Message.new(:note_off, 0, 50, 0))

# --- The virtual port: listening is listening to what is routed into it ------------

reset
$stub[:virtual] = 0x20
heard = []
h = MIDI.on(0x20, :note_on) { |m| heard << [m.note, m.source] }
check "virtual: listened in C by types, no filter", false, $stub[:filters].key?([0x20, MIDI::DST_RUBY])
check "virtual: note on queued", 1 << 0x9, $stub[:vlisten] & (1 << 0x9)
$stub[:queue] << [MIDI::DST_VIRTUAL, 0x01, 0x9, 0x90, 60, 100]
MIDI.run_filters
check "virtual: heard with its source", [[60, $__midi_source_info__[0x01] ? $__midi_source_info__[0x01][0] : :usb_host]], heard
# A filter on the route into the port: its output goes to the listeners, not out
MIDI.route(HOST, 0x20)
MIDI.filter(HOST, 0x20, types: [:note_on]) { |m| m.note += 12; m }
$stub[:sent] = []
$stub[:queue] << [0x20, 0x01, 0x9, 0x90, 61, 100]
MIDI.run_filters
check "virtual: filtered into the listeners", [60, 73], heard.map { |x| x[0] }
check "virtual: nothing sent to its transport", [], $stub[:sent]
MIDI.unfilter(HOST, 0x20)
MIDI.off(0x20, nil, h)
check "virtual: listening off", 0, $stub[:vlisten]
$stub[:virtual] = nil

puts "#{$passed} passed, #{$failed} failed"
exit(1) if $failed > 0

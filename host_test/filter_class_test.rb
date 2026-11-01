# Host tests for named filters (mrblib/midi_filter_class.rb): MIDI::Filter
# declarations and parameters, the registry, chains on routes, emit / every
# / on_clock and the notes a chain turns off. The C bindings are the stubs
# filter_test.rb uses.
#
#   ruby host_test/filter_class_test.rb
#   cat host_test/filter_stubs.rb mrblib/midi_constants.rb mrblib/midi_device.rb \
#       mrblib/midi_filter.rb mrblib/midi_route.rb mrblib/midi_input.rb \
#       mrblib/midi_clock.rb mrblib/midi_filter_class.rb \
#       host_test/filter_class_test.rb | RBENV_VERSION=mruby-3.2.0 mruby
#
# (run from the gem directory; mruby has no `load`)

unless Object.const_defined?(:MIDI) && MIDI.const_defined?(:Filter)
  dir = File.expand_path("..", File.dirname(__FILE__))
  load File.join(dir, "host_test/filter_stubs.rb")
  %w[midi_constants midi_device midi_filter midi_route midi_input midi_clock
     midi_filter_class].each do |f|
    load File.join(dir, "mrblib/#{f}.rb")
  end
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

# A clock on bit 0x40 and a clock of our own for every(ms)
$now = 0
module MIDI
  class << self
    def _uptime_ms; $now; end
    def _route_sources; 0x07 | 0x40; end
  end
  class Clock
    class << self
      def _bit; 0x40; end
    end
  end
end

def reset
  MIDI.unroute_all
  $__midi_filter_registry__.keys.each { |k| MIDI.unregister_filter(k) }
  $stub = { routes: [], filters: {}, queue: [], sent: [], started: 0 }
  $now = 0
end

class FakeTransport
  attr_reader :transport_id
  def initialize(bit); @transport_id = bit; end
end

HOST = FakeTransport.new(0x01)
UART = FakeTransport.new(0x02)
AMY  = FakeTransport.new(0x08)

# Queue a packet from src for the filters on dst and run them
def feed(src, dst, cin, b1, b2, b3)
  $stub[:queue] << [dst, src, cin, b1, b2, b3]
  MIDI.run_filters
end

def note_on(src, dst, note, vel = 100, ch = 0)
  feed(src, dst, 0x9, 0x90 | ch, note, vel)
end

def note_off(src, dst, note, ch = 0)
  feed(src, dst, 0x8, 0x80 | ch, note, 0)
end

def sent
  s = $stub[:sent]
  $stub[:sent] = []
  s
end

# --- Declarations ------------------------------------------------------------

class Shift < MIDI::Filter
  label "Shift"
  types :note_on, :note_off
  param :by, :int, range: -24..24, default: 12
  param :mode, :enum, choices: [:up, :down], default: :up
  param :on, :bool, default: true

  attr_reader :setups, :teardowns

  def setup
    @setups = (@setups || 0) + 1
  end

  def teardown
    @teardowns = (@teardowns || 0) + 1
  end

  def call(m)
    return m unless params[:on]
    m.note += params[:mode] == :up ? params[:by] : -params[:by]
    m
  end
end

class Twice < Shift
  label "Twice"
  param :by, :int, range: 0..12, default: 7
  param :gap, :note, default: 3

  def call(m)
    [m, m.with(note: m.note + params[:by])]
  end
end

class Silent < MIDI::Filter
end

check "label", "Shift", Shift.label
check "types", [:note_on, :note_off], Shift.types
check "default types are the channel messages", MIDI::CHANNEL_TYPES, Silent.types
check "params in order", [:by, :mode, :on], Shift.param_specs.map { |s| s[:name] }
check "a subclass inherits and overrides", [:by, :mode, :on, :gap], Twice.param_specs.map { |s| s[:name] }
check "...its own range", 12, Twice.param_spec(:by)[:max]
check "...the parent's types", [:note_on, :note_off], Twice.types
check "...the parent keeps its own", 24, Shift.param_spec(:by)[:max]
check "enum range is the indexes", [0, 1], [Shift.param_spec(:mode)[:min], Shift.param_spec(:mode)[:max]]
check "defaults", { by: 12, mode: :up, on: true }, Shift.new.params
check "given values", { by: -3, mode: :down, on: true }, Shift.new(by: -3, mode: :down).params
check "values are kept in range", 24, Shift.new(by: 99).params[:by]
check "enum by index", :down, Shift.new(mode: 1).params[:mode]
check "bool from 0", false, Shift.new(on: 0).params[:on]
check "code of an enum", 1, Shift._code(Shift.param_spec(:mode), :down)
check_raises("unknown parameter", "unknown parameter") { Shift.new(nope: 1) }
check_raises("bad enum", "is not one of") { Shift.new(mode: :sideways) }
check_raises("int needs a range", "needs range") do
  Class.new(MIDI::Filter) { param :x, :int }
end
check_raises("at most 8", "at most 8") do
  Class.new(MIDI::Filter) { 9.times { |i| param "p#{i}".to_sym, :bool } }
end

proto = Shift.new(by: 5)
copy = proto.instantiate(mode: :down)
check "instantiate copies the values", { by: 5, mode: :down, on: true }, copy.params
check "...as a new instance", false, copy.equal?(proto)
check "...the prototype is untouched", :up, proto.params[:mode]

# --- Registry ----------------------------------------------------------------

reset
hooks = []
$__midi_filter_registry_hooks__ << Proc.new { |e, n| hooks << [e, n] }
check "register returns the filter", proto, MIDI.register_filter(:shift, proto)
check "...names it", :shift, proto.name
check "...tells the hooks", [[:register, :shift]], hooks
check "filters", [:shift], MIDI.filters.keys
check "registered_filter", proto, MIDI.registered_filter("shift")
check_raises("not a filter", "is not a MIDI::Filter") { MIDI.register_filter(:x, Object.new) }
check_raises("bad name", "use a-z") { MIDI.register_filter(:"Bad Name", Shift.new) }
check_raises("long name", "use a-z") { MIDI.register_filter(:abcdefghijklmnop, Shift.new) }
preset = MIDI.register_filter(:shift_down, Shift.new(mode: :down), label: "Down")
check "label: names a preset", "Down", preset.label
check "...the class's label otherwise", "Shift", proto.label
check "...and its route instances keep it", "Down", preset.instantiate.label
MIDI.unregister_filter(:shift_down)
2.times { hooks.pop }
check "unregister", true, MIDI.unregister_filter(:shift)
check "...twice", false, MIDI.unregister_filter(:shift)
check "...told", [:unregister, :shift], hooks.last
16.times { |i| MIDI.register_filter("f#{i}", Silent.new) }
check "at most 16", false, MIDI.register_filter(:one_more, Silent.new)
check "...but replacing is fine", true, MIDI.register_filter(:f3, Silent.new).is_a?(Silent)
$__midi_filter_registry_hooks__.clear

# --- Chains ------------------------------------------------------------------

reset
MIDI.register_filter(:shift, Shift.new)
MIDI.register_filter(:twice, Twice.new)
chain_events = []
$__midi_chain_hooks__ << Proc.new { |e, c, f, x| chain_events << [e, f ? f.name : nil, x] }

route = MIDI.route(HOST, AMY)
filters = route.chain(:shift, [:twice, { by: 4 }])
check "chain returns the instances", [Shift, Twice], filters.map { |f| f.class }
check "...fresh ones", false, filters[0].equal?(MIDI.registered_filter(:shift))
check "...with the values given", 4, filters[1].params[:by]
check "...set up once", [1, 1], filters.map { |f| f.setups }
check "...attached hooks", [[:attach, :shift, nil], [:attach, :twice, nil], [:chain, nil, nil]], chain_events
check "one C filter entry with the chain's types", [[0x01, 0x08]], $stub[:filters].keys
check "route.chain reads it back", filters, route.chain

note_on(0x01, 0x08, 60)
check "in order: shift then twice", [[0x08, 0x9, 0x90, 72, 100], [0x08, 0x9, 0x90, 76, 100]], sent
feed(0x01, 0x08, 0xB, 0xB0, 74, 10)
check "types it does not take go through", [[0x08, 0xB, 0xB0, 74, 10]], sent

# A block filter on the same route runs after the chain
MIDI.filter(HOST, AMY, types: :note_on) { |m| m.velocity = 1; m }
note_on(0x01, 0x08, 60)
check "block filters follow the chain", [[0x08, 0x9, 0x90, 72, 1], [0x08, 0x9, 0x90, 76, 1]], sent
MIDI.unfilter(HOST, AMY, $__midi_filters__[MIDI._pair_key(0x01, 0x08)].last[3])

# set_param
chain_events.clear
filters[0].set_param(:by, 1)
check "set_param tells the hooks", [[:param, :shift, :by]], chain_events
check "...and the chain's specs", [[:shift, { by: 1, mode: :up, on: true }], [:twice, { by: 4, mode: :up, on: true, gap: 3 }]],
      MIDI.chains[[0x01, 0x08]].specs
note_off(0x01, 0x08, 60)
sent

# Replacing keeps instances that stay, and turns notes off once
note_on(0x01, 0x08, 50)
sent
chain_events.clear
kept = filters[1]
gone = filters[0]
again = route.chain(:twice)
check "a filter that stays keeps its instance", true, again[0].equal?(kept)
check "...the one that goes is torn down", 1, gone.teardowns
check "...and detached", false, gone.attached?
# (72 / 76 are still held from before set_param changed the shift)
check "the chain's notes are turned off",
      [[0x08, 0x8, 0x80, 51, 0], [0x08, 0x8, 0x80, 55, 0], [0x08, 0x8, 0x80, 72, 0], [0x08, 0x8, 0x80, 76, 0]],
      sent.sort
check "...hooks", [[:detach, :shift, nil], [:chain, nil, nil]], chain_events

# Unregistered names stay as gaps; registering again fills them
route.chain(:shift, :twice)
MIDI.unregister_filter(:shift)
check "an unregistered name leaves a gap", [nil, Twice], route.chain.map { |f| f && f.class }
check "...its name is kept", [:shift, :twice], MIDI.chains[[0x01, 0x08]].names
note_on(0x01, 0x08, 60)
check "...and is passed through", [[0x08, 0x9, 0x90, 60, 100], [0x08, 0x9, 0x90, 64, 100]], sent
MIDI.register_filter(:shift, Shift.new(by: 2))
check "registering fills it", [Shift, Twice], route.chain.map { |f| f && f.class }
check "...with the new prototype's values", 2, route.chain[0].params[:by]
note_off(0x01, 0x08, 60)
sent

# Re-registering replaces the instance but keeps the route's values
route.chain[0].set_param(:by, 3)
old = route.chain[0]
MIDI.register_filter(:shift, Shift.new(by: 9))
check "re-register makes a new instance", false, route.chain[0].equal?(old)
check "...keeping the values the route had", 3, route.chain[0].params[:by]
check "...the old one torn down", 1, old.teardowns

# Removing the route takes the chain down
note_on(0x01, 0x08, 40)
sent
c = route.chain
MIDI.unroute(HOST, AMY)
check "unroute drops the chain", {}, MIDI.chains
check "...its filters torn down", [1, 1], c.map { |f| f.teardowns }
check "...its notes off", 2, sent.size
check "...and its C entry", {}, $stub[:filters]

# MIDI.chain makes the route
reset
MIDI.register_filter(:shift, Shift.new)
check "MIDI.chain makes the route", [Shift], MIDI.chain(UART, AMY, :shift).map { |f| f.class }
check "...in the route table", [[0x02, 0x08, -1]], $stub[:routes]
check "chain([]) clears", [], MIDI.chain(UART, AMY, [])
check "...the C entry too", {}, $stub[:filters]
check "...the route stays", 1, $stub[:routes].size
check_raises("at most 4", "at most 4") { MIDI.chain(UART, AMY, :a, :b, :c, :d, :e) }

# A chain of only filters without types takes no C entry
class Lights < MIDI::Filter
  types []
end
MIDI.register_filter(:lights, Lights.new)
MIDI.chain(UART, AMY, :lights)
check "no types, no C entry", {}, $stub[:filters]
check "...but the chain is there", [:lights], MIDI.chains[[0x02, 0x08]].names
MIDI.unroute_all
check "unroute_all drops it", {}, MIDI.chains

# --- emit / every / on_clock ----------------------------------------------------

class Ticker < MIDI::Filter
  types :note_on, :note_off
  clock :internal
  attr_reader :ticks, :starts

  def setup
    @ticks = []
    @starts = 0
  end

  def call(m)
    @note = m.note if m.type == :note_on
    nil
  end

  def on_clock(tick)
    @ticks << tick
    emit(MIDI::Message.new(:note_on, 0, @note, 90)) if @note && tick % 2 == 0
  end

  def on_start
    @starts += 1
  end
end

reset
MIDI.register_filter(:ticker, Ticker.new)
MIDI.register_filter(:shift, Shift.new(by: 1))
t = MIDI.route(HOST, AMY).chain(:ticker, :shift)[0]
check "a clock :internal filter listens to MIDI::Clock", true, $stub[:filters].key?([0x40, MIDI::DST_RUBY])
note_on(0x01, 0x08, 60)
check "call's nil drops the note", [], sent
feed(0x40, MIDI::DST_RUBY, 0xF, 0xFA, 0, 0)
feed(0x40, MIDI::DST_RUBY, 0xF, 0xF8, 0, 0)
feed(0x40, MIDI::DST_RUBY, 0xF, 0xF8, 0, 0)
feed(0x40, MIDI::DST_RUBY, 0xF, 0xF8, 0, 0)
check "on_start", 1, t.starts
check "ticks count from the start", [0, 1, 2], t.ticks
check "emit goes through the filters after it", [[0x08, 0x9, 0x90, 61, 90], [0x08, 0x9, 0x90, 61, 90]], sent
MIDI.route(HOST, AMY).chain(:shift)
check "...the notes it sent are turned off", [[0x08, 0x8, 0x80, 61, 0]], sent
check "the clock listener goes with the last such filter", false, $stub[:filters].key?([0x40, MIDI::DST_RUBY])

class Blink < MIDI::Filter
  types []
  attr_reader :count, :handle
  def setup
    @count = 0
    @handle = every(10) { @count += 1 }
  end
end

reset
MIDI.register_filter(:blink, Blink.new)
b = MIDI.chain(HOST, AMY, :blink)[0]
$now = 9
MIDI.run_filters
check "every waits its interval", 0, b.count
$now = 10
MIDI.run_filters
$now = 25
MIDI.run_filters
check "...then runs once per run when due", 2, b.count
check "next timer", 5, MIDI._next_timer_ms
b.handle.cancel
$now = 100
MIDI.run_filters
check "cancel stops it", 2, b.count
b2 = MIDI.chain(HOST, AMY, [])
MIDI.register_filter(:blink, Blink.new)
b3 = MIDI.chain(HOST, AMY, :blink)[0]
MIDI.unroute(HOST, AMY)
check "timers go with the filter", 0, $__midi_filter_timers__.size

# Errors are reported once and drop the message
class Broken < MIDI::Filter
  types :note_on
  def call(m)
    raise "boom"
  end
end
reset
MIDI.register_filter(:broken, Broken.new)
MIDI.chain(HOST, AMY, :broken)
note_on(0x01, 0x08, 60)
note_on(0x01, 0x08, 61)
check "an error drops the message", [], sent

# output: a resolver, else the route's destination
reset
MIDI.register_filter(:shift, Shift.new)
f = MIDI.chain(HOST, AMY, :shift)[0]
check "output sends to the destination's bit", 0x08, f.output.transport.transport_id
$__midi_device_resolver__ = Proc.new { |bit| bit == 0x08 ? :amy_device : nil }
check "...or what the resolver gives", :amy_device, f.output
$__midi_device_resolver__ = nil
f.output.note_on(60, 10)
check "...a device that sends to it", 1, $stub[:sent].size

puts "#{$passed} passed, #{$failed} failed"
# A bare mruby has no exit; the failure count above is the result there
exit($failed == 0 ? 0 : 1) if respond_to?(:exit, true)
raise "#{$failed} failed" if $failed > 0

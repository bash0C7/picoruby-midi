# Host tests for MIDI::Clock (mrblib/midi_clock.rb) and how MIDI.start! /
# MIDI.bpm_loop (mrblib/midi_input.rb) use it. The C clock is stubbed: its
# tick count advances as the loop sleeps.
#
#   ruby host_test/clock_test.rb
#   cat host_test/filter_stubs.rb mrblib/midi_filter.rb mrblib/midi_route.rb \
#       mrblib/midi_input.rb mrblib/midi_clock.rb host_test/clock_test.rb | RBENV_VERSION=mruby-3.2.0 mruby
#
# (run from the gem directory; mruby has no `load`)

unless Object.const_defined?(:MIDI) && MIDI.const_defined?(:Clock)
  dir = File.expand_path("..", File.dirname(__FILE__))
  load File.join(dir, "host_test/filter_stubs.rb")
  load File.join(dir, "mrblib/midi_filter.rb")
  load File.join(dir, "mrblib/midi_route.rb")
  load File.join(dir, "mrblib/midi_input.rb")
  load File.join(dir, "mrblib/midi_clock.rb")
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

CLOCK_BIT = 0x08
SYNTH = 0x02

# The C clock: 120 BPM is a tick every ~20.8 ms; the loop's sleeps move time
$clock = { bpm: 120.0, follow: 0.0, count: 0, running: false, calls: [] }
$now_ms = 0

module MIDI
  class Clock
    class << self
      def _bit; CLOCK_BIT; end
      def _set_bpm(b); $clock[:bpm] = b.to_f; end
      def _bpm; $clock[:bpm]; end
      def _current_bpm; $clock[:follow] > 0 ? $clock[:follow] : $clock[:bpm]; end
      def _start; $clock[:running] = true; $clock[:calls] << :start; end
      def _continue; $clock[:running] = true; $clock[:calls] << :continue; end
      def _stop; $clock[:running] = false; $clock[:calls] << :stop; end
      def _running?; $clock[:running]; end
      def _count; $clock[:count] & 0x3FFFFFFF; end
      def _us_to_next; 0; end
    end
  end

  class << self
    def _uptime_ms; $now_ms; end
    def _route_sources; 0x07 | CLOCK_BIT; end   # The port declares the clock a source
    def _route_virtual; 0; end

    # Sleeping lets the clock tick: one tick per call
    def _idle_ms(ms)
      $now_ms += ms
      $clock[:count] += 1
    end
  end
end

class FakeTransport
  attr_reader :sent

  def initialize(bit)
    @bit = bit
    @sent = []
  end

  def transport_id; @bit; end
  def send_packet(cable, cin, b1, b2, b3); @sent << b1; 0; end
end

class FakeDevice
  attr_reader :transport

  def initialize(transport); @transport = transport; end
  def send_clock; @transport.sent << 0xF8; end
  def send_start; @transport.sent << 0xFA; end
  def send_stop; @transport.sent << 0xFC; end
end

def reset
  $stub[:routes] = []
  $clock[:calls] = []
  $clock[:running] = false
  $clock[:bpm] = 120.0
  $clock[:follow] = 0.0
end

# --- The clock ---

check "hardware", true, MIDI::Clock.hardware?
check "transport id", CLOCK_BIT, MIDI::Clock.transport_id
MIDI::Clock.bpm = 400
check "bpm clamped", 300.0, MIDI::Clock.bpm
MIDI::Clock.bpm = 90
check "bpm", 90.0, MIDI::Clock.bpm
$clock[:follow] = 133.0
check "bpm follows", 133.0, MIDI::Clock.bpm
reset

MIDI.route(MIDI::Clock, SYNTH)
check "route from the clock", [[CLOCK_BIT, SYNTH, -1]], $stub[:routes]
reset

# --- attach / detach ---

synth = FakeDevice.new(FakeTransport.new(SYNTH))
check "attach adds", :added, MIDI::Clock._attach(synth)
check "attach routed", [[CLOCK_BIT, SYNTH, -1]], $stub[:routes]
check "attach again: present", :present, MIDI::Clock._attach(synth)
MIDI::Clock._detach(synth, :present)
check "detach leaves a route it did not add", 1, $stub[:routes].size
MIDI::Clock._detach(synth, :added)
check "detach removes its route", [], $stub[:routes]
check "attach nil device", nil, MIDI::Clock._attach(nil)
check "attach unregistered", nil, MIDI::Clock._attach(FakeDevice.new(FakeTransport.new(0)))
reset

# --- MIDI.bpm_loop with output: the C clock drives it ---

ticks = []
MIDI.bpm_loop(output: synth, subdivisions: 4) do |c|
  ticks << c
  break if ticks.size == 3
end
check "bpm_loop yields every 6 clocks", [0, 6, 12], ticks
check "bpm_loop sends no clock itself", [], synth.transport.sent
check "bpm_loop starts the clock", [:start], $clock[:calls]
check "bpm_loop removes its route", [], $stub[:routes]
check "bpm_loop without bpm keeps the tempo", 120.0, $clock[:bpm]
reset

MIDI.bpm_loop(100, output: synth, send_start: false) { |c| break }
check "bpm_loop bpm sets the tempo", 100.0, $clock[:bpm]
check "bpm_loop send_start: false", [], $clock[:calls]
reset

# A patch already routes the clock to the synth: the loop leaves it there
MIDI.route(MIDI::Clock, synth)
MIDI.bpm_loop(output: synth) { |c| break }
check "bpm_loop keeps a patch", [[CLOCK_BIT, SYNTH, -1]], $stub[:routes]
reset

# --- MIDI.start! with output ---

ticks = []
MIDI.start!(bpm: 140, output: synth, subdivisions: 24) do |c|
  ticks << c
  break if ticks.size == 2
end
check "start! yields every clock", [0, 1], ticks
check "start! start and stop", [:start, :stop], $clock[:calls]
check "start! bpm", 140.0, $clock[:bpm]
check "start! removes its route", [], $stub[:routes]
reset

# --- An output the clock cannot reach: the loop sends the clock itself ---

ruby_only = FakeDevice.new(FakeTransport.new(0))
n = 0
MIDI.bpm_loop(output: ruby_only, subdivisions: 24, send_start: false) do |c|
  n += 1
  break if n == 3
end
check "fallback sends clocks", [0xF8, 0xF8, 0xF8], ruby_only.transport.sent
check "fallback leaves the clock alone", [], $clock[:calls]
reset

# --- No output: the block runs at its own rate and follows the tempo ---

stamps = []
MIDI.bpm_loop(subdivisions: 2) do |c|
  stamps << $now_ms
  MIDI::Clock.bpm = 60 if c == 1   # e.g. the Tempo screen
  break if c == 3
end
# 120 BPM / 2 = 250 ms per pass; the change in pass 1 takes from pass 2 on,
# at 60 BPM / 2 = 500 ms
check "no output: own cadence, follows the clock's tempo",
      [250, 250, 500], [stamps[1] - stamps[0], stamps[2] - stamps[1], stamps[3] - stamps[2]]
reset

# --- MIDI::Clock.new(device): a handle on the same clock ---

handle = MIDI::Clock.new(synth)
handle.start
check "handle start routes", [[CLOCK_BIT, SYNTH, -1]], $stub[:routes]
check "handle running", true, handle.running?
handle.bpm = 150
check "handle bpm is the clock's", 150.0, MIDI::Clock.bpm
$clock[:running] = false   # the end-of-script cleanup stopped the clock
check "handle not running once stopped elsewhere", false, handle.running?
$clock[:running] = true
handle.stop
check "handle stop", [:start, :stop], $clock[:calls]
check "handle stop unroutes", [], $stub[:routes]
reset

puts "#{$passed} passed, #{$failed} failed"
exit(1) if $failed > 0

# MIDI Clock
#
# The master clock: one per process, run in C by the platform port (an
# esp_timer on ESP32). It is a MIDI device that only sends -- route it to
# where the clock should go:
#
#   MIDI.route(MIDI::Clock, dev)   # Timing Clock (F8), 24 per quarter note
#   MIDI::Clock.bpm = 128
#   MIDI::Clock.start              # Start (FA) along the routes; .stop / .continue
#
# The F8s run whenever something is routed from the clock, started or not
# (receivers follow the tempo while stopped); Start / Stop are messages of
# their own. What the script does in between does not move the clock, so
# any loop works with it: MIDI.start!, MIDI.bpm_loop, `loop do`, ...
#
# Without a port (no C clock) the class methods keep the tempo only, and
# MIDI.start! / MIDI.bpm_loop send the clock from their own Ruby loop.
#
# MIDI::Clock.new(device) is the older interface: a handle on the same clock
# that routes it to `device` between start and stop.

module MIDI
  class Clock
    # Pulses Per Quarter Note (MIDI standard)
    PPQ = 24

    class << self
      # Whether the platform runs the clock in C
      def hardware?
        respond_to?(:_bit)
      end

      # Its bit in the transport registry (0 without a port)
      def transport_id
        hardware? ? _bit : 0
      end

      # @return [Float] the tempo the clock runs at
      def bpm
        hardware? ? _current_bpm : (@bpm || 120.0)
      end

      # Set the tempo (clamped to 20..300)
      def bpm=(value)
        value = value.to_f.clamp(20.0, 300.0)
        if hardware?
          _set_bpm(value)
        else
          @bpm = value
        end
      end

      # MIDI Start (FA) along the routes; the next F8 is the downbeat
      def start
        _start if hardware?
        nil
      end

      # MIDI Continue (FB) along the routes
      def continue
        _continue if hardware?
        nil
      end

      # MIDI Stop (FC) along the routes (the F8s go on)
      def stop
        _stop if hardware?
        nil
      end

      # Between start / continue and stop
      def running?
        hardware? ? _running? : false
      end

      # --- Transport (MIDI.route(MIDI::Clock, dev)) ---

      # Nothing is sent to the clock
      def send_packet(cable, cin, midi1, midi2, midi3)
        0
      end

      def bytes_available
        0
      end

      def read_available
        nil
      end

      def connected?
        transport_id != 0
      end

      def device_info
        "MIDI clock"
      end

      # Internal: route the clock to `device` for a loop or a handle.
      # @return [Symbol, nil] :added (this call routed it), :present (it
      #   already was), or nil (it cannot be: no port, or `device` is not a
      #   registered transport)
      def _attach(device)
        return nil unless device && hardware?
        src = transport_id
        dst = MIDI.route_bit(device)
        return nil if src == 0 || dst == 0
        # A route into the virtual port is its input, not its output: the
        # loop sends the clock to it itself, which goes out along its routes
        return nil if dst == MIDI._route_virtual
        return :present if (MIDI._route_targets_of(src) & dst) != 0
        return nil if MIDI._route_add(src, dst, MIDI::ROUTE_ALL_CHANNELS) != 0
        :added
      end

      # Internal: undo _attach (only the route it added)
      def _detach(device, attached)
        return unless attached == :added
        MIDI._route_remove(transport_id, MIDI.route_bit(device))
      end
    end

    # @param device [MIDI::Device, nil] where start / stop route the clock
    def initialize(device = nil)
      @device = device
      @attached = nil
      @running = false

      # External clock tracking
      @external_timestamps = []
      @external_bpm = nil
      @last_external_time = 0
    end

    # @return [Float] the clock's tempo
    def bpm
      Clock.bpm
    end

    # Set the clock's tempo
    # @param new_bpm [Float] BPM value (20.0 - 300.0)
    def bpm=(new_bpm)
      Clock.bpm = new_bpm
    end

    # Route the clock to the device and send MIDI Start (unless the clock
    # is running already, for other devices too)
    def start
      return if @running
      _attach
      if Clock.hardware?
        Clock.start unless Clock.running?
      else
        @device&.send_start
      end
    end

    # Send MIDI Stop and stop routing the clock to the device
    def stop
      return unless @running
      if Clock.hardware?
        Clock.stop
      else
        @device&.send_stop
      end
      Clock._detach(@device, @attached)
      @attached = nil
      @running = false
    end

    # Route the clock to the device and send MIDI Continue
    def continue
      return if @running
      _attach
      if Clock.hardware?
        Clock.continue unless Clock.running?
      else
        @device&.send_continue
      end
    end

    # Between start and stop (false once the clock was stopped elsewhere,
    # e.g. by the end-of-script cleanup)
    def running?
      return false unless @running
      Clock.hardware? ? Clock.running? : true
    end

    def running
      running?
    end

    # --- External Clock Sync ---

    # Call when MIDI Clock is received from external source
    # This calculates the external BPM
    def receive_clock
      now = _get_time_us

      @external_timestamps << now

      # Keep last PPQ timestamps (1 beat)
      while @external_timestamps.length > PPQ
        @external_timestamps.shift
      end

      # Calculate BPM from timestamps
      if @external_timestamps.length >= 2
        first_ts = @external_timestamps.first
        last_ts = @external_timestamps.last
        count = @external_timestamps.length - 1

        if count > 0
          avg_interval = (last_ts - first_ts).to_f / count
          if avg_interval > 0
            # us per clock -> BPM
            # 60,000,000 us/min / (interval_us * PPQ)
            @external_bpm = (60_000_000.0 / (avg_interval * PPQ)).round(2)
          end
        end
      end
    end

    # Call when MIDI Start is received
    def receive_start
      @external_timestamps.clear
      @external_bpm = nil
    end

    # Call when MIDI Stop is received
    def receive_stop
      # Keep timestamps for BPM reference
    end

    # Get detected external BPM
    # @return [Float, nil] Detected BPM or nil if not enough data
    def external_bpm
      @external_bpm
    end

    # Sync the clock's tempo to the external clock
    # @return [Boolean] true if synced, false if no external BPM available
    def sync_to_external
      if @external_bpm && @external_bpm > 0
        self.bpm = @external_bpm
        true
      else
        false
      end
    end

    # Calculate interval in microseconds for given BPM
    # @param bpm [Float] BPM value
    # @return [Integer] Microseconds per clock tick
    def self.bpm_to_interval_us(bpm)
      (60_000_000.0 / bpm / PPQ).to_i
    end

    private

    def _attach
      @running = true
      @attached ||= Clock._attach(@device)
    end

    # Get current time in microseconds
    # Note: mrubyc doesn't have 'defined?' keyword
    def _get_time_us
      Machine.uptime_us
    rescue
      0
    end
  end
end

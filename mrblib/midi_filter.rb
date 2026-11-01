# MIDI filters: Ruby blocks on a route
#
# A route (MIDI.route) forwards in C. A filter diverts what the route would
# forward to one destination into Ruby, so a block can rewrite, multiply or
# drop each message before it goes on. Only the message types the filter
# asks for leave C -- clock and everything else still go straight through --
# and destinations without a filter are untouched.
#
#   MIDI.filter(kb, syn, types: [:note_on, :note_off]) do |m|
#     m.note += 12
#     m                 # a Message is sent, an Array of them each sent,
#   end                 # nil / false dropped
#
# With no destination, a filter is a listener (what MIDI::Input#on was):
#
#   MIDI.on(kb, :note_on) { |m| puts m.note }
#
# The blocks run on the VM task wherever the script lets MIDI run:
# MIDI.sleep_ms, MIDI.bpm_loop / MIDI.start! and MIDI.process_all_inputs
# (and UI.process on Midori). A script that sleeps with plain sleep_ms does
# not run them. A full queue (the script did not let them run for a while)
# sends messages on unfiltered (overflow: :bypass, the default) or drops
# them (overflow: :drop).

# MIDI._pair_key(src, dst) => [[types, channel, overflow, block, warned, spec], ...]
$__midi_filters__ = {}
$__midi_filter_ready__ = false
# Route source bit => [key Symbol, MIDI::Device or nil], see MIDI._set_source_info
$__midi_source_info__ = {}
# Called at the top of every run_filters (UI::MIDIDevices.input follows the
# selected IN from one)
$__midi_filter_hooks__ = []
# Chains of named filters (midi_filter_class.rb): MIDI._pair_key(src, dst) => chain
$__midi_chains__ = {}
# MIDI::Filter#every timers, run from run_filters
$__midi_filter_timers__ = []

module MIDI
  # Destination of a listener (MIDI.on): Ruby itself, not a transport -- the
  # bit above the 16 transport bits (MIDI_ROUTE_DST_RUBY)
  DST_RUBY = 0x10000
  # Input of the virtual port (MIDI_ROUTE_DST_VIRTUAL): what is routed into
  # it, for MIDI.on(port) -- on Midori the Script device's midi_in
  DST_VIRTUAL = 0x20000
  # Every transport (MIDI_TMASK_ALL)
  ALL_TRANSPORTS = 0xFFFF
  # _pair_key: a source's slot, room for every destination up to DST_RUBY
  PAIR_KEY_STRIDE = DST_RUBY * 2

  # Message#source of the built-in route sources
  BUILTIN_SOURCES = { 0x01 => :usb_host, 0x02 => :uart, 0x04 => :usb_dev }

  # Types a filter takes when given none
  CHANNEL_TYPES = [:note_off, :note_on, :poly_aftertouch, :control_change,
                   :program_change, :channel_pressure, :pitch_bend]

  # One MIDI message as a filter or listener sees it.
  class Message
    STATUS = {
      note_off: 0x80, note_on: 0x90, poly_aftertouch: 0xA0, control_change: 0xB0,
      program_change: 0xC0, channel_pressure: 0xD0, pitch_bend: 0xE0
    }
    TYPE_OF_STATUS = {
      0x80 => :note_off, 0x90 => :note_on, 0xA0 => :poly_aftertouch,
      0xB0 => :control_change, 0xC0 => :program_change,
      0xD0 => :channel_pressure, 0xE0 => :pitch_bend
    }
    REALTIME = {
      0xF8 => :clock, 0xFA => :start, 0xFB => :continue, 0xFC => :stop,
      0xFE => :active_sensing, 0xFF => :system_reset
    }
    REALTIME_STATUS = {
      clock: 0xF8, start: 0xFA, continue: 0xFB, stop: 0xFC,
      active_sensing: 0xFE, system_reset: 0xFF
    }

    attr_accessor :type, :channel, :data1, :data2
    # Route source as a Symbol (:usb_host, :uart, :usb_dev, or as registered)
    attr_reader :source
    # MIDI::Device of the source, when one was registered for it
    attr_reader :device

    # @param type [Symbol] :note_on, :control_change, :clock, ...
    def initialize(type, channel = 0, data1 = 0, data2 = 0, source: nil, device: nil)
      @type = type
      @channel = channel
      @data1 = data1
      @data2 = data2
      @source = source
      @device = device
      @raw = nil
    end

    # Internal: a USB-MIDI packet as a Message. A note on with velocity 0 is
    # a note off.
    def self._from_packet(cin, b1, b2, b3, source = nil, device = nil)
      if cin >= 0x8 && cin <= 0xE
        type = TYPE_OF_STATUS[b1 & 0xF0]
        type = :note_off if type == :note_on && b3 == 0
        new(type, b1 & 0x0F, b2, b3, source: source, device: device)
      elsif cin == 0xF && REALTIME[b1]
        new(REALTIME[b1], 0, 0, 0, source: source, device: device)
      else
        m = new(:system, 0, b2, b3, source: source, device: device)
        m._raw = [cin, b1, b2, b3]
        m
      end
    end

    # Internal
    def _raw=(packet)
      @raw = packet
    end

    def channel_message?
      !STATUS[@type].nil?
    end

    def note;          @data1;     end
    def note=(v);      @data1 = v; end
    def velocity;      @data2;     end
    def velocity=(v);  @data2 = v; end
    def cc;            @data1;     end
    def cc=(v);        @data1 = v; end
    def program;       @data1;     end
    def program=(v);   @data1 = v; end

    # The message's value: velocity / pressure / CC value / program, or the
    # pitch bend as -8192..8191.
    def value
      case @type
      when :pitch_bend
        ((@data2 & 0x7F) << 7 | (@data1 & 0x7F)) - 8192
      when :program_change, :channel_pressure
        @data1
      else
        @data2
      end
    end

    def value=(v)
      case @type
      when :pitch_bend
        u = (v + 8192).clamp(0, 16383)
        @data1 = u & 0x7F
        @data2 = (u >> 7) & 0x7F
      when :program_change, :channel_pressure
        @data1 = v
      else
        @data2 = v
      end
    end

    # A copy with some fields changed.
    def with(type: nil, channel: nil, note: nil, velocity: nil, cc: nil, value: nil, program: nil)
      m = Message.new(type || @type, channel || @channel, @data1, @data2,
                      source: @source, device: @device)
      m._raw = @raw
      m.data1 = note if note
      m.data1 = cc if cc
      m.data1 = program if program
      m.data2 = velocity if velocity
      m.value = value if value
      m
    end

    # [cin, b1, b2, b3]. Data bytes are clamped to 0..127.
    def raw
      status = STATUS[@type]
      if status
        [status >> 4, status | (@channel & 0x0F), _clamp7(@data1), _clamp7(@data2)]
      elsif REALTIME_STATUS[@type]
        [0xF, REALTIME_STATUS[@type], 0, 0]
      else
        @raw || [0xF, 0xFE, 0, 0]
      end
    end

    def to_s
      if channel_message?
        "#<MIDI::Message #{@type} ch=#{@channel} #{@data1} #{@data2}>"
      else
        "#<MIDI::Message #{@type}>"
      end
    end
    alias inspect to_s

    private

    def _clamp7(v)
      v = v.to_i
      v < 0 ? 0 : (v > 127 ? 127 : v)
    end
  end

  # Keeps note offs in step with note ons through a filter whose change is not
  # the same every time (random notes, a scale switched while keys are held):
  # the note on goes through the block and what it became is remembered; the
  # note off is turned into note offs for exactly that.
  #
  #   map = MIDI::NoteMap.new
  #   MIDI.filter(kb, syn, types: [:note_on, :note_off]) do |m|
  #     map.apply(m) { |x| x.note = scale.sample; x }
  #   end
  class NoteMap
    def initialize
      @held = {}   # channel * 128 + note => [[note, channel], ...] it became
    end

    # @yieldparam message [MIDI::Message] a note on, to change as a filter would
    # @return [MIDI::Message, Array, nil] what to send
    def apply(m)
      key = m.channel * 128 + m.note
      if m.type == :note_on
        r = yield(m)
        out = r.is_a?(Array) ? r : (r ? [r] : [])
        @held[key] = out.map { |x| [x.note, x.channel] }
        r
      elsif m.type == :note_off
        became = @held.delete(key)
        return m unless became
        became.map { |nc| m.with(note: nc[0], channel: nc[1]) }
      else
        m
      end
    end

    # Note offs for everything still held (e.g. before changing the filter)
    def release_all
      out = []
      @held.each_value do |list|
        list.each { |nc| out << Message.new(:note_off, nc[1], nc[0], 0) }
      end
      @held = {}
      out
    end
  end

  class << self
    # Put a Ruby block on the route from `from` to `to`.
    #
    # @param from [Object] source transport, or a MIDI::Device on it
    # @param to [Object, nil] destination; nil for a listener (see MIDI.on)
    # @param types [Symbol, Array<Symbol>, nil] message types the block gets
    #   (default: every channel message). :clock / :start / :stop /
    #   :continue only when named. Other types pass the block unchanged.
    # @param channel [Integer, nil] 0..15 to give the block only that channel
    # @param overflow [Symbol] :bypass (send on unfiltered) or :drop, when
    #   the script has not let the filters run for too long
    # @yieldparam message [MIDI::Message]
    # @yieldreturn [MIDI::Message, Array<MIDI::Message>, nil] what to send
    # @return [Proc, false] the block (for MIDI.unfilter); false when the
    #   route or filter table is full
    def filter(from, to = nil, types: nil, channel: nil, overflow: :bypass, &block)
      raise ArgumentError, "MIDI.filter: a block is required" unless block
      src = _check_source(from, "MIDI.filter")
      dst = to.nil? ? DST_RUBY : route_bit(to)
      raise ArgumentError, "MIDI.filter: destination is not a registered transport" if dst == 0
      raise ArgumentError, "MIDI.filter: source and destination are the same" if src == dst
      if !channel.nil? && (channel < 0 || channel > 15)
        raise ArgumentError, "MIDI.filter: channel must be 0..15"
      end
      unless overflow == :bypass || overflow == :drop
        raise ArgumentError, "MIDI.filter: overflow must be :bypass or :drop"
      end
      tlist = _filter_types(types)
      _filter_ready

      # A filter on a route that is not there sets up the route first
      if dst != DST_RUBY && (_route_targets_of(src) & dst) == 0
        return false unless route(from, to)
      end

      key = _pair_key(src, dst)
      list = $__midi_filters__[key] || []
      list << [tlist, channel, overflow == :drop ? 1 : 0, block, false, types]
      unless _filter_apply(src, dst, list)
        list.pop
        return false
      end
      $__midi_filters__[key] = list
      _start_source(from, src)
      block
    end

    # Remove filters on the route from `from` to `to` (every route from
    # `from` when `to` is nil), or only `handler` when given. Routes and
    # MIDI.on listeners stay.
    # @return [Integer] filters removed
    def unfilter(from, to = nil, handler = nil)
      src = route_bit(from)
      only = to.nil? ? nil : route_bit(to)
      removed = 0
      # Chains of named filters on those routes go too (their notes off)
      if $__midi_chains__.size > 0
        $__midi_chains__.values.each do |c|
          next unless c.src == src && (only.nil? || c.dst == only)
          next unless handler.nil? || handler.equal?(c.block)
          _chain_drop(c.src, c.dst)
          removed += 1
        end
      end
      skey = _pair_key(src, 0)
      $__midi_filters__.keys.each do |key|
        next unless key - key % PAIR_KEY_STRIDE == skey
        dst = key % PAIR_KEY_STRIDE
        next if dst == DST_RUBY || (!only.nil? && dst != only)
        list = $__midi_filters__[key]
        before = list.size
        if handler
          list.delete_if { |f| f[3] == handler }
        else
          list = []
        end
        removed += before - list.size
        _filter_store(src, dst, list)
      end
      removed
    end

    # Listen to what arrives on `from`: the block gets each message of
    # `type` (:any = every channel message). Its return value is ignored.
    #
    #   MIDI.on(kb, :note_on) { |m| puts m.note }
    #   MIDI.on(kb, :clock)   { |m| ... }      # system realtime only when named
    #
    # @return [Proc, false] the block (for MIDI.off)
    def on(from, type = :any, channel: nil, &block)
      types = type == :any ? nil : type
      handler = filter(from, nil, types: types, channel: channel, &block)
      return false unless handler
      # Remember how it was asked for, for MIDI.off(from, type)
      $__midi_filters__[_pair_key(route_bit(from), DST_RUBY)].last[5] = type
      handler
    end

    # Stop listening on `from`: every listener, those for `type`, or the
    # one `handler`.
    # @return [Integer] listeners removed
    def off(from, type = nil, handler = nil)
      src = route_bit(from)
      key = _pair_key(src, DST_RUBY)
      list = $__midi_filters__[key]
      return 0 unless list
      before = list.size
      list.delete_if do |f|
        (handler.nil? || f[3] == handler) && (type.nil? || f[5] == type)
      end
      _filter_store(src, DST_RUBY, list)
      before - list.size
    end

    # External clock tempo arriving on `from` (USB host, UART or USB device;
    # 0.0 until enough clocks have arrived). With no argument, that of the
    # first MIDI::Input (deprecated).
    def external_bpm(from = nil)
      if from.nil?
        inp = $__midi_active_inputs__.first
        return inp ? inp.external_bpm : 0.0
      end
      return _external_bpm_from(from) if from.is_a?(Integer)
      # UI::MIDIDevices.input and the like know their own source
      return from.external_bpm if from.respond_to?(:external_bpm) && !from.respond_to?(:transport)
      _external_bpm_from(route_bit(from))
    end

    # Start a new tempo measurement on `from` (e.g. on MIDI Start).
    def reset_external_clock(from)
      _reset_external_clock_from(from.is_a?(Integer) ? from : route_bit(from))
    end

    # @return [Hash] overflows: messages the queue had no room for
    def filter_stats
      { overflows: _filter_overflows }
    end

    # Run the filters on what is queued. MIDI.sleep_ms, the clock loops and
    # process_all_inputs call this; call it yourself from a loop of your own.
    # @return [Integer] events handled
    def run_filters(max = 64)
      count = 0
      $__midi_filter_hooks__.each { |h| h.call } if $__midi_filter_hooks__.size > 0
      _run_timers if $__midi_filter_timers__.size > 0
      return 0 unless $__midi_filter_ready__
      while count < max
        ev = _filter_pop
        break unless ev
        count += 1
        _filter_event(ev)
      end
      count
    end

    # Name a route source for Message#source / #device (UI::MIDIDevices
    # registers its devices here).
    def _set_source_info(bit, key, device)
      $__midi_source_info__[bit] = [key, device]
    end

    # The key of the pair (src, dst) in $__midi_filters__ / $__midi_chains__.
    # The source counts by its bit's position, so keys stay small (mruby/c's
    # Integer may be 32 bits) with destinations up to DST_RUBY.
    def _pair_key(src, dst)
      i = 0
      i += 1 while (src >> i) > 1
      i * PAIR_KEY_STRIDE + dst
    end

    # Internal: wait up to `ms` for the filter queue, running the filters as
    # events come in and MIDI::Filter#every timers as they fall due. A plain
    # sleep when there are neither.
    def _idle_ms(ms)
      ms = ms.to_i
      if $__midi_filters__.size == 0 && $__midi_filter_timers__.size == 0
        Kernel.sleep_ms(ms) if ms > 0
        return
      end
      deadline = _uptime_ms + ms
      while true
        remaining = (deadline - _uptime_ms).to_i
        break if remaining <= 0
        wait = remaining
        due = _next_timer_ms
        if due
          if due <= 0
            run_filters
            next
          end
          wait = due if due < wait
        end
        if $__midi_filters__.size > 0
          run_filters if _filter_wait(wait)
        else
          Kernel.sleep_ms(wait)
        end
      end
    end

    # Internal: run the timers that are due
    def _run_timers
      now = _uptime_ms
      $__midi_filter_timers__.dup.each do |t|
        next if t.cancelled? || t.next_at > now
        t.next_at = t.next_at + t.interval
        t.next_at = now + t.interval if t.next_at <= now
        t._fire
      end
    end

    # Internal: ms until the next timer is due (<= 0: now), nil for none
    def _next_timer_ms
      return nil if $__midi_filter_timers__.size == 0
      now = _uptime_ms
      best = nil
      $__midi_filter_timers__.each do |t|
        d = t.next_at - now
        best = d if best.nil? || d < best
      end
      best
    end

    # Internal: take down the chains on (src, dst) (nil: any), their notes
    # off and their filter entries out
    def _chain_drop(src, dst)
      $__midi_chains__.keys.each do |key|
        c = $__midi_chains__[key]
        next unless (src.nil? || c.src == src) && (dst.nil? || c.dst == dst)
        $__midi_chains__.delete(key)
        c.teardown
        list = $__midi_filters__[key]
        if list
          _filter_store(c.src, c.dst, list.reject { |f| f[3].equal?(c.block) })
        end
      end
    end

    # Internals below (not private: mruby/c has no private singleton methods)

    def _filter_ready
      return if $__midi_filter_ready__
      raise "MIDI.filter: could not create the filter queue" if _filter_init != 0
      $__midi_filter_ready__ = true
    end

    def _filter_types(types)
      return CHANNEL_TYPES if types.nil?
      list = types.is_a?(Array) ? types : [types]
      list.each do |t|
        unless Message::STATUS[t] || Message::REALTIME_STATUS[t]
          raise ArgumentError, "MIDI.filter: unknown message type #{t.inspect}"
        end
      end
      list
    end

    # Push the Ruby filters on (src, dst) down to the C table: the union of
    # their types, one channel if all agree, :drop only if all drop.
    def _filter_apply(src, dst, list)
      bits = 0
      channel = nil
      same_channel = true
      drop = true
      i = 0
      list.each do |f|
        f[0].each do |t|
          s = Message::STATUS[t]
          if s
            bits |= 1 << (s >> 4)
            bits |= 1 << 0x9 if t == :note_off   # note on, velocity 0
          else
            bits |= 1 << 0xF
          end
        end
        if i == 0
          channel = f[1]
        elsif f[1] != channel
          same_channel = false
        end
        drop = false if f[2] == 0
        i += 1
      end
      # Listening on the virtual port is listening to what is routed into it:
      # C queues those types, the channels are told apart here
      if dst == DST_RUBY && src == _route_virtual
        _route_virtual_listen(bits)
        return true
      end
      ch = (same_channel && !channel.nil?) ? channel : ROUTE_ALL_CHANNELS
      _filter_set(src, dst, bits, ch, drop ? 1 : 0) == 0
    end

    # Store (src, dst)'s list, or drop it from Ruby and C when empty.
    def _filter_store(src, dst, list)
      key = _pair_key(src, dst)
      if list.size == 0
        $__midi_filters__.delete(key)
        if dst == DST_RUBY && src == _route_virtual
          _route_virtual_listen(0)
        else
          _filter_clear(src, dst)
        end
      else
        $__midi_filters__[key] = list
        _filter_apply(src, dst, list)
      end
    end

    def _filter_event(ev)
      dst = ev[0]
      src = ev[1]
      info = $__midi_source_info__[src]
      source = info ? info[0] : BUILTIN_SOURCES[src]
      device = info ? info[1] : nil
      skey = _pair_key(src, 0)
      bit = 1
      while bit <= DST_RUBY
        if (dst & bit) != 0
          list = $__midi_filters__[skey + bit]
          if list
            # A fresh Message per destination: the blocks may change it
            m = Message._from_packet(ev[2], ev[3], ev[4], ev[5], source, device)
            if bit == DST_RUBY
              _filter_listen(list, m)
            else
              _filter_chain(list, m, bit)
            end
          elsif bit != DST_RUBY
            # Its filter went away after the packet was queued: send it as is
            _deliver(bit, ev[2], ev[3], ev[4], ev[5], source, device)
          end
        end
        bit = bit << 1
      end
      # Routed into the virtual port: its listeners (MIDI.on(port))
      if (dst & DST_VIRTUAL) != 0
        _deliver(_route_virtual, ev[2], ev[3], ev[4], ev[5], source, device)
      end
    end

    # One packet to a route destination after the filters: a transport, or
    # for the virtual port, its listeners (into the port is the program's
    # input; a send to its transport would be output from it)
    def _deliver(dst, cin, b1, b2, b3, source = nil, device = nil)
      virt = _route_virtual
      if virt == 0 || dst != virt
        _transport_send(dst, cin, b1, b2, b3)
        return
      end
      list = $__midi_filters__[_pair_key(virt, DST_RUBY)]
      _filter_listen(list, Message._from_packet(cin, b1, b2, b3, source, device)) if list
    end

    def _filter_matches?(f, m)
      return false unless f[0].include?(m.type)
      f[1].nil? || !m.channel_message? || f[1] == m.channel
    end

    def _filter_call(f, m)
      f[3].call(m)
    rescue => e
      unless f[4]
        f[4] = true
        puts "[MIDI] filter error: #{e.message} (message dropped; reported once)"
      end
      nil
    end

    def _filter_listen(list, m)
      list.each do |f|
        _filter_call(f, m) if _filter_matches?(f, m)
      end
    end

    def _filter_chain(list, m, dst)
      msgs = [m]
      list.each do |f|
        out = []
        msgs.each do |x|
          if _filter_matches?(f, x)
            r = _filter_call(f, x)
            if r.is_a?(Array)
              r.each { |y| out << y if y.is_a?(Message) }
            elsif r.is_a?(Message)
              out << r
            end
          else
            out << x
          end
        end
        msgs = out
        break if msgs.size == 0
      end
      msgs.each do |x|
        p = x.raw
        _deliver(dst, p[0], p[1], p[2], p[3], x.source, x.device)
      end
    end
  end
end

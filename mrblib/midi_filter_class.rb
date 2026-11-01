# Named, reusable filters: MIDI::Filter and the chains that run them
#
# A MIDI::Filter is a class of filter (a chord harmonizer, an arpeggiator)
# with parameters it declares. An instance registered under a name is a
# prototype; putting the name on a route makes a fresh instance for that
# route, so state and parameter values are per route:
#
#   class Octave < MIDI::Filter
#     label "Octave"
#     types :note_on, :note_off
#     param :shift, :int, range: -3..3, default: 1
#
#     def call(m)
#       m.note += 12 * params[:shift]
#       m
#     end
#   end
#   MIDI.register_filter(:octave, Octave.new)
#
#   MIDI.route(kb, syn).chain(:octave, [:arp, { rate: :"1/8" }])
#
# The filters of a route run in the order given (a chain); a chain takes
# one entry of the C filter table, in front of the route's MIDI.filter
# blocks. See docs/MIDI_FILTER_REGISTRY.md.
#
# mruby only (keyword splats, class-level declarations): left out of the
# mruby/c build by mrbgem.rake.

$__midi_filter_registry__ = {}        # name => prototype, in registration order
# Called with (event, name): :register / :unregister
$__midi_filter_registry_hooks__ = []
# Called with (event, chain, filter, extra): :attach / :detach (a filter
# instance became active on a route or stopped being), :param (extra = the
# parameter name), :chain (the chain's names changed; filter nil)
$__midi_chain_hooks__ = []
# Class => its declarations, see MIDI::Filter._decl
$__midi_filter_decls__ = {}
# Chains that want MIDI::Clock's ticks, and the listener feeding them
$__midi_clock_chains__ = []
$__midi_clock_handler__ = nil
$__midi_clock_tick__ = 0
# bit => MIDI::Device, for Filter#output / #source (UI::MIDIDevices sets it)
$__midi_device_resolver__ = nil

module MIDI
  class Filter
    KINDS = [:int, :enum, :bool, :note, :channel]
    MAX_PARAMS = 8
    MAX_CHOICES = 8
    NAME_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789_"
    NAME_MAX = 15

    # A send-only transport for a registry bit, for #output when nothing
    # better is known about the destination
    class BitTransport
      attr_reader :transport_id

      def initialize(bit)
        @transport_id = bit
      end

      def send_packet(cable, cin, midi1, midi2, midi3)
        MIDI._transport_send(@transport_id, cin, midi1, midi2, midi3)
      end

      def connected?
        true
      end

      def bytes_available
        0
      end

      def read_available
        nil
      end

      def device_info
        "bit 0x#{@transport_id.to_s(16)}"
      end
    end

    # What every(ms) returns
    class Timer
      attr_reader :filter, :interval
      attr_accessor :next_at

      def initialize(filter, interval, block)
        @filter = filter
        @interval = interval
        @block = block
        @next_at = MIDI._uptime_ms + interval
        @cancelled = false
      end

      def cancel
        @cancelled = true
        $__midi_filter_timers__.delete(self)
        nil
      end

      def cancelled?
        @cancelled
      end

      def _fire
        @filter._guard { @block.call }
      end
    end

    # --- Declarations (class level) ---------------------------------------

    class << self
      # Name shown on the screen (default: the registered name)
      def label(text = nil)
        return _decl(:label) if text.nil?
        _own[:label] = text.to_s
      end

      # Message types call gets (default: every channel message)
      def types(*list)
        return _decl(:types) || MIDI::CHANNEL_TYPES if list.empty?
        list = list[0] if list.size == 1 && list[0].is_a?(Array)
        _own[:types] = MIDI._filter_types(list)
      end

      # Only this channel (0..15) goes to call
      def channel(ch = :__get)
        return _decl(:channel) if ch == :__get
        if !ch.nil? && (!ch.is_a?(Integer) || ch < 0 || ch > 15)
          raise ArgumentError, "MIDI::Filter.channel must be 0..15"
        end
        _own[:channel] = ch
      end

      # on_clock / on_start / on_stop from MIDI::Clock (:internal) or from
      # the route's source (:source)
      def clock(source = :__get)
        return _decl(:clock) if source == :__get
        unless source.nil? || source == :internal || source == :source
          raise ArgumentError, "MIDI::Filter.clock must be :internal or :source"
        end
        _own[:clock] = source
      end

      # Declare a parameter. kind is :int (range:), :enum (choices:),
      # :bool, :note (0..127) or :channel (nil or 0..15).
      def param(name, kind, range: nil, choices: nil, default: nil, label: nil, step: 1)
        name = name.to_sym
        raise ArgumentError, "param #{name}: unknown kind #{kind.inspect}" unless KINDS.include?(kind)
        spec = { name: name, kind: kind, label: (label || name).to_s, step: step }
        case kind
        when :int
          raise ArgumentError, "param #{name}: :int needs range:" unless range
          spec[:min] = range.first
          spec[:max] = range.exclude_end? ? range.last - 1 : range.last
        when :enum
          if !choices.is_a?(Array) || choices.empty? || choices.size > MAX_CHOICES
            raise ArgumentError, "param #{name}: :enum needs 1..#{MAX_CHOICES} choices:"
          end
          spec[:choices] = choices
          spec[:min] = 0
          spec[:max] = choices.size - 1
        when :bool
          spec[:min] = 0
          spec[:max] = 1
        when :note
          spec[:min] = 0
          spec[:max] = 127
        when :channel
          spec[:min] = -1
          spec[:max] = 15
        end
        spec[:default] = _default_for(spec, default)
        own = (_own[:params] ||= [])
        own.delete_if { |s| s[:name] == name }
        own << spec
        if param_specs.size > MAX_PARAMS
          own.pop
          raise ArgumentError, "param #{name}: at most #{MAX_PARAMS} parameters"
        end
        name
      end

      # Every parameter, the parent class's first
      def param_specs
        list = superclass.respond_to?(:param_specs) ? superclass.param_specs.dup : []
        own = ($__midi_filter_decls__[self] || {})[:params]
        if own
          own.each do |s|
            i = list.index { |x| x[:name] == s[:name] }
            if i
              list[i] = s
            else
              list << s
            end
          end
        end
        list
      end

      def param_spec(name)
        param_specs.find { |s| s[:name] == name }
      end

      # Internal

      def _own
        $__midi_filter_decls__[self] ||= {}
      end

      # A declaration, looked up the class chain
      def _decl(key)
        k = self
        while k
          d = $__midi_filter_decls__[k]
          return d[key] if d && d.key?(key)
          break if k == MIDI::Filter
          k = k.superclass
        end
        nil
      end

      def _default_for(spec, value)
        if value.nil?
          case spec[:kind]
          when :int     then return spec[:min] <= 0 && spec[:max] >= 0 ? 0 : spec[:min]
          when :enum    then return spec[:choices][0]
          when :bool    then return false
          when :note    then return 60
          when :channel then return nil
          end
        end
        _normalize(spec, value)
      end

      # A value for spec, kept in range: Integers for :enum are indexes and
      # 0 / 1 are taken for :bool (what the screen and NVS keep)
      def _normalize(spec, value)
        case spec[:kind]
        when :int, :note
          v = value.to_i
          v < spec[:min] ? spec[:min] : (v > spec[:max] ? spec[:max] : v)
        when :enum
          if value.is_a?(Integer)
            spec[:choices][value] || spec[:default]
          elsif spec[:choices].include?(value)
            value
          elsif value.is_a?(String) && spec[:choices].include?(value.to_sym)
            value.to_sym
          else
            raise ArgumentError, "param #{spec[:name]}: #{value.inspect} is not one of #{spec[:choices].inspect}"
          end
        when :bool
          value.is_a?(Integer) ? value != 0 : (value ? true : false)
        when :channel
          return nil if value.nil? || (value.is_a?(Integer) && value < 0)
          v = value.to_i
          v > 15 ? 15 : v
        end
      end

      # The integer the screen and NVS keep for value
      def _code(spec, value)
        case spec[:kind]
        when :enum    then spec[:choices].index(value) || 0
        when :bool    then value ? 1 : 0
        when :channel then value.nil? ? -1 : value
        else value
        end
      end
    end

    # --- Instances ----------------------------------------------------------

    # @param params [Hash] values for the declared parameters (the rest
    #   take their defaults). Subclasses put their own set-up in #setup; one
    #   that overrides initialize must take keywords only and call
    #   super(**params).
    def initialize(**params)
      @params = {}
      self.class.param_specs.each { |s| @params[s[:name]] = s[:default] }
      params.each do |k, v|
        spec = self.class.param_spec(k.to_sym)
        raise ArgumentError, "#{self.class}: unknown parameter #{k.inspect}" unless spec
        @params[spec[:name]] = self.class._normalize(spec, v)
      end
      @name = nil
      @chain = nil
      @index = nil
      @warned = false
    end

    # A fresh instance for a route: this one's parameter values, then params
    def instantiate(params = nil)
      merged = @params.dup
      params.each { |k, v| merged[k.to_sym] = v } if params
      known = {}
      merged.each { |k, v| known[k] = v if self.class.param_spec(k) }
      f = self.class.new(**known)
      f._name = @name
      f._label = @label
      f
    end

    # --- To override -----------------------------------------------------

    # Attached to a route (params are set)
    def setup
    end

    # One message. Return a Message to send it, an Array to send several,
    # nil / false to drop it.
    def call(m)
      m
    end

    # Detached. The notes the chain sent are already turned off.
    def teardown
    end

    def param_changed(name, value)
    end

    # Build an operating panel: ui.layout(cols:, rows:) { ... } (see
    # UI::MIDIDevices). No panel unless this calls ui.layout.
    def layout(ui)
    end

    def on_clock(tick)
    end

    def on_start
    end

    def on_stop
    end

    # --- For subclasses -----------------------------------------------------

    # { name => value }
    def params
      @params
    end

    # Set a parameter (kept in range); runs param_changed and tells the
    # screen. Returns the value set.
    def set_param(name, value)
      spec = self.class.param_spec(name.to_sym)
      raise ArgumentError, "#{self.class}: unknown parameter #{name.inspect}" unless spec
      v = self.class._normalize(spec, value)
      return v if @params[spec[:name]] == v
      @params[spec[:name]] = v
      _guard { param_changed(spec[:name], v) } if @chain
      @chain._param_set(self, spec[:name]) if @chain
      v
    end

    # Send messages on from here: through the filters after this one, then
    # to the destination. For filters that play by themselves (on_clock /
    # every); call's return value is the way to send from call.
    def emit(msgs)
      @chain._emit_from(@index + 1, msgs) if @chain
      nil
    end

    # Run the block every ms milliseconds (while attached). Returns a
    # handle; #cancel stops it.
    def every(ms, &block)
      raise ArgumentError, "every: a block is required" unless block
      ms = ms.to_i
      ms = 1 if ms < 1
      t = Timer.new(self, ms, block)
      $__midi_filter_timers__ << t
      t
    end

    # The destination's MIDI::Device (sysex and the like go here directly)
    def output
      @chain ? @chain.output : nil
    end

    # The source's MIDI::Device
    def source
      @chain ? @chain.source : nil
    end

    # The MIDI::Route this filter is on
    def route
      @chain ? @chain.route : nil
    end

    def name
      @name
    end

    # The name on the screen: the one given to register_filter (label:), else
    # the class's, else the registered name
    def label
      @label || self.class.label || @name.to_s
    end

    def attached?
      !@chain.nil?
    end

    def to_s
      "#<#{self.class} #{@name.inspect}>"
    end
    alias inspect to_s

    # --- Internal ---------------------------------------------------------

    def _name=(name)
      @name = name
    end

    def _label=(text)
      @label = text
    end

    def _attach(chain, index)
      @chain = chain
      @index = index
    end

    def _index=(index)
      @index = index
    end

    def _detach
      $__midi_filter_timers__.delete_if { |t| t.filter.equal?(self) }
      @chain = nil
      @index = nil
    end

    # Whether call takes m
    def _takes?(m)
      return false unless self.class.types.include?(m.type)
      ch = self.class.channel
      ch.nil? || !m.channel_message? || ch == m.channel
    end

    # Run a block of the filter's, reporting its first error only
    def _guard
      yield
    rescue => e
      unless @warned
        @warned = true
        puts "[MIDI] filter #{@name || self.class} error: #{e.message} (reported once)"
      end
      nil
    end
  end

  # The filters on one route, in order. Made by MIDI.chain.
  class FilterChain
    MAX = 4
    CLOCK_TYPES = [:clock, :start, :stop, :continue]

    attr_reader :src, :dst, :route, :filters, :block

    def initialize(src, dst, route)
      @src = src
      @dst = dst
      @route = route
      @specs = []         # [[name, {param => value}], ...] as asked for
      @filters = []       # Instances, nil where the name is not registered
      @sounding = {}      # channel * 128 + note => note ons sent, not yet off
      @tick = 0           # Source clock ticks (clock :source)
      chain = self
      @block = Proc.new { |m| chain._process(m) }
    end

    # [[name, params], ...]: the names and each filter's current values
    def specs
      out = []
      @specs.each_with_index do |s, i|
        f = @filters[i]
        out << [s[0], f ? f.params.dup : s[1]]
      end
      out
    end

    def names
      @specs.map { |s| s[0] }
    end

    # The filters' C types, as a MIDI.filter entry wants them
    def types
      list = []
      @filters.each do |f|
        next unless f
        f.class.types.each { |t| list << t unless list.include?(t) }
        if f.class.clock == :source
          CLOCK_TYPES.each { |t| list << t unless list.include?(t) }
        end
      end
      list
    end

    def channel
      chans = []
      @filters.each { |f| chans << f.class.channel if f }
      return nil if chans.empty?
      first = chans[0]
      chans.all? { |c| c == first } ? first : nil
    end

    def output
      MIDI._device_of(@route ? @route.to : nil, @dst)
    end

    def source
      MIDI._device_of(@route ? @route.from : nil, @src)
    end

    # Replace the chain with specs ([[name, params], ...]): filters that
    # stay (same name at any place) keep their instance and state.
    def set(specs)
      old = @filters
      old_names = names
      new_filters = []
      specs.each do |s|
        i = old_names.index(s[0])
        if i && old[i]
          f = old[i]
          old[i] = nil
          old_names[i] = nil
          s[1].each { |k, v| f.set_param(k, v) } if s[1]
          new_filters << f
        else
          new_filters << _make(s[0], s[1])
        end
      end
      gone = old.compact
      release unless gone.empty?
      gone.each { |f| _drop(f) }
      @specs = specs.map { |s| [s[0], s[1] || {}] }
      @filters = new_filters
      @filters.each_with_index do |f, i|
        next unless f
        if f.attached?
          f._index = i
        else
          f._attach(self, i)
          f._guard { f.setup }
          MIDI._chain_hook(:attach, self, f, nil)
        end
      end
      _clock_sync
      MIDI._chain_hook(:chain, self, nil, nil)
      self
    end

    # A registered name came (back) or went: make or drop its instances
    def refresh(name)
      i = 0
      changed = false
      while i < @specs.size
        if @specs[i][0] == name
          f = @filters[i]
          params = f ? f.params.dup : @specs[i][1]
          if f
            release
            _drop(f)
          end
          nf = _make(name, params)
          @filters[i] = nf
          if nf
            nf._attach(self, i)
            nf._guard { nf.setup }
            MIDI._chain_hook(:attach, self, nf, nil)
          end
          changed = true
        end
        i += 1
      end
      _clock_sync if changed
      changed
    end

    # Off: every filter detached, the notes sent turned off
    def teardown
      release
      @filters.each { |f| _drop(f) if f }
      @filters = []
      @specs = []
      $__midi_clock_chains__.delete(self)
      MIDI._clock_listen
      MIDI._chain_hook(:chain, self, nil, nil)
    end

    # Note off for every note this chain sent and has not turned off
    def release
      @sounding.each_key do |key|
        MIDI._deliver(@dst, 0x8, 0x80 | (key / 128), key % 128, 0)
      end
      @sounding = {}
    end

    # A message from the route (MIDI.filter's block)
    def _process(m)
      if CLOCK_TYPES.include?(m.type)
        _clock_event(m, :source)
        return m
      end
      out = _run_from(0, [m])
      out.each { |x| _track(x) }
      out
    end

    # Through filters i.. then straight to the destination
    def _emit_from(i, msgs)
      list = msgs.is_a?(Array) ? msgs : [msgs]
      list = list.select { |x| x.is_a?(Message) }
      _run_from(i, list).each do |x|
        _track(x)
        p = x.raw
        MIDI._deliver(@dst, p[0], p[1], p[2], p[3], x.source, x.device)
      end
    end

    def _run_from(i, msgs)
      while i < @filters.size && !msgs.empty?
        f = @filters[i]
        if f
          out = []
          msgs.each do |x|
            if f._takes?(x)
              r = f._guard { f.call(x) }
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
        end
        i += 1
      end
      msgs
    end

    # A tick, start or stop for the filters following that clock
    def _clock_event(m, from)
      if from == :source
        if m.type == :clock
          tick = @tick
          @tick += 1
        else
          @tick = 0 if m.type == :start
        end
      else
        tick = $__midi_clock_tick__
      end
      @filters.each do |f|
        next unless f && f.class.clock == from
        case m.type
        when :clock then f._guard { f.on_clock(tick) }
        when :start then f._guard { f.on_start }
        when :stop  then f._guard { f.on_stop }
        end
      end
    end

    def _param_set(filter, name)
      i = @filters.index { |f| f.equal?(filter) }
      @specs[i][1] = filter.params.dup if i
      MIDI._chain_hook(:param, self, filter, name)
    end

    def _track(m)
      key = (m.channel & 0x0F) * 128 + (m.data1.to_i & 0x7F)
      if m.type == :note_on
        @sounding[key] = (@sounding[key] || 0) + 1
      elsif m.type == :note_off
        n = (@sounding[key] || 0) - 1
        if n > 0
          @sounding[key] = n
        else
          @sounding.delete(key)
        end
      end
    end

    private

    def _make(name, params)
      proto = $__midi_filter_registry__[name]
      return nil unless proto
      begin
        proto.instantiate(params)
      rescue => e
        puts "[MIDI] filter #{name}: #{e.message}"
        nil
      end
    end

    def _drop(f)
      f._guard { f.teardown }
      f._detach
      MIDI._chain_hook(:detach, self, f, nil)
    end

    def _clock_sync
      wants = @filters.any? { |f| f && f.class.clock == :internal }
      if wants
        $__midi_clock_chains__ << self unless $__midi_clock_chains__.include?(self)
      else
        $__midi_clock_chains__.delete(self)
      end
      MIDI._clock_listen
    end
  end

  class Route
    # The route's chain of named filters. With names, set it (in this
    # order; [name, {param => value}] for values); with [] or nil, clear
    # it; with nothing, the filter instances.
    def chain(*specs)
      return MIDI.chain_of(@from, @to) if specs.empty?
      MIDI.chain(@from, @to, *specs)
    end

    def unchain
      MIDI.chain(@from, @to, [])
    end
  end

  class << self
    # Register a filter (a MIDI::Filter instance, the prototype) under a
    # name. The same name again replaces it; routes using it get a new
    # instance and keep their parameter values. label: is the name on the
    # screen (default: the class's label) -- give one when a class is
    # registered more than once, as presets, so the buttons tell them apart.
    # @return [MIDI::Filter, false] false when 16 are registered already
    def register_filter(name, filter, label: nil)
      raise ArgumentError, "register_filter: #{filter.inspect} is not a MIDI::Filter" unless filter.is_a?(Filter)
      name = _filter_name(name)
      replacing = $__midi_filter_registry__.key?(name)
      return false if !replacing && $__midi_filter_registry__.size >= MIDI_FILTER_REGISTRY_MAX
      filter._name = name
      filter._label = label.to_s if label
      $__midi_filter_registry__[name] = filter
      $__midi_filter_registry_hooks__.each { |h| h.call(:register, name) }
      $__midi_chains__.each_value { |c| c.refresh(name) && _chain_apply(c) }
      filter
    end

    # @return [Boolean] whether it was registered
    def unregister_filter(name)
      name = name.to_sym
      return false unless $__midi_filter_registry__.key?(name)
      $__midi_filter_registry__.delete(name)
      $__midi_chains__.each_value { |c| c.refresh(name) && _chain_apply(c) }
      $__midi_filter_registry_hooks__.each { |h| h.call(:unregister, name) }
      true
    end

    # { name => prototype }, in registration order
    def filters
      $__midi_filter_registry__
    end

    def registered_filter(name)
      $__midi_filter_registry__[name.to_sym]
    end

    # Put named filters on the route from `from` to `to`, in order,
    # replacing its chain; [] clears it. Each is a name or [name, {param =>
    # value}]. The route is made when there is none.
    # @return [Array<MIDI::Filter, nil>, false] the instances (nil for a name
    #   not registered), false when the route or filter table is full
    def chain(from, to, *specs)
      src = _check_source(from, "MIDI.chain")
      dst = route_bit(to)
      raise ArgumentError, "MIDI.chain: destination is not a registered transport" if dst == 0
      raise ArgumentError, "MIDI.chain: source and destination are the same" if src == dst
      list = _chain_specs(specs)
      key = _pair_key(src, dst)
      c = $__midi_chains__[key]
      if list.empty?
        _chain_drop(src, dst) if c
        return []
      end
      if (_route_targets_of(src) & dst) == 0
        return false unless route(from, to)
      end
      _filter_ready
      unless c
        c = FilterChain.new(src, dst, Route.new(from, to, nil))
        $__midi_chains__[key] = c
      end
      c.set(list)
      return false unless _chain_apply(c)
      _start_source(from, src)
      c.filters
    end

    # The instances on the route from `from` to `to` ([] for none)
    def chain_of(from, to)
      c = $__midi_chains__[_pair_key(route_bit(from), route_bit(to))]
      c ? c.filters : []
    end

    # Every chain: { [src, dst] => FilterChain }
    def chains
      out = {}
      $__midi_chains__.each { |k, c| out[[c.src, c.dst]] = c }
      out
    end

    # Internal below

    def _filter_name(name)
      s = name.to_s
      ok = !s.empty? && s.size <= Filter::NAME_MAX
      s.each_char { |ch| ok = false unless Filter::NAME_CHARS.include?(ch) } if ok
      raise ArgumentError, "filter name #{name.inspect}: use a-z, 0-9 and _ (up to #{Filter::NAME_MAX})" unless ok
      s.to_sym
    end

    def _chain_specs(specs)
      specs = [] if specs.size == 1 && (specs[0].nil? || specs[0] == [])
      raise ArgumentError, "MIDI.chain: at most #{FilterChain::MAX} filters" if specs.size > FilterChain::MAX
      specs.map do |s|
        if s.is_a?(Array)
          raise ArgumentError, "MIDI.chain: #{s.inspect} is not [name, {params}]" unless s.size <= 2 && (s[1].nil? || s[1].is_a?(Hash))
          [s[0].to_sym, s[1] || {}]
        else
          [s.to_sym, {}]
        end
      end
    end

    # Put the chain's entry at the head of its route's filter list, or take
    # it out when no filter is live (only unregistered names)
    def _chain_apply(c)
      key = _pair_key(c.src, c.dst)
      list = ($__midi_filters__[key] || []).reject { |f| f[3].equal?(c.block) }
      ty = c.types
      unless ty.empty?
        list.unshift([ty, c.channel, 0, c.block, false, :__chain])
      end
      if list.empty?
        $__midi_filters__.delete(key)
        _filter_clear(c.src, c.dst)
        return true
      end
      $__midi_filters__[key] = list
      _filter_apply(c.src, c.dst, list)
    end

    def _chain_hook(event, chain, filter, extra)
      $__midi_chain_hooks__.each { |h| h.call(event, chain, filter, extra) }
    end

    def _device_of(obj, bit)
      return obj if obj.is_a?(Device)
      r = $__midi_device_resolver__
      d = r ? r.call(bit) : nil
      return d if d
      if obj && !obj.is_a?(Integer) && obj.respond_to?(:send_packet)
        return Device.new(obj)
      end
      Device.new(Filter::BitTransport.new(bit))
    end

    # One listener on MIDI::Clock while some chain has a clock :internal
    # filter
    def _clock_listen
      want = $__midi_clock_chains__.size > 0
      bit = Clock.transport_id
      if want && $__midi_clock_handler__.nil? && bit != 0
        $__midi_clock_handler__ = filter(bit, nil, types: FilterChain::CLOCK_TYPES) do |m|
          if m.type == :start
            $__midi_clock_tick__ = 0
          end
          $__midi_clock_chains__.each { |c| c._clock_event(m, :internal) }
          $__midi_clock_tick__ += 1 if m.type == :clock
        end
      elsif !want && $__midi_clock_handler__
        off(bit, nil, $__midi_clock_handler__)
        $__midi_clock_handler__ = nil
      end
    end
  end

  # register_filter's limit (the screen's list in C has as many rows)
  MIDI_FILTER_REGISTRY_MAX = 16
end

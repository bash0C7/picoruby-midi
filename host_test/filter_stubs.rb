# Stand-ins for the C bindings filter_test.rb exercises (see there).

$__midi_active_inputs__ = []
$stub = { routes: [], filters: {}, queue: [], sent: [], started: 0 }

module Kernel
  def self.sleep_ms(ms); end
end

module MIDI
  class << self
    def _uptime_ms; 0; end
    def _route_sources; 0x07 | ($stub[:virtual] || 0); end
    def _route_virtual; $stub[:virtual] || 0; end
    def _route_virtual_listen(types); $stub[:vlisten] = types; end

    def _route_add(src, dst, ch)
      $stub[:routes] << [src, dst, ch]
      0
    end

    def _route_add_sel(src, dst, sel)
      $stub[:routes] << [src, dst, MIDI::ROUTE_SEVERAL, sel]
      0
    end

    def _route_remove(src, dst)
      before = $stub[:routes].size
      $stub[:routes].delete_if { |r| r[0] == src && (r[1] & dst) != 0 }
      before - $stub[:routes].size
    end

    def _route_clear
      $stub[:routes] = []
      $stub[:filters] = {}
    end

    def _route_start
      $stub[:started] += 1
      0
    end

    def _route_targets_of(src)
      all = 0
      $stub[:routes].each { |r| all |= r[1] if r[0] == src }
      all
    end

    def _transport_send(mask, cin, b1, b2, b3)
      $stub[:sent] << [mask, cin, b1, b2, b3]
    end

    def _route_watch(mask); $stub[:watch] = mask; end
    def _filter_init; 0; end

    def _filter_set(src, dst, types, ch, overflow)
      return -1 if $stub[:full]
      $stub[:filters][[src, dst]] = [types, ch, overflow]
      0
    end

    def _filter_clear(src, dst)
      $stub[:filters].delete([src, dst]) ? 1 : 0
    end

    def _filter_pop; $stub[:queue].shift; end
    def _filter_wait(ms); !$stub[:queue].empty?; end
    def _filter_overflows; 0; end
    def _external_bpm_from(src); 120.0; end
    def _reset_external_clock_from(src); end
  end
end

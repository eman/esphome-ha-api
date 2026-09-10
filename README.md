<p align="center">
  <img src="docs/banner.png" width="900"
       alt="Home Assistant History - ESPHome sensors that arrive with their history already loaded. A chart of a day's data is complete at boot; live values extend it from there.">
</p>

ESPHome's built-in `homeassistant` sensor gives a device live values pushed over the native
API — but only from the moment it connects. A panel drawing a time-series graph boots to an
empty chart and spends hours filling it in.

A `ha_history` sensor behaves **exactly like a `homeassistant` sensor** — same `entity_id`,
same live pushes, same `NaN` for `unavailable` — and additionally keeps one value per bucket
(5 minutes or an hour) over a window (a rolling duration, or today so far). On startup it asks
Home Assistant for the recent past and hands your renderer a filled buffer within a few
hundred milliseconds of connecting. From then on the live pushes extend the same buffer, so
there is no seam between "history" and "now".

```yaml
external_components:
  - source: github://eman/esphome-ha-history
    components: [ha_history]

time:
  - platform: homeassistant

sensor:
  - platform: ha_history
    id: pv
    entity_id: sensor.pv_power
    history:
      window: today      # or 24h, 3d, ...
      bucket: 5min       # or hour
      statistic: mean    # mean | min | max | state | change
    on_history_loaded:
      - lambda: |-
          ESP_LOGI("pv", "%u points, %.2f..%.2f",
                   (unsigned) id(pv).history_size(), id(pv).history_min(), id(pv).history_max());
    on_history_update:   # a bucket closed, or the day rolled over
      - script.execute: redraw
```

No credentials, no HTTP client, no InfluxDB. Everything rides the encrypted API connection
Home Assistant already holds to the device.

## How it works

History comes from Home Assistant's **recorder statistics** — the same 5-minute and hourly
aggregates that draw the graphs in Home Assistant's own UI. The hub sends the
`recorder.get_statistics` action over the native API with a `response_template`, and Home
Assistant renders that Jinja *server-side* into a compact list of `[bucket index, value]` pairs
before it crosses the wire: a full day at 5-minute resolution is ~3–5 KB rather than the 26 KB
of raw statistics rows.

Live pushes are then folded into the current bucket the way Home Assistant computes its own
statistics: the `mean` is time-weighted, the value in force at the bucket boundary is carried
in, and `unavailable` holds the previous value rather than poisoning the bucket. Matching that
arithmetic is what makes backfilled and live buckets indistinguishable.

Seven minutes after the first load, one narrow follow-up request repairs the bucket that was in
progress at boot — the only one that can't match, because the device saw only part of it and
Home Assistant's newest row lags the clock by up to five minutes.

## Requirements

- **Home Assistant 2025.x or newer**, for action responses in the ESPHome integration. Older
  versions silently never reply.
- The device adopted in the ESPHome integration, with **"Allow the device to perform Home
  Assistant actions"** enabled in the device's options. It defaults to off. When it is off,
  Home Assistant sends *nothing back* — no error — so the only symptom is a timeout; the hub
  names this option in its warning after two of them.
- Entities with a **`state_class`** (`measurement`, `total` or `total_increasing`). That is
  what gives an entity statistics. Text sensors, binary sensors and plain sensors have none,
  and their raw history is far too large to pull onto a microcontroller (one power sensor
  measured 577 KB for a day; its statistics were 26 KB).
- `time: platform: homeassistant` (or another time source with the same timezone as HA), so
  `today` and the bucket boundaries agree with Home Assistant's.
- **ESP32 family.** The ESP8266's 8 KiB API frame and 40 KB heap are not enough.

## Configuration

### `ha_history:` (hub, optional)

Sets defaults for every sensor. Auto-loaded by the platform; only write it to change a default.

| Key | Default | |
|---|---|---|
| `window` | `24h` | `today`, or a duration (`24h`, `3d`, …) |
| `bucket` | `5min` | `5min` or `hour` |
| `retry_interval` | `30s` | first retry after a failed request; doubles to a 10 min cap |
| `time_id` | the sole `time:` component | |

### `sensor: platform: ha_history`

Everything the `homeassistant` sensor platform accepts (`entity_id`, `name`, `id`, filters,
`internal` — default `true`, …), plus:

| Key | |
|---|---|
| `history.window` | overrides the hub default |
| `history.bucket` | overrides the hub default |
| `history.statistic` | `mean` (default), `min`, `max`, `state`, `change`. `mean/min/max` for `state_class: measurement`; `change` for `total_increasing` meters (a reset counts from zero, as HA does). |
| `on_history_loaded` | backfill merged into the buffer (fires per successful load) |
| `on_history_update` | a live bucket closed, or the day rolled over |

`attribute:` is rejected — statistics exist only for the entity's state.

### `button: platform: ha_history`

Re-runs the backfill. Exists for one moment in particular: you have just enabled "Allow the
device to perform Home Assistant actions" and the hub has already stopped retrying.

### Limits

The reply must fit one API frame (32 KiB), and **exceeding it drops the API connection** — after
which Home Assistant reconnects and the device would ask again. So the size is bounded at
config time (`esphome config` rejects the offending sensor) and guarded at runtime (a connection
lost right after a request is treated as "reply too large" and backed off for ten minutes):

- `window / bucket ≤ 1000 points` with PSRAM, `≤ 500` without. 24 h @ 5 min = 288; 7 d @ hour = 168.
- `bucket: 5min` needs `window ≤ 10 days` — Home Assistant purges 5-minute statistics after that.

## Reading the buffer

```cpp
id(pv).history_loaded()                       // backfill has landed
id(pv).history_size()                         // points held
id(pv).history_copy(starts, values, cap)      // parallel arrays, oldest first; returns count
id(pv).history_min(), id(pv).history_max()
id(pv).bucket_seconds()                       // 300 or 3600
id(pv).window_start(), id(pv).window_seconds()  // for the x axis; epoch UTC
id(pv).history()                              // const SeriesBuffer & (at(i).start / .value)
```

Gaps are represented by absence — a bucket Home Assistant has no row for is simply not in the
buffer, so map x by `start`, not by index. Bucket starts are UTC-epoch aligned, as HA's are.

### LVGL

`components/ha_history/lvgl_helpers.h` compiles when the config has an `lvgl:` block, and offers:

```cpp
using namespace esphome::ha_history;
// Filled-area sparkline into a transparent (ARGB8888) canvas; zero-based axis by default.
lvgl::sparkline(id(my_canvas), id(pv), 0xF2C230);
// Points for a `line` widget.
static lv_point_precise_t pts[320];
size_t n = lvgl::line_points(id(pv), pts, 320, width, height);
lv_line_set_points(id(my_line), pts, n);
```

See `examples/lvgl_sparkline.yaml`. Call both from `on_history_loaded` and `on_history_update`.
ESPHome has no LVGL `chart` widget (as of 2026.8), which is why these exist.

## Behaviour worth knowing

- **Backfilled buckets win** where a live bucket already exists. HA's statistics come from the
  full sample stream; the device's live bucket from whatever it happened to see.
- **Two Home Assistant instances** connected to one device both push states, so live samples
  arrive twice — harmless to a time-weighted mean. History is requested from one of them.
- **Units.** Statistics come back in the entity's *stored* unit; live pushes in its *display*
  unit. If you have changed an entity's display unit in Home Assistant, the two differ and the
  join shows a step. There is no unit metadata in the reply to detect this automatically.
- **After an API outage** longer than one bucket the history is loaded again; gaps are filled
  the same way the boot backfill fills them.
- **Failures are loud and bounded.** Three unanswered requests on one connection and the hub
  stops until the API reconnects or the reload button is pressed. Live values keep flowing
  throughout — the subscription is independent of all of this.

## Development

`components/ha_history/series.h` is the buffer and bucket arithmetic, in plain C++17 with no
ESPHome dependency. `tests/` runs it on the host:

```sh
make -C tests test
```

CI compiles both examples for `esp32` and `esp32p4` on every push.

## License

MIT.

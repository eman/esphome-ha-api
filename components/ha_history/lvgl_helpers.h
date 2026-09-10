#pragma once

// Optional LVGL glue. Compiled only when the config has an `lvgl:` block.
//
// The component's product is the buffer; these two functions are the minimum
// that makes it drawable without every user rewriting the same lambda. ESPHome
// (2026.8, and upstream main) has no LVGL `chart` widget, so the choices are
// the `line` widget or a `canvas`:
//
//   line_points()  fills an lv_point_precise_t array for lv_line_set_points()
//   sparkline()    paints a filled-area trace straight into a transparent
//                  ARGB8888 canvas's draw buffer
//
// Both take the sensor by pointer, which is what `id(my_sensor)` yields, and
// both map x over the sensor's window, so a `today` buffer occupies the
// fraction of the width that the day has used up so far, and a rolling window
// always fills it. Call from on_history_loaded and on_history_update.

#include "esphome/core/defines.h"

#ifdef USE_LVGL

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esphome/components/lvgl/lvgl_esphome.h"

#include "ha_history_sensor.h"

namespace esphome {
namespace ha_history {
namespace lvgl {

namespace detail {
inline float x_of(uint32_t start, uint32_t window_start, uint32_t window_s, int32_t w) {
  if (window_s == 0)
    return 0.0f;
  const float f = (float) ((int64_t) start - (int64_t) window_start) / (float) window_s;
  return std::min((float) (w - 1), std::max(0.0f, f * (float) (w - 1)));
}
}  // namespace detail

/// Fills `pts` for lv_line_set_points(). Returns the number of points written.
/// y runs from `y_max` at the top to `y_min` at the bottom; pass NAN for either
/// to take it from the data (with 0 always included, so a flat trace sits on
/// the floor rather than mid-air).
inline size_t line_points(const HaHistorySensor *s, lv_point_precise_t *pts, size_t cap, int32_t w, int32_t h,
                          float y_min = NAN, float y_max = NAN) {
  const auto &buf = s->history();
  const size_t n = std::min(cap, buf.size());
  if (n == 0)
    return 0;
  if (std::isnan(y_min))
    y_min = std::min(0.0f, buf.min_value());
  if (std::isnan(y_max))
    y_max = std::max(0.0f, buf.max_value());
  float span = y_max - y_min;
  if (span < 1e-9f)
    span = 1.0f;

  const uint32_t ws = s->window_start(), wl = s->window_seconds();
  for (size_t i = 0; i < n; i++) {
    const Point &p = buf.at(i);
    pts[i].x = detail::x_of(p.start, ws, wl, w);
    pts[i].y = (1.0f - (p.value - y_min) / span) * (float) (h - 1);
  }
  return n;
}

/// Filled-area sparkline into a transparent (ARGB8888) canvas.
///
/// Writes the draw buffer directly: lv_canvas_set_px() costs tens of
/// microseconds a pixel and a 244x42 trace took half a second through it.
/// `zero_based` pins the floor of the y axis at zero, which is what a power or
/// energy trace wants; a temperature trace wants false.
inline void sparkline(lv_obj_t *canvas, const HaHistorySensor *s, uint32_t rgb, uint8_t line_opa = 191,
                      uint8_t fill_top_opa = 77, uint8_t fill_bot_opa = 5, bool zero_based = true) {
  lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
  if (db == nullptr || db->data == nullptr || db->header.cf != LV_COLOR_FORMAT_ARGB8888)
    return;
  auto *px = reinterpret_cast<uint32_t *>(db->data);
  const int32_t stride = (int32_t) (db->header.stride / 4);
  const int32_t w = (int32_t) db->header.w, h = (int32_t) db->header.h;
  memset(px, 0, (size_t) stride * h * sizeof(uint32_t));

  const auto &buf = s->history();
  const size_t n = buf.size();
  if (n < 2 || w < 2 || h < 2) {
    lv_obj_invalidate(canvas);
    return;
  }

  float lo = buf.min_value(), hi = buf.max_value();
  if (zero_based) {
    lo = std::min(0.0f, lo);
    hi = std::max(0.0f, hi);
  }
  float span = hi - lo;
  if (span < 1e-9f)
    span = 1.0f;

  auto put = [&](int32_t x, int32_t y, uint8_t opa) {
    if (x < 0 || y < 0 || x >= w || y >= h)
      return;
    px[y * stride + x] = ((uint32_t) opa << 24) | (rgb & 0x00FFFFFFu);
  };

  const uint32_t ws = s->window_start(), wl = s->window_seconds();
  // Walk columns; interpolate between the two buckets that straddle each one.
  size_t seg = 0;
  int32_t prev_y = -1;
  const int32_t x_first = (int32_t) detail::x_of(buf.at(0).start, ws, wl, w);
  const int32_t x_last = (int32_t) detail::x_of(buf.at(n - 1).start, ws, wl, w);
  for (int32_t x = x_first; x <= x_last; x++) {
    while (seg + 2 < n && detail::x_of(buf.at(seg + 1).start, ws, wl, w) < (float) x)
      seg++;
    const float xa = detail::x_of(buf.at(seg).start, ws, wl, w);
    const float xb = detail::x_of(buf.at(seg + 1).start, ws, wl, w);
    const float f = xb > xa ? std::min(1.0f, std::max(0.0f, ((float) x - xa) / (xb - xa))) : 0.0f;
    const float v = buf.at(seg).value + (buf.at(seg + 1).value - buf.at(seg).value) * f;
    int32_t y = (int32_t) std::lround((1.0f - (v - lo) / span) * (float) (h - 1));
    y = std::min<int32_t>(h - 1, std::max<int32_t>(0, y));
    const int32_t y0 = (int32_t) std::lround((1.0f - (0.0f - lo) / span) * (float) (h - 1));  // the zero line
    const int32_t floor_y = zero_based ? std::min<int32_t>(h - 1, std::max<int32_t>(0, y0)) : h - 1;

    // Area fill from the trace to the floor, fading toward the floor.
    const int32_t a = std::min<int32_t>(y, floor_y), b = std::max<int32_t>(y, floor_y);
    for (int32_t fy = a; fy <= b; fy++) {
      const float g = (b > a) ? (float) (fy - a) / (float) (b - a) : 0.0f;
      put(x, fy, (uint8_t) std::lround(fill_top_opa + (fill_bot_opa - fill_top_opa) * g));
    }
    // Stroke, bridging vertical jumps so a steep ramp stays continuous.
    put(x, y, line_opa);
    if (prev_y >= 0)
      for (int32_t sy = std::min<int32_t>(prev_y, y); sy <= std::max<int32_t>(prev_y, y); sy++)
        put(x, sy, line_opa);
    prev_y = y;
  }
  lv_obj_invalidate(canvas);
}

}  // namespace lvgl
}  // namespace ha_history
}  // namespace esphome

#endif  // USE_LVGL

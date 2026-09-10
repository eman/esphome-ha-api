#pragma once

// The data model behind a `ha_history` sensor, with no ESPHome dependencies.
//
// Everything here is plain C++17 so it can be compiled and tested on the host
// (see tests/). The ESPHome-facing classes in ha_history.h wrap these and add
// the API plumbing; nothing in this file knows about Home Assistant, JSON or
// LVGL.
//
// Two ideas carry the whole design:
//
//  * A SeriesBuffer is a sorted array of (bucket_start, value) points. Gaps are
//    represented by absence, not by NaN placeholders: Home Assistant omits
//    buckets it has no rows for, and so do we.
//
//  * A BucketAccumulator turns irregular live pushes into one value per
//    bucket, computed the way Home Assistant computes its own statistics -
//    time-weighted for the mean, with the previous value carried in across the
//    boundary and held across `unavailable`. Matching HA's arithmetic is what
//    makes the join between backfilled and live buckets invisible.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace ha_history {

/// Which statistic a bucket holds. Mirrors the `types` accepted by
/// `recorder.get_statistics`, minus `sum`, which cannot be extended live
/// without the previous running total.
enum class Statistic : uint8_t { MEAN, MIN, MAX, STATE, CHANGE };

inline const char *to_string(Statistic s) {
  switch (s) {
    case Statistic::MEAN:
      return "mean";
    case Statistic::MIN:
      return "min";
    case Statistic::MAX:
      return "max";
    case Statistic::STATE:
      return "state";
    case Statistic::CHANGE:
      return "change";
  }
  return "?";
}

/// One bucket. `start` is the bucket's start as a UTC epoch.
struct Point {
  uint32_t start;
  float value;
};

/// Rounds an epoch down to a bucket boundary. Home Assistant's 5-minute and
/// hourly rows start on UTC-epoch-aligned boundaries, so this is how both
/// sides agree on where bucket 0 is - including for a local midnight that is
/// not hour-aligned (half-hour timezones).
inline uint32_t align_down(uint32_t t, uint32_t bucket_s) { return bucket_s == 0 ? t : t - (t % bucket_s); }

/// A fixed-capacity array of points sorted by `start`.
///
/// Not a ring: inserts are almost always appends, capacity is at most ~1000,
/// and a plain sorted array makes "replace the bucket at this start" and
/// "drop everything before" trivial and obviously correct. Storage is supplied
/// by the owner so the ESPHome side can place it in PSRAM.
class SeriesBuffer {
 public:
  SeriesBuffer() = default;

  /// Adopts `storage` (capacity points). The buffer never allocates.
  void attach(Point *storage, size_t capacity) {
    this->buf_ = storage;
    this->cap_ = capacity;
    this->count_ = 0;
  }

  size_t size() const { return this->count_; }
  size_t capacity() const { return this->cap_; }
  bool empty() const { return this->count_ == 0; }
  const Point &at(size_t i) const { return this->buf_[i]; }
  const Point *data() const { return this->buf_; }

  /// Inserts `p` in order, replacing an existing point with the same start.
  /// When full, the oldest point is evicted - unless `p` is itself older than
  /// everything held, in which case it is dropped. Returns false only then.
  bool put(Point p) {
    if (this->buf_ == nullptr || this->cap_ == 0)
      return false;

    // Fast path: append.
    if (this->count_ == 0 || p.start > this->buf_[this->count_ - 1].start) {
      if (this->count_ == this->cap_)
        this->evict_oldest_();
      this->buf_[this->count_++] = p;
      return true;
    }

    // Find the first point at or after p.start.
    size_t lo = 0, hi = this->count_;
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (this->buf_[mid].start < p.start)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo < this->count_ && this->buf_[lo].start == p.start) {
      this->buf_[lo].value = p.value;  // replace
      return true;
    }
    if (this->count_ == this->cap_) {
      if (lo == 0)
        return false;  // older than everything we keep
      this->evict_oldest_();
      lo--;
    }
    memmove(&this->buf_[lo + 1], &this->buf_[lo], (this->count_ - lo) * sizeof(Point));
    this->buf_[lo] = p;
    this->count_++;
    return true;
  }

  /// Looks up the point starting exactly at `start`.
  bool get(uint32_t start, float *out) const {
    for (size_t i = 0; i < this->count_; i++) {
      if (this->buf_[i].start == start) {
        if (out != nullptr)
          *out = this->buf_[i].value;
        return true;
      }
      if (this->buf_[i].start > start)
        break;
    }
    return false;
  }

  void clear() { this->count_ = 0; }

  /// Removes every point that starts before `start`. The rolling-window trim.
  void drop_before(uint32_t start) {
    size_t n = 0;
    while (n < this->count_ && this->buf_[n].start < start)
      n++;
    if (n == 0)
      return;
    memmove(this->buf_, &this->buf_[n], (this->count_ - n) * sizeof(Point));
    this->count_ -= n;
  }

  float min_value() const {
    float m = NAN;
    for (size_t i = 0; i < this->count_; i++)
      m = std::isnan(m) ? this->buf_[i].value : std::min(m, this->buf_[i].value);
    return m;
  }
  float max_value() const {
    float m = NAN;
    for (size_t i = 0; i < this->count_; i++)
      m = std::isnan(m) ? this->buf_[i].value : std::max(m, this->buf_[i].value);
    return m;
  }

  uint32_t first_start() const { return this->count_ ? this->buf_[0].start : 0; }
  uint32_t last_start() const { return this->count_ ? this->buf_[this->count_ - 1].start : 0; }

  /// Copies out as parallel arrays, oldest first. Renderers use this rather
  /// than a pointer into the buffer.
  size_t copy(uint32_t *starts, float *values, size_t cap) const {
    const size_t n = std::min(cap, this->count_);
    for (size_t i = 0; i < n; i++) {
      if (starts != nullptr)
        starts[i] = this->buf_[i].start;
      if (values != nullptr)
        values[i] = this->buf_[i].value;
    }
    return n;
  }

 protected:
  void evict_oldest_() {
    if (this->count_ == 0)
      return;
    memmove(this->buf_, &this->buf_[1], (this->count_ - 1) * sizeof(Point));
    this->count_--;
  }

  Point *buf_{nullptr};
  size_t cap_{0};
  size_t count_{0};
};

/// The in-progress bucket, fed by live pushes.
///
/// Reproduces Home Assistant's own statistics arithmetic
/// (homeassistant/components/sensor/recorder.py):
///
///  * MEAN is time-weighted. A value holds from when it arrived (clamped to
///    the bucket start) until the next value or the bucket end. The value in
///    force at the bucket start - carried over from the previous bucket - is
///    part of the mean, exactly as HA includes the state valid before the
///    period.
///  * MIN/MAX include that carried value.
///  * STATE is the last value.
///  * CHANGE is the sum of increases; a decrease is read as a meter reset and
///    the new value counts from zero, as HA does for total_increasing.
///
/// Non-finite values (Home Assistant's `unavailable`/`unknown` arrive as NaN)
/// are never fed in: the previous value is held across the gap, which is also
/// what HA does.
class BucketAccumulator {
 public:
  bool is_open() const { return this->open_; }
  uint32_t start() const { return this->start_; }
  uint32_t end() const { return this->end_; }

  /// The value currently in force - what the next bucket should be seeded
  /// with. NaN if nothing has ever been seen.
  float last_value() const { return this->have_value_ ? this->last_ : NAN; }

  /// Opens the bucket [start, start + bucket_s). `carried` is the value in
  /// force at the boundary (NaN if unknown - the first bucket after boot).
  void begin(uint32_t start, uint32_t bucket_s, Statistic st, float carried) {
    this->open_ = true;
    this->st_ = st;
    this->start_ = start;
    this->end_ = start + bucket_s;
    this->weighted_sum_ = 0.0;
    this->weighted_dur_ = 0;
    this->change_ = 0.0f;
    this->samples_ = 0;
    if (std::isfinite(carried)) {
      this->have_value_ = true;
      this->last_ = carried;
      this->since_ = start;
      this->min_ = this->max_ = carried;
    } else {
      this->have_value_ = false;
      this->last_ = NAN;
      this->since_ = start;
      this->min_ = this->max_ = NAN;
    }
  }

  /// Feeds one live value that arrived at `now` (epoch). Values outside the
  /// bucket's span are clamped to it; the caller closes the bucket at the
  /// boundary before opening the next.
  void add(float value, uint32_t now) {
    if (!this->open_ || !std::isfinite(value))
      return;
    const uint32_t t = std::min(std::max(now, this->start_), this->end_);

    if (this->have_value_) {
      this->settle_(t);
      // A decrease on a monotonically increasing meter is a reset: the new
      // reading counts from zero rather than as a negative change.
      this->change_ += (value < this->last_) ? value : (value - this->last_);
    }
    this->have_value_ = true;
    this->last_ = value;
    this->since_ = t;
    this->min_ = std::isnan(this->min_) ? value : std::min(this->min_, value);
    this->max_ = std::isnan(this->max_) ? value : std::max(this->max_, value);
    this->samples_++;
  }

  /// Closes the bucket, producing its point. Returns false if there is nothing
  /// to report - no carried value and no samples.
  bool close(Point *out) {
    if (!this->open_)
      return false;
    this->open_ = false;
    if (!this->have_value_)
      return false;
    this->settle_(this->end_);

    float v = NAN;
    switch (this->st_) {
      case Statistic::MEAN:
        v = this->weighted_dur_ > 0 ? (float) (this->weighted_sum_ / this->weighted_dur_) : this->last_;
        break;
      case Statistic::MIN:
        v = this->min_;
        break;
      case Statistic::MAX:
        v = this->max_;
        break;
      case Statistic::STATE:
        v = this->last_;
        break;
      case Statistic::CHANGE:
        v = this->change_;
        break;
    }
    if (out != nullptr)
      *out = Point{this->start_, v};
    return std::isfinite(v);
  }

  /// How many live values this bucket saw. A backfill can use this to decide
  /// whether the live figure is trustworthy - a bucket that saw one sample in
  /// its final seconds is not, and HA's row for it should win.
  uint16_t samples() const { return this->samples_; }

 protected:
  /// Accounts for the currently held value up to time `t`.
  void settle_(uint32_t t) {
    if (t > this->since_) {
      const uint32_t dur = t - this->since_;
      this->weighted_sum_ += (double) this->last_ * dur;
      this->weighted_dur_ += dur;
      this->since_ = t;
    }
  }

  bool open_{false};
  Statistic st_{Statistic::MEAN};
  uint32_t start_{0}, end_{0};
  bool have_value_{false};
  float last_{NAN};
  uint32_t since_{0};
  double weighted_sum_{0.0};
  uint32_t weighted_dur_{0};
  float min_{NAN}, max_{NAN};
  float change_{0.0f};
  uint16_t samples_{0};
};

/// Inserts one backfilled point given as (bucket index, value) relative to an
/// anchor - the shape the response template produces. Rejects indices that
/// would land outside a sane range so a malformed reply cannot write
/// nonsense timestamps.
inline bool put_indexed(SeriesBuffer &into, uint32_t anchor, uint32_t bucket_s, long idx, float value,
                        long max_idx = 100000) {
  if (idx < 0 || idx > max_idx || !std::isfinite(value))
    return false;
  return into.put(Point{anchor + (uint32_t) idx * bucket_s, value});
}

}  // namespace ha_history
}  // namespace esphome

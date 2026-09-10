// Host-side tests for series.h - no ESPHome, no hardware. `make test`.
#include "../components/ha_history/series.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace esphome::ha_history;

static int failures = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
      failures++;                                                                     \
    }                                                                                 \
  } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

static void test_buffer_append_and_order() {
  Point store[4];
  SeriesBuffer b;
  b.attach(store, 4);
  CHECK(b.put({300, 1.f}));
  CHECK(b.put({600, 2.f}));
  CHECK(b.put({0, 0.f}));  // out of order: goes first
  CHECK(b.size() == 3 && b.at(0).start == 0 && b.at(1).start == 300 && b.at(2).start == 600);
  // Replace by start.
  CHECK(b.put({300, 9.f}));
  CHECK(b.size() == 3 && b.at(1).value == 9.f);
  // Fill, then evict oldest on append.
  CHECK(b.put({900, 3.f}));
  CHECK(b.put({1200, 4.f}));
  CHECK(b.size() == 4 && b.first_start() == 300 && b.last_start() == 1200);
  // Older than everything while full: dropped.
  CHECK(!b.put({0, 5.f}));
  CHECK(b.size() == 4 && b.first_start() == 300);
  // Insert in the middle while full: evicts oldest, keeps order.
  CHECK(b.put({1000, 7.f}));
  CHECK(b.size() == 4 && b.first_start() == 600 && b.at(2).start == 1000 && b.last_start() == 1200);
}

static void test_buffer_drop_clear_minmax_copy() {
  Point store[8];
  SeriesBuffer b;
  b.attach(store, 8);
  for (uint32_t i = 0; i < 6; i++)
    b.put({i * 300, (float) i});
  CHECK_NEAR(b.min_value(), 0.f, 1e-6);
  CHECK_NEAR(b.max_value(), 5.f, 1e-6);
  b.drop_before(900);
  CHECK(b.size() == 3 && b.first_start() == 900);
  float v;
  CHECK(b.get(1200, &v) && v == 4.f);
  CHECK(!b.get(1250, &v));
  uint32_t starts[8];
  float vals[8];
  CHECK(b.copy(starts, vals, 8) == 3 && starts[0] == 900 && vals[2] == 5.f);
  b.clear();
  CHECK(b.empty() && std::isnan(b.min_value()));
}

static void test_mean_time_weighted() {
  // Bucket [1000, 1300). Carried value 2 holds for 100 s, then 10 for 200 s.
  BucketAccumulator a;
  a.begin(1000, 300, Statistic::MEAN, 2.f);
  a.add(10.f, 1100);
  Point p;
  CHECK(a.close(&p));
  CHECK(p.start == 1000);
  CHECK_NEAR(p.value, (2.f * 100 + 10.f * 200) / 300.f, 1e-4);  // 7.333
}

static void test_mean_uneven_spacing_and_nan_hold() {
  BucketAccumulator a;
  a.begin(0, 300, Statistic::MEAN, NAN);  // boot: no carried value
  a.add(4.f, 60);                         // 4 holds 60..
  a.add(NAN, 120);                        // unavailable: held, ignored
  a.add(8.f, 240);                        // 4 held 60..240 (180 s), 8 holds 240..300 (60 s)
  Point p;
  CHECK(a.close(&p));
  CHECK_NEAR(p.value, (4.f * 180 + 8.f * 60) / 240.f, 1e-4);  // 5.0 over the 240 s observed
  CHECK(a.samples() == 2);
  CHECK_NEAR(a.last_value(), 8.f, 1e-6);
}

static void test_boot_bucket_without_samples_is_empty() {
  BucketAccumulator a;
  a.begin(0, 300, Statistic::MEAN, NAN);
  Point p;
  CHECK(!a.close(&p));
  // Carried value alone is enough.
  a.begin(300, 300, Statistic::MEAN, 3.f);
  CHECK(a.close(&p) && p.value == 3.f);
}

static void test_min_max_include_seed() {
  BucketAccumulator a;
  a.begin(0, 300, Statistic::MIN, 5.f);
  a.add(7.f, 10);
  a.add(6.f, 20);
  Point p;
  CHECK(a.close(&p) && p.value == 5.f);
  a.begin(0, 300, Statistic::MAX, 5.f);
  a.add(7.f, 10);
  CHECK(a.close(&p) && p.value == 7.f);
}

static void test_state_and_change_with_reset() {
  BucketAccumulator a;
  a.begin(0, 300, Statistic::STATE, 1.f);
  a.add(2.f, 10);
  a.add(3.f, 20);
  Point p;
  CHECK(a.close(&p) && p.value == 3.f);

  // Meter goes 100 -> 105 -> 2 (reset) -> 5: change = 5 + 2 + 3 = 10.
  a.begin(0, 3600, Statistic::CHANGE, 100.f);
  a.add(105.f, 100);
  a.add(2.f, 200);
  a.add(5.f, 300);
  CHECK(a.close(&p));
  CHECK_NEAR(p.value, 10.f, 1e-5);
  // With no carried value, the first sample sets the baseline and counts no change.
  a.begin(0, 3600, Statistic::CHANGE, NAN);
  a.add(50.f, 10);
  a.add(52.f, 20);
  CHECK(a.close(&p));
  CHECK_NEAR(p.value, 2.f, 1e-5);
}

static void test_indexed_payload_and_merge() {
  Point store[16];
  SeriesBuffer b;
  b.attach(store, 16);
  const uint32_t anchor = 1000, bucket = 300;
  // Live committed a bucket first...
  b.put({1600, 99.f});
  // ...then backfill arrives with a gap (index 1 missing) and out of order.
  CHECK(put_indexed(b, anchor, bucket, 2, 2.5f));
  CHECK(put_indexed(b, anchor, bucket, 0, 0.5f));
  CHECK(!put_indexed(b, anchor, bucket, -1, 1.f));
  CHECK(!put_indexed(b, anchor, bucket, 3, NAN));
  CHECK(b.size() == 2);  // 1000 (idx 0) and 1600 (idx 2 replaced the live point)
  float v;
  CHECK(b.get(1600, &v) && v == 2.5f);  // backfill wins
  CHECK(!b.get(1300, &v));              // the gap stays a gap
  CHECK(align_down(1299, bucket) == 1200);
  CHECK(align_down(19800, 3600) == 18000);  // a +5:30 midnight is not hour-aligned
}

int main() {
  test_buffer_append_and_order();
  test_buffer_drop_clear_minmax_copy();
  test_mean_time_weighted();
  test_mean_uneven_spacing_and_nan_hold();
  test_boot_bucket_without_samples_is_empty();
  test_min_max_include_seed();
  test_state_and_change_with_reset();
  test_indexed_payload_and_merge();
  if (failures == 0) {
    std::printf("series.h: all tests passed\n");
    return 0;
  }
  std::printf("series.h: %d failure(s)\n", failures);
  return 1;
}

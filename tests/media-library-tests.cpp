#include "media-library.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

static mms::Item item(const char *name, mms::MediaKind kind, int modified = 0) {
  return {name, kind, fs::file_time_type{} + std::chrono::seconds(modified)};
}

static std::set<std::string> keys(const std::vector<mms::Item> &items) {
  std::set<std::string> result;
  for (const auto &entry : items) result.insert(mms::normalized_key(entry.path));
  return result;
}

static std::size_t adjacent_video_pairs(const std::vector<mms::Item> &items,
                                        bool previous_was_video) {
  std::size_t result = 0;
  bool previous = previous_was_video;
  for (const auto &entry : items) {
    const bool video = entry.kind == mms::MediaKind::video;
    if (previous && video) ++result;
    previous = video;
  }
  return result;
}

static void verify_shuffle_cycle(std::vector<mms::Item> input,
                                 bool previous_was_video,
                                 std::size_t expected_pairs) {
  const auto expected = keys(input);
  for (unsigned seed = 0; seed < 100; ++seed) {
    auto cycle = input;
    std::mt19937 rng(seed);
    mms::order_items(cycle, mms::SortMode::shuffle, rng,
                     previous_was_video);
    assert(cycle.size() == input.size());
    assert(keys(cycle) == expected); // every item exactly once
    assert(adjacent_video_pairs(cycle, previous_was_video) == expected_pairs);
  }
}

int main() {
  mms::MediaKind kind{};
  const auto fallback = mms::stable_frame_size(0, 0);
  assert(fallback.width == 1920 && fallback.height == 1080);
  const auto canvas = mms::stable_frame_size(2560, 1440);
  assert(canvas.width == 2560 && canvas.height == 1440);

  const auto portrait_cover = mms::cover_transform(1000, 2000, 1920, 1080);
  assert(std::abs(portrait_cover.scale - 1.92) < 0.000001);
  assert(std::abs(portrait_cover.offset_x) < 0.000001);
  assert(std::abs(portrait_cover.offset_y + 1380.0) < 0.000001);
  const auto wide_cover = mms::cover_transform(3840, 1080, 1920, 1080);
  assert(std::abs(wide_cover.scale - 1.0) < 0.000001);
  assert(std::abs(wide_cover.offset_x + 960.0) < 0.000001);
  assert(std::abs(wide_cover.offset_y) < 0.000001);
  assert(mms::cover_transform(0, 1080, 1920, 1080).scale == 0.0);

  const auto swipe_left = mms::transition_spec(mms::TransitionKind::swipe_left);
  const auto swipe_right = mms::transition_spec(mms::TransitionKind::swipe_right);
  const auto slide_left = mms::transition_spec(mms::TransitionKind::slide_left);
  const auto slide_right = mms::transition_spec(mms::TransitionKind::slide_right);
  assert(std::strcmp(swipe_left.source_id, "swipe_transition") == 0);
  assert(std::strcmp(swipe_left.direction, "left") == 0);
  assert(std::strcmp(swipe_right.direction, "right") == 0);
  assert(std::strcmp(slide_left.source_id, "slide_transition") == 0);
  assert(std::strcmp(slide_left.direction, "left") == 0);
  assert(std::strcmp(slide_right.direction, "right") == 0);
  assert(std::strcmp(mms::transition_spec(mms::TransitionKind::fade_to_black).source_id,
                     "fade_to_color_transition") == 0);

  assert(mms::classify("photo.JPEG", kind) && kind == mms::MediaKind::image);
  assert(mms::classify("clip.MKV", kind) && kind == mms::MediaKind::video);
  assert(!mms::classify("notes.txt", kind));

  const auto root = fs::temp_directory_path() /
      ("mixed-media-slideshow-test-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root / "subdirectory.mp4");
  std::ofstream(root / "b.mp4") << "not actually media";
  std::ofstream(root / "A.png") << "not actually media";
  std::ofstream(root / "ignore.txt") << "x";

  auto scanned = mms::scan_folder(root);
  assert(scanned.size() == 2);
  std::mt19937 rng(42);
  mms::order_items(scanned, mms::SortMode::alphabetical, rng);
  assert(scanned[0].path.filename() == "A.png");
  assert(scanned[1].path.filename() == "b.mp4");
  assert(mms::preserved_index(scanned, scanned[1].path) == 1);
  assert(mms::preserved_index(scanned, root / "gone.jpg") == scanned.size());

  const auto first_status = mms::playlist_status(scanned, 0);
  assert(first_status.position == 1 && first_status.total == 2);
  assert(first_status.current_filename == "A.png");
  assert(first_status.next_filename == "b.mp4");
  const auto final_status = mms::playlist_status(scanned, 1);
  assert(final_status.position == 2 && final_status.total == 2);
  assert(final_status.current_filename == "b.mp4");
  assert(final_status.next_filename.empty()); // next cycle is not realized yet
  const auto empty_status = mms::playlist_status({}, 0);
  assert(empty_status.position == 0 && empty_status.total == 0);
  assert(empty_status.current_filename.empty() && empty_status.next_filename.empty());

  assert(mms::still_duration_ms(30.0) == 30000);
  assert(mms::still_time_ms(12.345, 30.0) == 12345);
  assert(mms::still_time_ms(-1.0, 30.0) == 0);
  assert(mms::still_time_ms(31.0, 30.0) == 30000);
  assert(std::abs(mms::still_seek_seconds(12500, 30.0) - 12.5) < 0.000001);
  assert(mms::still_seek_seconds(-1, 30.0) == 0.0);
  assert(mms::still_seek_seconds(31000, 30.0) == 30.0);

  // Normal modes remain a complete deterministic filename/date ordering.
  std::vector<mms::Item> normal = {
      item("z.jpg", mms::MediaKind::image, 20),
      item("A.mp4", mms::MediaKind::video, 30),
      item("m.png", mms::MediaKind::image, 10),
  };
  auto alphabetical = normal;
  mms::order_items(alphabetical, mms::SortMode::alphabetical, rng, true);
  assert(alphabetical[0].path == "A.mp4");
  assert(alphabetical[1].path == "m.png");
  assert(alphabetical[2].path == "z.jpg");
  auto date = normal;
  mms::order_items(date, mms::SortMode::date, rng, true);
  assert(date[0].path == "m.png");
  assert(date[1].path == "z.jpg");
  assert(date[2].path == "A.mp4");

  const std::vector<mms::Item> separable = {
      item("i1.jpg", mms::MediaKind::image),
      item("i2.jpg", mms::MediaKind::image),
      item("i3.jpg", mms::MediaKind::image),
      item("v1.mp4", mms::MediaKind::video),
      item("v2.mp4", mms::MediaKind::video),
      item("v3.mp4", mms::MediaKind::video),
  };
  verify_shuffle_cycle(separable, false, 0); // no avoidable pairs in a cycle
  verify_shuffle_cycle(separable, true, 0);  // nor across a video boundary
  auto realized_queue = separable;
  std::mt19937 status_rng(7);
  mms::order_items(realized_queue, mms::SortMode::shuffle, status_rng);
  const auto shuffle_status = mms::playlist_status(realized_queue, 2);
  assert(shuffle_status.position == 3 && shuffle_status.total == realized_queue.size());
  assert(shuffle_status.current_filename ==
         realized_queue[2].path.filename().u8string());
  assert(shuffle_status.next_filename ==
         realized_queue[3].path.filename().u8string());
  for (unsigned seed = 0; seed < 100; ++seed) {
    auto first_cycle = separable;
    auto second_cycle = separable;
    std::mt19937 cycle_rng(seed);
    mms::order_items(first_cycle, mms::SortMode::shuffle, cycle_rng);
    const bool boundary_video =
        first_cycle.back().kind == mms::MediaKind::video;
    mms::order_items(second_cycle, mms::SortMode::shuffle, cycle_rng,
                     boundary_video);
    assert(keys(first_cycle) == keys(separable));
    assert(keys(second_cycle) == keys(separable));
    assert(adjacent_video_pairs(second_cycle, boundary_video) == 0);
  }

  // Four videos and one image have only two gaps.  Exactly two adjacent pairs
  // are unavoidable (three when the preceding cycle also ended on video).
  const std::vector<mms::Item> excess_videos = {
      item("i.jpg", mms::MediaKind::image),
      item("v1.mp4", mms::MediaKind::video),
      item("v2.mp4", mms::MediaKind::video),
      item("v3.mp4", mms::MediaKind::video),
      item("v4.mp4", mms::MediaKind::video),
  };
  verify_shuffle_cycle(excess_videos, false, 2);
  verify_shuffle_cycle(excess_videos, true, 3);

  fs::remove_all(root);
  std::cout << "media-library-tests: all checks passed\n";
}

#include "media-library.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

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

  auto items = mms::scan_folder(root);
  assert(items.size() == 2);
  std::mt19937 rng(42);
  mms::order_items(items, mms::SortMode::alphabetical, rng);
  assert(items[0].path.filename() == "A.png");
  assert(items[1].path.filename() == "b.mp4");
  assert(mms::preserved_index(items, items[1].path) == 1);
  assert(mms::preserved_index(items, root / "gone.jpg") == items.size());

  auto shuffled = items;
  mms::order_items(shuffled, mms::SortMode::shuffle, rng);
  assert(shuffled.size() == items.size());
  mms::order_items(items, mms::SortMode::date, rng);
  assert(items.size() == 2);

  fs::remove_all(root);
  std::cout << "media-library-tests: all checks passed\n";
}

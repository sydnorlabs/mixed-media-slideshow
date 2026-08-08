#include "media-library.hpp"

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int main() {
  mms::MediaKind kind{};
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

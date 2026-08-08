#include "media-library.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <system_error>
#include <unordered_set>

namespace mms {
std::string normalized_key(const std::filesystem::path &path) {
  auto s = path.lexically_normal().generic_u8string();
#ifdef _WIN32
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
#endif
  return s;
}

bool classify(const std::filesystem::path &path, MediaKind &kind) {
  static const std::unordered_set<std::string> images = {
      ".bmp", ".gif", ".jpeg", ".jpg", ".png", ".tga", ".tif",
      ".tiff", ".webp"};
  static const std::unordered_set<std::string> videos = {
      ".3g2", ".3gp", ".avi", ".flv", ".m2ts", ".m4v", ".mkv",
      ".mov", ".mp4", ".mpeg", ".mpg", ".mts", ".ogv", ".ts",
      ".vob", ".webm", ".wmv"};
  auto ext = path.extension().u8string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (images.count(ext)) { kind = MediaKind::image; return true; }
  if (videos.count(ext)) { kind = MediaKind::video; return true; }
  return false;
}

std::vector<Item> scan_folder(const std::filesystem::path &folder) {
  std::vector<Item> result;
  std::error_code ec;
  std::filesystem::directory_iterator it(
      folder, std::filesystem::directory_options::skip_permission_denied, ec);
  const std::filesystem::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code item_ec;
    if (!it->is_regular_file(item_ec) || item_ec) continue;
    MediaKind kind;
    if (!classify(it->path(), kind)) continue;
    auto modified = it->last_write_time(item_ec);
    if (item_ec) modified = std::filesystem::file_time_type::min();
    result.push_back({it->path(), kind, modified});
  }
  return result;
}

void order_items(std::vector<Item> &items, SortMode mode, std::mt19937 &rng,
                 bool previous_was_video) {
  if (mode == SortMode::shuffle) {
    std::vector<Item> images;
    std::vector<Item> videos;
    for (auto &item : items)
      (item.kind == MediaKind::video ? videos : images).push_back(std::move(item));
    std::shuffle(images.begin(), images.end(), rng);
    std::shuffle(videos.begin(), videos.end(), rng);

    // Images create N+1 gaps in which a single video has no video neighbor.
    // When the preceding cycle ended on video, the first gap is not free.  Fill
    // every zero-cost gap first, then distribute any excess videos.  Each excess
    // video necessarily adds exactly one adjacent-video pair, so this achieves
    // the mathematical minimum when videos outnumber the available separators.
    std::vector<std::vector<Item>> gaps(images.size() + 1);
    std::vector<std::size_t> free_gaps;
    for (std::size_t i = previous_was_video ? 1 : 0; i < gaps.size(); ++i)
      free_gaps.push_back(i);
    std::shuffle(free_gaps.begin(), free_gaps.end(), rng);

    std::size_t video_index = 0;
    for (const auto gap : free_gaps) {
      if (video_index == videos.size()) break;
      gaps[gap].push_back(std::move(videos[video_index++]));
    }

    std::vector<std::size_t> excess_gaps(gaps.size());
    for (std::size_t i = 0; i < excess_gaps.size(); ++i) excess_gaps[i] = i;
    std::shuffle(excess_gaps.begin(), excess_gaps.end(), rng);
    std::size_t excess_index = 0;
    while (video_index < videos.size()) {
      const auto gap = excess_gaps[excess_index++ % excess_gaps.size()];
      gaps[gap].push_back(std::move(videos[video_index++]));
    }

    items.clear();
    items.reserve(images.size() + videos.size());
    for (std::size_t i = 0; i < gaps.size(); ++i) {
      for (auto &video : gaps[i]) items.push_back(std::move(video));
      if (i < images.size()) items.push_back(std::move(images[i]));
    }
    return;
  }
  std::stable_sort(items.begin(), items.end(), [mode](const Item &a, const Item &b) {
    if (mode == SortMode::date && a.modified != b.modified)
      return a.modified < b.modified;
    return normalized_key(a.path.filename()) < normalized_key(b.path.filename());
  });
}

std::size_t preserved_index(const std::vector<Item> &items,
                            const std::filesystem::path &current) {
  const auto key = normalized_key(current);
  for (std::size_t i = 0; i < items.size(); ++i)
    if (normalized_key(items[i].path) == key) return i;
  return items.size();
}

PlaylistStatus playlist_status(const std::vector<Item> &items,
                               std::size_t index) {
  if (items.empty() || index >= items.size()) return {0, items.size(), {}, {}};
  return {index + 1, items.size(), items[index].path.filename().u8string(),
          index + 1 < items.size()
              ? items[index + 1].path.filename().u8string()
              : std::string{}};
}

static int64_t seconds_to_ms(double seconds) {
  if (!std::isfinite(seconds) || seconds <= 0.0) return 0;
  constexpr double max_ms =
      static_cast<double>(std::numeric_limits<int64_t>::max());
  const double milliseconds = seconds * 1000.0;
  if (!std::isfinite(milliseconds) || milliseconds >= max_ms)
    return std::numeric_limits<int64_t>::max();
  return static_cast<int64_t>(milliseconds);
}

int64_t still_duration_ms(double duration_seconds) {
  return seconds_to_ms(duration_seconds);
}

int64_t still_time_ms(double elapsed_seconds, double duration_seconds) {
  return seconds_to_ms(std::min(std::max(0.0, elapsed_seconds),
                                std::max(0.0, duration_seconds)));
}

double still_seek_seconds(int64_t milliseconds, double duration_seconds) {
  const double seconds = milliseconds <= 0 ? 0.0 : milliseconds / 1000.0;
  return std::min(seconds, std::max(0.0, duration_seconds));
}

FrameSize stable_frame_size(uint32_t base_width, uint32_t base_height) {
  // OBS has no base dimensions before video is initialized. Keep a useful,
  // deterministic 16:9 source transform box until video info is available.
  if (base_width == 0 || base_height == 0) return {1920, 1080};
  return {base_width, base_height};
}

CoverTransform cover_transform(uint32_t source_width, uint32_t source_height,
                               uint32_t frame_width, uint32_t frame_height) {
  if (source_width == 0 || source_height == 0 || frame_width == 0 ||
      frame_height == 0)
    return {0.0, 0.0, 0.0};
  const double scale = std::max(static_cast<double>(frame_width) / source_width,
                                static_cast<double>(frame_height) / source_height);
  return {scale,
          (static_cast<double>(frame_width) - source_width * scale) / 2.0,
          (static_cast<double>(frame_height) - source_height * scale) / 2.0};
}

TransitionSpec transition_spec(TransitionKind kind) {
  switch (kind) {
  case TransitionKind::cut: return {"cut_transition", nullptr};
  case TransitionKind::fade: return {"fade_transition", nullptr};
  case TransitionKind::swipe_left: return {"swipe_transition", "left"};
  case TransitionKind::swipe_right: return {"swipe_transition", "right"};
  case TransitionKind::slide_left: return {"slide_transition", "left"};
  case TransitionKind::slide_right: return {"slide_transition", "right"};
  case TransitionKind::fade_to_black:
    return {"fade_to_color_transition", nullptr};
  }
  return {"fade_transition", nullptr};
}
} // namespace mms

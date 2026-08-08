#include "media-library.hpp"

#include <algorithm>
#include <cctype>
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

void order_items(std::vector<Item> &items, SortMode mode, std::mt19937 &rng) {
  if (mode == SortMode::shuffle) {
    std::shuffle(items.begin(), items.end(), rng);
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
} // namespace mms

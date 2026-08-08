#pragma once

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace mms {
enum class MediaKind { image, video };
enum class SortMode { alphabetical, date, shuffle };
enum class TransitionKind {
  cut = 0,
  fade = 1,
  swipe_left = 2,
  swipe_right = 3,
  slide_left = 4,
  slide_right = 5,
  fade_to_black = 6,
};

struct FrameSize {
  uint32_t width;
  uint32_t height;
};

struct CoverTransform {
  double scale;
  double offset_x;
  double offset_y;
};

struct TransitionSpec {
  const char *source_id;
  const char *direction;
};
struct Item {
  std::filesystem::path path;
  MediaKind kind;
  std::filesystem::file_time_type modified;
};

std::string normalized_key(const std::filesystem::path &path);
bool classify(const std::filesystem::path &path, MediaKind &kind);
std::vector<Item> scan_folder(const std::filesystem::path &folder);
void order_items(std::vector<Item> &items, SortMode mode, std::mt19937 &rng);
std::size_t preserved_index(const std::vector<Item> &items,
                            const std::filesystem::path &current);
FrameSize stable_frame_size(uint32_t base_width, uint32_t base_height);
CoverTransform cover_transform(uint32_t source_width, uint32_t source_height,
                               uint32_t frame_width, uint32_t frame_height);
TransitionSpec transition_spec(TransitionKind kind);
} // namespace mms

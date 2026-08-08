#pragma once

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace mms {
enum class MediaKind { image, video };
enum class SortMode { alphabetical, date, shuffle };
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
} // namespace mms

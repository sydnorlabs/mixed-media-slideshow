#include <obs-module.h>
#include <graphics/vec2.h>

#include "media-library.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
MODULE_EXPORT const char *obs_module_description(void) {
  return "Folder-driven image and video slideshow with audio and media controls";
}

namespace {
constexpr const char *kId = "mixed_media_slideshow";

struct Slideshow {
  obs_source_t *source{};
  obs_source_t *transition{};
  obs_source_t *media{};
  obs_scene_t *frame_scene{};
  obs_sceneitem_t *frame_background{};
  obs_sceneitem_t *frame_item{};
  std::vector<mms::Item> items;
  std::vector<mms::Item> raw_items;
  std::filesystem::path folder;
  std::size_t index{};
  mms::SortMode order{mms::SortMode::alphabetical};
  std::mt19937 rng{std::random_device{}()};
  std::atomic<double> still_seconds{30.0};
  std::atomic<double> item_elapsed{};
  double refresh_elapsed{};
  uint32_t transition_ms{500};
  uint32_t frame_width{1920};
  uint32_t frame_height{1080};
  bool loop{true};
  mms::TransitionKind requested_transition{mms::TransitionKind::fade};
  mms::TransitionKind active_transition{mms::TransitionKind::fade};
  bool restart_on_activate{true};
  std::atomic<bool> paused{};
  std::atomic<bool> stopped{};
  bool activated_once{};
  bool dashboard_ready{};
  bool updating{};
  std::mutex dashboard_mutex;
  std::string dashboard_status{"No supported media loaded"};
  obs_source_t *timeline_media{};
  bool timeline_is_video{};
  std::mutex audio_mutex;
  std::mutex transition_mutex;
};

static bool same_raw(const std::vector<mms::Item> &a,
                     const std::vector<mms::Item> &b) {
  if (a.size() != b.size()) return false;
  auto sorted = [](const std::vector<mms::Item> &v) {
    std::vector<std::pair<std::string, std::filesystem::file_time_type>> out;
    for (const auto &x : v) out.emplace_back(mms::normalized_key(x.path), x.modified);
    std::sort(out.begin(), out.end(), [](const auto &x, const auto &y) {
      return x.first < y.first;
    });
    return out;
  };
  return sorted(a) == sorted(b);
}

static mms::FrameSize current_frame_size() {
  struct obs_video_info video {};
  return obs_get_video_info(&video)
             ? mms::stable_frame_size(video.base_width, video.base_height)
             : mms::stable_frame_size(0, 0);
}

static void configure_frame(Slideshow *s) {
  if (!s->frame_item || s->items.empty() || s->index >= s->items.size()) return;
  struct vec2 bounds {static_cast<float>(s->frame_width),
                      static_cast<float>(s->frame_height)};
  if (s->frame_background) {
    obs_sceneitem_set_bounds_type(s->frame_background, OBS_BOUNDS_STRETCH);
    obs_sceneitem_set_bounds_alignment(s->frame_background, OBS_ALIGN_CENTER);
    obs_sceneitem_set_bounds_crop(s->frame_background, false);
    obs_sceneitem_set_bounds(s->frame_background, &bounds);
  }
  obs_sceneitem_set_bounds_type(s->frame_item, OBS_BOUNDS_SCALE_INNER);
  obs_sceneitem_set_bounds_alignment(s->frame_item, OBS_ALIGN_CENTER);
  obs_sceneitem_set_bounds_crop(s->frame_item, false);
  obs_sceneitem_set_bounds(s->frame_item, &bounds);
}

static obs_source_t *get_transition_ref(Slideshow *s) {
  std::lock_guard<std::mutex> lock(s->transition_mutex);
  return obs_source_get_ref(s->transition);
}

// This source forwards private media audio itself, so the transition is video
// only from the parent source's perspective.  Complete it when its video has
// finished; otherwise libobs waits for an audio-tree render that deliberately
// never occurs and retains both transition inputs.
static void transition_video_stopped(void *param, calldata_t *calldata) {
  auto *s = static_cast<Slideshow *>(param);
  auto *signalled = static_cast<obs_source_t *>(calldata_ptr(calldata, "source"));
  obs_source_t *transition = nullptr;
  {
    std::lock_guard<std::mutex> lock(s->transition_mutex);
    if (s->transition == signalled)
      transition = obs_source_get_ref(signalled);
  }
  if (!transition) return;

  obs_source_t *destination =
      obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B);
  if (destination) {
    obs_transition_set(transition, destination);
    obs_source_release(destination);
  }
  obs_source_release(transition);
}

static obs_source_t *create_transition(Slideshow *s,
                                       mms::TransitionKind requested,
                                       mms::TransitionKind &active) {
  auto make = [](mms::TransitionKind kind) {
    const auto spec = mms::transition_spec(kind);
    obs_data_t *settings = obs_data_create();
    if (spec.direction)
      obs_data_set_string(settings, "direction", spec.direction);
    if (kind == mms::TransitionKind::fade_to_black) {
      obs_data_set_int(settings, "color", 0x000000);
      obs_data_set_int(settings, "switch_point", 50);
    }
    obs_source_t *result = obs_source_create_private(
        spec.source_id, "Mixed Media Slideshow transition", settings);
    obs_data_release(settings);
    return result;
  };

  obs_source_t *result = make(requested);
  active = requested;
  if (!result && requested != mms::TransitionKind::fade) {
    const auto failed = mms::transition_spec(requested);
    blog(LOG_WARNING,
         "[Mixed Media Slideshow] Built-in transition '%s' could not be "
         "created; falling back to Fade",
         failed.source_id);
    result = make(mms::TransitionKind::fade);
    active = mms::TransitionKind::fade;
  }
  if (result) {
    signal_handler_connect(obs_source_get_signal_handler(result),
                           "transition_video_stop", transition_video_stopped, s);
  }
  return result;
}

static bool replace_transition(Slideshow *s) {
  {
    std::lock_guard<std::mutex> lock(s->transition_mutex);
    if (s->transition && s->active_transition == s->requested_transition)
      return true;
  }
  mms::TransitionKind active;
  obs_source_t *replacement = create_transition(s, s->requested_transition, active);
  if (!replacement) {
    blog(LOG_ERROR, "[Mixed Media Slideshow] OBS fade transition is unavailable");
    return false;
  }
  obs_transition_set_size(replacement, s->frame_width, s->frame_height);
  obs_transition_set_scale_type(replacement, OBS_TRANSITION_SCALE_STRETCH);
  if (s->frame_scene)
    obs_transition_set(replacement, obs_scene_get_source(s->frame_scene));
  obs_source_t *old;
  {
    std::lock_guard<std::mutex> lock(s->transition_mutex);
    old = s->transition;
    s->transition = replacement;
    s->active_transition = active;
  }
  if (old) {
    signal_handler_disconnect(obs_source_get_signal_handler(old),
                              "transition_video_stop", transition_video_stopped, s);
    obs_source_release(old);
  }
  return true;
}

static void audio_capture(void *param, obs_source_t *,
                          const struct audio_data *audio, bool muted) {
  auto *s = static_cast<Slideshow *>(param);
  if (muted || !audio) return;
  struct obs_audio_info info {};
  if (!obs_get_audio_info(&info)) return;
  struct obs_source_audio out {};
  for (size_t i = 0; i < MAX_AV_PLANES; ++i) out.data[i] = audio->data[i];
  out.frames = audio->frames;
  out.speakers = info.speakers;
  out.format = AUDIO_FORMAT_FLOAT_PLANAR;
  out.samples_per_sec = info.samples_per_sec;
  out.timestamp = audio->timestamp;
  std::lock_guard<std::mutex> lock(s->audio_mutex);
  if (s->source) obs_source_output_audio(s->source, &out);
}

static obs_source_t *make_media(const mms::Item &item) {
  obs_data_t *settings = obs_data_create();
  const std::string path = item.path.u8string();
  obs_source_t *child = nullptr;
  if (item.kind == mms::MediaKind::image) {
    obs_data_set_string(settings, "file", path.c_str());
    obs_data_set_bool(settings, "unload", false);
    child = obs_source_create_private("image_source", "Mixed Media Slideshow image", settings);
  } else {
    obs_data_set_bool(settings, "is_local_file", true);
    obs_data_set_string(settings, "local_file", path.c_str());
    obs_data_set_bool(settings, "looping", false);
    obs_data_set_bool(settings, "restart_on_activate", false);
    obs_data_set_bool(settings, "close_when_inactive", false);
    obs_data_set_bool(settings, "clear_on_media_end", false);
    // OBS 32.2.1's ffmpeg_source defaults to software decoding.  Do not opt
    // private sources into its CUDA probe on Intel-only systems.
    obs_data_set_bool(settings, "hw_decode", false);
    child = obs_source_create_private("ffmpeg_source", "Mixed Media Slideshow video", settings);
  }
  obs_data_release(settings);
  return child;
}

static obs_source_t *make_black_background(uint32_t width, uint32_t height) {
  obs_data_t *settings = obs_data_create();
  obs_data_set_int(settings, "width", width);
  obs_data_set_int(settings, "height", height);
  obs_data_set_int(settings, "color", 0xFF000000);
  obs_source_t *background = obs_source_create_private(
      "color_source", "Mixed Media Slideshow black matte", settings);
  obs_data_release(settings);
  return background;
}

static std::string dashboard_text(const Slideshow *s) {
  const auto status = mms::playlist_status(s->items, s->index);
  if (status.position == 0) return "No supported media loaded";
  std::string text = std::to_string(status.position) + " / " +
                     std::to_string(status.total) + "\nCurrent: " +
                     status.current_filename + "\nNext: ";
  if (!status.next_filename.empty()) {
    text += status.next_filename;
  } else if (s->loop && s->order != mms::SortMode::shuffle &&
             !s->items.empty()) {
    text += s->items.front().path.filename().u8string();
  } else {
    text += s->loop ? "(next cycle pending)" : "(end of playlist)";
  }
  return text;
}

static void refresh_dashboard(Slideshow *s) {
  const std::string status = dashboard_text(s);
  {
    std::lock_guard<std::mutex> lock(s->dashboard_mutex);
    s->dashboard_status = status;
  }
  if (s->source && s->dashboard_ready && !s->updating)
    obs_source_update_properties(s->source);
}

static void set_timeline_media(Slideshow *s, obs_source_t *media,
                               bool is_video) {
  obs_source_t *old = nullptr;
  {
    std::lock_guard<std::mutex> lock(s->dashboard_mutex);
    old = s->timeline_media;
    s->timeline_media = media ? obs_source_get_ref(media) : nullptr;
    s->timeline_is_video = is_video;
  }
  if (old) obs_source_release(old);
}

static obs_source_t *get_timeline_media(Slideshow *s, bool &is_video) {
  std::lock_guard<std::mutex> lock(s->dashboard_mutex);
  is_video = s->timeline_is_video;
  return obs_source_get_ref(s->timeline_media);
}

static void detach_media(Slideshow *s) {
  set_timeline_media(s, nullptr, false);
  if (s->media)
    obs_source_remove_audio_capture_callback(s->media, audio_capture, s);
  if (s->frame_scene) obs_scene_release(s->frame_scene);
  s->frame_scene = nullptr;
  s->frame_background = nullptr;
  s->frame_item = nullptr;
  if (s->media) obs_source_release(s->media);
  s->media = nullptr;
}

static bool load_index(Slideshow *s, std::size_t wanted, bool forward) {
  if (s->items.empty()) {
    detach_media(s);
    if (s->transition) obs_transition_clear(s->transition);
    s->stopped = true;
    refresh_dashboard(s);
    return false;
  }
  if (wanted >= s->items.size()) return false;
  std::size_t candidate = wanted;
  for (std::size_t attempts = 0;
       attempts < s->items.size() && candidate < s->items.size(); ++attempts) {
    obs_source_t *next = make_media(s->items[candidate]);
    obs_scene_t *next_frame = next ? obs_scene_create_private(
        "Mixed Media Slideshow media frame") : nullptr;
    obs_source_t *background = nullptr;
    obs_sceneitem_t *background_item = nullptr;
    if (next_frame) {
      background = make_black_background(s->frame_width, s->frame_height);
      if (background) background_item = obs_scene_add(next_frame, background);
    }
    obs_sceneitem_t *next_item = next_frame ? obs_scene_add(next_frame, next) : nullptr;
    const bool background_ready = background_item != nullptr;
    if (background) obs_source_release(background);
    if (next && next_frame && next_item && background_ready) {
      obs_source_add_audio_capture_callback(next, audio_capture, s);

      obs_source_t *old_media = s->media;
      obs_scene_t *old_frame = s->frame_scene;
      s->media = next;
      s->frame_scene = next_frame;
      s->frame_background = background_item;
      s->frame_item = next_item;
      s->index = candidate;
      set_timeline_media(s, next,
                         s->items[candidate].kind == mms::MediaKind::video);
      configure_frame(s);

      obs_source_t *framed = obs_scene_get_source(next_frame);
      const bool cut = s->active_transition == mms::TransitionKind::cut ||
                       s->transition_ms == 0 || !old_media;
      if (cut)
        obs_transition_set(s->transition, framed);
      else
        obs_transition_start(s->transition, OBS_TRANSITION_MODE_AUTO,
                             s->transition_ms, framed);

      if (old_media)
        obs_source_remove_audio_capture_callback(old_media, audio_capture, s);
      if (old_frame) obs_scene_release(old_frame);
      if (old_media) obs_source_release(old_media);

      s->item_elapsed = 0.0;
      s->stopped = false;
      s->paused = false;
      if (s->items[candidate].kind == mms::MediaKind::video)
        obs_source_media_restart(next);
      blog(LOG_INFO, "[Mixed Media Slideshow] Loaded: %s",
           s->items[candidate].path.u8string().c_str());
      refresh_dashboard(s);
      return true;
    }
    if (next_frame) obs_scene_release(next_frame);
    if (next) obs_source_release(next);
    blog(LOG_WARNING, "[Mixed Media Slideshow] Could not create source for: %s",
         s->items[candidate].path.u8string().c_str());
    if (forward) {
      ++candidate;
    } else if (candidate == 0) {
      candidate = s->items.size();
    } else {
      --candidate;
    }
  }
  s->stopped = true;
  return false;
}

static bool begin_cycle(Slideshow *s, bool avoid_current_video_boundary) {
  const bool previous_was_video = avoid_current_video_boundary && s->media &&
      !s->items.empty() && s->index < s->items.size() &&
      s->items[s->index].kind == mms::MediaKind::video;
  auto cycle = s->raw_items;
  mms::order_items(cycle, s->order, s->rng, previous_was_video);
  s->items = std::move(cycle);
  return load_index(s, 0, true);
}

static void advance(Slideshow *s, bool forward) {
  if (s->items.empty()) return;
  if (forward && s->index + 1 >= s->items.size()) {
    if (!s->loop) {
      const bool already_stopped = s->stopped.exchange(true);
      if (s->media && s->items[s->index].kind == mms::MediaKind::video)
        obs_source_media_stop(s->media);
      if (s->source && !already_stopped) obs_source_media_ended(s->source);
      return;
    }
    begin_cycle(s, true);
    return;
  }
  if (!forward && s->index == 0 && !s->loop) return;
  const auto next = forward ? s->index + 1
                            : (s->index + s->items.size() - 1) % s->items.size();
  if (!load_index(s, next, forward) && forward && s->loop)
    begin_cycle(s, true);
}

static void refresh(Slideshow *s, bool force) {
  auto raw = mms::scan_folder(s->folder);
  if (!force && same_raw(raw, s->raw_items)) return;
  const auto current = (!s->items.empty() && s->index < s->items.size())
                           ? s->items[s->index].path : std::filesystem::path{};

  // A real live-folder change must not reshuffle the already-played prefix or
  // replay it.  Keep that prefix, update its metadata, and make one safe order
  // from only the unplayed survivors plus newly discovered files.
  if (!force && s->order == mms::SortMode::shuffle && !current.empty()) {
    std::unordered_map<std::string, mms::Item> available;
    for (const auto &entry : raw)
      available.emplace(mms::normalized_key(entry.path), entry);
    const auto current_key = mms::normalized_key(current);
    if (available.count(current_key)) {
      std::vector<mms::Item> rebuilt;
      std::unordered_set<std::string> played;
      std::size_t rebuilt_index = 0;
      for (std::size_t i = 0; i <= s->index && i < s->items.size(); ++i) {
        const auto key = mms::normalized_key(s->items[i].path);
        const auto found = available.find(key);
        if (found == available.end()) continue;
        if (key == current_key) rebuilt_index = rebuilt.size();
        rebuilt.push_back(found->second);
        played.insert(key);
      }
      std::vector<mms::Item> remaining;
      for (const auto &entry : raw)
        if (!played.count(mms::normalized_key(entry.path)))
          remaining.push_back(entry);
      const bool current_is_video =
          rebuilt[rebuilt_index].kind == mms::MediaKind::video;
      mms::order_items(remaining, mms::SortMode::shuffle, s->rng,
                       current_is_video);
      rebuilt.insert(rebuilt.end(), remaining.begin(), remaining.end());
      s->raw_items = std::move(raw);
      s->items = std::move(rebuilt);
      s->index = rebuilt_index;
      refresh_dashboard(s);
      return;
    }

    // If the current file disappeared, finish the old cycle's unplayed
    // survivors before allowing any surviving prefix item to repeat.
    std::unordered_set<std::string> played;
    for (std::size_t i = 0; i <= s->index && i < s->items.size(); ++i)
      played.insert(mms::normalized_key(s->items[i].path));
    std::vector<mms::Item> remaining;
    for (const auto &entry : raw)
      if (!played.count(mms::normalized_key(entry.path)))
        remaining.push_back(entry);
    const bool current_was_video = s->index < s->items.size() &&
        s->items[s->index].kind == mms::MediaKind::video;
    s->raw_items = std::move(raw);
    if (remaining.empty()) {
      if (s->loop) {
        begin_cycle(s, true);
      } else {
        const bool already_stopped = s->stopped.exchange(true);
        if (s->media && current_was_video) obs_source_media_stop(s->media);
        if (s->source && !already_stopped) obs_source_media_ended(s->source);
      }
    } else {
      mms::order_items(remaining, mms::SortMode::shuffle, s->rng,
                       current_was_video);
      s->items = std::move(remaining);
      load_index(s, 0, true);
    }
    refresh_dashboard(s);
    return;
  }

  s->raw_items = raw;
  mms::order_items(raw, s->order, s->rng);
  s->items = std::move(raw);
  if (s->items.empty()) {
    load_index(s, 0, true);
    blog(LOG_WARNING, "[Mixed Media Slideshow] No supported media in folder");
    return;
  }
  const auto preserved = mms::preserved_index(s->items, current);
  if (preserved < s->items.size()) {
    s->index = preserved;
    configure_frame(s);
  } else {
    load_index(s, std::min(s->index, s->items.size() - 1), true);
  }
  refresh_dashboard(s);
}

static const char *source_name(void *) { return obs_module_text("Source.Name"); }

static void update(void *data, obs_data_t *settings) {
  auto *s = static_cast<Slideshow *>(data);
  s->updating = true;
  s->folder = std::filesystem::u8path(obs_data_get_string(settings, "folder"));
  s->still_seconds = std::max(0.1, obs_data_get_double(settings, "still_duration"));
  s->order = static_cast<mms::SortMode>(obs_data_get_int(settings, "order"));
  s->loop = obs_data_get_bool(settings, "loop");
  const int64_t transition_value = obs_data_get_int(settings, "transition");
  s->requested_transition = transition_value >= 0 && transition_value <= 6
      ? static_cast<mms::TransitionKind>(transition_value)
      : mms::TransitionKind::fade;
  s->transition_ms = static_cast<uint32_t>(
      std::max<int64_t>(0, obs_data_get_int(settings, "fade_ms")));
  s->restart_on_activate = obs_data_get_bool(settings, "restart_on_activate");
  const auto frame = current_frame_size();
  s->frame_width = frame.width;
  s->frame_height = frame.height;
  if (!replace_transition(s)) {
    s->updating = false;
    return;
  }
  obs_transition_set_size(s->transition, s->frame_width, s->frame_height);
  configure_frame(s);
  refresh(s, true);
  if (!s->media && !s->items.empty()) load_index(s, 0, true);
  s->updating = false;
  refresh_dashboard(s);
}

static void *create(obs_data_t *settings, obs_source_t *source) {
  auto *s = new Slideshow;
  s->source = source;
  const auto frame = current_frame_size();
  s->frame_width = frame.width;
  s->frame_height = frame.height;
  update(s, settings);
  if (!s->transition) {
    delete s;
    return nullptr;
  }
  s->dashboard_ready = true;
  return s;
}

static void destroy(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  {
    std::lock_guard<std::mutex> lock(s->audio_mutex);
    s->source = nullptr;
  }
  detach_media(s);
  obs_source_t *transition;
  {
    std::lock_guard<std::mutex> lock(s->transition_mutex);
    transition = s->transition;
    s->transition = nullptr;
  }
  if (transition) {
    signal_handler_disconnect(obs_source_get_signal_handler(transition),
                              "transition_video_stop", transition_video_stopped, s);
    obs_source_release(transition);
  }
  delete s;
}

static uint32_t width(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  return s->frame_width;
}
static uint32_t height(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  return s->frame_height;
}
static void render(void *data, gs_effect_t *) {
  auto *s = static_cast<Slideshow *>(data);
  obs_source_t *transition = get_transition_ref(s);
  if (transition) {
    obs_source_video_render(transition);
    obs_source_release(transition);
  }
}

static void tick(void *data, float seconds) {
  auto *s = static_cast<Slideshow *>(data);
  const auto frame = current_frame_size();
  if (frame.width != s->frame_width || frame.height != s->frame_height) {
    s->frame_width = frame.width;
    s->frame_height = frame.height;
    obs_transition_set_size(s->transition, frame.width, frame.height);
    configure_frame(s);
  }
  s->refresh_elapsed += seconds;
  if (s->refresh_elapsed >= 1.0) {
    s->refresh_elapsed = 0.0;
    refresh(s, false);
  }
  if (!s->media || s->paused || s->stopped || s->items.empty()) return;
  const double elapsed = s->item_elapsed.load() + seconds;
  s->item_elapsed = elapsed;
  const auto &item = s->items[s->index];
  if (item.kind == mms::MediaKind::image) {
    if (elapsed > 1.5 &&
        (obs_source_get_width(s->media) == 0 || obs_source_get_height(s->media) == 0)) {
      blog(LOG_WARNING, "[Mixed Media Slideshow] Skipping unreadable image: %s",
           item.path.u8string().c_str());
      advance(s, true);
    } else if (elapsed >= s->still_seconds.load()) {
      advance(s, true);
    }
  } else if (elapsed > 0.75) {
    const auto state = obs_source_media_get_state(s->media);
    if (state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_ERROR ||
        state == OBS_MEDIA_STATE_STOPPED) {
      if (state == OBS_MEDIA_STATE_ERROR)
        blog(LOG_WARNING, "[Mixed Media Slideshow] Skipping unreadable video: %s",
             item.path.u8string().c_str());
      advance(s, true);
    }
  }
}

static void restart(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  if (!s->media && !s->items.empty()) load_index(s, s->index, true);
  s->item_elapsed = 0.0;
  s->stopped = false;
  s->paused = false;
  if (s->media && s->items[s->index].kind == mms::MediaKind::video)
    obs_source_media_restart(s->media);
}
static void play_pause(void *data, bool pause) {
  auto *s = static_cast<Slideshow *>(data);
  if (s->stopped && !pause) restart(s);
  s->paused = pause;
  if (s->media && !s->items.empty() &&
      s->items[s->index].kind == mms::MediaKind::video)
    obs_source_media_play_pause(s->media, pause);
}
static void stop(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  s->stopped = true;
  s->paused = false;
  if (s->media && !s->items.empty() &&
      s->items[s->index].kind == mms::MediaKind::video)
    obs_source_media_stop(s->media);
}
static void next(void *data) { advance(static_cast<Slideshow *>(data), true); }
static void previous(void *data) { advance(static_cast<Slideshow *>(data), false); }
static int64_t media_duration(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  bool video = false;
  obs_source_t *media = get_timeline_media(s, video);
  if (!media) return 0;
  const int64_t result = video
      ? std::max<int64_t>(0, obs_source_media_get_duration(media))
      : mms::still_duration_ms(s->still_seconds.load());
  obs_source_release(media);
  return result;
}
static int64_t media_time(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  bool video = false;
  obs_source_t *media = get_timeline_media(s, video);
  if (!media) return 0;
  const int64_t result = video
      ? std::max<int64_t>(0, obs_source_media_get_time(media))
      : mms::still_time_ms(s->item_elapsed.load(), s->still_seconds.load());
  obs_source_release(media);
  return result;
}
static void media_set_time(void *data, int64_t milliseconds) {
  auto *s = static_cast<Slideshow *>(data);
  bool video = false;
  obs_source_t *media = get_timeline_media(s, video);
  if (!media) return;
  if (!video) {
    s->item_elapsed =
        mms::still_seek_seconds(milliseconds, s->still_seconds.load());
    obs_source_release(media);
    return;
  }
  // OBS's public wrapper queues the seek only when the child advertises a
  // media_set_time callback.  The private ffmpeg_source in OBS 32.2.1 does;
  // wait for its real duration before exposing a meaningful seek range.
  const int64_t duration = obs_source_media_get_duration(media);
  if (duration > 0)
    obs_source_media_set_time(media,
                              std::min(std::max<int64_t>(0, milliseconds),
                                       duration));
  obs_source_release(media);
}
static enum obs_media_state state(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  if (s->stopped.load()) return OBS_MEDIA_STATE_STOPPED;
  if (s->paused.load()) return OBS_MEDIA_STATE_PAUSED;
  bool video = false;
  obs_source_t *media = get_timeline_media(s, video);
  if (!media) return OBS_MEDIA_STATE_NONE;
  obs_source_release(media);
  return OBS_MEDIA_STATE_PLAYING;
}
static void activate(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  // libobs calls source_info::activate on the real inactive-to-active edge.
  // Initial creation already starts at a cycle head; later activations begin a
  // deliberate new cycle rather than resuming at a surprising middle item.
  if (s->restart_on_activate && s->activated_once) begin_cycle(s, true);
  s->activated_once = true;
}

static bool button(obs_properties_t *, obs_property_t *property, void *data) {
  auto *s = static_cast<Slideshow *>(data);
  if (!s || !s->source) return false;
  const char *name = obs_property_name(property);
  // Properties callbacks run on the UI thread.  Use the public parent-source
  // wrappers so libobs queues each action for the source video tick rather
  // than mutating playlist/private-source state from Qt.
  if (strcmp(name, "next") == 0)
    obs_source_media_next(s->source);
  else if (strcmp(name, "previous") == 0)
    obs_source_media_previous(s->source);
  else if (strcmp(name, "restart") == 0)
    obs_source_media_restart(s->source);
  else if (strcmp(name, "play_pause") == 0)
    obs_source_media_play_pause(s->source,
                                !s->paused.load() && !s->stopped.load());
  else if (strcmp(name, "stop") == 0)
    obs_source_media_stop(s->source);
  return true;
}

static obs_properties_t *properties(void *data) {
  obs_properties_t *p = obs_properties_create();
  obs_properties_t *status_group = obs_properties_create();
  std::string status = "Playback status is available on an active source";
  if (data) {
    auto *s = static_cast<Slideshow *>(data);
    std::lock_guard<std::mutex> lock(s->dashboard_mutex);
    status = s->dashboard_status;
  }
  obs_property_t *status_text = obs_properties_add_text(
      status_group, "playback_status_value", status.c_str(), OBS_TEXT_INFO);
  obs_property_text_set_info_word_wrap(status_text, true);
  obs_properties_add_group(p, "playback_status", obs_module_text("PlaybackStatus"),
                           OBS_GROUP_NORMAL, status_group);
  obs_properties_add_path(p, "folder", obs_module_text("Folder"),
                          OBS_PATH_DIRECTORY, nullptr, nullptr);
  obs_properties_add_float(p, "still_duration", obs_module_text("StillDuration"),
                           0.1, 86400.0, 0.1);
  obs_property_t *order = obs_properties_add_list(
      p, "order", obs_module_text("Order"), OBS_COMBO_TYPE_LIST,
      OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(order, obs_module_text("Alphabetical"), 0);
  obs_property_list_add_int(order, obs_module_text("Date"), 1);
  obs_property_list_add_int(order, obs_module_text("Shuffle"), 2);
  obs_properties_add_bool(p, "loop", obs_module_text("Loop"));
  obs_property_t *transition = obs_properties_add_list(
      p, "transition", obs_module_text("Transition"), OBS_COMBO_TYPE_LIST,
      OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(transition, obs_module_text("Cut"), 0);
  obs_property_list_add_int(transition, obs_module_text("Fade"), 1);
  obs_property_list_add_int(transition, obs_module_text("SwipeLeft"), 2);
  obs_property_list_add_int(transition, obs_module_text("SwipeRight"), 3);
  obs_property_list_add_int(transition, obs_module_text("SlideLeft"), 4);
  obs_property_list_add_int(transition, obs_module_text("SlideRight"), 5);
  obs_property_list_add_int(transition, obs_module_text("FadeToBlack"), 6);
  obs_properties_add_int(p, "fade_ms", obs_module_text("TransitionDuration"),
                         0, 10000, 50);
  obs_properties_add_bool(p, "restart_on_activate", obs_module_text("RestartOnActivate"));
  obs_properties_add_button2(p, "previous", obs_module_text("Previous"), button, data);
  obs_properties_add_button2(p, "next", obs_module_text("Next"), button, data);
  obs_properties_add_button2(p, "restart", obs_module_text("Restart"), button, data);
  obs_properties_add_button2(p, "play_pause", obs_module_text("PlayPause"), button, data);
  obs_properties_add_button2(p, "stop", obs_module_text("Stop"), button, data);
  return p;
}

static void defaults(obs_data_t *settings) {
  obs_data_set_default_double(settings, "still_duration", 30.0);
  obs_data_set_default_int(settings, "order", 0);
  obs_data_set_default_bool(settings, "loop", true);
  obs_data_set_default_int(settings, "transition", 1);
  obs_data_set_default_int(settings, "fade_ms", 500);
  obs_data_set_default_bool(settings, "restart_on_activate", true);
}

static obs_source_info make_info() {
  obs_source_info info {};
  info.id = kId;
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW |
                      OBS_SOURCE_CONTROLLABLE_MEDIA;
  info.get_name = source_name;
  info.create = create;
  info.destroy = destroy;
  info.get_width = width;
  info.get_height = height;
  info.get_defaults = defaults;
  info.get_properties = properties;
  info.update = update;
  info.activate = activate;
  info.video_tick = tick;
  info.video_render = render;
  info.media_play_pause = play_pause;
  info.media_restart = restart;
  info.media_stop = stop;
  info.media_next = next;
  info.media_previous = previous;
  info.media_get_duration = media_duration;
  info.media_get_time = media_time;
  info.media_set_time = media_set_time;
  info.media_get_state = state;
  info.icon_type = OBS_ICON_TYPE_SLIDESHOW;
  return info;
}
} // namespace

bool obs_module_load(void) {
  static const obs_source_info info = make_info();
  obs_register_source(&info);
  blog(LOG_INFO, "[Mixed Media Slideshow] version %s loaded", PLUGIN_VERSION);
  return true;
}

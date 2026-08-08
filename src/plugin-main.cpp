#include <obs-module.h>
#include <graphics/vec2.h>

#include "media-library.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
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
  obs_sceneitem_t *frame_item{};
  std::vector<mms::Item> items;
  std::vector<mms::Item> raw_items;
  std::filesystem::path folder;
  std::size_t index{};
  mms::SortMode order{mms::SortMode::alphabetical};
  std::mt19937 rng{std::random_device{}()};
  double still_seconds{30.0};
  double item_elapsed{};
  double refresh_elapsed{};
  uint32_t transition_ms{500};
  uint32_t frame_width{1920};
  uint32_t frame_height{1080};
  bool loop{true};
  mms::TransitionKind requested_transition{mms::TransitionKind::fade};
  mms::TransitionKind active_transition{mms::TransitionKind::fade};
  bool restart_on_activate{true};
  bool paused{};
  bool stopped{};
  bool activated_once{};
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
  if (!s->frame_item) return;
  struct vec2 bounds {static_cast<float>(s->frame_width),
                      static_cast<float>(s->frame_height)};
  obs_sceneitem_set_bounds_type(s->frame_item, OBS_BOUNDS_SCALE_OUTER);
  obs_sceneitem_set_bounds_alignment(s->frame_item, OBS_ALIGN_CENTER);
  obs_sceneitem_set_bounds_crop(s->frame_item, true);
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

static void detach_media(Slideshow *s) {
  if (s->media)
    obs_source_remove_audio_capture_callback(s->media, audio_capture, s);
  if (s->frame_scene) obs_scene_release(s->frame_scene);
  s->frame_scene = nullptr;
  s->frame_item = nullptr;
  if (s->media) obs_source_release(s->media);
  s->media = nullptr;
}

static bool load_index(Slideshow *s, std::size_t wanted, bool forward) {
  if (s->items.empty()) {
    detach_media(s);
    if (s->transition) obs_transition_clear(s->transition);
    s->stopped = true;
    return false;
  }
  std::size_t candidate = wanted % s->items.size();
  for (std::size_t attempts = 0; attempts < s->items.size(); ++attempts) {
    obs_source_t *next = make_media(s->items[candidate]);
    obs_scene_t *next_frame = next ? obs_scene_create_private(
        "Mixed Media Slideshow cover frame") : nullptr;
    obs_sceneitem_t *next_item = next_frame ? obs_scene_add(next_frame, next) : nullptr;
    if (next && next_frame && next_item) {
      obs_source_add_audio_capture_callback(next, audio_capture, s);

      obs_source_t *old_media = s->media;
      obs_scene_t *old_frame = s->frame_scene;
      s->media = next;
      s->frame_scene = next_frame;
      s->frame_item = next_item;
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

      s->index = candidate;
      s->item_elapsed = 0.0;
      s->stopped = false;
      s->paused = false;
      if (s->items[candidate].kind == mms::MediaKind::video)
        obs_source_media_restart(next);
      blog(LOG_INFO, "[Mixed Media Slideshow] Loaded: %s",
           s->items[candidate].path.u8string().c_str());
      return true;
    }
    if (next_frame) obs_scene_release(next_frame);
    if (next) obs_source_release(next);
    blog(LOG_WARNING, "[Mixed Media Slideshow] Could not create source for: %s",
         s->items[candidate].path.u8string().c_str());
    candidate = forward ? (candidate + 1) % s->items.size()
                        : (candidate + s->items.size() - 1) % s->items.size();
  }
  s->stopped = true;
  return false;
}

static void advance(Slideshow *s, bool forward) {
  if (s->items.empty()) return;
  if (forward && s->index + 1 >= s->items.size() && !s->loop) {
    s->stopped = true;
    if (s->media && s->items[s->index].kind == mms::MediaKind::video)
      obs_source_media_stop(s->media);
    return;
  }
  if (!forward && s->index == 0 && !s->loop) return;
  const auto next = forward ? (s->index + 1) % s->items.size()
                            : (s->index + s->items.size() - 1) % s->items.size();
  load_index(s, next, forward);
}

static void refresh(Slideshow *s, bool force) {
  auto raw = mms::scan_folder(s->folder);
  if (!force && same_raw(raw, s->raw_items)) return;
  const auto current = (!s->items.empty() && s->index < s->items.size())
                           ? s->items[s->index].path : std::filesystem::path{};
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
  } else {
    load_index(s, std::min(s->index, s->items.size() - 1), true);
  }
}

static const char *source_name(void *) { return obs_module_text("Source.Name"); }

static void update(void *data, obs_data_t *settings) {
  auto *s = static_cast<Slideshow *>(data);
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
  if (!replace_transition(s)) return;
  obs_transition_set_size(s->transition, s->frame_width, s->frame_height);
  configure_frame(s);
  refresh(s, true);
  if (!s->media && !s->items.empty()) load_index(s, 0, true);
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
  s->item_elapsed += seconds;
  const auto &item = s->items[s->index];
  if (item.kind == mms::MediaKind::image) {
    if (s->item_elapsed > 1.5 &&
        (obs_source_get_width(s->media) == 0 || obs_source_get_height(s->media) == 0)) {
      blog(LOG_WARNING, "[Mixed Media Slideshow] Skipping unreadable image: %s",
           item.path.u8string().c_str());
      advance(s, true);
    } else if (s->item_elapsed >= s->still_seconds) {
      advance(s, true);
    }
  } else if (s->item_elapsed > 0.75) {
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
static enum obs_media_state state(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  if (s->stopped) return OBS_MEDIA_STATE_STOPPED;
  if (s->paused) return OBS_MEDIA_STATE_PAUSED;
  return s->media ? OBS_MEDIA_STATE_PLAYING : OBS_MEDIA_STATE_NONE;
}
static void activate(void *data) {
  auto *s = static_cast<Slideshow *>(data);
  if (s->restart_on_activate && s->activated_once) restart(s);
  s->activated_once = true;
}

static bool button(obs_properties_t *, obs_property_t *property, void *data) {
  const char *name = obs_property_name(property);
  if (strcmp(name, "next") == 0) next(data);
  else if (strcmp(name, "previous") == 0) previous(data);
  else if (strcmp(name, "restart") == 0) restart(data);
  else if (strcmp(name, "play_pause") == 0) {
    auto *s = static_cast<Slideshow *>(data);
    play_pause(s, !s->paused && !s->stopped);
  } else if (strcmp(name, "stop") == 0) stop(data);
  return true;
}

static obs_properties_t *properties(void *data) {
  obs_properties_t *p = obs_properties_create();
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

file(READ "${SOURCE_FILE}" source)

function(require_literal literal description)
  string(FIND "${source}" "${literal}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Missing ${description}: ${literal}")
  endif()
endfunction()

function(forbid_literal literal description)
  string(FIND "${source}" "${literal}" position)
  if(NOT position EQUAL -1)
    message(FATAL_ERROR "Found forbidden ${description}: ${literal}")
  endif()
endfunction()

# The parent is the single scene-tree audio source. Private ffmpeg audio is
# forwarded explicitly, while the internal video transition is not enumerated.
require_literal("OBS_SOURCE_AUDIO" "parent audio output flag")
require_literal("obs_source_output_audio(s->source, &out)" "private-media audio forwarding")
forbid_literal("info.enum_active_sources" "internal transition audio-tree enumeration")

# Intel-only Windows systems must not be opted into ffmpeg_source's CUDA probe.
require_literal("obs_data_set_bool(settings, \"hw_decode\", false)" "safe ffmpeg decode setting")
forbid_literal("obs_data_set_bool(settings, \"hw_decode\", true)" "forced hardware decoding")

# With no transition audio-tree route, video completion must explicitly collapse
# A/B inputs, and graphics rendering must hold its own transition reference.
require_literal("\"transition_video_stop\", transition_video_stopped" "transition completion signal")
require_literal("obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B)" "completed destination lookup")
require_literal("obs_transition_set(transition, destination)" "transition input collapse")
require_literal("obs_source_t *transition = get_transition_ref(s)" "graphics callback ownership")

# Every media kind uses non-cropping Fit + Center over an opaque black matte.
require_literal("obs_sceneitem_set_bounds_type(s->frame_item, OBS_BOUNDS_SCALE_INNER)" "media fit sizing")
require_literal("obs_sceneitem_set_bounds_crop(s->frame_item, false)" "non-cropping media frame")
forbid_literal("OBS_BOUNDS_SCALE_OUTER" "cover/crop framing")
require_literal([["color_source", "Mixed Media Slideshow black matte"]] "private media black matte")
require_literal([[obs_data_set_int(settings, "color", 0xFF000000)]] "opaque black media matte")
require_literal("const bool background_ready = background_item != nullptr" "matte required for every media kind")

# Activation policy must use libobs's source activation callback.
require_literal("info.activate = activate" "libobs activation callback registration")

# Playback dashboard uses only public media callbacks and source Properties.
require_literal("info.media_get_duration = media_duration" "parent duration callback")
require_literal("info.media_get_time = media_time" "parent timeline callback")
require_literal("info.media_set_time = media_set_time" "parent seek callback")
require_literal("obs_source_media_get_duration(media)" "private-media duration forwarding")
require_literal("obs_source_media_get_time(media)" "private-media time forwarding")
require_literal("obs_source_media_set_time(media" "safe private-media seek forwarding")
require_literal("get_timeline_media(s, video)" "strong private-media timeline reference")
require_literal("OBS_TEXT_INFO" "read-only Properties status")
require_literal("obs_source_update_properties(s->source)" "live Properties status refresh")
forbid_literal("QWidget" "private Qt dashboard injection")
forbid_literal("obs_frontend" "frontend UI dashboard injection")
require_literal("obs_source_media_ended(s->source)" "parent terminal media signal")
require_literal("obs_source_media_next(s->source)" "queued Properties media controls")
forbid_literal("strcmp(name, \"next\") == 0) next(data)" "direct UI-thread playlist mutation")

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

# Photos cover while videos contain over an opaque private black frame.
require_literal("video ? OBS_BOUNDS_SCALE_INNER : OBS_BOUNDS_SCALE_OUTER" "per-media frame sizing")
require_literal("obs_sceneitem_set_bounds_crop(s->frame_item, !video)" "video contain without crop")
require_literal([["color_source", "Mixed Media Slideshow video background"]] "private video black background")
require_literal([[obs_data_set_int(settings, "color", 0xFF000000)]] "opaque black video background")

# Activation policy must use libobs's source activation callback.
require_literal("info.activate = activate" "libobs activation callback registration")

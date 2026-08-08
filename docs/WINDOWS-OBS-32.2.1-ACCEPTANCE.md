# Windows OBS 32.2.1 runtime acceptance checklist

Record the OBS log and exact test media used. Do not mark an item passed unless
it was observed interactively on Windows x64 in OBS Studio 32.2.1.

- [ ] CI job passed, DLL has PE32+ x86-64 headers, and archive layout validation passed.
- [ ] Clean OBS 32.2.1 x64 starts with no plugin loader error; log shows version `1.0.0 loaded`.
- [ ] **Mixed Media Slideshow** appears in the source picker and can be added.
- [ ] Default still duration is 30 seconds and two readable images advance accordingly.
- [ ] MP4, MKV, and WebM test videos play to their natural end; picture is visible and audio reaches the OBS mixer/recording without duplication.
- [ ] A still after a video starts only after that video ends.
- [ ] Adding, renaming, and removing files is reflected within two seconds; the current item stays selected when it still exists.
- [ ] Alphabetical and date (oldest first) orders match the files; shuffle contains each file once per cycle.
- [ ] Loop wraps; loop-off stops at the final item.
- [ ] Cut changes immediately; fade visibly blends for the configured duration.
- [ ] OBS media-toolbar and Properties buttons work for next, previous, restart, play/pause, and stop on both stills and videos.
- [ ] With restart-on-activation enabled, leave and return to the scene: the current item restarts. Disable it and confirm playback position is retained.
- [ ] Unsupported extensions are ignored. Test corrupt image, corrupt video, deleted-current-file, unreadable folder, and empty folder; each fails safely and later valid files still play.
- [ ] Source survives Properties changes, scene duplication, collection save/reload, and repeated OBS shutdown/startup without crash or stale private sources.
- [ ] A short recording contains expected video transitions and synchronized video audio.

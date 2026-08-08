# Windows OBS 32.2.1 runtime acceptance checklist

Record the OBS log and exact test media used. Do not mark an item passed unless
it was observed interactively on Windows x64 in OBS Studio 32.2.1.

- [ ] CI job passed, DLL has PE32+ x86-64 headers, and archive layout validation passed.
- [ ] Clean OBS 32.2.1 x64 starts with no plugin loader error; log shows version `1.0.2 loaded`.
- [ ] **Mixed Media Slideshow** appears in the source picker and can be added.
- [ ] Default still duration is 30 seconds and two readable images advance accordingly.
- [ ] MP4, MKV, and WebM test videos play to their natural end; picture is visible and audio reaches the OBS mixer/recording without duplication.
- [ ] On an Intel-only machine, rapidly alternate Next/Previous among images and audible videos during every transition type for at least five minutes; OBS remains running, audio appears only on the parent source, and the log contains no `nvcuda.dll` load attempts from this plugin's media sources.
- [ ] A still after a video starts only after that video ends.
- [ ] Adding, renaming, and removing files is reflected within two seconds; the current item stays selected when it still exists.
- [ ] Alphabetical and date (oldest first) cycles match the files. Let every item finish twice: Loop starts at the first item after each complete cycle; loop-off stops at the final item, including when that item is a video and after a live-folder change.
- [ ] In Shuffle, observe at least two complete cycles: every supported file appears exactly once in each cycle, the second cycle is newly shuffled, and an unchanged periodic folder scan neither changes the realized order nor replays an item.
- [ ] With enough images to separate the videos, Shuffle never places videos together, including the boundary between two cycles. Then test more videos than images + 1 and confirm only the mathematically unavoidable minimum adjacent-video pairs occurs.
- [ ] The source transform box remains the OBS base-canvas aspect/size while switching among portrait, landscape, and differently sized image/video files; there is no late resize or snap.
- [ ] **Photo Cover:** every photo, including its first visible frame, proportionally fills and stays centered in that fixed box without stretching; only outside edges are center-cropped and there are no bars.
- [ ] **Video Contain:** 16:9 fills naturally. Portrait 1080x1920 and other non-16:9 videos are fully visible, centered, unzoomed, uncropped, and undistorted with solid black letter/pillarbox bars; no underlying scene content leaks through the bars.
- [ ] Cut changes immediately; Fade visibly blends for the configured duration.
- [ ] Swipe Left/Right and Slide Left/Right move in the labeled direction; Fade to Black reaches black between items. Confirm the OBS log reports no transition fallback during these checks.
- [ ] OBS media-toolbar and Properties buttons work for next, previous, current-item restart, play/pause, and stop on both stills and videos, including at cycle boundaries and after a folder refresh.
- [ ] With restart-on-activation enabled, leave and return to the scene: alphabetical/date intentionally starts at its first item and Shuffle starts a newly generated safe full cycle. Disable it and confirm the current item/timer or video position is retained instead. Confirm the explicit Restart control still restarts only the current item.
- [ ] Unsupported extensions are ignored. Test corrupt image, corrupt video, deleted-current-file, unreadable folder, and empty folder; each fails safely and later valid files still play.
- [ ] Source survives Properties changes, scene duplication, collection save/reload, and repeated OBS shutdown/startup without crash or stale private sources.
- [ ] A short recording contains expected video transitions and synchronized video audio.

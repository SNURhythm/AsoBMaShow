# Android device verification — 2026-10-23

## Baseline and build

The release baseline is the successful **main push** Android beta deployment
[run 37307837985](https://github.com/SNURhythm/AsoBMaShow/actions/runs/37307837985),
2026-10-05, commit `8d1a56e535abcf663a86b19d9047623d8505cae4`.
The later local installation and October 17 manual workflow are not this baseline.

Tested `develop` at `105fee897` on a connected Samsung SM-G781N (Galaxy S20 FE),
Android 13, 1080 × 2400, using Vulkan. Built with
`scripts/android_firebase_deploy.sh --build-only --version-code 3` and installed
the signed restricted-file release APK over version code 2. Build and lint passed.
No Firebase or Play upload was performed.

**Result: two confirmed app filesystem failures and one system Files integration
failure. This is a broad device smoke test, not full release certification.**

## Confirmed failures

### Loose-file storage conversion cannot publish outputs

1. Put a BMS, 48 kHz stereo PCM WAV, and MPEG-1 BGA in a folder under Documents/BMS.
2. Select only that folder in storage management; request FLAC and H.264 conversion.
3. Both operations fail with `Invalid argument`; the original WAV and MPG remain.

Observed at 19:44:46 KST. Encoding succeeds, but publication fails in
`src/storage/StoragePlatform.cpp:238`, where `renameNoReplace` uses
`renameat2(..., RENAME_NOREPLACE)`.
A native probe on disposable files reproduced EINVAL on the app's external
Documents filesystem. The same call succeeded in `/data/local/tmp`; ordinary
rename and file/directory fsync succeeded on external storage.

The failure is specific to the publication primitive on this device's external
storage. A replacement must preserve no-clobber and recovery guarantees; simply
using an unchecked ordinary rename would not be equivalent.

### Scores save without their replay

1. Play the test chart through to results.
2. Open its records: the score exists, but Watch is unavailable.
3. Logs report `SavedWithoutReplay` and
   `Atomic replay install failed: Operation not permitted`.

Reproduced on two completed plays at 19:47:53 and 19:48:47 KST.
`src/replay/ReplayFileStore.cpp:765` installs the replay using `create_hard_link`.
Creating a hard link between disposable files in external Documents independently
fails with EPERM. The quarantine restore path also uses hard links at line 486.
Saved-replay playback and the new replay keysound behavior therefore could not be
validated end to end. Synthetic autoplay works.

### Open in Files returns immediately because DocumentsUI crashes

File management → Open in Files launches `com.google.android.documentsui`, which
crashes at 19:57:17 KST with:

```text
NullPointerException: Attempt to invoke interface method
'boolean com.android.documentsui.ConfigStore.isPrivateSpaceInDocsUIEnabled()'
on a null object reference
    at com.android.documentsui.dirlist.IconHelper.<init>(IconHelper.java:81)
```

AsoBMaShow remains running. This is an observed integration failure with a stack
trace in the system Files app; it does not establish an app provider defect.
DocumentsProvider browsing/editing was not validated through that UI.

## Device checks

| Area changed since baseline | Evidence and outcome |
| --- | --- |
| SDL3, renderer, geometry and input | Release startup uses Vulkan. Touch gameplay registered judgements; portrait gameplay, pause, exit, and landscape restoration worked. No AsoBMaShow crash was captured. This does not measure physical input latency or validate simultaneous multitouch. |
| Redesigned shell | Play, Course, Music, and IR tabs reachable. Search retained across tab navigation. Music playback position advanced across a visit to IR. Disabled-account IR state and its Settings/back navigation worked; no account submission tested. |
| Localization and theme | Korean UI and Simplified Chinese switching rendered; light/dark switching worked. Original Korean/dark settings restored. Other locales were not exhaustively inspected. |
| Presentation and skins | LITONE12 7K1S preview/calibration loaded. Built-in 5K preview loaded; note color controls responded. Judgement indicator and keybomb effects rendered in preview/autoplay. Portrait selector/settings/gameplay rendered. Full lane-cover, 5K1S custom-skin, scratchless floating, and customization combinations remain unverified. |
| Offset calibration | Audio/display controls separate; display mute control shown. Cancel discarded changes; Apply persisted audio +10 ms; Reset restored zero. Measurement accepted 16 injected taps and produced a recommendation. Scripted ADB timing is not a valid latency calibration; the recommendation was discarded. |
| ZIP storage conversion | WAV→FLAC and MPEG-1→H.264 succeeded in the selected ZIP: 3,411,657 → 637,614 bytes. ZIP CRC check passed, BMS bytes unchanged, decoded PCM exactly equal (192,000 bytes), and resulting MP4 fully decoded. |
| Converted archive playback | Chart references still used `.wav`/`.mpg`; fallback resolution loaded converted media. BGA rendered and gameplay completed. Two scores persisted. Loose-folder conversion failed as above. |
| Android archive import | ACTION_VIEW with a MediaStore ZIP URI offered extraction and completed with `Imported archive. Library refreshed.` Extracted under Documents/BMS. Other intent/persistence combinations were not exercised. |
| Autoplay/lifecycle | 32-note fixture finished with 32 PGREAT / 64 EX after pause, Home, foreground return, and resume. BGA and keybombs visible. |
| Android video output | `scripts/android_replay_surface_verify.py` passed 320×180/60, 2400×1080/60, and 2400×1080/120 modes, checking frame count, color/orientation, thin lines and duration. This is the native surface harness, not a completed user replay export. |
| Audio decoding/preparation | `scripts/benchmark_android_chart_audio.py` passed direct/prepared/early cases for the small WAV fixture and an existing 814-sound chart. Prepared handoff reported zero opens and decoded frames. Observed prepared gameplay load 93–120 ms vs direct 603–633 ms; these are diagnostic samples, not controlled performance claims. |
| AERY tables/downloads | Existing AERY table rows and a 5K chart were visible. Detection edge cases, gingerrush network downloads, and automatic post-download optimization remain unverified. |

## Limits and cleanup

No full-size archive, interrupted conversion/recovery, low-space, HEVC/Vorbis,
delete-BGA, multiplayer input device, sustained thermal/performance, course-play,
or online-account test was performed. Apple-only video/skin paths do not receive
Android validation. The two filesystem bugs need correction and device retesting
before these workflows can be considered ready.

The installed version-code-3 build was retained. Original profile settings and
application UI settings were restored from pre-test copies. Only the generated
test chart folders, ZIPs, and Downloads import file were removed. Test score rows
were retained; existing user scores were not replaced. No production assets were
converted. Local screenshots, logs, probes, benchmark results, and fixture copies
are in `/tmp/asobmashow-device-test-20261023`; these temporary artifacts are not
committed and may disappear after cleanup/reboot.

## Fix follow-up on `fix/android-device-storage-files`

Attribution against the release baseline:

- Storage conversion's exclusive rename was introduced October 17 in `5da6a59c`.
- The failing Files directory-document launch was introduced October 6 in
  `d45d18db` / `5d2701e2`. Launching the provider root successfully opened the
  same system Files app on this device.
- Replay hard-link installation dates to July 28 (`7fd2ecd56`) and is unchanged
  since the release baseline. The baseline also used external storage for
  profiles, so this appears to be a pre-existing Android incompatibility. The
  old release APK was not rerun to establish an observed before/after result.

Version code 4 was built and installed locally with no distribution. The Files
entry point now launches the system browser's supported root URI. Android storage
and replay publication share the DocumentsProvider mutation monitor and use a
scoped regular-file rename when exclusive rename is unsupported. The fallback
rejects occupied destinations and applies only inside app-owned containers;
external-container fallback additionally requires Android 11+. On earlier or
unknown versions, other apps may write there without participating in the
monitor; see [Android's external directory access restrictions](https://developer.android.com/about/versions/11/privacy/storage#other-apps-data).
Internal-container fallback remains available on older versions. Arbitrary
all-files-access folders still require native exclusive-rename support.

Device retest:

- Open in Files opens AsoBMaShow; navigating into BMS lists the existing library
  and generated fixture without a DocumentsUI crash.
- Loose-folder FLAC/H.264 conversion: 2 converted, 0 failures, 3.1 MiB saved.
  Resulting folder contains the unchanged BMS, FLAC and MP4 (637,272 bytes total).
  Decoded FLAC matches all 192,000 PCM bytes from the source WAV; full MP4 decode
  passes. Existing library media was not selected.
- Completing the converted chart creates a 1,364-byte `.brd` replay. Its record
  offers Watch, including input keysound mode. Saved-replay playback completed
  with the same 0 EX / 32 POOR result.

Host verification: the new publication test and existing storage transaction /
replay store tests pass. The affected library, replay, results and Android groups
pass **125/125**. The signed Android build and lint also pass. Regression coverage
includes file identity preservation, occupied files/directories/dangling links,
unsafe sources, out-of-scope paths, and serialized competing publishers. Existing
transaction tests cover interruption/recovery; physical power-loss recovery was
not exercised on the phone.

Fresh review identified the older-Android external-directory race before push.
Regression cases for unknown SDK, API 28/29, and API 30/33 demonstrated failure
before adding the version gate and passed afterward. The affected 125-test set
was rerun for that change. Profile/UI settings were restored exactly, generated
chart files removed, and the version-code-4 build retained. Test scores and the
new test replay remain. No new app or DocumentsUI crash was captured during the
fix retest.

## Follow-up: custom-skin score and clear display

The reported `"P" stands for "Party!" [Another]` attempts were present in the
score database with replays, but both had `Modified` eligibility, so best-score
and clear queries correctly excluded them. Their normal play settings did not
cause that classification. LITONE12 initially had no published touch layout;
Android required that layout but lacked the iOS retry after the first skin frame.
Fallback judgements then invalidated ranking. Android's layout requirement was
added in `9d6bed07` on October 7, after the October 5 release baseline.

The retry now includes Android. A fallback judgement or mine also cancels the
pending retry, preventing a late frame from replacing already committed fallback
results with a fresh worker. Such late-layout attempts remain Modified; historical
Modified attempts are not rewritten.

Version code 5 was built locally. On the same chart, skin and unchanged EX-HARD
settings, the initial empty layout was followed by canonical authority activation
17 ms later. The completed attempt saved with a replay and Verified eligibility;
the selector immediately displayed `0 / 2886` and `FAILED`. The policy and realtime
worker tests pass (2/2), and the signed Android build/lint pass. Fresh review
confirmed the fallback handoff guards. Settings remained unchanged.

The final guard build was reinstalled and retested with injected touch input:
1 BAD, 5 POOR and 3 KPOOR were saved with Verified eligibility and a replay.
Authority activated 21 ms after the initial missing layout. Score and FAILED
status remained visible after force-stop/relaunch and reselecting the chart.
This verifies failed-clear persistence; a successful full-chart clear was not
performed during this follow-up. The final version-code-5 build remains installed.

## Follow-up: compact Records modal

Compact footer buttons used half the panel content width without subtracting its
two border pixels. Two buttons plus the gap exceeded Yoga's available width,
wrapping all four actions into separate rows and placing the final actions below
the modal. Constructor and resize calculations now use the bordered inner width.

The existing rotation test now enables all four actions and checks each against
the panel bounds, plus the intended two-by-two arrangement. It failed before the
fix and passes afterward at 585×1266, 1266×585 and 480×1000. The same test executable
also verifies desktop restoration and compact export-options scrolling.

Signed Android build/lint passed; version code **5** was reused as requested.
After reinstalling on the connected phone and selecting an existing record,
Watch, G-BATTLE, View Result and Export Video were all fully visible in two rows.
No settings were changed for this check.

The redundant status-message row was removed entirely, moving Share/Delete up
and giving the records list 44 additional layout pixels. Status feedback uses
the existing temporary title message instead. Existing modal layout, scrolling
and rotation checks pass.

The requested final layout uses a single row of four compact actions. Its footer
is 72 pixels tall instead of 152, giving the list another 80 pixels. The rotation
regression now checks one-row alignment, non-overlap and containment of all four
actions at the existing portrait, landscape and narrow sizes.

The final signed version-code-5 build and modal tests pass. Device inspection
confirmed the removed status gap, one-row action captions, and four fully visible
records instead of two. Temporary feedback now restores the active options-page
title as well as the list title. Settings were unchanged.

# Plan: review fixes (main @ 3ab7805)

Source: whole-repo review of 2026-10-05 (5 read-only reviewer agents, key
findings re-verified by the controller). No separate spec file: the review
findings quoted in each task ARE the requirements.

Branch: `review-fixes` (single branch for all tasks, per user).

## Global Constraints

- Target machine: Lenovo Yoga 520-14IKB (Kaby Lake U, 2C/4T, 15 W), Ubuntu,
  generic (non-RT) kernel, TASCAM US-144MKII. Optimise for 2 cores.
- Realtime audio thread rule: code that runs on the audio thread
  (`AlsaDriver` audio loop, `AudioHost::OnProcess` and everything it calls,
  `Lv2Pedalboard::Run`, `Lv2Effect::Run`, `Vst3Effect` process path) must not
  allocate, free, lock a non-PI mutex, log, or make blocking syscalls.
- Match surrounding code style (naming, brace style, comment density). C++20.
  TypeScript/React with MUI in `vite/`.
- Every behavioural change gets a test where the code is testable without
  hardware (Catch2 tests in `src/*Test.cpp`, registered in
  `src/CMakeLists.txt` `pipedaltest`). Frontend has no unit-test runner:
  frontend tasks verify with `cd vite && npx tsc -b && npx eslint <changed files>`
  and `npx vite build --outDir <scratch dir>` (do not overwrite `vite/dist`
  unless the build already writes there).
- C++ build: `cmake --build build -j$(nproc) --target pipedaltest pipedald`
  from repo root (build dir already configured). Run tests with
  `build/src/pipedaltest "<tag>"`. Known pre-existing failures: in `[Build]`,
  `PiPedalAlsaTest` (hardware) and two leak-counter tests. Do not try to fix
  those.
- Do not change: `config/config.json` lv2_path, `ENABLE_VST3` default (0),
  upstream remote, any file outside the repo.
- No network calls to external services from tests or during
  implementation (no live TONE3000 API calls). Mock them.
- One commit (or a few) per task, message in English, imperative, ending with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Tasks

### Task 1: Web server path traversal and exception safety

Files: `src/WebServer.cpp`, `src/WebServerMod.cpp`,
`PiPedalCommon/src/HtmlHelper.cpp`, tests in `src/WebServerTest.cpp` (or a new
`src/UriTest.cpp`-style test registered in `pipedaltest`).

Requirements:
1. Static file handler (`WebServer.cpp` ~1185-1189, the loop doing
   `filename /= requestUri.segment(i)`) and modgui resource handler
   (`WebServerMod.cpp` ~235-238): reject (404) any decoded segment that is
   empty, `.`, `..`, or contains `/`, `\\` or `\0`. After building the path,
   `std::filesystem::weakly_canonical` it and require it to be inside the
   canonical root (reuse an existing `IsSubdirectory` helper if present,
   otherwise add one). Factor the check into one shared function used by both
   handlers.
2. `HtmlHelper::decode_url_segment`: the bounds check `pStart + 2 < pEnd`
   must become a check on `p` (two more chars available after `%`). A
   truncated or invalid escape must not read past `pEnd`.
3. `WebServer::on_http` (and `on_validate` / `on_open` if they can throw):
   wrap the whole body so that no exception escapes; respond 400 for
   `std::invalid_argument` from URL decoding, 500 otherwise, and log.
   `requestHandler->wants()` (~1086) must be inside the guarded region.
4. Tests: a unit test for the shared path-safety function covering
   `%2Fetc%2Fpasswd`, `..%2F..`, `..`, `.`, empty, normal nested path; a test
   that `decode_url_segment` on `"%"`, `"%4"`, `"%zz"` throws
   `std::invalid_argument` (or returns safely) without out-of-bounds reads.

### Task 2: Audio-thread MIDI deferral and zero-input mix bugs

Files: `src/AudioHost.cpp`, `src/Lv2Effect.cpp`.

1. Deferred MIDI (`AudioHost.cpp` ~1050-1128): writer stores
   `[size, bytes...]`, reader reads `[deviceIndex, count, bytes...]`. Make the
   reader match the writer (one length byte, then bytes; length read as
   `uint8_t`). Fix `for (size_t j = 0; j < remaining; ++i)` to `++j`. After
   all deferred messages are replayed without interruption, set
   `deferredMidiMessageCount = 0`.
2. `Lv2Effect::MixOutput` (~1039-1040): `std::max(1.0f, ...)` must be
   `std::min(1.0f, ...)` for both `pluginLevel` and `inputLevel`
   (triangular mix: mix 0 => plugin 0, input 1; mix 0.5 => 1,1; mix 1 =>
   plugin 1, input 0).
3. Extract the deferred-MIDI encode/decode into a small testable helper
   (header-only or in AudioHost) and add Catch2 tests: multiple messages
   round-trip, partial consumption when a snapshot request interrupts,
   count reset after full replay. Add a test for the mix-level function
   (extract the two-level computation into a tiny inline function).

### Task 3: Pedalboard notification data race

File: `src/PiPedalModel.cpp` (~684-705 `FirePedalboardChanged` and any other
place that calls `subscriber->OnPedalboardChanged(..., this->pedalboard)` or
passes `this->pedalboard` by reference to subscribers after releasing
`mutex`).

Take a copy (`Pedalboard snapshot = this->pedalboard;`) while holding the
lock and pass the snapshot to all subscribers. Audit the file for the same
pattern with other model members passed by reference after unlock (presets,
banks) and fix the same way. No test harness exists for multi-threaded model
access; verify by build + existing tests and describe the audit in the
report.

### Task 4: Frontend websocket reliability

Files: `vite/src/pipedal/PiPedalSocket.tsx`, `vite/src/pipedal/PiPedalModel.tsx`,
`vite/src/pipedal/AppThemed.tsx` (+ new small component if needed).

1. Reconnect backoff: 250 ms, 500 ms, 1 s, 2 s, then 3 s cap, with ±20%
   jitter; retry forever (remove `MAX_RETRY_TIME` fatal path and unused
   `MAX_RETRIES`). Retry immediately on `window` `online` event and on
   `visibilitychange` to visible.
2. On connection loss reject every pending request reservation with an
   `Error("Disconnected")`; `request()` while not connected rejects
   immediately; `send()` while not connected: keep only the latest message
   per (message, instanceId, key) for `setControl`/`loadPreset`-style
   idempotent commands and flush them after reconnect + state reload;
   other sends are dropped with a console warning.
3. Ignore socket `onerror` once the socket has opened; let `onclose` drive
   reconnection (no fatal "Server connection lost" modal for a network blip).
4. Replace the full-screen reconnecting modal with a non-blocking banner
   ("Reconnecting…") over the last known UI, which stays visible but
   non-interactive (pointer-events disabled on the main content).
5. On connection lost also clear `monitorPortSubscriptions` and
   `midiListeners`; add `.catch` to the `monitorPort().then` chain (~2601).
6. Background handling: delay the disconnect on page hide by 30 s (re-use the
   commented-out code ~1580-1590 if suitable); cancel if visible again.
7. `loadServerState` (~1462-1530): issue independent requests concurrently
   with `Promise.all` instead of 19 sequential awaits; keep result
   application order where it matters.

### Task 5: Frontend rendering performance and small UI fixes

Files: `vite/src/pipedal/PiPedalModel.tsx`, `PedalboardView.tsx`,
`MainPage.tsx`, `AppThemed.css`, `SettingsDialog.tsx`, `LoadPluginDialog.tsx`,
`VuMeter.tsx`, `ContentAlignment.tsx`, `App.tsx`/dialog import sites,
`vite.config.ts`.

1. Control value changes (`_setPedalboardControlValue` ~1908-1936 and MIDI
   value path ~960): do not clone the whole pedalboard and re-run layout per
   message. Update the value without publishing a new pedalboard object to
   structural observers, notify `_controlValueChangeItems`, and coalesce any
   needed pedalboard publication to at most once per animation frame.
   PedalboardView layout (`makeChain`/`doLayout`) must be memoised on
   structure (ids, uris, enabled, split config), not control values.
   Ensure saved/"changed" state still updates correctly.
2. Dial drag sends (`previewPedalboardValue` -> `_setServerControl`
   ~2111-2142): coalesce to one send per animation frame per
   (instanceId,key), latest value wins; final value always sent.
3. Signal-flow connector animation (`.ppSignalFlow`, LED pulse): off by
   default; add a "Animate signal flow" toggle in Settings (Display section),
   persisted like other display settings (localStorage with try/catch).
4. Code-splitting: `React.lazy` + `Suspense` for heavy dialogs
   (TextInfoDialog, PluginInfoDialog and the markdown stack, Tone3000
   dialogs, Wifi dialogs, SettingsDialog, LoadPluginDialog); add
   `manualChunks` splitting react/mui vendor code; remove the raised
   `chunkSizeWarningLimit` if the main chunk drops under the default.
   Report before/after chunk sizes.
5. A–Z rail in LoadPluginDialog (~614-647, sizes ~69-73): width 36 px, one
   pointer handler on the rail mapping y to letter with pointer-move scrubbing,
   `role="navigation"`/buttons with `aria-label`, keyboard reachable.
6. `VuMeter.tsx` `componentDidUpdate`: compare instanceId against
   `undefined`/`-1` sentinel, not truthiness (instanceId 0 is valid); cancel
   rAF on unmount.
7. `ContentAlignment.tsx`: wrap localStorage access in try/catch.

### Task 6: Lock-free RT ring buffer

Files: `src/RingBuffer.hpp` (+ `src/RingBufferReader.hpp` if needed),
`src/AudioHost.cpp` (writer serialisation), new test `src/RingBufferTest.cpp`.

1. Make the ring buffer single-producer/single-consumer lock-free:
   positions as `std::atomic<int64_t>` (or size_t) with acquire/release;
   `readSpace`/`writeSpace`/`read`/`write` take no lock on either side.
2. Where multiple non-RT threads write the host->RT ring, serialise writers
   on the host side with a mutex that the RT side never touches.
3. Wake-up of the non-RT reader (currently `cvRead.notify_all()` from the RT
   writer): replace with a mechanism that does not take a mutex on the RT side
   (e.g. a POSIX semaphore `sem_post`, or an eventfd write), posted at most
   once per audio cycle. The reader waits with timeout on it.
4. Remove the incorrect "volatile = ordering barrier" comments.
5. Tests: SPSC stress test (producer/consumer threads, 1e6 variable-size
   messages, verify content and order), wrap-around, full-buffer behaviour,
   reader timeout wake-up.

### Task 7: RT thread hygiene

Files: `src/AudioHost.cpp`, `src/AlsaDriver.cpp`, `src/AlsaSequencer.cpp`,
`src/Lv2Pedalboard.cpp`, `src/Lv2Effect.cpp`, `src/Worker.cpp`.

1. rtsvc service thread (`AudioHost.cpp` ~1504): run SCHED_OTHER (nice -5
   if permitted) instead of SCHED_RR 85.
2. Xrun handling (`AlsaDriver.cpp` ~1566, ~1626, ~1670): no logging on the
   audio thread — count xruns in an atomic and log/report from a non-RT
   thread; failed recovery requests a restart from a non-RT thread instead of
   calling `RestartAlsa()` on the audio thread (keep current behaviour when no
   such thread exists — document).
3. Abnormal-stop fallback loop (~1923-1927): drop to SCHED_OTHER, sleep one
   period, and only drain input commands; do not run the DSP chain.
4. MIDI: call `ReadMidiData` once per cycle before the PCM read, not inside
   the partial-read loop; do not reset `midiEventCount` mid-cycle; check
   `snd_seq_event_input_pending(seq, 1)` before `snd_seq_event_input`; remove
   the `midiEvents.resize` on overflow (drop excess events, count them).
5. `RingBufferWriter::write` full-buffer path (`RingBufferReader.hpp`
   ~355-358): no logging from RT; count drops in an atomic.
6. Small per-cycle waste: `Lv2Pedalboard.cpp` ~652 shared_ptr copy per effect
   (use raw/realtime list); input-volume loop order (~522-529: channel loop
   outside sample loop); `buffer->memory.resize` in `Lv2Effect.cpp` ~1393 —
   reserve adequately at activation so RT never grows it (and never resize on
   RT; drop/flag instead).
7. Tests where testable (MIDI read scheduling helper, xrun counter); otherwise
   build + existing tests.

### Task 8: Platform latency tuning

Files: `src/CpuGovernor.cpp`, `src/SchedulerPriority.cpp`, audio thread entry
in `src/AlsaDriver.cpp`, `src/AudioHost.cpp` or new
`src/CpuDmaLatency.{hpp,cpp}`.

1. While audio is running, hold `/dev/cpu_dma_latency` open with value 0
   (int32 write); close on stop. If open fails (permissions), log once and
   continue. Add the needed permission note to the systemd unit/docs only if
   the daemon cannot open it (check `debian/` and install scripts; daemon runs
   as `pipedal_d`).
2. Governor list: read
   `/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors` instead
   of the hard-coded `{performance, ondemand, powersave}` list (fall back to
   the hard-coded list if unreadable); selecting an unavailable governor must
   not throw to the client.
3. `SchedulerPriority.cpp` ~112: use `errno` with `strerror`.
4. Audio thread start: set FTZ/DAZ explicitly (`_mm_setcsr(_mm_getcsr() |
   0x8040)` on x86_64, guarded by arch; aarch64 equivalent via FPCR if simple,
   else no-op).
5. Output conversion clamps (`CopyPlayback*` in `AlsaDriver.cpp`): NaN/Inf
   samples become 0, not INT_MIN.
6. Tests: governor list parsing from a fixture string; NaN clamp conversion.

### Task 9: CPU-use measurement

File: `src/CpuUse.hpp` (+ consumers).

DSP load = time from PCM read return to write start, divided by period
duration (nframes / sampleRate). Publish via `std::atomic<float>` (or a
seqlock), no mutex on the audio thread. Remove the double counting of
`Driver`. Keep the reported JSON shape the UI expects. Add a unit test with a
fake clock.

### Task 10: VU / monitor-port traffic

Files: `src/PiPedalModel.cpp` (~1884-1891), `src/PiPedalSocket.cpp`
(~2413-2440), `vite/src/pipedal/PiPedalModel.tsx` (VU handler), `VuMeter.tsx`.

1. Remove the outer loop that delivers the whole update vector N times.
2. Send one batched `onVuUpdates` message per tick per client containing all
   meter updates, with a single ack; client dispatches to subscribed meters.
   Keep backwards handling of the old single message on the client is NOT
   required (client and server ship together).
3. `updateRequestOutstanding`: make it `std::atomic<int>` or modify only
   under `subscriptionMutex`; it must never get stuck > 0.

### Task 11: Instantiate plugins outside the model mutex

File: `src/PiPedalModel.cpp` (`FirePedalboardChanged` ~684-692,
`LoadCurrentPedalboard` ~3094-3115, `UpdateCurrentPedalboard` ~781).

Build the `Lv2Pedalboard` (`PluginHost::CreateLv2Pedalboard` /
`UpdateLv2PedalboardStructure`) from a copy of the Pedalboard without holding
`mutex`; use a generation counter so a stale build (pedalboard changed again
meanwhile) is discarded; retake the lock to install it and call
`audioHost->SetPedalboard`. `OnNotifyMonitorPort` and other model calls must
not block for the duration of plugin instantiation. Preserve the snapshot
fast path. Keep CrashGuard semantics. Document the threading in a comment.

### Task 12: Persistence safety

Files: `PiPedalCommon/src/ofstream_synced.cpp` (+hpp), `src/Storage.cpp`,
`src/PiPedalModel.cpp`.

1. Atomic save: write to `<file>.tmp`, flush, `fsync(fd)`, `rename` over the
   target, `fsync` the directory. Remove the global `::sync()`.
2. On load, if the main file is missing/empty/unparseable and a
   `.$$$` backup (`SaveBankFile`, Storage.cpp ~816) or `.tmp` exists and
   parses, restore it (log). `SaveBankIndex` uses the same atomic write.
3. Autosave the current (edited) preset 3 s after the last change using the
   existing delayed-post mechanism (`PostDelayed` or equivalent), in addition
   to the existing shutdown save; do not write while nothing changed.
4. Tests: atomic write leaves either old or new content (simulate by checking
   tmp+rename sequence), backup restore path.

### Task 13: Socket reservations, uploads, static caching

Files: `src/PiPedalSocket.cpp` (~1018), `src/WebServer.cpp`,
`src/WebServerConfig.cpp` (~1121-1144), `config/config.json`
(`maxUploadSize` only).

1. Request reservations: own them (`std::unique_ptr` or value), store in an
   `unordered_map` keyed by reply id, erase on reply, on send failure and in
   `FinalCleanup`.
2. Uploads: default `maxUploadSize` 64 MiB; at most 2 concurrent uploads
   (reject with 503 otherwise); `t3k_uploadAsset` and `IsSafeMediaPath`
   accept only known audio/IR/model extensions (.wav .flac .mp3 .ogg .aiff
   .nam .json .zip as currently used — check callers) and must not delete an
   existing file before the new one is fully written (write temp then rename).
3. Static files: send `ETag` (size+mtime) and honour `If-None-Match` with 304
   for files under the web root; keep existing gzip sibling logic.
4. Tests for extension allow-list and ETag generation/match.

### Task 14: Suspend bypassed plugins (opt-in)

Files: `src/Lv2Effect.cpp` (~1093-1196), `src/Lv2Pedalboard.cpp` (~532-535),
settings plumbing (`src/PiPedalModel.cpp`, config/settings JSON, Settings UI).

Add a setting "Suspend bypassed plugins (saves CPU)" default OFF. When ON:
after a plugin's bypass crossfade has fully completed, skip
`lilv_instance_run` (output = input passthrough as the bypass path already
produces); on re-enable, run normally (crossfade in). For A/B splits, skip
the branch whose blend is exactly 0 once the transition completed. Setting
reaches the RT thread via the existing host->RT message path (no locks).
Test the state machine (fade complete => suspended) with a unit test if the
logic can be isolated.

### Task 15: Reuse plugin instances across preset switches (NAM load time)

Files: `src/PluginHost.cpp` (~1568-1600), `src/Lv2Pedalboard.cpp` (~115-119),
`src/PiPedalModel.cpp` (`LoadCurrentPedalboard`), `src/Lv2Effect.*`.

When loading a pedalboard whose structure differs, reuse an existing effect
instance from the outgoing pedalboard if: same plugin URI, same
channel/IO configuration, and identical persisted plugin state (LV2 state
blob and path-type patch properties such as model/IR file). Matching by
instance id is NOT required for this path; each old instance is reused at
most once. Reused instances get control values applied through the normal
path and their instance id remapped consistently for RT message routing
(check what `ExistingEffectMap` keyed by instance id assumes and adapt
safely). If any doubt about a plugin's internal state, do not reuse (VST3:
never reuse). Log at debug level how many instances were reused. Add a unit
test for the matching function (pure logic on Pedalboard items).

### Task 16: VST3 SetControl off the audio thread

Files: `src/Vst3Effect.cpp` (~195-206), `src/vst3/Vst3EffectImpl.hpp`
(~303, ~465, ~702).

Do `plainParamToNormalized` / `setParamNormalized` on the non-RT side before
the value is queued to RT; RT receives an already-normalised value. Replace
`parameterMutex` on the RT path with a lock-free transfer (e.g. VST3 SDK
`ParameterChangeTransfer` if available, or an SPSC queue). `ENABLE_VST3` is 0
by default: the code must still compile with it 0; if the VST3 SDK is
available at `../vst3sdk` build with it enabled to verify, else report that
the VST3 path was not compiled.

### Task 17: TONE3000 device-code sign-in with QR

Files: `src/Tone3000Downloader.cpp` (+hpp), new
`src/Tone3000Auth.{hpp,cpp}` if cleaner, `src/PiPedalSocket.cpp` (new
messages), `vite/src/pipedal/t3k/*`, Tone3000 dialog UI.

Implement OAuth 2.0 device authorization grant (RFC 8628) in the daemon:
`POST {origin}/api/v1/oauth/device_authorization` (form: `client_id`) ->
`device_code, user_code, verification_uri, verification_uri_complete,
expires_in, interval`; poll `POST {origin}/api/v1/oauth/token` with
`grant_type=urn:ietf:params:oauth:grant-type:device_code`, `device_code`,
`client_id`; handle `authorization_pending`, `slow_down` (+5 s),
`expired_token`, `access_denied`. Store access+refresh token in the daemon's
private storage (file mode 0600); refresh with `grant_type=refresh_token`
60 s before expiry and once after a 401. UI: "Sign in with phone" showing a
QR code of `verification_uri_complete` plus the user code, with status
updates over the websocket. Keep the existing popup flow as fallback if the
device endpoint returns an error. Use the existing client id constant (do
not add new keys). Reference implementation (MIT, read-only):
`/tmp/claude-1000/-projects-audio-pipedal/f7540e20-1e35-4e81-bf45-95478f7944d6/scratchpad/tone3000-plugin/plugin/ui/services/OAuth.cpp`
and `Tone3000Session.h`. Use an existing HTTP client in the codebase (check
how `Tone3000Downloader.cpp` does HTTP). QR: use a small MIT/ISC JS QR
library already in node_modules if present, otherwise add `qrcode` npm
package. Tests: token-response parsing and polling state machine with mocked
HTTP (no live calls).

### Task 18: In-app TONE3000 catalog browser

Files: daemon proxy endpoints (`src/Tone3000*.cpp`, `src/PiPedalSocket.cpp`),
`vite/src/pipedal/t3k/*` new browser dialog.

Proxy through the daemon (token stays server-side), endpoints (Bearer auth,
origin `https://www.tone3000.com/api/v1`): `GET /tones/search` (params
`page, page_size, query, sort, gears, format, tags, makes, creators,
calibrated, verified, architecture`; tags/makes joined with `_`, creators
with `,`; sort bestMatch|trending|popular|newest|oldest; default sort
bestMatch with query text else trending), `GET /tones/trending[?gear=]`
(anonymous allowed), `GET /tones/{favorited|downloaded}`,
`PUT/DELETE /tones/{id}/favorite`, `GET /models?tone_id&page&page_size&architecture=2`,
`GET /tags`, `GET /makes`. Model download reuses the existing server-side
download path. UI: search box, gear/format chips, sort, paged results
(virtualised list), favourite toggle (optimistic), per-tone model picker,
download button feeding the existing downloader; usable at phone width.
Reference: `.../scratchpad/tone3000-plugin/plugin/ui/services/Tone3000Client.cpp`,
`model/ToneQuery.cpp`. Tests: query-string builder and response parsing
(C++ side) with fixtures.

### Task 19: IR classification on download

Files: TONE3000 download path (`src/Tone3000Downloader.cpp`), wherever IR
downloads choose a destination/plugin.

Classify downloaded IRs: catalog gear `cab` => cab IR, `space` => reverb IR;
unknown gear => length < 1.0 s cab, else reverb. Store into the matching
existing upload directory (cab IRs vs reverb impulses — check existing
directory names used by ToobConvolution/ToobCabIR in PiPedal's audio_uploads
layout). Unit test the classifier.

### Task 20: Upstream requests document for ToobAmp

File: `docs/TOOBAMP-UPSTREAM-REQUESTS.md`.

Write a concise document (for opening issues on rerdavies/ToobAmp) covering:
NAM core v0.6.0 update (hot-path allocation fix #312, cached A2 prewarm #319,
LSTM RT-safety fix), improved noise gate (band-passed sidechain 80 Hz–5 kHz,
5 dB hysteresis, hold, 4:1 expander close), optional binary model cache
(.namb, JSON fallback for A2/slimmable), model latency/polarity probe. Cite
sources (tone3000-plugin, nam-pedal, NAM core release notes). No code.

## Deferred (not in this plan)

- lilv plugin metadata cache / startup scan cache (L effort, measure first).
- Out-of-process VST3 scanning (L).
- Phase-interleaved NAM oversampling and RT worker pool (CPU cost too high on
  2 cores).
- Model latency probe inside PiPedal (needs NAM runtime, which PiPedal does
  not link; listed in Task 20 for ToobAmp).
- CPU affinity pinning of the audio thread (needs measurement on the Yoga).

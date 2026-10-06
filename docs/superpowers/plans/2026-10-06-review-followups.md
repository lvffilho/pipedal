# Plan: review follow-ups (deferred minors from 2026-10-05 review-fixes)

Source: deferred-minor list from the review-fixes ledger
(`.superpowers/archive/2026-10-05-review-fixes/progress.md`, extracted to
`.superpowers/archive/minors.txt`). Items already fixed by the final fix wave
(e097f0b, 8db87b0) are excluded. Branch: `review-fixes` (continue on it).

## Global Constraints

Same as `docs/superpowers/plans/2026-10-05-review-fixes.md` Global Constraints:
RT audio thread rule (no alloc / non-PI mutex / log / blocking syscalls), match
surrounding style, Catch2 tests in `pipedaltest` where testable, frontend verified
with `npx tsc -b` + vite build to a scratch dir, no live TONE3000 calls, `[Build]`
baseline 7 cases 4 pass 3 fail, commits end with
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
Build: `cmake --build build -j24 --target pipedaltest pipedald`.

## Tasks

### Task F1: Web server hardening follow-ups
Files: src/WebServer.cpp, src/WebServerConfig.cpp, PiPedalCommon/src/HtmlHelper.cpp, src/WebPathSafetyTest.cpp.
1. Canonicalise the static web root once at startup (not per request) and pass it to TryResolveUnderRoot.
2. std::invalid_argument (URL decode) thrown inside intercept handler bodies -> 400, not 500.
3. When serving a `.gz` sibling, containment-check it (resolved path under root) like the main file.
4. WebServerConfig.cpp path endpoints (download/upload/thumbnail: ~529, 716, 841/849, 908, 940, 1121) additionally use the canonical containment helper (keep existing checks).
5. Tests for the 400/500 mapping at helper level (factor the status mapping into a testable function).
6. Guard-page mmap test: RAII wrapper so REQUIRE failure doesn't leak.
7. NotFound reuses ErrorResponse.

### Task F2: Uploads, temp files, IR, packaging follow-ups
Files: src/UploadPolicy.hpp, src/WebServerConfig.cpp, src/TemporaryFile.cpp/.hpp, src/IrClassifier.cpp, src/ConfigMain.cpp, src/WebServer.cpp.
1. Upload allow-list: include every audio extension in MimeTypes::AudioExtensions (.m4a .aac .opus etc.) plus existing .nam .json .zip .md.
2. `.uploading` temp name unique per upload (mkstemps sibling in target dir) so concurrent uploads to the same target can't collide in the copy fallback.
3. Slow-loris: bound how long an upload slot can be held (websocketpp/asio read timeout or a slot age limit enforced at body progress) — if websocketpp gives no hook, document precisely and skip.
4. TemporaryFile move ctor/assign explicitly clear the source path.
5. IrClassifier GetWavLengthSeconds: clamp data chunk size to file size; share the "/var/pipedal/audio_uploads" constant with IsSafeMediaPath instead of hard-coding.
6. ConfigMain: after installing the udev rule run `udevadm control --reload` and `udevadm trigger` for the cpu_dma_latency device (ignore failure).
7. Tests where isolable (allow-list, temp name uniqueness, WAV clamp).

### Task F3: Frontend follow-ups
Files: vite/src/pipedal/PiPedalSocket.tsx, PiPedalModel.tsx, AppThemed.tsx, LoadPluginDialog.tsx, t3k/Tone3000CatalogDialog.tsx, t3k/tone3000-catalog.ts.
1. Foreground return: only one reconnect path (model's exitBackgroundState owns it; socket's visibility handler skips if a retry/attempt is already pending).
2. Socket `online`/`visibilitychange` listeners removed on dispose/close (add a dispose method used when the socket is replaced).
3. Reconnect banner must not cover the app bar's top strip (offset below app bar or use a Snackbar-style placement).
4. A–Z rail: ignore non-primary pointer buttons.
5. Catalog: cap search text by UTF-8 byte length (TextEncoder) to match the server's 200-byte limit; "All models" must not trigger the downloader's second picker (pass an explicit all-selected flag); model count label uses downloadableModelCount (respects A2 pref); failed load-more doesn't auto-retry on scroll (only Retry button); filter panel scrollable (overflowY auto) on short screens; ensure download status dialog stacks above the full-screen catalog dialog; serialise favourite toggles per tone (ignore taps while a request for that tone is in flight).
Verify: `cd vite && npx tsc -b`, vite build to scratch dir.

### Task F4: RT path / ring buffer follow-ups
Files: src/RingBuffer.hpp, src/RingBufferTest.cpp, src/AlsaDriver.cpp, src/AlsaDriverRealtime.hpp, PiPedalCommon/src/AlsaSequencer.cpp(+hpp), src/AudioHost.cpp, src/DummyAudioDriver.cpp, src/Lv2Effect.cpp.
1. RingBuffer::waitFor: sem_trywait to drain a stale post before returning Ready (no unbounded sem count growth); deadline computation safe at time_point::min/max.
2. RingBufferTest: wrap-around test verifies every message; no REQUIRE on the main thread while threads are joinable (record failure, join, then REQUIRE); wake test made deterministic (reader signals it is about to block).
3. Report droppedPathPatchProperties and host-writer droppedWrites in rtsvc's periodic statistics.
4. PeriodDuration(): read bufferSize/sampleRate via atomics (or snapshot published at open) — no data race with rtsvc restart.
5. resync_streams failure: keep the error text (copied into a preallocated fixed buffer, no allocation on RT) and log it from rtsvc with the restart.
6. AlsaSequencer::midiChannel(): replace the per-event std::mutex lock on RT with an atomic; remove dead IsChannelSelected.
7. ESTRPIPE: fix the sign (`-ESTRPIPE`) and route suspend recovery through the existing deferred-restart hand-off (no resume/sleep loop on the RT thread).
8. DummyAudioDriver: same once-per-cycle, fixed-capacity MIDI pattern as AlsaDriver.
Tests where isolable.

### Task F5: CpuUse / governor follow-ups
Files: src/CpuUse.hpp/.cpp, src/JackDriver.cpp, src/DummyAudioDriver.cpp, src/CpuUseTest.cpp, src/CpuGovernor.cpp, src/PiPedalModel.cpp, src/PiPedalSocket.cpp, src/CMakeLists.txt.
1. Publish CPU use + overhead as one consistent snapshot (pack into one atomic<uint64_t> or seqlock).
2. Jack and Dummy drivers mark read/write points consistently (or document why not if impossible).
3. Remove the comment-only CpuUse.cpp translation unit from the build.
4. Tests: period-unset fallback, window rollover/ramp-up, xrun gap.
5. CpuGovernor writeAndVerify: include <iostream> or switch to Lv2Log.
6. Rejected governor: SetGovernorSettings returns an error to the client (handler replies error; UI reverts optimistic value).
7. On load, a persisted governor not in the available list is replaced by the current governor (persisted), so the UI shows the real value and the monitor thread doesn't retry.

### Task F6: VST3 follow-ups
Files: src/Vst3Effect.cpp, src/vst3/Vst3EffectImpl.hpp, src/vst3/Vst3RtParameterSlots.hpp, src/Vst3RtParameterSlotsTest.cpp, src/CMakeLists.txt.
1. restartComponent(kParamValuesChanged): call FlushControlChanges() before refreshControlValues() instead of discarding queued SetControl values.
2. Check sem_init return value (throw on failure at construction).
3. Guard the [vst3_rt] test / static_assert so builds on targets without lock-free atomic<double> (armv6) still compile (test skipped there).
4. performEdit / fireControlChanged take controllerMutex around controller calls.
5. Worker thread: modest priority (SCHED_OTHER nice -5 if permitted) to bound latency under load.
Compile with ENABLE_VST3=0 (default) AND verify with a scratch ENABLE_VST3=1 build using the SDK at /projects/audio/vst3sdk (edit only a scratch copy of src/CMakeLists.txt, never commit the flag).

### Task F7: Model / persistence / TONE3000 backend follow-ups
Files: src/Storage.cpp/.hpp, src/PiPedalSocket.cpp, src/Tone3000Auth.cpp/.hpp, src/Curl.cpp, src/Lv2Pedalboard.cpp/.hpp, src/Lv2Effect.cpp, src/PiPedalModel.cpp, related tests.
1. Storage::GetBankFile uses RecoverFileFromBackup like LoadBankFile; a failed restore must not delete the `.tmp` candidate (write recovery to a different temp name); currentPreset write mutex at file scope (already hoisted? verify) and documented.
2. vuUpdateDropped: std::atomic<bool>.
3. TONE3000 auth: track detached t3kAuthGetAccessToken and catalog threads (counter + wait with timeout in Close, or weak alive flag in the listener) so none touches the model after destruction; clear deviceState/userCode/verificationUri* after sign-out; a client closing its dialog cancels only a flow it started (track starter client id); coalesce forced refreshes (skip if refreshed < 5 s ago); Curl's /tmp/PipedalCurl.log created 0600 (or under the daemon's private dir).
4. Instance reuse: BorrowedEffect pointer vectors start null-filled with the effect's sizes (no copy of live vectors); clear borrowedEffect where appropriate after the swap; narrow the model-mutex hold in FindReusableInstances (copy candidate list under lock, call save() outside if safe w.r.t. effect lifetime — keep a shared_ptr to the running pedalboard).
5. Dedupe sidechainSourceEffectIndices.
6. Unit test for FindReusableInstances matching inputs (live-state description) if isolable.

### Task F8: Tests and docs follow-ups
Files: src/DeferredMidiTest.cpp, docs/TOOBAMP-UPSTREAM-REQUESTS.md.
1. Deferred MIDI: boundary tests (size >= 128 rejected, exact capacity boundary, program-change replay test with a callback that calls Clear()).
2. TOOBAMP doc: mark the unsourced latency example (L61-62) [unverified] or cite it; trim the context bullets.

## Not doing (rulings)
- Hardware validation items (ToobNAM reuse match rate, resume sound, RT timing on the Yoga) — need the device.
- Pre-existing live buffer re-point hazard in the edit-time borrow path (needs RT-side handoff; large) and reuse-build failure after re-point (pre-existing, now staged in preset reuse).
- previewControl reorder (<= 1 frame), wake-post wording deviation, IsOriginUrl exact match (fails safe), frozen output controls while suspended (documented), reuse install skipping config check (safe via borrow check), VST3 state merge skipped during builds (VST3 off by default), stale borrowing builds installed (harmless), installed configs keeping 512 MiB (packaging), dropped EffectReplaced/FreeVuSubscriptions on ring-full (only counted; requires ring sizing redesign), gear enum premise for IRs (needs live API), parked audio thread timeout (bounded by terminateAudio), failure-path allocation on RT after recovery failed (already failed path).

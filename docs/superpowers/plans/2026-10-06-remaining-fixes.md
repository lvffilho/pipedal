# Plan: remaining fixes (everything not needing the Yoga hardware)

Source: items parked or ruled "not doing" in
`docs/superpowers/plans/2026-10-06-review-followups.md` and its ledger
(`.superpowers/archive/2026-10-06-review-followups/progress.md`), plus the earlier
ledger (`.superpowers/archive/2026-10-05-review-fixes/progress.md`), plus the
pre-existing test/tooling failures. Branch: `review-fixes` (continue; base 10f6a5d).

## Global Constraints

Same as `docs/superpowers/plans/2026-10-05-review-fixes.md`: RT audio thread rule
(no alloc / free / non-PI mutex / log / blocking syscall), match surrounding style,
Catch2 tests in `pipedaltest` where testable, frontend verified with `npx tsc -b`
+ vite build to a scratch dir, commits end with
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
Build: `cmake --build build -j24 --target pipedaltest pipedald`.

Live TONE3000 API calls ARE allowed now. Credentials live in `~/.env.tone3000`
(`PUBLISHABLE_KEY`, `SECRET_KEY`, and a legacy API key — list the variable names with `sed "s/=.*/=<redacted>/" ~/.env.tone3000`). Never print, log, commit or copy their values;
read them at runtime only; any live test is hidden (`[.]`) and skips cleanly when
the file is absent.

## Tasks

### Task G1: Borrowed-instance re-point via RT hand-off
Files: src/Lv2Pedalboard.cpp/.hpp, src/Lv2Effect.cpp/.hpp, src/AudioHost.cpp, src/PiPedalModel.cpp, related tests.
1. Edit-time borrow path (pedalboard edits that reuse running instances) still re-points a live effect's buffers from the non-RT thread while the audio thread may be running it. Route it through the same staged hand-off used by preset reuse (staged pointers applied in Lv2Pedalboard::UpdateAudioPorts on the audio thread).
2. A reuse build that fails after staging must leave the running pedalboard's effects untouched (no freed buffers referenced).
3. Staging-branch zero-input borrowed outputs: borrowed effects in a zero-input branch get correctly staged outputs.
4. Tests for the staging/rollback logic where isolable.

### Task G2: Model / builder concurrency leftovers
Files: src/PiPedalModel.cpp/.hpp, src/PedalboardBuilder.hpp, src/AudioHost.cpp (only if needed), tests.
Run after G1.
1. Reuse install must check the audio-config version like a full build does.
2. A borrowing build that is superseded or stale must not be installed.
3. VST3 state merge must not be skipped while a full build is outstanding (merge after it lands, or include VST3 state in the reconciliation snapshot).
4. Edits made during an outstanding build must target the pedalboard that will be installed (not only the old one by id); post-install pre-swap window: colliding non-reused ids must not receive the old path property, and RT parameter requests must go to the right pedalboard.
5. PreviewControl: no unlocked double read of the shared_ptr (take one local copy under the proper lock/atomic).
6. Narrow the model-mutex hold in FindReusableInstances safely: snapshot what's needed (including plugin state via the existing state-save path, serialised against UpdatePluginState) so save()/mainThreadPathProperties are not raced; if truly impossible, prove it in a comment with the exact race.

### Task G3: Audio-host message reliability
Files: src/AudioHost.cpp/.hpp, src/RingBuffer.hpp (if needed), tests.
Run after G1.
1. Dropped EffectReplaced / FreeVuSubscriptions messages on a full host→audio or audio→host ring must not leak or be lost: retry from a non-RT pending list on the non-RT side, or reserve capacity for them, so the effect/VU subscription is eventually freed.
2. previewControl sends must not reorder against immediate sends (keep per-control ordering).
3. Ring wake posts: per audio cycle at most one wake post per reader (match spec wording) or document precisely why per re-arm is equivalent.
4. Tests.

### Task G4: ALSA restart / parked audio thread
Files: src/AlsaDriver.cpp, src/AlsaDriverRealtime.hpp, src/AudioHost.cpp (callback only), tests.
1. While the audio thread is parked in WaitForDeferredRestart (up to ~10 s of retries), keep processing host commands (OnProcessCommandsOnly or equivalent) so UI commands are not dropped.
2. Parked-thread wait has an explicit upper bound consistent with the retry budget.
3. Failure paths after a failed recovery must not allocate on the RT thread.
4. Tests where isolable (state machine helpers).

### Task G5: VST3 controller re-entrancy
Files: src/Vst3Effect.cpp, src/vst3/Vst3EffectImpl.hpp, src/vst3/Vst3RtParameterSlots.hpp, tests.
1. restartComponent must never block a thread that may be the audio thread: try_lock; if busy, defer the restart work to the control worker.
2. Re-entrant restartComponent from inside FlushControlChanges: parameterValues for slots not yet delivered must not be left stale (snapshot pending set; refresh skips/after-fixes those indices).
3. Verify with default build AND a scratch ENABLE_VST3=1 build (SDK at /projects/audio/vst3sdk; never commit the flag).

### Task G6: TONE3000 live verification and fixes
Files: src/Tone3000*.cpp/.hpp, src/Curl.cpp, src/IrClassifier.cpp, src/WebServerConfig.cpp, src/PiPedalSocket.cpp, vite/src/pipedal/t3k/*, tests.
1. Find out what PUBLISHABLE_KEY / SECRET_KEY / the legacy API key are for in the TONE3000 API (public docs, tone-3000 repos) and how PiPedal should use them (if at all). Do not embed the secret in the frontend or in committed code.
2. Hidden live integration test(s) `[.][t3k_live]` reading ~/.env.tone3000 at runtime: exercise the catalog/search, tone info, model list endpoints PiPedal uses; assert the response shapes PiPedal parses still match.
3. Verify the real gear enum values (amp/full-rig/pedal/outboard/ir …) and fix IrClassifier's mapping / premise accordingly.
4. Curl: bound each request's max-time below Tone3000Auth's CLOSE_TIMEOUT on the shutdown path (or make Close abort in-flight curl).
5. IsOriginUrl: exact string match → parse and compare scheme/host/port.
6. Any mismatch found live → fix it with a unit test using recorded (sanitised) fixtures.

### Task G7: Frontend leftovers and ESLint
Files: vite/src/pipedal/*, vite/eslint.config.*, vite/package.json.
1. Pending reconnect queue: when loadServerState fails with a non-disconnect error, clear the pending queue (no stale loadPreset replay).
2. Fix the broken ESLint config ("Unexpected key server") so `npx eslint` runs; fix or explicitly disable (with reason) the resulting errors in files touched on this branch only.
3. `npx tsc -b`, vite build, eslint.

### Task G8: Small leftovers
Files: src/DeferredMidi.hpp(+Test), src/UploadSlowLorisTest.cpp, src/WebServer.cpp, src/AudioHost.cpp (stats read only), src/CpuGovernor.cpp, packaging config files, tests.
1. DeferredMidi capacity rule: exact-fit (`size + 1 + count <= BUFFER_SIZE`), RT-safe; update tests.
2. UploadSlowLorisTest: make the ::1 attempt meaningful (bind the test server to match) or remove it.
3. websocketpp logs slow-loris request errors at elevel::fatal: demote via PiPedal's websocketpp logger hook (filter by message/channel) to debug.
4. AudioHost stats: read CPU use and overhead from one CpuUse snapshot.
5. CpuGovernor writeAndVerify: warn once per retry loop, not every iteration.
6. Installed configs keep 512 MiB upload limit: find the packaged config (debian/ or config/ templates) and the upgrade path; make the default consistent with the code (or document in the config why 512 MiB).

### Task G9: Pre-existing test failures
Files: as needed (tests and code).
1. `[Build]` 3 failing cases: diagnose each; fix the code or the test (if the test is environment-dependent, make it skip with a clear reason when the environment is missing).
2. `vst3test` segfault (scratch ENABLE_VST3=1 build only): diagnose and fix.
3. Report root causes.

## Not doing
- Hardware validation on the Yoga (resume sound, RT timing, ToobNAM reuse rate).
- Frozen output controls/VU while a plugin is bypass-suspended: intentional design (documented).

### Task G10: Pre-swap window messages and parameter-request completion
Files: src/AudioHost.cpp/.hpp, src/RingBufferReader.hpp, src/PiPedalModel.cpp, tests.
Run after G3.
1. MidiValueChanged, AtomOutput and OnPatchSetReply produced by the outgoing pedalboard between a full-build install and the audio-thread swap must not be applied to a new item that shares the instance id (carry source identity, e.g. instance-id lineage or effect pointer, in the ring message and check it in the model).
2. ParameterRequestComplete must never be lost: when the audio→host ring is full, defer it like the other release messages; at Close, complete any request whose completion is still in the output ring.
3. sendRealtimeParameterRequest: close the race between the `active` check and Close() draining the ring (take the lock or re-check under it) so every request completes.
4. Tests.

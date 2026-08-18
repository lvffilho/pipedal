# VST3 revival and hotspot crash fix

Notes for the branch `vst3-and-hotspot-fixes`, taken off upstream `32c45bf`.

Two unrelated things live here. The hotspot fix and the `AlsaSequencer`
fix stand on their own and matter to every install. The VST3 work only
matters if `ENABLE_VST3` is turned on.

## Validation environment

- Ubuntu (kernel 7.0.0-29-generic), x86_64, 4 cores / 7 GB RAM
- GCC 15, `-DRELEASE -DNDEBUG`
- VST3 SDK checked out beside the repo as `../vst3sdk`
- Test plugins: `adelay` and `mda-vst3` built from the SDK samples, plus 17
  DPF-based plugins from the distro `zam-plugins` package. 52 plugins total.
- Runtime harness: the `vst3test` target, re-enabled by this branch.

The SDK's own `validator` was run against the sample plugins first
(1598 tests passed, 0 failed) so that any failure seen afterwards could be
attributed to the host rather than the plugin.

## 1. `AlsaSequencer`: a lock_guard that locked nothing

`midiChannel()` and `midiChannel(uint32_t)` wrote:

```cpp
std::lock_guard { connectionsMutex };
```

That is an unnamed temporary. It is destroyed at the end of the full
expression, so the mutex is released immediately and both accessors to
`midiChannel_` ran unsynchronised. Naming the guard fixes it.

## 2. `HotspotManager`: segfault loop when the wi-fi device is unmanaged

### Symptom

`pipedald` in a `SIGSEGV` restart loop — 55 core dumps in 11 minutes,
restarting every ~11 seconds. Identical journal sequence each time:

```
HotspotManager: Enabling PiPedal hotspot.
HotspotManager: Activation failed: [org.freedesktop.NetworkManager.UnknownConnection]
  Connection 'PiPedal Hotspot' is not available on device wlp2s0
  because device is not available
HotspotManager: PiPedal hotspot disabled.
-> SIGSEGV
```

### Cause

`StartHotspot()` is reached only from D-Bus signal handlers
(`OnWlanStateChanged`, `OnEthernetStateChanged`, `onAccessPointsChanged`).
Its `catch` called `onError()`, which calls `ReleaseNetworkManager()`,
which destroys the `NetworkManager` and `Device` proxies — while their own
dispatch is still on the stack.

`onError()` additionally latches `State::Error`, and `MaybeStartHotspot()`
returns early on that state. So even without the crash, one transient
failure disabled the hotspot until the daemon was restarted.

### How to reproduce

On a netplan-managed system, let netplan configure the wi-fi interface and
set `autoStartMode` in `/var/pipedal/config/wifiConfig.json` so the hotspot
is wanted. netplan emits a udev rule setting `NM_UNMANAGED=1` for any
interface it configures:

```
/run/udev/rules.d/90-netplan.rules:
  SUBSYSTEM=="net", ..., ENV{ID_NET_NAME}=="wlp2s0", ENV{NM_UNMANAGED}="1"
```

NetworkManager then reports the device as unmanaged, reason 77 ("unmanaged
via udev rule"), `AddAndActivateConnection2` throws, and the daemon dies.

Confirm with `nmcli -f GENERAL.STATE,GENERAL.REASON device show <wlan>`.

### Fix

Treat activation failure as recoverable: log it, drop the active
connection, return to `State::Monitoring`, and let the existing device and
state-change handlers retry. No proxy is destroyed inside the callback, and
no permanent error state is latched.

### Operator-side note

This is not a pipedal misconfiguration to work around, but on a
netplan/networkd host the hotspot cannot work at all until NetworkManager
owns the wi-fi device. Removing the `wifis:` block from the netplan config
is enough: the udev rule flips to `NM_UNMANAGED="0"` and, because
`/usr/lib/NetworkManager/conf.d/10-globally-managed-devices.conf` already
carries `except:type:wifi`, NetworkManager picks the device up on its own.

## 3. Build plumbing

- `vst3_lib` compiled `public.sdk/source/vst/utility/stringconvert.cpp` but
  not `common/commonstringconvert.cpp`. Current SDK releases split the
  helpers and moved `Steinberg::StringConvert::convert()` into the latter,
  so linking failed with undefined references to it. This resurfaces
  whenever the bundled SDK is updated.
- The `vst3test` executable was commented out while the
  `target_link_libraries` / `target_include_directories` calls naming it
  were not, so configuring with `ENABLE_VST3` on failed on an unknown
  target. `${VST3_FILES}`, referenced in the commented block, is never set
  anywhere and was dropped.

## 4. VST3 host: compilation

`ENABLE_VST3` is `0` upstream and the comment above it says "Deprecated,
and non-functional". It is more than deprecated — it does not compile from
a clean checkout. Twelve errors in five groups:

| Problem | Where |
|---|---|
| `GetNumberOfInputAudioBuffers()` declared 3x, `GetNumberOfOutputAudioPorts()` 2x; one duplicate returned the output count from an input-named function | `Vst3EffectImpl.hpp` |
| `buffers.inputs.size()` — `IAudioClient::Buffers` is a plain struct with `numInputs`/`numOutputs` counts | `Vst3EffectImpl.hpp` |
| `NEW` undeclared — macro lives in `base/source/fdebug.h`, included by neither TU | `Vst3Host.cpp`, `Vst3Effect.cpp` |
| `RtInversionGuard` undeclared — the class is compiled out behind `#ifdef JUNK` in `RtInversionGuard.hpp` | `Vst3Effect.cpp` |
| `Vst3EffectImpl` abstract — 15 pure virtuals added to `IEffect` were never implemented | `Vst3EffectImpl.hpp` |
| lambda captured a non-existent `collation` | `PluginHost.cpp` |

`RtInversionGuard` was removed at the two call sites rather than
resurrected: nothing else instantiates it, and reviving it would change
scheduling behaviour across the daemon.

## 5. VST3 host: runtime defects

### Double activation

`Load()` called `component->setActive(true)` twice with no intervening
deactivate. SDK sample plugins tolerate it; DPF does not, and tripped

```
DISTRHO_SAFE_ASSERT_RETURN(! fIsActive)   // DistrhoPluginInternal.hpp
```

once per instance — 17 of 17 DPF plugins tested. Removing the second call
took the assertion count from 17 to 0.

### Read-only parameters exposed as inputs

`Vst3Host` computed `isReadOnly` and used it only for
`port.not_on_gui(isReadOnly | notOnGui)`. `Lv2PluginUiPort::is_input_`
defaults to `true`, so every read-only parameter — every meter — was
exposed as an input control, and the snapshot machinery in
`AudioHost.cpp` tried to save and restore values into parameters the
plugin will not accept. `mda SpecMeter` is 35 such parameters.

Now `port.is_input(!isReadOnly)`.

### Silent processing failure

`process()` returned `false` when `IAudioProcessor::process()` failed and
told nobody: audio stopped and no error surfaced. It now reports through
the same realtime error channel LV2 effects use.

`HasErrorMessage()` / `TakeErrorMessage()` are called from
`Lv2Pedalboard::Run` — the realtime audio callback — on every block, so:

- the message lives in a fixed `char[1024]`, like `Lv2Effect`, never a
  `std::string`;
- `SetErrorMessage()` takes `const char*` so callers cannot be tempted to
  concatenate on the audio thread;
- the text is built once in `Load()`;
- a latch reports once per failure episode instead of once per block.

### State persistence

`GetLv2State()` returned `false` and `SetLv2State()` did nothing, so VST3
presets silently reloaded with default values.

VST3 has no per-property state model like LV2 patch properties; it
serialises the whole plugin to one opaque `IBStream` blob. The blob is now
carried in a single `Lv2PluginState` entry under
`http://two-play.com/ns/pipedal#vst3State`, with `atomType_` set to
`LV2_ATOM__Chunk`, which reuses the existing preset, snapshot and bank
plumbing unchanged.

`SetLv2State()` ignores an entry it does not recognise (state saved by a
different effect type) and catches a restore failure rather than
propagating it. That last point is not defensive programming for its own
sake: `ZamAutoSat` serialises a 1-byte state that its own deserialiser
then rejects, and before the catch was added it aborted the process with
an uncaught `Vst3Exception`. A plugin that cannot read back its own state
must not take down preset loading.

`SetState()` also indexed `&state[0]` without checking for an empty
vector.

## 6. Results

`vst3test`, 52 plugins:

| | Before | After |
|---|---|---|
| Build | fails, 12 errors | clean |
| Crashes | n/a | 0 |
| DPF activate assertions | 17 | 0 |
| State round-trip OK | 0 (stubs) | 41 |

The 10 plugins that still report a state mismatch fail only on parameters
VST3 deliberately keeps out of component state: `kIsProgramChange`
("Program") and `IMidiMapping` targets ("Mod Wheel", "Pitch Bend",
"Sustain", "Aftertouch", the mda instrument filter-mod controls). Those are
transient performance state, not settings.

## 7. Web server: a GET for any missing path aborted the daemon

Found while probing modgui URLs. `WebServerImpl::on_http()`'s static-file
fallback called `std::filesystem::file_size()` **before** the
`file.open()` / `NotFound()` check that exists to handle a missing file.
`file_size()` throws `filesystem_error`, and that block sits outside the
try/catch guarding the request handlers above it, so the exception escaped
into websocketpp's handler chain. websocketpp does not catch, so
`std::terminate()` aborted the process.

```
Thread "ppdl_web_4" received signal SIGABRT
abort()  ->  std::terminate()  ->  __cxa_throw()
  ->  pipedal::WebServerImpl::on_http(std::weak_ptr<void>)
  ->  websocketpp::connection<CustomPpConfig>::process_handshake_request()
```

One unauthenticated GET, reachable from the LAN and from the wi-fi hotspot,
killed the service. `/favicon.ico` from a browser or any crawler was enough.
Verified fatal for `/naoexiste-xyz123`, `/var/`, `/img/`, `/resources` and
`/resources/`.

Note it is **SIGABRT, not SIGSEGV**, so it does not appear when grepping the
journal for `status=11/SEGV` — which is how it went unnoticed here for a
while.

After the fix those paths return 404 and the process id is unchanged across
all of them.

## 8. modgui: null dereference without an `ns` parameter

`ModWebInterceptImpl::get_response()` assigned `pluginInfo` only inside
`if (!ns.empty())`, then called `pluginInfo->modGui()` unconditionally.

This is not a corner case. pipedal passes plugin identity as a query suffix
injected into templates as `{{_ns}}`. A modgui written against MOD's
convention references its images, stylesheet and script by plain relative
path, and the browser resolves those **without** the query string — so
opening such a plugin crashed the daemon. The `guitaramp-suite` ("Hex
Chain") bundle is one: its templates use `{{symbol}}`, `{{name}}` and
`{{uri}}`, never `{{_ns}}`.

Missing `ns` is now a reported error and the plugin falls back to pipedal's
generic control UI. Rendering third-party MOD-convention artwork would need
pipedal to rewrite relative resource URLs, which this branch does not
attempt.

Also fixed an `&&` that should be `||`: an existing directory passed the
"not exists and not regular file" test and then threw from `file_size()`.

## Known gaps

- **Patch properties remain no-ops.** `SetPatchProperty`,
  `RequestPatchProperty` and `RequestAllPathPatchProperties` have no VST3
  equivalent and are explicit no-ops.
- **No port flag for program-change or MIDI-mapped parameters.**
  `Vst3Host` knows `isProgramChange` from `ParameterInfo::flags` but
  `Lv2PluginUiPort` has nowhere to record it, so snapshots treat those
  parameters as ordinary controls. Adding a flag touches the JSON
  serialisation and the React UI, so it was left out of this branch. This
  is what the remaining `vst3test` mismatches are about.
- **`canAutomate` is dead.** Computed in `Vst3Host.cpp` and never used.
- **MIDI input, sidechain and the editor are untested.** `vst3test`
  exercises audio processing, controls, program lists and state only.
- **`pipedald` does not stop within `TimeoutStopSec=15`** and is SIGKILLed
  on every `systemctl restart`. Unrelated to VST3, but it has a
  consequence worth knowing: `CrashGuard::LeaveCrashGuardZone()` only
  removes `crash_guard.data` on an orderly exit, so the crash counter is
  never cleared by a normal restart. Once it passes 4,
  `PiPedalModel.cpp` deliberately loads a blank pedalboard instead of the
  saved one. Not diagnosed here.

## The ENABLE_VST3 flag

The last commit on this branch flips `set (ENABLE_VST3 0)` to `1`. Every
other commit leaves the upstream default alone, so that commit can be
dropped when submitting upstream while still letting a reviewer turn the
path on to evaluate it.

## Appendix: what the VST3 path bought in practice

The VST3 work is not theoretical. Two things on the validation machine only
work because of it.

**Plugins that ship no LV2.** `ArborealAudio/STR-X` declares `FORMATS AU VST3`
and `FORMATS VST`. Without a VST3 host in pipedal it is simply unusable there.

**CLAP, with no pipedal code at all.** `free-audio/clap-wrapper` is a CLAP host
packaged as a VST3, so `CLAP -> clap-wrapper -> pipedal's VST3 host` works
today. Verified end to end: the VST3 cache went 17 -> 18 with
`/usr/local/lib/vst3/ZamComp.vst3`, and the SDK validator passes it 47/47.

The wrapper resolves its target at scan time by its **own filename**, looking
for a same-named `.clap` in the standard search paths (`/usr/lib/clap`,
`~/.clap`). One build therefore serves every plugin — copy the bundle and
rename it. Confirmed by copying `ZamComp.vst3` to `ZamTube.vst3`, which then
loaded `ZamTube.clap` (validator reports `name = ZamTube`).

Two build notes, both from a toolchain newer than the wrapper expects:

- `-DCLAP_SDK_ROOT=...` is required; the recursive clone does not fetch it.
- Against VST3 SDK 3.8.1 it fails with `reference to 'iid' is ambiguous` —
  the wrapper has not been updated for 3.8.x. `-DCLAP_WRAPPER_DOWNLOAD_
  DEPENDENCIES=TRUE` pulls the 3.8.0 it was written against and builds clean.

## Appendix: still open

- **Intermittent SEGV on shutdown.** After "Stopping web server", roughly one
  in three `systemctl stop` calls dumps core (4 of 12 measured in one boot).
  Nothing is lost at runtime — the process is exiting — but
  `CrashGuard::LeaveCrashGuardZone()` only removes `crash_guard.data` on a
  clean exit, so the counter accumulates across restarts. Past 4,
  `PiPedalModel.cpp:416` deliberately discards the working pedalboard and
  loads an empty one. An attempt to catch it under gdb caught a clean
  shutdown instead; not characterised.
- **Third-party modgui artwork.** The two template bugs are fixed and
  MOD-convention bundles now render, but pipedal does not rewrite relative
  resource URLs, so a template that omits `{{_ns}}` on its own asset links
  still falls back to generic controls for those assets.
- **`jsonTest` leak.** 27 `json_object`s outstanding. The count is positive,
  so these are live objects rather than unbalanced copies. Not diagnosed.

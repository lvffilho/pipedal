# Handoff — branch `vst3-and-hotspot-fixes`

State as of 2026-08-18. Written so another agent can pick this up without
re-deriving anything. Companion document: `docs/VST3-AND-HOTSPOT-FIXES.md`
(reproduction steps and reasoning for each fix).

---

## 1. Where things are

| | |
|---|---|
| Fork | `git@github.com-lvffilho:lvffilho/pipedal.git` |
| Branch | `vst3-and-hotspot-fixes`, **pushed**, 16 commits off upstream `32c45bf` |
| Other local branch | `ui-modernization` (commit `d40603a`) — **no file overlap**, independent work |
| Local clone | `/projects/audio/pipedal` |
| Test server | `user@192.168.100.172` (hostname `lvtechnology-music`) — password auth only, **ask the user for the password**; no key installed, no `sshpass` locally |
| Server checkout | `/data/pipedal` — same 16 commits applied, plus deployed binary |
| VST3 SDK | `/data/vst3sdk` (3.8.1), referenced as `../vst3sdk` |
| Plugin build area | `/data/plugins/build-src` |

The server checkout and the local repo were kept byte-identical (verified by
md5) for every file touched. If you change one, sync the other.

---

## 2. Commit series

The first 14 are upstream candidates. The last 2 configure this machine and
**must be dropped when submitting a PR**.

```
8a94514 Extend the default lv2_path for this machine    <- local only
af4be17 Enable ENABLE_VST3                              <- local only
ad58b0f Document the CLAP path and the remaining open issues
069260e Log modgui resource errors instead of discarding them
c5d0777 Fix two modgui template failures that killed the whole plugin UI
16fcb41 Actually delete the json_array/json_object copy constructors
df125e1 Don't let a websocket handler abort the daemon
9a0c8bd Document the two web server crashes
c64f41c Fix null dereference serving modgui resources without an ns parameter
610b754 Stop a GET for any missing path from aborting the daemon
a117675 Document the VST3 revival and the hotspot crash fix
8fe4a27 Add a state round-trip check to vst3test
92dbba6 Make the VST3 host compile and run again
1da12d8 Make the VST3 targets buildable again
b6f9657 Don't tear down NetworkManager when hotspot activation fails
1b9dbe6 Fix lock_guard that locks nothing in AlsaSequencer
```

### The two that matter most to any pipedal install

**`610b754` — remote DoS, one unauthenticated GET.** `WebServerImpl::on_http()`
called `std::filesystem::file_size()` before the `file.open()`/`NotFound()`
check meant to handle a missing file. `file_size()` throws, that block sits
outside the try/catch guarding the request handlers, so the exception escaped
into websocketpp — which does not catch — and `std::terminate()` aborted the
process. A browser asking for `/favicon.ico` was enough. Reachable from the LAN
and from the wi-fi hotspot. **Note it is SIGABRT, not SIGSEGV**, so
`grep status=11/SEGV` on the journal does not show it — that is why it went
unnoticed for a while.

**`b6f9657` — 55 core dumps in 11 minutes.** `StartHotspot()` is reached only
from D-Bus signal handlers; its catch called `onError()` → `ReleaseNetworkManager()`,
destroying the NetworkManager/Device proxies while their own dispatch was still
on the stack. It also latched `State::Error`, which `MaybeStartHotspot()` treats
as permanent, so one transient failure disabled the hotspot until restart.

### The rest, briefly

- `1b9dbe6` — `std::lock_guard { mutex };` is an unnamed temporary, destroyed at
  the end of the full expression. Two accessors to `midiChannel_` ran unsynchronised.
- `1da12d8` — `vst3_lib` did not compile `common/commonstringconvert.cpp`
  (current SDKs split the helpers there); `vst3test` was commented out while the
  calls referring to it were not.
- `92dbba6` — VST3 host: 3 duplicate method declarations, `buffers.inputs.size()`
  on a plain struct, missing `NEW` macro include, `RtInversionGuard` compiled out
  behind `#ifdef JUNK`, 15 unimplemented `IEffect` pure virtuals, plus 4 runtime
  defects (double `setActive`, read-only params exposed as inputs, silent
  `process()` failure, no-op state persistence).
- `8fe4a27` — state round-trip check in `vst3test`.
- `c64f41c` / `c5d0777` / `069260e` — modgui: null deref without `ns`; same-name
  nested sections mismatched by `find(endTag, ix)`; section over an absent
  variable threw instead of rendering nothing; errors were swallowed by
  `on_http()` replacing the body with a generic `ec.message()`.
- `df125e1` / `16fcb41` — hardening. Not exploitable today; fuzzing found nothing.

---

## 3. Verification already done — do not redo

| Check | Result |
|---|---|
| `vst3test`, 52 plugins | 0 crashes; DPF activate assertions 17 → 0; state round-trip 0 → 41 |
| HTTP fuzz, 26 probes | 0 crashes (path traversal, 4 KB path, `%00`, bad methods, all `/var/*`, modgui variants) |
| WebSocket fuzz, 10 payloads | 0 crashes (empty, truncated, non-JSON, 200 KB, 400-deep, binary) — **connect to `/pipedal`**, not `/`; `wants()` requires `segment(0) == "pipedal"` |
| modgui across 41 plugins | neither template error occurs; remaining 500s are correct "Plugin not found" / "no ModGui" |
| SDK validator on CLAP wrapper | 47/47 |

The 10 remaining `vst3test` state mismatches are **not bugs**: they are
`kIsProgramChange` and `IMidiMapping` targets ("Program", "Mod Wheel", "Pitch
Bend", "Sustain", "Aftertouch"), which VST3 deliberately keeps out of component
state.

`pipedaltest "[Build]"` fails 3 of 7 — **pre-existing**, verified identical with
`ENABLE_VST3=0`. `PiPedalAlsaTest` is hardware-dependent ("ALSA port not found"),
the other two are leak counters that were already red.

---

## 4. Server state

**Deployed:** `/usr/sbin/pipedald` carries all fixes. Audio on a Tascam
US-144MKII, `S24_3LE`, 44100, **256x3**, 0 xruns. Hotspot up
(`PiPedal Hotspot`, SSID `lvtechnology-pedal`, 192.168.60.1). mDNS announcing
`_pipedal._tcp`. 776 LV2 plugins.

**Config that must survive an install.** `CMakeLists.txt:60` does
`install(DIRECTORY config/ DESTINATION /etc/pipedal/config)`, so **any edit made
directly in `/etc/pipedal/config/config.json` is reverted by the next
`./install.sh`**. That is why the machine's `lv2_path` lives in the repo's
`config/config.json` (commit `8a94514`). Order matters — lilv prefers the higher
`lv2:minorVersion`, falling back to first-found for bundles without one; the
mod-desktop set collides with 19 system bundles and is older in every case, so it
goes last.

`/usr/local/lib/lv2-mod-desktop` is a **symlink** to the versioned mod-desktop
directory. Repoint the symlink when that package updates; a dangling symlink
loses ~205 plugins silently. See `/usr/local/lib/lv2-mod-desktop.README`.

**Deploy sequence:**

```bash
sudo chown -R user:user /data/pipedal/build   # install.sh leaves root-owned
                                              # artifacts that break
                                              # extract_toobamp_lv2_files
cd /data/pipedal && cmake --build build -j$(nproc)
sudo ./install.sh
sudo systemctl stop pipedald
sudo rm -f /var/pipedal/crash_guard.data      # see CrashGuard note below
sudo systemctl start pipedald
```

**Environment facts worth not rediscovering:**

- The service runs as **`pipedal_d`**, so `~/.lv2` is invisible to it. Install
  LV2 to `/usr/lib/lv2`.
- `avahi-daemon` **must be installed** — only the client libs ship as build
  dependencies. Without the daemon the Android app connects to the hotspot but
  never lists the device, and the log says `DNS/SD announcement timed out.`
- The wi-fi must be **managed by NetworkManager**. netplan emits a udev rule
  setting `NM_UNMANAGED=1` for interfaces it configures; removing the `wifis:`
  block flips it to `0` and NM takes over (its own conf.d already has
  `except:type:wifi`). This was done; backup at
  `/etc/netplan/00-installer-config.yaml.bak-wifi`.
- `net.ipv4.ip_forward=1` is persisted in `/etc/sysctl.d/99-pipedal-hotspot.conf`
  so hotspot clients reach the internet.
- `mda-lv2` is **apt-pinned out** (`/etc/apt/preferences.d/99-no-mda-lv2`): the
  packaged 1.2.10 is older than the locally built 1.2.12 and was winning by path
  order.

---

## 5. Hardware and performance

**i7-7500U is 2 physical cores / 4 threads** — `neofetch` reporting "(4)" is
threads. It is a 15 W part: under sustained load the clock settles at **2700 MHz**
(base), not the 3500 turbo. Thermal throttle counters are 0, so this is turbo
budget, not heat.

**The bottleneck is the per-period deadline on one thread**, not total CPU.
pipedal runs the LV2 chain serially on `ppdl_alsaDriver` (`SCHED_RR 90`). At 256
frames / 44100 Hz that thread has **5.8 ms** for the whole chain. The second core
only helps plugins that spawn their own threads.

The **"CPU %" in the UI status bar is the fraction of that deadline** — that is
the number to watch, not `top`. Measured 17.9% with Tuner + ParamEQ + NAM +
CabIR + Flanger + ConvReverb + Hex Amp, 0 xruns. Keep it under ~60%.

**Known trap:** `toob-convolution-reverb-stereo` spawns 7 threads and ToobAmp
gives `crvb_crvb3` priority `SCHED_RR 4` — below the LV2 workers. At 512 frames
it has 11.6 ms to deliver; at 128 it has 2.9 ms. When it misses, ALSA still gets
its buffer on time (**so no xrun is logged**) but the reverb output is wrong —
audible as fluctuating volume. 512x3 is stable, 128x3 is not; 256x3 is in use.
Per the official latency doc, reduce the **buffer count** rather than the period
to cut latency without shrinking that deadline: 256x2 gives 11.6 ms total while
keeping the same 5.8 ms deadline.

---

## 6. Build pitfalls on this machine

CMake 4.2, GCC 15, meson 1.10 are newer than most of these projects expect.
Every one of these was hit and solved:

| Symptom | Fix |
|---|---|
| `cmake_minimum_required` rejected | `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` |
| `'exchange' is not a member of 'std'` (JUCE ≤ 2022) | `-DCMAKE_CXX_FLAGS="-include utility"`. **Do not patch the JUCE header** — it is included inside `namespace juce`. |
| `xcb-icccm not found` | `-Dgui=disabled` — headless does not use plugin GUIs |
| `Program 'faust' not found` | `apt install faust` |
| `zita-resampler not found` | `apt install libzita-resampler-dev libzita-convolver-dev` |
| `dep/dpf/Makefile.base.mk: No such file` | `.gitmodules` uses **SSH** URLs; `git -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init --recursive` |
| `hvcc ... Error 127` | `pipx install hvcc`, and pass `PATH` through `sudo` |
| clap-wrapper: `reference to 'iid' is ambiguous` | it is not updated for VST3 SDK 3.8.x; `-DCLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=TRUE` fetches the 3.8.0 it expects |
| clap-wrapper: `Unable to detect clap` | `-DCLAP_SDK_ROOT=/data/plugins/build-src/clap` |

---

## 7. Plugins added this session (759 → 776, plus 6 more built after)

`lsp-plugins-lv2` from apt (194 plugins). Built from source in
`/data/plugins/build-src`: KPP (7 guitar plugins, meson+faust), BYOD,
CHOWTapeModel, ChowPhaser Mono/Stereo, Noise Repellent (+ libspecbleach),
stomptuner, wolf-shaper, cchorus, dfjpverb, stereocrossdelay, and 5 Wasted-Audio
(`WSTD_*`). Then STR-X, SmartAmp, Proteus, Chameleon, TS-M1N3, NeuralPi — all six
producing **both LV2 and VST3**.

**Check before adding more**: the machine already has all three neural amp
modelers (`toob-nam`, `toob-ml`, AIDA-X `rt-neural-generic`), KlonCentaur,
`gxts9`, `gx_amp#GUITARIX`, four IR loaders and five tuners. Most obvious
recommendations are already installed.

**Do not install** the apt `guitarix` package — it pulls 55 packages (GTK, fonts)
for LV2 that is already present. **AmpForge** is the wrong shape: it hosts a
pedalboard chain internally (redundant with pipedal) and its drag-and-drop UI
does not render headless.

Plugins with JUCE/Qt/GTK UIs show as **generic controls** in pipedal. Only
`modgui` renders artwork — and that path only started working with `c5d0777`.

---

## 8. CLAP works today, with no pipedal code

`CLAP → clap-wrapper (VST3 shim) → pipedal's VST3 host`. Verified: VST3 cache
17 → 18, SDK validator 47/47.

The wrapper resolves its target by **its own filename**, looking for a same-named
`.clap` in `/usr/lib/clap` or `~/.clap`. So **one build serves every plugin** —
copy the bundle and rename it. Recipe left at
`/data/plugins/build-src/wrap-clap-as-vst3.sh <PluginName>`, then
`rm /var/pipedal/vst3cache.json && systemctl restart pipedald`.

The machine's 17 `.clap` files all have native VST3 equivalents, so wrapping them
adds nothing — the test wrappers were removed. The value is the proven capability
for a CLAP-only plugin.

---

## 9. Open work, highest value first

1. **Open the upstream PR** at `rerdavies/pipedal` with the first 14 commits.
   Drop `af4be17` and `8a94514`. Lead with `610b754` — it is a remotely
   triggerable DoS on a daemon that exposes a wi-fi hotspot.

2. **Intermittent SEGV on shutdown.** After "Stopping web server", roughly 1 in 3
   `systemctl stop` dumps core (4 of 12 measured in one boot). Harmless at
   runtime, but `CrashGuard::LeaveCrashGuardZone()` only removes
   `crash_guard.data` on a clean exit, so the counter accumulates. Past 4,
   `PiPedalModel.cpp:416` **deliberately discards the working pedalboard** and
   loads an empty one — this is how the user lost a pedalboard during this
   session. An attempt to catch it under gdb caught a clean shutdown instead.
   Workaround until fixed: delete `crash_guard.data` before the last restart.

3. **Restart `pipedald`** so the last six plugins appear in the UI, then try
   **256x2** to cut ~6 ms of latency without shrinking the convolution deadline.

Lower value: `jsonTest` leak (27 live `json_object`s, positive count so they are
retained rather than mis-copied); 5 anonymous-node LV2 metadata warnings at
startup (lilv skips those records); pipedal does not rewrite relative modgui
resource URLs, so MOD-convention templates that omit `{{_ns}}` still fall back to
generic controls for those assets.

---

## 10. Corrections made during this session

Recorded because they cost time and could be repeated:

- **Counting crashes with `grep status=11/SEGV` undercounts.** The web server
  crash is SIGABRT. Compare PIDs before/after, or grep `core-dump` and
  `Failed with result` too.
- **The first WebSocket fuzz tested nothing.** Connecting to `/` was rejected at
  session open (`Invalid segement number.` ×10, matching the 10 messages sent).
  The socket factory requires `/pipedal`.
- **Lowering the period was the wrong advice** for the volume fluctuation the
  user heard; the buffer *count* is the lever. See §5.
- **lilv picks by version, not path order.** The path order still matters only
  for bundles that declare no version.
- **`~/.lv2` was not a duplicate to ignore** — it was invisible to the service
  the whole time, which is a different problem from having two copies.

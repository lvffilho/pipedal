# Upstream requests for ToobAmp (rerdavies/ToobAmp)

Audience: the ToobAmp maintainer. Four independent requests, each suitable as its own issue. Motivation is framed for a 2-core laptop running PiPedal at small buffer sizes, where every audio-thread allocation and every percent of CPU counts.

Verification legend: **[verified]** = checked against the named source (NeuralAmpModelerCore release notes and PRs, local clones of tone-3000/tone3000-plugin and jaffco/nam-pedal, installed ToobAmp 1.3.85 `.ttl` files). **[unverified]** = not checkable from those sources. ToobAmp's own C++ source was not available, so its internals (NAM core version, gate DSP details) are unverified unless visible in the `.ttl` files.

## 1. Update the bundled NeuralAmpModelerCore to v0.6.0

**Motivation.** Several changes in v0.5.x to v0.6.0 remove audio-thread allocations or cut CPU/start-up cost, which matters most on a small CPU with a small buffer. Which NAM core revision ToobAmp 1.3.85 bundles is **[unverified]**; the points below apply only if it predates these releases.

**Proposed change.** Bump the NAM core submodule to v0.6.0 (released 2026-10-01 **[verified]**) and re-run the existing model tests. Items most relevant to PiPedal users, all **[verified]** from the release notes and PR descriptions:

- **#312, "Remove a per-call allocation and two redundant copies from the hot path."** `ActivationPReLU::apply` copied its slope vector on every call (a heap allocation once per sample when a WaveNet layer uses `gated`/`blended` gating with PReLU). `LayerArray::Process` also zeroed the full max-buffer-size head accumulator every block, even when the block was smaller. The PR states no behaviour change.
- **#319, "Cache A2 prewarm state."** Caches one steady-state input sample per A2 convolution and rebuilds ring histories from it on later prewarms, for both the A2 fast path and the generic WaveNet fallback. Conditioned WaveNets still run full silence prewarm. Reduces model load/Reset cost.
- **#299, "Fix LSTM real-time safety" (fixes issue #218).** `LSTMCell::get_hidden_state()` returned an `Eigen::VectorXf` by value, so LSTM models heap-allocated on every sample. The PR reports 128 allocs/frees before the fix and 0/0 after, and adds an allocation-tracking test. This is a hard real-time violation fix for LSTM models.
- **#324, "Optimize long linear convolution scheduling."** Only relevant to very long linear (IR-style) models. Lower priority. The PR's numbers are from Apple M1, not x86.
- Related A2 correctness fixes: #301, #300, #321/#322, #316. Background: A2 fast path arrived in v0.5.1 (#251); prewarm can be skipped on `Reset()` since v0.5.4 (#285).

**Sources.** https://github.com/sdatkinson/NeuralAmpModelerCore/releases (v0.5.0 to v0.6.0); PRs #312, #319, #299, #324 in the same repository.

**Effort.** Small to medium: submodule bump plus fixing any API drift. The release notes mark #335 (1-to-n / n-to-1 channels in `Linear` models) as BREAKING, and v0.5.0 marks #247 and #250 as BREAKING, so a rebuild and model regression run are needed. Whether ToobAmp touches those APIs is **[unverified]**.

## 2. Improved noise-gate detector

**Motivation.** Pickup hum and hiss near the threshold can hold a gate open or make it chatter; this is common with cheap laptop audio interfaces. The TONE3000 plugin documents a zero-latency, allocation-free gate design that is safe for the audio thread.

**Current ToobAmp state.** The shipped `ToobNoiseGate.ttl` (microVersion 85) already exposes Threshold, Hysteresis, Attack, Hold, Release and Range **[verified]**. Whether its detector is band-limited and whether it closes as an expander is **[unverified]**, so this request is "adopt whichever of these pieces ToobAmp lacks."

**Proposed change.** From `NoiseGate.h` in tone3000-plugin (header comments; MIT licence **[verified]**):

- Sidechain detector band-passed: 12 dB/oct high-pass at 80 Hz plus 6 dB/oct low-pass at 5 kHz. Only the detector is filtered, never the audible path.
- Hysteresis: open at the threshold, start closing only after the envelope has stayed below a threshold 5 dB lower for the whole hold time (`kHysteresisDb = 5.0`).
- Peak envelope follower with fast rise (0.2 ms) and slower fall (25 ms), plus hold.
- Downward-expander close: the target gain tracks the envelope along a 4:1 expander curve (cubic in gain) down to the range floor, rather than slamming to silence. Gain smoothing uses about 0.2 ms attack and a one-pole release.
- Upstream defaults: release 50 ms, hold 20 ms, range 80 dB. Per-sample cost is a handful of multiplies; no lookahead, so zero added latency.

**Sources.** `plugin/include/NoiseGate.h`, `test/src/gate_tests.cpp`, and the "Noise gate" paragraph of `README.md` in https://github.com/tone-3000/tone3000-plugin (clone at commit 4f2321d).

**Effort.** Small (about 150 lines of DSP). Implementing from the described design avoids any licence question; if code is copied, retain the MIT notice. Keep parameter symbols unchanged (or bump the minor version) for preset compatibility.

## 3. Optional binary model cache (.namb) with JSON fallback

**Motivation.** `.nam` files are JSON; parsing large weight arrays is slow and allocation-heavy, and it happens while the user waits for a pedalboard to load. On a 2-core machine that competes with the running audio thread. A cache keyed on the source file would make later loads close to a `memcpy` of the weights.

**Proposed change.** On first load, convert the `.nam` to `.namb` in a cache directory (keyed on path plus mtime/size) and load the cache next time. If conversion or `.namb` parsing fails, or the model needs something the binary format does not support, fall back to the JSON path. Architectures the `.namb` loader lists as supported: Linear, ConvNet, LSTM, WaveNet (including recursive condition DSP) **[verified]**. A2/slimmable (SlimmableContainer) support in `.namb` was not found in the loader README **[unverified; assume the JSON fallback is required for A2 and slimmable models]**.

Facts from the `nam-binary-loader` README **[verified]**: format version 1, little-endian, magic "NAMB", CRC32 checksum, weights stored as raw float32; claimed ~80% file-size reduction for a typical WaveNet and load time "essentially memcpy for weights". These are the author's claims; no measurement was reproduced here. The loader does not depend on nlohmann/json, and `nam2namb` converts `.nam` to `.namb`. A cache should also be invalidated on format-version and NAM-core-version change (recommendation, not from a source).

**Sources.** https://github.com/tone-3000/nam-binary-loader (MIT, copyright Joao Felipe Santos), as used by https://github.com/jaffco/nam-pedal (`NAMPedal.cpp` loads `.namb` from an SD card).

**Effort.** Medium: converter integration, cache invalidation, fallback, and a test comparing JSON-loaded and `.namb`-loaded output. Measure JSON load time on a few real models first; if loads are already fast, defer.

## 4. Model latency and polarity probe at load time

**Motivation.** Some user-trained NAM models carry a baked-in pure delay (for example 17 samples, 0.35 ms at 48 kHz **[unverified; no source in this repo]**) and some are polarity-inverted. This is inaudible alone but audible when the output is summed with a correlated signal (dry mix, parallel chains, IR). PiPedal cannot do this itself because it does not link the NAM runtime, so it belongs in ToobAmp.

**Proposed change.** After a model loads (and after prewarm/Reset, because the processor is stateful), run a short known probe off the audio thread (reference defaults: 100 ms linear sweep, 48 kHz, -20 dBFS, 10 ms lag window), cross-correlate the output against the dry probe, and record latency in samples, an inverted flag and a confidence/ambiguity indication. Use it to optionally compensate latency and polarity. The reference `measureTimeDomain()` needs no FFT; `measureGccPhat()` is the more robust alternative.

Tone3000's catalog run (11,463 A2 models) reports 99.8% time-domain accuracy on an injected 37-sample delay, with known blind spots: models with two near-equal opposite-sign lobes, and about 1.8% of very high-gain models whose output barely correlates with any probe; these are flagged by `lobeRatio`/`ambiguous` and `peakRatio` **[verified from `plugin/docs/model-latency.md` and `results-catalog.txt`; the numbers are the authors' own]**.

**Sources.** `plugin/docs/model-latency.md` and `plugin/docs/model-latency/nam_latency.h` (dependency-free, MIT) in https://github.com/tone-3000/tone3000-plugin; results in `plugin/docs/model-latency/results-catalog.txt`.

**Effort.** Small to medium: the header is self-contained; the work is running it off the audio thread and any latency plumbing. How PiPedal handles LV2-reported latency was not checked here **[unverified]**.

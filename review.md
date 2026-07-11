# Spectrum.cpp Review

Priority scale: P0 = critical crash/memory safety, P1 = high realtime or stability risk, P2 = medium robustness issue, P3 = low correctness/UX issue.

## Findings

### P0 — FFT output is indexed out of bounds - DONE
- Location: `src/Spectrum.cpp:253`, `src/Spectrum.cpp:260`, `src/Spectrum.cpp:263`
- Category: Safety, GUI stability, correctness
- Rack-SDK's `dsp::RealFFT::rfft()` documents canonical real FFT output as `output[0] = F(0)`, `output[1] = F(n/2)`, then interleaved real/imaginary values through `output[length - 1]`. `performFFT()` instead treats `FFT_SIZE / 2 + 1` bins as fully interleaved complex pairs. The last loop iteration reads `output[2048]` and `output[2049]` for a 2048-float vector, and DC/Nyquist are decoded incorrectly even before the overread.
- Impact: undefined behavior, possible plugin crash, corrupted low/high-bin magnitudes, and downstream GUI instability.
- Recommendation: keep the Rack-SDK canonical layout: `magnitudes[0] = abs(output[0])`, `magnitudes[FFT_SIZE / 2] = abs(output[1])`, and for `1 <= k < FFT_SIZE / 2` use `hypot(output[2 * k], output[2 * k + 1])`.

### P1 — FFT analysis allocates on the audio thread - DONE
- Location: `src/Spectrum.cpp:243`, `src/Spectrum.cpp:253`, `src/Spectrum.cpp:245`
- Category: Performance, realtime safety
- Every FFT frame allocates/copies multiple vectors: `input(capture)`, `output(...)`, `magnitudes(...)`, plus the `std::pair`/vector return copy into `magnitudes`. This runs from `process()`, so allocator locks or heap stalls can produce audio dropouts.
- Impact: intermittent glitches under load, especially when multiple Spectrum modules are present.
- Recommendation: make input/output/magnitude buffers module members, sized during construction, and update them in place without returning vectors from the audio path.

### P1 — Saved enum values can crash theme lookup
- Location: `src/Spectrum.cpp:76`, `src/Spectrum.cpp:78`, `src/Spectrum.cpp:80`, `src/Spectrum.cpp:331`, `src/Spectrum.cpp:346`
- Category: Safety, GUI stability
- `dataFromJson()` casts raw patch integers directly to `DisplayMode` and `Theme`. `getActiveColor()`, `getInactiveColor()`, and `getPeakColor()` then index `THEMES[static_cast<int>(theme)]` without validation.
- Impact: a malformed or future-version patch can put `currentTheme` outside `0..2`, causing out-of-bounds reads during draw and likely crashing Rack's UI thread.
- Recommendation: validate JSON types and clamp or reject enum values on load. Also guard the color helpers with a safe default theme.

### P2 — Audio and UI threads race on display state - DONE
- Location: `src/Spectrum.cpp:300`, `src/Spectrum.cpp:303`, `src/Spectrum.cpp:415`, `src/Spectrum.cpp:416`
- Category: GUI stability, safety
- The engine thread writes `bands[b].dbLevel` and `bands[b].peakLevel` while the UI thread reads them during `drawLayer()`. Plain `float` sharing across threads is a C++ data race even if it usually works on common desktop CPUs.
- Impact: occasional inconsistent frames today; undefined behavior under sanitizers or less forgiving targets.
- Recommendation: publish a display snapshot with double buffering and an atomic index, or use atomics for the values the UI reads.

### P2 — DSP uses global engine sample rate instead of `ProcessArgs` - DONE
- Location: `src/Spectrum.cpp:285`, `src/Spectrum.cpp:292`, `src/Spectrum.cpp:298`
- Category: Safety, performance, testability
- Analysis code reads `APP->engine->getSampleRate()` from helper functions instead of using `args.sampleRate` already provided to `process()`.
- Impact: unnecessary global coupling in the DSP path and possible inconsistencies during sample-rate changes or headless/unit-style execution.
- Recommendation: pass `args.sampleRate` into `analyzeFFT()`, `getFrequencyEdges()`, `calculateFallDecay()`, and `updateBandLevels()`.

### P3 — Dot display does not light the top row at full scale - DONE
- Location: `src/Spectrum.cpp:362`, `src/Spectrum.cpp:364`, `src/Spectrum.cpp:531`, `src/Spectrum.cpp:538`
- Category: GUI symmetry, correctness
- `DisplayGrid` adds one extra row, making `grid.gridHeight` larger than `getAvailableDisplayHeight()`. `drawActiveDots()` compares against the smaller available height, so at `level == 1.0f` the top active row can remain inactive while the peak indicator can still draw there.
- Impact: visual ceiling mismatch between active level and peak level.
- Recommendation: either remove the extra row, scale active height against `grid.gridHeight`, or derive active rows from normalized row indices.

### P3 — Stereo summing can hide anti-phase channel energy - DONE
- Location: `src/Spectrum.cpp:197`, `src/Spectrum.cpp:202`, `src/Spectrum.cpp:204`
- Category: Symmetry, correctness
- Left and right inputs are averaged in the voltage domain before analysis. This is symmetric by channel order, but equal opposite-polarity stereo content cancels to silence even though both channels contain energy.
- Impact: the analyzer can under-report wide/phase-inverted stereo signals.
- Recommendation: if the module is intended to show stereo energy rather than mono compatibility, combine channels by RMS/power, max magnitude, or separate L/R spectra.

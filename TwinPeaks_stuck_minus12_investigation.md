# Twin Peaks stuck at -12 V: investigation

Date: 2026-07-11  
Scope: investigation only; no source changes

## Conclusion

The strongest root-cause candidate is undefined behavior from an uninitialized
`RipplesEngine::Frame` member in `TwinPeaks::process()`.

`frameB.xfm_knob` is never assigned in `src/TwinPeaks.cpp`, but
`RipplesEngine::process()` always reads it. Its value therefore comes from stale
stack memory. A bad value (especially an infinity or NaN left by another module)
can corrupt the filter's recursive state and rail its output. Stack contents and
module-processing order are patch-dependent, which explains why adding/removing
other modules could trigger the fault, why reopening the same patch could
recreate it, and why a newly added Twin Peaks in that patch could also be
affected while a fresh patch works.

Confidence: **medium-high**, pending inspection/reproduction with the user's
actual `.vcv` patch on Windows. The patch attachment was not present in the
workspace or temporary attachment locations available during this investigation.

## Evidence

### 1. Filter B has an uninitialized field that is consumed by DSP

`RipplesEngine::Frame` declares `xfm_knob` at `src/filter/ripples.hpp:118`.
Twin Peaks assigns it for `frameA` at `src/TwinPeaks.cpp:100`, but the Filter B
setup at lines 104-111 never assigns `frameB.xfm_knob`.

The engine unconditionally evaluates this field at
`src/filter/ripples.hpp:159-161`:

```cpp
frame.xfm_knob * frame.b_output
```

Although Twin Peaks sets `frameB.b_output` to zero, multiplying an indeterminate
floating-point value is still undefined behavior at the C++ level. If the stale
bits represent infinity or NaN, IEEE floating-point evaluation of `value * 0`
also produces NaN.

Inspection of the currently built `build/src/TwinPeaks.cpp.o` confirms that the
compiler leaves the Filter B `xfm_knob` stack slot unwritten and that
`RipplesEngine::process()` is a separate call which reads the complete frame.
The zero multiplication was not optimized away in this build.

Rack's plugin build uses `-O3 -funsafe-math-optimizations`, so behavior once a
NaN is involved is not portable or reliably predictable across macOS and
Windows builds.

### 2. The exact -12 V value is Twin Peaks' own rail

`src/TwinPeaks.cpp:134` explicitly clamps the mixed result to `[-12, 12]`.
Thus four channels reading exactly -12 V are consistent with the internal DSP
producing a very large negative or invalid result; -12 V is not separately
stored module state.

This clamp was introduced in commit `8343909` (released in the v2.0.4 history),
so a runaway that might previously have appeared as an extreme/non-finite value
now presents as a precise rail.

### 3. Recursive filters can remain poisoned

Both the anti-alias filters (`src/filter/sos.hpp`) and Rack RC filter used by
`RipplesEngine` keep recursive state. There is no finite-value check or recovery
path in `RipplesEngine::process()`. Once non-finite data enters these filters,
later finite input is not guaranteed to recover them.

`RipplesEngine::setSampleRate()` resets the state, but normal processing does
not. Twin Peaks has no custom `dataToJson()`/`dataFromJson()`, so this recursive
state is **not** written to the patch.

### 4. The report's persistence does not imply saved Twin Peaks DSP state

Twin Peaks only relies on Rack's standard serialization of parameters, bypass
state, module identity, and cables. Its internal engines are reconstructed and
sample-rate-initialized when a patch is loaded.

The saved patch can nevertheless recreate the bug indirectly by recreating:

- the same module set and processing order;
- the same cables, polyphony, and parameter values;
- similar stack contents at the uninitialized read;
- or an upstream module that emits a non-finite voltage.

The fact that adding modules immediately preceded the failure is particularly
consistent with stack-layout/process-order sensitivity.

## Alternative/compounding cause

An upstream module could emit infinity or NaN on an audio/CV cable. Twin Peaks
does not validate `input`, frequency CV, FM CV, resonance CV, or intermediate
outputs before feeding recursive filters. This could independently poison an
instance. The included patch is needed to inspect upstream outputs and decide
whether this occurred.

This alternative alone does not explain an unconnected new instance as well as
the uninitialized `frameB.xfm_knob` does. The exact meaning of “just connecting
a single audio out cable” should be confirmed: whether only Twin Peaks' OUT was
connected, or whether an upstream audio output was connected to its IN.

## Findings that are unlikely to be the cause

- Negative frequency/resonance modulation is supported by the current math and
  does not by itself explain persistent -12 V.
- Filter state is not serialized by Twin Peaks.
- Rack calls `onSampleRateChange()` when adding a module, and Twin Peaks resets
  both engine banks in that callback.
- The mode index is bounded by the configured three-position switch under
  ordinary Rack parameter loading.

## Recommended next verification steps (before implementation)

1. Obtain the offending `.vcv` file and note Rack version, Cella version, engine
   sample rate, and engine thread count.
2. Confirm whether a brand-new Twin Peaks has no inputs at all when it rails.
3. Inspect the saved Twin Peaks parameters/cables and test every connected source
   for non-finite or extreme voltages.
4. Reproduce on a Windows x64 debug/instrumented build after deliberately
   seeding `frameB.xfm_knob` with NaN. This should establish the visible rail and
   whether all polyphonic channels poison together.
5. As a diagnostic only, compare behavior after module reset or an engine sample
   rate change, both of which reset filter state.

## Likely fix direction (not implemented)

- Fully value-initialize both `Frame` objects and explicitly set Filter B's
  cross-modulation field to zero.
- Add a finite-value containment/recovery policy around recursive DSP inputs,
  state, and output so one invalid sample cannot permanently poison an instance.
- Add a regression test covering silence, negative CV, polyphony, deliberately
  non-finite input, state recovery, and repeated patch/module creation order.


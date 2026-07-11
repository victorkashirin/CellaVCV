# Spectrum Feature Review

This review is based on `src/Spectrum.cpp` as it stands now. The module already has a strong visual identity: a wide VFD-style display, 12 musical-ish frequency bands, stereo input, configurable dB range, fall/peak fall timing, dot and bar renderers, alpha mode, optional frequency labels, peak indicators, and three color themes. The most important next step is to turn those pieces into a more discoverable, more informative, and more expressive analyzer without making the panel feel busy.

## Current Strengths

- The big display is the right centerpiece. It gives the module room to be beautiful instead of feeling like a utility afterthought.
- Dot mode has character and already feels more distinctive than a generic spectrum analyzer.
- Bar mode gives a familiar metering option for users who want readability over personality.
- Alpha mode is a good direction because it lets level shape brightness, not just height.
- Show Labels is useful and points toward a family of display aids rather than a single fixed look.
- Top and bottom dB range controls are powerful, even though they are currently hidden in the context menu.
- Separate fall and peak fall timing lets the analyzer feel snappy, smooth, or performative.
- Theme persistence is already in place, so adding visual modes can follow the same pattern.

## Adjustments Before New Features

These fixes will make future visual work feel reliable and intentional.

- Add thread-safe display snapshots. The audio thread writes band levels while the UI thread reads them. Publish a small double-buffered display state or use atomics for the band values before adding more drawing complexity. - DONE
- Validate saved enum values on JSON load. Clamp `displayMode` and `currentTheme` to known values so older, malformed, or future patches cannot crash the display.
- Restore no-input decay. `handleNoInputDecay()` exists but is currently disabled. With no cables connected, the display should fade cleanly instead of holding stale analysis.
- Revisit stereo summing. Averaging L and R voltage can hide anti-phase stereo energy. For an analyzer, RMS/power sum is usually more useful than mono voltage sum. This also opens the door to proper stereo modes. - DONE
- Use `ProcessArgs::sampleRate` through the analysis path instead of repeatedly querying the global engine sample rate. - DONE
- Improve bar mode's inactive scaffold. Dot mode shows inactive cells; bar mode currently draws only active segments. A dim inactive grid or subtle baseline would make the mode easier to read. - DONE
- Calibrate input gain and dB mapping. `INPUT_GAIN = 0.1f` may be sensible for Rack voltage, but it should be documented and matched to the default top/bottom dB range so a typical 10 Vpp signal looks right out of the box.
- Add the missing Spectrum documentation. `plugin.json` points to a `#spectrum` manual anchor, but the manual does not currently include a Spectrum section.
- Fix Spectrum metadata. The `plugin.json` keywords and tags appear copied from Bezier. Use analyzer-focused metadata such as `spectrum`, `analyzer`, `visualizer`, `meter`, `audio`, and `utility`.

## High-Impact Visual Modes

These are the additions most likely to make the module stand out while staying useful.

### Display Mode

Extend the current Dots/Bars choice into a small set of clearly named renderers:

- Dots: keep the current VFD matrix identity.
- Bars: classic segmented analyzer, with dim inactive segments.
- Curve: draw a smooth spectrum line or filled contour across the 12 bands. This is less retro, more studio-like, and useful for reading broad tonal shape.
- Waterfall: a short scrolling history behind or below the current spectrum. This would be a signature visual mode if performance stays reasonable.
- Hybrid: dots for current level with a faint curve/outline for peak or average level.

Recommended first addition: Curve. It reuses the same band data, is cheap to draw, and gives the module a more modern analysis view without changing DSP.

### Intensity Mode

Expand alpha mode into a brightness behavior:

- Solid: current non-alpha behavior.
- Alpha: current brightness-by-level behavior.
- Glow: stronger halo on active dots/peaks, with controlled bloom.
- Ghost: slow fading afterimage/trail for recent energy.
- Clean: reduced glow and high contrast for precise reading.

Recommended first addition: rename Alpha Mode internally into `IntensityMode` while preserving existing patch compatibility.

### Peak Mode

Make peak behavior a named visual mode instead of only a timing slider:

- Off: no peak indicator.
- Fast: short musical peak hold.
- Classic: current red peak indicator.
- Infinite Hold: holds maximum until reset or mode change.
- Average + Peak: current level plus a slower average ghost trace and peak marker.

This creates useful metering behavior and gives users a way to move between playful and technical displays.

### Color Mode

Keep themes, but add a second axis for how color is applied:

- Theme: one active color, one inactive color, one peak color.
- Heat: low bands cool, high bands warm, or low levels dim and high levels hot.
- Band: each frequency region gets a restrained color identity.
- Stereo: left/mid and right/side use related but distinct colors.
- Alert: peaks, clipping, or very hot bands shift toward the peak color.

The key is to keep the palettes restrained. The Cella visual language can stay elegant while still being more expressive than a single-color meter.

## Standout Analysis Features

These would make Spectrum feel like a creative instrument tool, not just a display.


### Stereo And Mid/Side Modes

The module has L and R inputs, so use them as a visual advantage:

- Mono Energy: RMS/power sum of L and R.
- Left/Right Split: show L and R as mirrored, overlaid, or alternating bands.
- Mid/Side: show mono center energy and stereo side energy.
- Side Highlight: tint bands where side energy is high.
- Phase Warning: subtle indicator when L/R cancellation is likely.

This would give the module a clear reason to exist over a simple mono spectrum analyzer.

### Scale And Band Layout Modes

Current 12-band layout is simple and readable. Add optional layouts for different jobs:

- 12 Band VFD: current layout, default.
- 24 Band Detail: more analysis without becoming dense.
- 31 Band Third-Octave: familiar graphic EQ/analyzer spacing.
- Bass Focus: more resolution below 250 Hz.
- Full FFT Curve: continuous-looking line mode using more bins.

Recommended path: keep 12 bands as the identity, then add Bass Focus and Curve before attempting dense 31-band drawing.

### Musical Helpers

These should stay optional, but they would make the display delightful and practical:

- Note labels for band centers.
- Pitch tracking marker for the dominant low-frequency peak.
- Kick/Bass focus overlay below 250 Hz.
- Mix zone shading with very subtle background bands.
- Pink/flat compensation option so noise and full mixes read more intuitively.
- Momentary Freeze to inspect a moving spectrum.

## Intuitive Design Adjustments

- Move the most-used view choices closer to the surface. Consider small on-panel buttons or display-edge controls for Display, Labels, Intensity, and Theme. The context menu can keep advanced options.
- Keep the clean panel, but make discoverability better. A tiny row of icons or short labels near the display would be enough.
- Group context menu items by user goal: View, Labels, Motion, Stereo, Calibration, Theme.
- Add short, plain option names. Prefer `Glow`, `Ghost`, `Notes`, `Zones`, `Reference`, and `Mid/Side` over technical descriptions.
- Provide good defaults: Dots, Classic theme, Solid or subtle Alpha, labels off, RMS stereo energy, peak on, useful dB range.
- Add one-click presets: Studio, VFD, Mix Check, Bass Focus, Ambient Glow. Presets should only change visual/analysis settings, not hidden calibration unexpectedly.
- Make advanced sliders easier to understand. Rename `Top` and `Bottom` to `Top dB` and `Bottom dB` in UI text if Rack display permits.
- Add reset actions for range, peaks, and reference.
- Avoid making every beautiful mode loud. Some users will want a calm, precise analyzer in a busy Rack patch.

## Recommended Build Order

1. Stabilize the foundation: display snapshot, JSON clamping, no-input decay, RMS stereo energy, sample-rate plumbing, bar inactive scaffold, docs and metadata.
3. Convert Alpha Mode into Intensity Mode: Solid, Alpha, Glow, Ghost, Clean. Preserve old patches that saved `alphaMode`.
4. Add Curve display mode. It is the cheapest new view with the biggest readability win.
5. Add Peak Mode: Off, Classic, Infinite Hold, Average + Peak.
7. Add Stereo/Mid-Side visual modes.
8. Add Bass Focus or 24-band layout once the display architecture handles multiple band layouts cleanly.
9. Consider Waterfall after the cheaper modes are polished. It can be beautiful, but it needs careful performance and visual tuning.

## Signature Direction

The strongest identity for this module would be:

> A beautiful VFD-inspired spectrum analyzer that can switch between musical labels, glow/ghost intensity, curve or dot rendering, stereo/mid-side insight, and reference comparison.

That keeps the module delightful while making every visual flourish answer a real user question: What frequency range is active? How loud is it? How did it change? Is it centered or wide? Where should I focus?

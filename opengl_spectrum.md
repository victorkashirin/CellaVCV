# OpenGL Spectrum: prospective design and implementation plan

## Executive recommendation

Build a second module, tentatively **Spectrum GL** with slug `SpectrumGL`, and leave the existing `Spectrum` module and its patch compatibility untouched. Both modules should embed the same new DSP-only analyzer class, while each keeps its own presentation state and renderer.

Use Rack's `rack::widget::OpenGlWidget`. Do **not** create a GLFW window or OpenGL context: Rack already owns the GLFW window, context, framebuffer lifecycle, scaling, and composition into the NanoVG scene. The SDK exposes OpenGL/GLEW through `rack.hpp`.

The best first experiment is a single full-display fragment shader driven by a small RGBA data texture. This is simple, fast, and compatible with Rack's OpenGL 2.0 context. It can already deliver procedural dots/bars, analytic glow, scanlines, vignette, glass/reflection, noise, color grading, stereo coloring, and peak highlights. More stateful effects, such as a waterfall or feedback trails, can follow once the basic lifecycle and performance are proven.

The result should answer two questions independently:

1. Does the OpenGL aesthetic materially improve the module?
2. Does it remain reliable and cheap enough across macOS, Windows, Linux, Retina/HiDPI, zoom levels, screenshots, and the module browser?

## Findings from the current code and SDK

### Current Spectrum implementation

`src/Spectrum.cpp` currently combines four responsibilities in one translation unit:

- FFT capture and analysis (`RealFFT`, Hann window, stereo captures, magnitudes);
- band aggregation and smoothing (12 fixed bands, level fall and peak fall);
- audio/UI communication (`SpectrumBand` atomics);
- NanoVG presentation, menus, themes, and patch persistence.

The current FFT size is 2048. At 48 kHz it produces a new analysis frame approximately every 42.7 ms, or 23.4 times per second. Drawing may happen at the Rack UI frame rate, so interpolation and visual animation belong on the UI/render side rather than the audio thread.

The existing display is large: 496 x 320 Rack units. At a 2x backing scale this is roughly 992 x 640 pixels, about 635,000 fragments per rendered frame. A direct, low-pass fragment shader is reasonable; several full-resolution blur passes or long per-pixel loops are not a good starting point.

There are some DSP behaviors worth preserving deliberately during extraction:

- left/right power is combined after the FFT, avoiding anti-phase cancellation;
- DC and Nyquist use Rack's packed real-FFT layout specially;
- band edges are geometric means, with the last edge at Nyquist;
- fast attack and exponential fall are evaluated once per FFT frame;
- peak state is updated at audio rate at present;
- missing channels are zeroed and tracked per capture frame;
- all FFT vectors are allocated during construction, not in `process()`.

The GL version should initially use identical values and controls so an A/B comparison measures rendering rather than an accidental DSP change.

### The correct Rack integration point

The installed SDK provides `rack::widget::OpenGlWidget` in `include/widget/OpenGlWidget.hpp`. It derives from `FramebufferWidget`, marks itself dirty each UI frame, and calls a virtual `drawFramebuffer()` while Rack's framebuffer is bound. Rack later composites that framebuffer into its NanoVG scene.

There is also a minimal working example at:

`/Users/victorkashirin/code/Rack/BogaudioModules/src/TestGl.cpp`

The corresponding Rack implementation is at:

`/Users/victorkashirin/code/Rack/Rack/src/widget/OpenGlWidget.cpp`

This means the module should instantiate an `OpenGlWidget` child in the same display rectangle now occupied by `VFDCustomDisplay`. GLFW itself is an implementation detail here. Calling `glfwCreateWindow()`, changing Rack's window hints, making a new context current, or swapping buffers would interfere with Rack and is unnecessary.

### Important OpenGL compatibility limit

The plan was rechecked against the newly pulled **Rack SDK 2.6.6** at `/Users/victorkashirin/code/Rack/Rack-SDK-2.6.6`. Its `include/window/Window.hpp` still defines `NANOVG_GL2`, so Rack continues to expose the GL2 rendering path. The `OpenGlWidget` and `FramebufferWidget` public headers are unchanged from the previous SDK, as is the fixed-capacity SPSC `dsp::RingBuffer` used in the proposed audio/UI handoff. Therefore the production baseline remains:

- OpenGL 2.0;
- GLSL 1.10 as the safest guaranteed shader language (GLSL 1.20 can be selected only after runtime validation);
- compatibility-profile drawing;
- ordinary 2D textures and RGBA8 framebuffer content.

Do not base the module on VAOs, instancing, uniform buffer objects, geometry shaders, compute shaders, SSBOs, modern integer textures, or an assumed floating-point render target. Those can be optional capability paths later, but they cannot be the baseline for this SDK.

A GL2 implementation is still enough for the proposed look. A fullscreen quad can be drawn with compatibility vertex input, and almost all initial aesthetics can be procedural in one fragment shader.

The 2.6.6 SDK also still uses C++11 in `compile.mk`, bundles GLFW 3.4 and GLEW, and links plugins through `libRack` in the same way. Its unrelated API additions, such as font fallback handling, do not alter this module design. Build the experiment explicitly against the new SDK, for example with `make RACK_DIR=/Users/victorkashirin/code/Rack/Rack-SDK-2.6.6`, so local environment variables cannot silently select the older checkout.

### Build and packaging impact

No new third-party graphics dependency should be needed. `rack.hpp` includes Rack's GLEW/OpenGL declarations, and `plugin.mk` already links the plugin to `libRack`. The existing Makefile compiles every `src/*.cpp` and packages the entire `res` directory.

Recommended shader resources:

```text
res/shaders/spectrum_gl.vert
res/shaders/spectrum_gl.frag
```

They can be loaded through `asset::plugin()` plus `system::readFile()`. Because `res` is already a distributable, no Makefile packaging change is required. During early development external shader files make iteration pleasant. Before release, consider embedding a minimal fallback shader so a missing/corrupt resource produces a plain display instead of an empty panel.

## Proposed architecture

```text
audio thread                         UI / GL thread
------------                         --------------
inputs L/R
    |
SpectrumAnalyzer::processSample()
    |
FFT every 2048 samples
    |
SpectrumFrame ---------------------> latest complete snapshot
  - mono 12-band levels                  |
  - L/R 12-band levels                   +--> NanoVG Spectrum renderer
  - peaks (optional)                     |
  - 1025 bins (optional later)            +--> OpenGL Spectrum renderer
  - sequence/sample rate                       - smoothing/interpolation
                                                - data texture upload
                                                - fragment shader
```

### 1. Shared DSP core

Suggested files:

```text
src/spectrum/SpectrumAnalyzer.hpp
src/spectrum/SpectrumAnalyzer.cpp
src/spectrum/SpectrumTypes.hpp
```

Suggested division:

- `SpectrumConfig`: FFT size, band centers, channel count, noise floor, analysis gain;
- `SpectrumFrame`: one immutable analysis result plus a sequence number;
- `SpectrumAnalyzer`: capture buffers, Hann window, `RealFFT`, magnitude calculation, stereo power combination, band aggregation, and audio-rate input handling;
- renderer-specific state: peak animation, ghost trails, display range normalization, theme, visual mode, and frame interpolation.

The analyzer should accept plain samples/connection flags and parameters rather than reaching into a particular Rack `Module`. That makes it testable and genuinely reusable:

```cpp
analyzer.process(leftVoltage, rightVoltage,
                 leftConnected, rightConnected,
                 sampleRate, fallSeconds);
```

It should return or publish raw dB results. Avoid baking the current top/bottom display range into FFT output; normalization is a view concern and may change between frames without requiring new analysis.

For the first extraction, keep exactly 12 band outputs and current scaling. Once parity is established, optionally publish the full 1025-bin magnitude/dB spectrum as well. Full bins unlock a smooth continuous curve and waterfall, while the existing 12 bands remain useful for the VFD meter modes.

### 2. Safe audio-to-UI transfer

No GL operation, shader operation, lock, file read, logging call, or allocation may occur in `Module::process()`.

The current per-value atomics are simple but do not yield a coherent frame: the UI can observe values from adjacent FFT updates. That is usually invisible with 12 bands, but a waterfall needs a whole consistent row. Prefer a fixed-capacity single-producer/single-consumer queue of complete `SpectrumFrame` values. Rack already provides `dsp::RingBuffer<T, S>`.

Recommended behavior:

- audio thread produces one `SpectrumFrame` at FFT boundaries;
- if the queue is full, drop the new visualization frame rather than block audio;
- UI thread drains all available frames each step and retains the newest;
- all storage is fixed-size and allocated as part of the module;
- visual interpolation happens between the last two UI snapshots.

At roughly 23 frames/s, even a frame containing 1025 floats is modest. If the initial shared frame contains only 36 band values, the transfer cost is negligible.

Avoid an informal double buffer made of ordinary floats plus an atomic front index. Unless the producer can prove it never laps the consumer, it can overwrite a buffer while the UI reads it, which is a C++ data race. A bounded SPSC queue has clearer ownership.

### 3. Separate module, shared base behavior

Add a new module rather than a renderer switch inside `Spectrum`:

```text
src/SpectrumGL.cpp
extern Model* modelSpectrumGL;        // plugin.hpp
p->addModel(modelSpectrumGL);         // plugin.cpp
new SpectrumGL entry                  // plugin.json
```

Use slug `SpectrumGL` and a visible name such as `Spectrum GL` or `Spectrum Experimental`. Never rename or reuse the existing `Spectrum` slug, because patch identity depends on it.

Both modules can embed `SpectrumAnalyzer`. A small shared module-side helper or base class can own common parameters, inputs, JSON validation, and analyzer calls, but avoid making UI/render classes inherit a large coupled base solely to remove a few lines. DSP reuse is the important part.

Initially give Spectrum GL the same panel size, inputs, top/bottom/fall/peak controls, band layout, stereo options, and themes. Reusing the current panel SVG is fine for the comparison; a distinct `GL` label or experimental badge would prevent confusion in the module browser and patches.

When `module == nullptr` in the module browser, render a deterministic idle/demo frame. Never dereference a null module.

### 4. GL renderer classes

Suggested internal structure:

```text
SpectrumGLDisplay : rack::widget::OpenGlWidget
  - SpectrumGL* module                 non-owning
  - SpectrumGLRenderer renderer        owns UI-thread GL resources
  - local smoothed/ghost state

SpectrumGLRenderer
  - shader program and locations
  - small spectrum data texture
  - initialize(), draw(), destroy()
  - compile/link/error reporting
```

Keep GL object ownership on the UI thread. Initialize lazily on the first `drawFramebuffer()` with a current context, and also respond correctly to context recreation. Delete textures/programs in `onContextDestroy()` before delegating to the base implementation. Do not rely only on the C++ destructor, because the context might already be gone.

Shader compilation failure should be logged once with the compiler/linker message. The widget should then draw a simple dark fallback or delegate to a minimal non-shader path, rather than retry and log every frame.

## Recommended first rendering pipeline

### Data upload

Use a small nearest-filtered RGBA8 texture, for example 16 x 4 pixels:

- row 0: mono level, mono peak, mono ghost, spare;
- row 1: left level, left peak, left ghost, spare;
- row 2: right level, right peak, right ghost, spare;
- row 3: auxiliary/transient values or future use.

Only the first 12 columns are initially used. Convert normalized floats to bytes on the UI thread and update the texture with one `glTexSubImage2D()` per new display snapshot. This avoids depending on dynamic indexing of uniform arrays on old GLSL drivers and gives the shader direct band lookup.

For a later continuous spectrum/waterfall mode, use a second 1024 x 1 or 1024 x N RGBA8 texture. Log-frequency resampling can happen either on the UI thread or in the shader. UI-side resampling makes the shader cheaper and more predictable on old GPUs.

### Draw

Draw one fullscreen quad into the framebuffer already bound by `OpenGlWidget`. The fragment shader receives:

- resolution in actual framebuffer pixels from `getFramebufferSize()`;
- time and UI frame delta;
- data texture;
- colors/theme;
- stereo and visual mode flags;
- effect intensity and quality;
- label-area/display bounds if needed.

Use `gl_FragCoord` so dots, segment gaps, scanlines, and glow are pixel-consistent at different Rack zoom and HiDPI scales. Account explicitly for OpenGL's bottom-left fragment origin.

The first shader should calculate the band index directly from X, fetch one texel, then use signed-distance-style math for a bar or dot cell. Analytic glow based on distance is substantially cheaper than a multipass Gaussian blur and looks convincing for VFD/phosphor graphics.

### NanoVG and OpenGL composition

Use OpenGL for the animated display area. Keep static panel art, jacks, and context menus in normal Rack/NanoVG widgets. For frequency labels, the simplest robust choice is a small transparent NanoVG sibling layered after the OpenGL display. This preserves Rack font rendering and avoids building a font atlas solely for twelve labels.

Do not begin a second NanoVG frame inside `SpectrumGLDisplay::drawFramebuffer()`. Rack already manages NanoVG and the containing framebuffer lifecycle.

### Shared OpenGL state discipline

Rack and every plugin share one context. A renderer that leaks state can corrupt unrelated widgets. In each draw:

- query/save the shader program, active texture unit, texture binding, buffer bindings, blend configuration, viewport, and any state actually changed;
- set viewport from `getFramebufferSize()`;
- explicitly set blend/depth/cull/scissor state required by the shader;
- leave Rack's framebuffer binding alone in the one-pass implementation;
- restore every changed state and the previous program/texture unit;
- do not call `glfwSwapBuffers()`, `glfwMakeContextCurrent()`, or `glFinish()`.

GL2 compatibility state stacks can help for fixed-function state, but shader program and relevant bindings should still be restored explicitly. During development, wrap the draw with targeted `glGetError()` diagnostics; avoid per-frame GL error polling/logging in release builds.

## Visual directions worth testing

### Tier 1: high impact, low-to-moderate risk

These fit in the initial single fragment shader:

1. **Phosphor/VFD bloom**: sharp emissive core plus two analytic falloff lobes, with brightness driven by level and transient energy.
2. **Better segmented display**: rounded rectangles or circles with softly lit inactive cells, small manufacturing variation, and subpixel edge softness.
3. **Glass face**: subtle vertical gradient, edge vignette, top reflection, fine dust/noise, and a faint inner bezel shadow.
4. **Scanlines and grille**: very subtle, pixel-space modulation; scale intensity down at low zoom to prevent moire.
5. **Color temperature and overload**: base theme color shifts toward white/yellow at high energy and toward red around peaks.
6. **Stereo energy coloring**: left/right hues blend where energy agrees; decorrelating bands show separated colors.
7. **Peak flare**: a short horizontal bloom and tiny sparkle at peak position rather than only a flat red segment.
8. **Micro-motion**: slow noise drift, restrained brightness flutter, and attack flashes. These make the display feel alive without obscuring readings.

### Tier 2: moderate implementation cost

1. **Continuous log-frequency curve**: upload full FFT data, draw a smooth filled spectrum behind or instead of 12 bands.
2. **Waterfall/spectrogram**: update one row of a 2D history texture per FFT frame, scroll with a texture offset rather than copying the whole texture.
3. **Persistence trails**: fade the existing display framebuffer with a translucent black pass and add the new emission. This is simple but resets when Rack recreates the framebuffer. A dedicated ping-pong texture gives deterministic feedback and distortion at higher complexity.
4. **Energy fog**: low-frequency bands create broad slow illumination behind the display; highs create fine sparkle.
5. **Soft CRT curvature**: warp display coordinates slightly, with vignette and edge focus loss. Keep a flat mode for users who value measurement accuracy.
6. **Animated theme morphing**: interpolate theme uniforms on the UI thread instead of abruptly switching colors.

### Tier 3: experiments, not first-release requirements

1. **3D extruded bars/terrain**: build bar geometry on CPU or use a lightweight fragment raymarch. Visually strong, but less readable and more expensive.
2. **GPU particle sparks**: on GL2, update a small CPU particle list and render point sprites; transform feedback is not a safe baseline.
3. **Chromatic lens effects**: tasteful at low intensity, but easy to make blurry or gimmicky.
4. **Audio-reactive plasma/fluid background**: procedural rather than a real simulation for GL2. A real multipass fluid solver is disproportionate for a module display.
5. **Post-process bloom chain**: downsample and separable blur into auxiliary FBOs. Use only if analytic glow is visibly insufficient and measurements show budget.

Recommended aesthetic target: polished phosphor instrument, not a generic game shader. Readability and immediate frequency/level recognition should remain the primary constraint.

## Phased implementation

### Phase 0: establish parity and measurements

Goal: create a reliable baseline before changing architecture.

- capture screenshots/video of representative current modes and themes;
- record UI frame cost with one Spectrum at 100%, 75%, and 50% Rack zoom, including Retina/HiDPI;
- prepare test signals: silence, sine sweeps, pink noise, impulses, anti-phase stereo, left-only, right-only, and hot/clipped input;
- document expected 12-band/peak values for a few deterministic signals;
- decide the comparison panel name and visual defaults.

Exit criterion: repeatable A/B fixtures and known current behavior.

### Phase 1: extract shared FFT logic without visual changes

Goal: make `Spectrum` consume `SpectrumAnalyzer` while looking and behaving the same.

- move constants/types and FFT work into the proposed shared files;
- add the fixed-capacity `SpectrumFrame` handoff;
- keep all construction-time allocation and preserve current analysis math;
- move renderer-only ghost state out of DSP;
- keep JSON keys, model slug, parameter IDs, input IDs, and existing module behavior stable;
- build and run the existing module before adding GL.

Tests should cover packed DC/Nyquist handling, band edge clamping at low sample rates, anti-phase energy, disconnected channels, no NaN/inf at silence, and analyzer output parity within a small tolerance.

Exit criterion: the existing NanoVG module has no intentional visual or DSP regression.

### Phase 2: add Spectrum GL as a separate registered module

Goal: prove Rack lifecycle integration with a minimal GL display.

- create `SpectrumGL.cpp`, model registration, manifest entry, and panel distinction;
- embed the same analyzer and controls;
- implement `SpectrumGLDisplay : OpenGlWidget`;
- render a plain full-screen gradient/12 flat bands with a minimal shader;
- implement shader compile/link diagnostics and fallback behavior;
- implement context destroy/recreate handling;
- handle `module == nullptr` for the browser preview;
- verify patch save/load and coexistence with original Spectrum.

Exit criterion: no crashes or corrupted Rack UI through module add/remove, zoom, theme change, screenshots, window resize, plugin reload in a dev build, or context recreation where available.

### Phase 3: implement the comparison-quality GL2 shader

Goal: deliver a visibly superior but readable display.

- add the small RGBA band-data texture;
- implement dots and bars with analytic antialiasing/glow;
- port themes, mono/L-R split, peaks, labels, show-unlit behavior, and range controls;
- add restrained glass, vignette, scanline, noise, overload, and peak-flare effects;
- interpolate levels on the UI thread so 23 Hz FFT updates appear smooth;
- expose a small number of meaningful context-menu presets instead of many raw shader knobs;
- add an `Effects: Off / Subtle / Full` choice for a fair readability/performance comparison.

Exit criterion: feature parity for the useful existing modes plus a clearly differentiated GL look.

### Phase 4: add one signature mode

Goal: test something NanoVG would not do as naturally.

Recommended order:

1) **Phosphor/VFD bloom**
2) **Glass face**
3) **Micro-motion**
4) **Better segmented display**

Do one at a time and measure it.

Exit criterion: the signature mode stays responsive with multiple module instances and has a low-quality/off fallback.

Implementation status:

- Phosphor/VFD bloom is implemented as an optional signature mode. It adds near and wide analytic halo lobes behind the discrete segments without another framebuffer pass.
- `Effects: Off` disables the signature pass, `Subtle` evaluates only the near lobe, and `Full` enables the additional wide halo.
- The selection is patch-persisted as `signatureMode`; new Spectrum GL instances default to `Phosphor Bloom`, while phase-3 patches without the new key load it off to preserve their appearance.
- The `Full` preset is intentionally conspicuous enough for a clear A/B comparison; `Subtle` is the restrained shipping candidate rather than merely a numerically smaller but visually indistinguishable setting.
- Full bloom samples neighboring bands and the opposite stereo channel so the field spreads horizontally. It is composited as a background plane and occluded by lit/peak segment cores, preserving their foreground colors.
- In `Solid` light response, bloom energy is independent of bar height; other light responses retain level-dependent bloom intensity.
- `Subtle` uses a single broader near-halo at about 65% of Full's maximum bloom energy, keeping the lower-cost path visibly distinct from Off.
- Glass Face is implemented as a second optional signature mode without changing the persisted values of Off or Phosphor Bloom.
- The glass pass is composited over the emitters and follows Rack's top-down panel lighting with a bright top rim, shallow overhead reflection, vertical face tint, full-frame edge vignette, and faint inner-bezel shadow. `Full` additionally enables a broader top reflection, static dust, and fine glass grain; `Subtle` retains only the cheaper layered gradients.
- Micro Motion is implemented as a third optional signature mode, appended without changing the persisted values of Off, Phosphor Bloom, or Glass Face.
- It keeps meter geometry stationary while adding a slowly drifting phosphor field, per-band brightness breathing, and short attack halos driven by a positive-only UI-side envelope. `Full` deliberately uses a clearly visible colored drift field, stronger slow breathing, a second quicker flutter component, and a broader/brighter attack halo for meaningful A/B evaluation; `Subtle` retains lower-amplitude motion.
- Attack envelopes are derived and decayed over roughly 180 ms on the UI thread from complete analyzer snapshots, leaving the audio thread unchanged and preventing falling levels from triggering flashes. A small rise threshold rejects steady-state FFT jitter so sustained tones cannot continuously refresh the envelope. The shader renders a narrow hot core inside the broader attack halo so fast transients remain legible.
- The animated illumination field is confined to glow immediately around lit cells rather than filling an entire high-energy band column, keeping sustained readings clean while preserving visible drift on the emitters.

### Phase 5: hardening and decision

Goal: decide whether to ship, merge ideas back, or keep experimental.

- test macOS arm64 and x64 where possible, Windows x64, and Linux x64;
- test Intel/integrated graphics as well as modern discrete/Apple GPUs;
- test Rack zoom, HiDPI, light/dark panels, browser preview, screenshots, duplicate/delete, patch reload, engine sample-rate changes, bypass, and no-input state;
- test 1, 4, 8, and 16 module instances;
- verify that GL state is not leaked by viewing cables, menus, tooltips, other framebuffer widgets, and other vendors' GL modules after Spectrum GL draws;
- inspect shader failure logs by deliberately injecting a compile error;
- compare visual preference blind if possible, and measure UI/GPU cost rather than relying only on feel.

Decision options:

- ship both modules if GL provides a distinctive mode and acceptable cost;
- keep GL experimental if driver/lifecycle issues remain;
- backport only improved DSP/smoothing and a few restrained ideas if users prefer the clarity of NanoVG;
- eventually make GL the successor, but preserve the original slug/model indefinitely for old patches.

## Performance and quality budget

Suggested initial targets on a typical integrated GPU:

- zero measurable increase in audio-thread time compared with extracted NanoVG Spectrum;
- no allocations or locks in `process()`;
- one data texture update only when a new analyzer frame arrives;
- one fullscreen shader pass per UI frame for the standard mode;
- average display render cost below roughly 0.5 ms for one module at common zoom, with graceful scaling across multiple instances;
- no shader loop over all FFT bins per pixel;
- no full-frame CPU-to-GPU texture upload for a waterfall—upload only the new row;
- no `glFinish()` synchronization;
- `oversample = 1` initially, because Rack/HiDPI already increases framebuffer resolution;
- automatically reduce fine noise/scanlines or use the simple preset when the display is very small on screen.

If a 60 Hz animated display is too expensive, override `step()` to dirty the framebuffer at 30 Hz for expensive modes. Do not reduce audio analysis rate simply to solve rendering cost; the two rates should remain independent.

## Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Assuming modern OpenGL | Shader failure on Rack's GL2 context | Make GLSL 1.10/GL2 the baseline; capability-gate optional paths |
| GL state leakage | Other Rack UI or plugins render incorrectly | Save/restore every changed state; add cross-plugin visual tests |
| GL resources outlive context | Crash during shutdown/reload | Delete in `onContextDestroy()`; lazy reinitialize after recreation |
| Audio/UI data race | Undefined behavior or torn waterfall rows | Fixed-capacity SPSC frame queue; immutable consumer snapshot |
| Shader failure yields blank panel | Bad UX and hard debugging | Log compiler/linker output once; draw a simple fallback |
| Fill-rate cost at HiDPI | Dropped UI frames with many instances | Single pass, direct band indexing, effect quality presets, optional 30 Hz |
| Feedback depends on Rack FBO persistence | Trail resets during zoom/reallocation | Accept reset initially or own explicit ping-pong textures later |
| Moire from scanlines/dots | Unpleasant at low zoom | Work in physical pixels and fade detail based on pixel density |
| New module changes DSP during comparison | Invalid A/B conclusion | Establish analyzer parity first and share identical DSP |
| Too many visual controls | Menu complexity and non-cohesive design | Curated named presets plus one effects-strength setting |
| Duplicate modules double FFT cost | Misleading side-by-side benchmark | Benchmark each renderer alone for steady-state cost; label side-by-side as functional comparison |

## Suggested initial file plan

```text
src/
  Spectrum.cpp                         existing NanoVG module, refactored
  SpectrumGL.cpp                       new module and widget
  spectrum/
    SpectrumAnalyzer.hpp
    SpectrumAnalyzer.cpp
    SpectrumTypes.hpp
    SpectrumGLRenderer.hpp
    SpectrumGLRenderer.cpp
res/
  VFDFreqAnalyzerGL.svg                optional distinct panel
  VFDFreqAnalyzerGL-dark.svg           optional distinct dark panel
  shaders/
    spectrum_gl.vert
    spectrum_gl.frag
```

`SpectrumGLRenderer` could remain private to `SpectrumGL.cpp` for the first spike. Split it once the lifecycle works or when shader modes make the file unwieldy. The analyzer, however, should be extracted before duplicating the module so the two implementations cannot drift.

## Concrete first spike

A useful two-to-three session spike would be:

1. Copy the module shell to `SpectrumGL` but immediately replace copied FFT code with the shared analyzer.
2. Add a bare `OpenGlWidget` in the 496 x 320 display area.
3. Compile a GLSL 1.10 vertex/fragment pair and draw a full-screen quad.
4. Upload 12 normalized band values in a 16 x 1 RGBA8 texture.
5. Render emissive bars with a core, analytic glow, unlit scaffold, vignette, and subtle scanlines.
6. Overlay NanoVG frequency labels as a sibling widget.
7. Add an effects-off shader uniform to compare flat GL bars against the styled version.
8. Run the lifecycle and GL-state smoke tests before adding waterfall/history resources.

This spike is enough to evaluate the central aesthetic proposition without committing to complex framebuffer or multipass infrastructure.

## Final assessment

The project is technically feasible and Rack already exposes the appropriate integration class. The main correction to the initial idea is that GLFW should not be used to create or manage anything: use Rack's existing context through `OpenGlWidget`. The second major constraint is OpenGL 2.0, which favors a compact procedural shader and texture-driven data path rather than a modern GPU architecture.

The likely sweet spot is a hybrid UI: OpenGL for the animated spectral surface, NanoVG/SVG for text, panel furniture, and controls. Extracting the analyzer first makes the comparison credible and improves the original module regardless of whether the GL experiment ultimately ships.

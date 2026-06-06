#include "plugin.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>

// ============================================================================
//  Configuration & Constants
// ============================================================================
namespace VFDConfig {
// DSP Constants
static constexpr int FFT_SIZE = 2048;
static constexpr int SPEC_BINS = FFT_SIZE / 2 + 1;
static constexpr int NUM_BANDS = 12;
static constexpr float INPUT_GAIN = 0.1f;
static constexpr float MIN_DELAY_TIME = 0.001f;
static constexpr float NOISE_FLOOR_DB = -120.0f;
static constexpr float DENORMAL_THRESHOLD = 1e-6f;
static constexpr int NUM_AUDIO_CHANNELS = 2;

// Parameter Ranges
static constexpr float UPPER_DB_MIN = -40.0f;
static constexpr float UPPER_DB_MAX = 0.0f;
static constexpr float UPPER_DB_DEFAULT = -12.0f;
static constexpr float LOWER_DB_MIN = -120.0f;
static constexpr float LOWER_DB_MAX = -60.0f;
static constexpr float LOWER_DB_DEFAULT = -100.0f;
static constexpr float FALL_DELAY_MIN = 0.01f;
static constexpr float FALL_DELAY_MAX = 2.0f;
static constexpr float FALL_DELAY_DEFAULT = 0.01f;
static constexpr float PEAK_FALL_DELAY_MIN = 0.01f;
static constexpr float PEAK_FALL_DELAY_MAX = 3.0f;
static constexpr float PEAK_FALL_DELAY_DEFAULT = 1.0f;

// UI Layout Constants
static constexpr float DISPLAY_WIDTH = 496.0f;
static constexpr float DISPLAY_X_OFFSET = 0.0f;
static constexpr float DISPLAY_HEIGHT = 320.0f;
static constexpr float DISPLAY_Y_OFFSET = 26.0f;

// Display Constants - Dots Mode
static constexpr float DOT_RADIUS = 2.0f;
static constexpr float DOT_SPACING = 2.0f;
static constexpr float BAND_MARGIN = 3.0f;
static constexpr int MIN_COLUMNS = 3;
static constexpr int MAX_COLUMNS = 100;

// Display Constants - Bars Mode
static constexpr int BAR_SEGMENTS = 30;            // Number of segments in bar mode
static constexpr float BAR_SEGMENT_HEIGHT = 4.5f;  // Height of each bar segment

// Label Constants
static constexpr float LABEL_HEIGHT = 12.0f;  // Space reserved for frequency labels

// Frequency band edges (Hz)
static constexpr std::array<float, NUM_BANDS> BAND_CENTERS = {25.f,  40.f,   63.f,   100.f,  160.f,  250.f,
                                                              500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f};
}  // namespace VFDConfig

// Display Modes
enum class DisplayMode { DOTS, BARS };

// Intensity Modes
enum class IntensityMode { SOLID, ALPHA, GLOW, GHOST, CLEAN };

// Theme System
enum class Theme { CLASSIC, WARM, COOL };

struct ThemeColors {
    NVGcolor active;
    NVGcolor inactive;
    NVGcolor peak;
};

// Predefined themes
static const std::array<ThemeColors, 3> THEMES = {
    {// CLASSIC (original colors)
     {nvgRGB(0x93, 0xEA, 0xFF), nvgRGB(0x20, 0x20, 0x20), nvgRGB(0xFF, 0x30, 0x30)},
     // WARM
     {nvgRGB(0xFF, 0xB3, 0x47), nvgRGB(0x2A, 0x1A, 0x10), nvgRGB(0xFF, 0x47, 0x47)},
     // COOL
     {nvgRGB(0x47, 0xFF, 0x87), nvgRGB(0x10, 0x2A, 0x1A), nvgRGB(0xFF, 0x87, 0x47)}}};

// Theme-based color functions
static NVGcolor getActiveColor(Theme theme) { return THEMES[static_cast<int>(theme)].active; }

static NVGcolor getInactiveColor(Theme theme) { return THEMES[static_cast<int>(theme)].inactive; }

static NVGcolor getPeakColor(Theme theme) { return THEMES[static_cast<int>(theme)].peak; }

template <typename T>
static inline T clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ============================================================================
//  DSP Helper Structures
// ============================================================================
struct SpectrumBand {
    // Written by the audio thread and read by the UI thread.
    std::atomic<float> dbLevel;
    std::atomic<float> peakLevel;

    SpectrumBand() : dbLevel(VFDConfig::NOISE_FLOOR_DB), peakLevel(0.0f) {}

    float getDbLevel() const { return dbLevel.load(std::memory_order_relaxed); }

    void setDbLevel(float value) { dbLevel.store(value, std::memory_order_relaxed); }

    float getPeakLevel() const { return peakLevel.load(std::memory_order_relaxed); }

    void updateLevel(float newDb, float fallDecay) {
        float currentDb = getDbLevel();
        float updatedDb;
        if (newDb >= currentDb) {
            updatedDb = newDb;  // Fast attack
        } else {
            updatedDb = currentDb * fallDecay + newDb * (1.0f - fallDecay);
        }

        // Prevent excessive decay
        updatedDb = std::max(updatedDb, VFDConfig::NOISE_FLOOR_DB);
        setDbLevel(updatedDb);
    }

    void updatePeak(float currentNormalized, float peakDecay) {
        float currentPeak = getPeakLevel();
        float updatedPeak;
        if (currentNormalized >= currentPeak) {
            updatedPeak = currentNormalized;
        } else {
            updatedPeak = currentPeak * peakDecay;
            if (updatedPeak < VFDConfig::DENORMAL_THRESHOLD) {
                updatedPeak = 0.0f;
            }
        }
        peakLevel.store(clamp(updatedPeak, 0.0f, 1.0f), std::memory_order_relaxed);
    }
};

struct DisplayRange {
    float topDb;
    float bottomDb;
    float rangeDb;

    DisplayRange(float top, float bottom) : topDb(top), bottomDb(bottom) { rangeDb = topDb - bottomDb; }

    float normalizeDb(float db) const {
        float clampedDb = clamp(db, bottomDb, topDb);
        return (clampedDb - bottomDb) / rangeDb;
    }
};

struct VFDQuantity : ParamQuantity {
    std::string getDisplayValueString() override { return rack::string::f("%0.2f", getDisplayValue()); }
};

// ============================================================================
//  Module
// ============================================================================
struct Spectrum : Module {
    enum ParamIds { UPPER_PARAM, LOWER_PARAM, FALL_DELAY_PARAM, PEAK_FALL_DELAY_PARAM, NUM_PARAMS };
    enum InputIds { IN_L_INPUT, IN_R_INPUT, NUM_INPUTS };

    // DSP state
    dsp::RealFFT fft{VFDConfig::FFT_SIZE};
    std::vector<float> window;
    std::array<std::vector<float>, VFDConfig::NUM_AUDIO_CHANNELS> captures;
    std::vector<float> fftOutput;
    std::vector<float> magnitudes;
    std::array<bool, VFDConfig::NUM_AUDIO_CHANNELS> frameChannelActive = {};
    std::array<SpectrumBand, VFDConfig::NUM_BANDS> bands;
    int writePos = 0;

    // Display state
    DisplayMode displayMode = DisplayMode::DOTS;
    IntensityMode intensityMode = IntensityMode::SOLID;
    bool showLabels = false;
    bool showUnlitSegments = true;
    Theme currentTheme = Theme::CLASSIC;

    Spectrum() {
        config(NUM_PARAMS, NUM_INPUTS, 0, 0);
        configureParameters();
        configureInputs();
        initializeDSP();
    }

    void configureParameters() {
        configParam<VFDQuantity>(UPPER_PARAM, VFDConfig::UPPER_DB_MIN, VFDConfig::UPPER_DB_MAX,
                                 VFDConfig::UPPER_DB_DEFAULT, "Top", " dB");
        configParam<VFDQuantity>(LOWER_PARAM, VFDConfig::LOWER_DB_MIN, VFDConfig::LOWER_DB_MAX,
                                 VFDConfig::LOWER_DB_DEFAULT, "Bottom", " dB");
        configParam<VFDQuantity>(FALL_DELAY_PARAM, VFDConfig::FALL_DELAY_MIN, VFDConfig::FALL_DELAY_MAX,
                                 VFDConfig::FALL_DELAY_DEFAULT, "Fall Delay", "s");
        configParam<VFDQuantity>(PEAK_FALL_DELAY_PARAM, VFDConfig::PEAK_FALL_DELAY_MIN, VFDConfig::PEAK_FALL_DELAY_MAX,
                                 VFDConfig::PEAK_FALL_DELAY_DEFAULT, "Peak Fall Delay", "s");
    }

    void configureInputs() {
        configInput(IN_L_INPUT, "Left");
        configInput(IN_R_INPUT, "Right");
    }

    void initializeDSP() {
        window.resize(VFDConfig::FFT_SIZE);
        for (auto& capture : captures) {
            capture.resize(VFDConfig::FFT_SIZE);
        }
        fftOutput.resize(VFDConfig::FFT_SIZE);
        magnitudes.resize(VFDConfig::SPEC_BINS);

        // Generate Hann window
        for (int i = 0; i < VFDConfig::FFT_SIZE; ++i) {
            window[i] = 0.5f * (1.f - std::cos(2.f * M_PI * i / (VFDConfig::FFT_SIZE - 1)));
        }
    }

    void process(const ProcessArgs& args) override {
        processWindowedInput(args.sampleRate);
        updateDecayAndPeaks(args);
        // handleNoInputDecay(args);
    }

    void processWindowedInput(float sampleRate) {
        const bool leftConnected = inputs[IN_L_INPUT].isConnected();
        const bool rightConnected = inputs[IN_R_INPUT].isConnected();
        const float windowValue = window[writePos] * VFDConfig::INPUT_GAIN;

        captures[0][writePos] = leftConnected ? inputs[IN_L_INPUT].getVoltage() * windowValue : 0.f;
        captures[1][writePos] = rightConnected ? inputs[IN_R_INPUT].getVoltage() * windowValue : 0.f;

        frameChannelActive[0] = frameChannelActive[0] || leftConnected;
        frameChannelActive[1] = frameChannelActive[1] || rightConnected;

        if (++writePos >= VFDConfig::FFT_SIZE) {
            writePos = 0;
            analyzeFFT(sampleRate);
            frameChannelActive.fill(false);
        }
    }

    void updateDecayAndPeaks(const ProcessArgs& args) {
        DisplayRange range(params[UPPER_PARAM].getValue(), params[LOWER_PARAM].getValue());
        float peakDecay = calculateDecay(args.sampleTime, params[PEAK_FALL_DELAY_PARAM].getValue());

        for (auto& band : bands) {
            float currentNormalized = range.normalizeDb(band.getDbLevel());
            band.updatePeak(currentNormalized, peakDecay);
        }
    }

    void handleNoInputDecay(const ProcessArgs& args) {
        if (!inputs[IN_L_INPUT].isConnected() && !inputs[IN_R_INPUT].isConnected()) {
            float fallDecay = calculateDecay(args.sampleTime, params[FALL_DELAY_PARAM].getValue());

            for (auto& band : bands) {
                float decayedDb = std::max(band.getDbLevel() * fallDecay, VFDConfig::NOISE_FLOOR_DB);
                band.setDbLevel(decayedDb);
            }
        }
    }

    float calculateDecay(float deltaTime, float delayParam) {
        float delay = std::max(VFDConfig::MIN_DELAY_TIME, delayParam);
        return std::exp(-deltaTime / delay);
    }

    void analyzeFFT(float sampleRate) {
        performFFT();
        auto frequencyEdges = getFrequencyEdges(sampleRate);
        float fallDecay = calculateFallDecay(sampleRate);

        updateBandLevels(magnitudes, frequencyEdges, VFDConfig::SPEC_BINS, fallDecay, sampleRate);
    }

    void performFFT() {
        std::fill(magnitudes.begin(), magnitudes.end(), 0.f);

        int activeChannels = 0;
        // Combine channel powers after the FFT so anti-phase stereo still reports energy.
        for (int channel = 0; channel < VFDConfig::NUM_AUDIO_CHANNELS; ++channel) {
            if (!frameChannelActive[channel]) {
                continue;
            }

            ++activeChannels;
            fft.rfft(captures[channel].data(), fftOutput.data());
            fft.scale(fftOutput.data());
            addFFTBinPowers();
        }

        if (activeChannels == 0) {
            return;
        }

        const float channelScale = 1.f / activeChannels;
        for (float& magnitude : magnitudes) {
            magnitude = std::sqrt(magnitude * channelScale);
        }
    }

    void addFFTBinPowers() {
        magnitudes[0] += fftOutput[0] * fftOutput[0];
        magnitudes[VFDConfig::FFT_SIZE / 2] += fftOutput[1] * fftOutput[1];
        for (int k = 1; k < VFDConfig::FFT_SIZE / 2; ++k) {
            float re = fftOutput[2 * k];
            float im = fftOutput[2 * k + 1];
            magnitudes[k] += re * re + im * im;
        }
    }

    std::array<float, VFDConfig::NUM_BANDS + 1> getFrequencyEdges(float sampleRate) {
        std::array<float, VFDConfig::NUM_BANDS + 1> edges;

        // First edge: geometric mean between first center and a reasonable low
        // frequency
        edges[0] = std::sqrt(VFDConfig::BAND_CENTERS[0] * (VFDConfig::BAND_CENTERS[0] / 2.0f));

        // Middle edges: geometric mean between adjacent centers
        for (int i = 1; i < VFDConfig::NUM_BANDS; ++i) {
            edges[i] = std::sqrt(VFDConfig::BAND_CENTERS[i - 1] * VFDConfig::BAND_CENTERS[i]);
        }

        // Last edge: Nyquist frequency
        edges[VFDConfig::NUM_BANDS] = sampleRate * 0.5f;

        return edges;
    }

    float calculateFallDecay(float sampleRate) {
        float fallDelay = std::max(VFDConfig::MIN_DELAY_TIME, params[FALL_DELAY_PARAM].getValue());
        float deltaTime = static_cast<float>(VFDConfig::FFT_SIZE) / sampleRate;
        return std::exp(-deltaTime / fallDelay);
    }

    void updateBandLevels(const std::vector<float>& magnitudes,
                          const std::array<float, VFDConfig::NUM_BANDS + 1>& edges, int specBins, float fallDecay,
                          float sampleRate) {
        for (int b = 0; b < VFDConfig::NUM_BANDS; ++b) {
            float avgMagnitude = calculateBandMagnitude(magnitudes, edges[b], edges[b + 1], specBins, sampleRate);
            float newDb = 20.f * std::log10(avgMagnitude + VFDConfig::DENORMAL_THRESHOLD);
            bands[b].updateLevel(newDb, fallDecay);
        }
    }

    float calculateBandMagnitude(const std::vector<float>& magnitudes, float fLo, float fHi, int specBins,
                                 float sampleRate) {
        int binLo = clamp<int>(std::floor(fLo * VFDConfig::FFT_SIZE / sampleRate), 0, specBins - 1);
        int binHi = clamp<int>(std::ceil(fHi * VFDConfig::FFT_SIZE / sampleRate), binLo + 1, specBins);
        if (fHi >= sampleRate * 0.5f) {
            binHi = specBins;
        }

        float sum = 0.f;
        for (int k = binLo; k < binHi; ++k) {
            sum += magnitudes[k];
        }
        return sum / (binHi - binLo);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "displayMode", json_integer(static_cast<int>(displayMode)));
        json_object_set_new(rootJ, "intensityMode", json_integer(static_cast<int>(intensityMode)));
        json_object_set_new(rootJ, "showLabels", json_boolean(showLabels));
        json_object_set_new(rootJ, "showUnlitSegments", json_boolean(showUnlitSegments));
        json_object_set_new(rootJ, "currentTheme", json_integer(static_cast<int>(currentTheme)));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* displayModeJ = json_object_get(rootJ, "displayMode");
        if (displayModeJ) {
            displayMode = static_cast<DisplayMode>(json_integer_value(displayModeJ));
        }

        json_t* intensityModeJ = json_object_get(rootJ, "intensityMode");
        if (intensityModeJ) {
            intensityMode = static_cast<IntensityMode>(json_integer_value(intensityModeJ));
        }

        json_t* showLabelsJ = json_object_get(rootJ, "showLabels");
        if (showLabelsJ) {
            showLabels = json_boolean_value(showLabelsJ);
        }

        json_t* showUnlitSegmentsJ = json_object_get(rootJ, "showUnlitSegments");
        if (showUnlitSegmentsJ) {
            showUnlitSegments = json_boolean_value(showUnlitSegmentsJ);
        }

        json_t* currentThemeJ = json_object_get(rootJ, "currentTheme");
        if (currentThemeJ) {
            currentTheme = static_cast<Theme>(json_integer_value(currentThemeJ));
        }
    }
};

// ============================================================================
//  Display Grid Helper
// ============================================================================
struct DisplayGrid {
    int columns;
    int rows;
    float gridWidth;
    float gridHeight;
    float xStart;
    float yStart;

    DisplayGrid(float availableWidth, float availableHeight, float xOffset) {
        columns = calculateColumns(availableWidth);
        rows = calculateRows(availableHeight) + 1;

        gridWidth = columns * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING) - VFDConfig::DOT_SPACING;
        gridHeight = rows * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING) - VFDConfig::DOT_SPACING;

        xStart = xOffset + (availableWidth - gridWidth) / 2;
        yStart = 2 * VFDConfig::BAND_MARGIN;
    }

    int calculateColumns(float availableWidth) {
        int cols = static_cast<int>((availableWidth + VFDConfig::DOT_SPACING) /
                                    (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING));
        return clamp(cols, VFDConfig::MIN_COLUMNS, VFDConfig::MAX_COLUMNS);
    }

    int calculateRows(float availableHeight) {
        return static_cast<int>((availableHeight + VFDConfig::DOT_SPACING) /
                                (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING));
    }
};

// ============================================================================
//  VFD Display Widget
// ============================================================================
struct VFDCustomDisplay : LedDisplay {
    Spectrum* module;
    std::array<float, VFDConfig::NUM_BANDS> ghostLevels = {};
    std::chrono::steady_clock::time_point lastGhostUpdate = std::chrono::steady_clock::now();

    struct IntensityStyle {
        float levelAlpha = 1.0f;
        float peakAlpha = 1.0f;
        float inactiveAlpha = 1.0f;
        float activeGlowAlpha = 0.0f;
        float activeGlowRadius = VFDConfig::DOT_RADIUS * 3.0f;
        float peakGlowAlpha = 0.0f;
        float peakGlowRadius = VFDConfig::DOT_RADIUS * 3.0f;
    };

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1 || !module) return;

        nvgSave(args.vg);
        drawBands(args.vg);
        if (module->showLabels) {
            drawFrequencyLabels(args.vg);
        }
        nvgRestore(args.vg);
    }

    void drawBands(NVGcontext* vg) {
        size_t numBands = VFDConfig::NUM_BANDS;
        const float totalWidth = box.size.x - 2 * VFDConfig::BAND_MARGIN;
        const float bandWidth = totalWidth / numBands;

        DisplayRange range(module->params[0].getValue(), module->params[1].getValue());
        updateGhostLevels(range);

        for (size_t b = 0; b < numBands; b++) {
            drawSingleBand(vg, b, bandWidth, range);
        }
    }

    void drawSingleBand(NVGcontext* vg, size_t bandIndex, float bandWidth, const DisplayRange& range) {
        float level = range.normalizeDb(module->bands[bandIndex].getDbLevel());
        float peakLevel = module->bands[bandIndex].getPeakLevel();
        float ghostLevel = ghostLevels[bandIndex];

        float xOffset = VFDConfig::BAND_MARGIN + bandIndex * bandWidth;
        float availableWidth = bandWidth - VFDConfig::BAND_MARGIN * 2;

        if (module->displayMode == DisplayMode::DOTS) {
            drawDotsMode(vg, level, peakLevel, ghostLevel, xOffset, availableWidth);
        } else {
            drawBarsMode(vg, level, peakLevel, ghostLevel, xOffset, availableWidth);
        }
    }

    void updateGhostLevels(const DisplayRange& range) {
        auto now = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastGhostUpdate).count();
        lastGhostUpdate = now;

        deltaTime = clamp(deltaTime, 0.0f, 0.1f);
        float ghostDecay = std::exp(-deltaTime / 0.9f);

        for (size_t b = 0; b < VFDConfig::NUM_BANDS; ++b) {
            float level = range.normalizeDb(module->bands[b].getDbLevel());
            if (level >= ghostLevels[b]) {
                ghostLevels[b] = level;
            } else {
                ghostLevels[b] *= ghostDecay;
                if (ghostLevels[b] < VFDConfig::DENORMAL_THRESHOLD) {
                    ghostLevels[b] = 0.0f;
                }
            }
        }
    }

    void drawDotsMode(NVGcontext* vg, float level, float peakLevel, float ghostLevel, float xOffset,
                      float availableWidth) {
        DisplayGrid grid(availableWidth, getAvailableDisplayHeight(),
                         xOffset + VFDConfig::BAND_MARGIN + VFDConfig::DOT_RADIUS);

        IntensityStyle style = getIntensityStyle(level, peakLevel);

        if (module->showUnlitSegments) {
            drawDotGrid(vg, grid, createAlphaColor(getInactiveColor(module->currentTheme), style.inactiveAlpha));
        }

        if (module->intensityMode == IntensityMode::GHOST && ghostLevel > level) {
            drawGhostDots(vg, grid, level, ghostLevel);
        }

        if (level > 0.0f) {
            drawActiveDots(vg, grid, level, style);
        }

        if (peakLevel > 0.0f) {
            drawPeakIndicator(vg, grid, peakLevel, style);
        }
    }

    void drawBarsMode(NVGcontext* vg, float level, float peakLevel, float ghostLevel, float xOffset,
                      float availableWidth) {
        float barX = xOffset + VFDConfig::BAND_MARGIN;
        float barWidth = availableWidth;
        float barHeight = getAvailableDisplayHeight();
        float barY = 2 * VFDConfig::BAND_MARGIN + 1.5f;

        // Use configured values directly
        int numSegments = VFDConfig::BAR_SEGMENTS;
        float segmentHeight = VFDConfig::BAR_SEGMENT_HEIGHT;

        // Calculate spacing to fit segments with bottom margin
        float totalSegmentHeight = numSegments * segmentHeight;
        float availableSpacing = barHeight - totalSegmentHeight;
        float segmentSpacing = availableSpacing / (numSegments - 1);

        // Ensure minimum spacing
        if (segmentSpacing < 0.5f) {
            segmentSpacing = 0.5f;
        }

        // Start from bottom with margin
        float startY = barY;

        IntensityStyle style = getIntensityStyle(level, peakLevel);
        float activeBarGlowAlpha = 0.0f;
        float peakBarGlowAlpha = 0.0f;
        if (module->intensityMode == IntensityMode::GLOW) {
            activeBarGlowAlpha = style.activeGlowAlpha;
            peakBarGlowAlpha = style.peakGlowAlpha;
        } else if (module->intensityMode == IntensityMode::ALPHA || module->intensityMode == IntensityMode::GHOST) {
            activeBarGlowAlpha = style.activeGlowAlpha * 0.45f;
            peakBarGlowAlpha = style.peakGlowAlpha * 0.45f;
        }

        if (module->showUnlitSegments) {
            drawBarScaffold(vg, barX, barWidth, startY, numSegments, segmentHeight, segmentSpacing,
                            style.inactiveAlpha);
        }

        if (module->intensityMode == IntensityMode::GHOST && ghostLevel > level) {
            drawGhostBars(vg, barX, barWidth, startY, numSegments, segmentHeight, segmentSpacing, level, ghostLevel);
        }

        // Draw active segments
        if (level > 0.0f) {
            int activeSegments = static_cast<int>(std::ceil(level * numSegments));
            NVGcolor activeColor = getActiveColor(module->currentTheme);

            for (int i = 0; i < activeSegments && i < numSegments; i++) {
                int segmentIndex = numSegments - 1 - i;  // Start from bottom
                float segY = startY + segmentIndex * (segmentHeight + segmentSpacing);

                drawBarSegmentWithIntensity(vg, barX, segY, barWidth, segmentHeight, activeColor, style.levelAlpha,
                                            activeBarGlowAlpha);
            }
        }

        // Draw peak segment
        if (peakLevel > 0.0f) {
            int peakSegment = numSegments - 1 - static_cast<int>(std::floor(peakLevel * numSegments));
            peakSegment = clamp(peakSegment, 0, numSegments - 1);

            // Ensure peak is never below current level
            int currentActiveSegment = numSegments - static_cast<int>(std::ceil(level * numSegments));
            peakSegment = std::min(peakSegment, currentActiveSegment);

            float segY = startY + peakSegment * (segmentHeight + segmentSpacing);
            drawBarSegmentWithIntensity(vg, barX, segY, barWidth, segmentHeight, getPeakColor(module->currentTheme),
                                        style.peakAlpha, peakBarGlowAlpha);
        }
    }

    void drawBarScaffold(NVGcontext* vg, float x, float width, float startY, int numSegments, float segmentHeight,
                         float segmentSpacing, float inactiveAlpha) {
        NVGcolor inactiveColor = createAlphaColor(getInactiveColor(module->currentTheme), inactiveAlpha);
        for (int i = 0; i < numSegments; i++) {
            float segY = startY + i * (segmentHeight + segmentSpacing);
            drawBarSegment(vg, x, segY, width, segmentHeight, inactiveColor);
        }
    }

    void drawBarSegment(NVGcontext* vg, float x, float y, float width, float height, NVGcolor color) {
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillColor(vg, color);
        nvgFill(vg);
    }

    void drawBarSegmentWithIntensity(NVGcontext* vg, float x, float y, float width, float height, NVGcolor color,
                                     float alpha, float glowAlpha) {
        if (glowAlpha > 0.0f) {
            drawBarGlow(vg, x, y, width, height, color, glowAlpha);
        }
        drawBarSegment(vg, x, y, width, height, createAlphaColor(color, alpha));
    }

    void drawBarGlow(NVGcontext* vg, float x, float y, float width, float height, NVGcolor color, float alpha) {
        drawBarSegment(vg, x - 7.0f, y - 4.0f, width + 14.0f, height + 8.0f,
                       createAlphaColor(color, alpha * 0.18f));
        drawBarSegment(vg, x - 4.5f, y - 2.5f, width + 9.0f, height + 5.0f,
                       createAlphaColor(color, alpha * 0.28f));
        drawBarSegment(vg, x - 2.5f, y - 1.5f, width + 5.0f, height + 3.0f,
                       createAlphaColor(color, alpha * 0.42f));
    }

    void drawDotGrid(NVGcontext* vg, const DisplayGrid& grid, NVGcolor color) {
        nvgFillColor(vg, color);
        for (int c = 0; c < grid.columns; c++) {
            for (int r = 0; r < grid.rows; r++) {
                float dx = grid.xStart + c * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);
                float dy = grid.yStart + r * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);

                nvgBeginPath(vg);
                nvgCircle(vg, dx, dy, VFDConfig::DOT_RADIUS);
                nvgFill(vg);
            }
        }
    }

    void drawGhostDots(NVGcontext* vg, const DisplayGrid& grid, float level, float ghostLevel) {
        int activeRows = clamp(static_cast<int>(std::ceil(level * grid.rows)), 0, grid.rows);
        int ghostRows = clamp(static_cast<int>(std::ceil(ghostLevel * grid.rows)), 0, grid.rows);
        int firstGhostRow = grid.rows - ghostRows;
        int firstActiveRow = grid.rows - activeRows;
        int trailRows = std::max(firstActiveRow - firstGhostRow, 1);
        NVGcolor activeColor = getActiveColor(module->currentTheme);

        for (int c = 0; c < grid.columns; c++) {
            for (int r = firstGhostRow; r < firstActiveRow; r++) {
                float dy = grid.yStart + r * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);
                float dx = grid.xStart + c * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);
                float rowFade = static_cast<float>(r - firstGhostRow + 1) / trailRows;

                nvgBeginPath(vg);
                nvgCircle(vg, dx, dy, VFDConfig::DOT_RADIUS);
                nvgFillColor(vg, createAlphaColor(activeColor, 0.22f + 0.28f * rowFade));
                nvgFill(vg);
            }
        }
    }

    void drawGhostBars(NVGcontext* vg, float x, float width, float startY, int numSegments, float segmentHeight,
                       float segmentSpacing, float level, float ghostLevel) {
        int activeSegments = clamp(static_cast<int>(std::ceil(level * numSegments)), 0, numSegments);
        int ghostSegments = clamp(static_cast<int>(std::ceil(ghostLevel * numSegments)), 0, numSegments);
        int trailSegments = std::max(ghostSegments - activeSegments, 1);
        NVGcolor activeColor = getActiveColor(module->currentTheme);

        for (int i = activeSegments; i < ghostSegments; i++) {
            int segmentIndex = numSegments - 1 - i;
            float segY = startY + segmentIndex * (segmentHeight + segmentSpacing);
            float segmentFade = static_cast<float>(ghostSegments - i) / trailSegments;
            drawBarSegment(vg, x, segY, width, segmentHeight,
                           createAlphaColor(activeColor, 0.22f + 0.28f * segmentFade));
        }
    }

    void drawActiveDots(NVGcontext* vg, const DisplayGrid& grid, float level, const IntensityStyle& style) {
        int activeRows = clamp(static_cast<int>(std::ceil(level * grid.rows)), 0, grid.rows);
        int firstActiveRow = grid.rows - activeRows;

        for (int c = 0; c < grid.columns; c++) {
            for (int r = firstActiveRow; r < grid.rows; r++) {
                float dy = grid.yStart + r * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);
                float dx = grid.xStart + c * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);
                drawActiveDot(vg, dx, dy, style);
            }
        }
    }

    void drawActiveDot(NVGcontext* vg, float x, float y, const IntensityStyle& style) {
        if (!module) return;

        NVGcolor activeColor = getActiveColor(module->currentTheme);
        drawDotGlow(vg, x, y, activeColor, style.activeGlowRadius, style.activeGlowAlpha);

        // Core dot
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, VFDConfig::DOT_RADIUS);
        nvgFillColor(vg, createAlphaColor(activeColor, style.levelAlpha));
        nvgFill(vg);
    }

    void drawPeakIndicator(NVGcontext* vg, const DisplayGrid& grid, float peakLevel, const IntensityStyle& style) {
        NVGcolor peakColor = getPeakColor(module->currentTheme);
        int peakRow = clamp(grid.rows - static_cast<int>(std::ceil(peakLevel * grid.rows)), 0, grid.rows - 1);
        float dy = grid.yStart + peakRow * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);

        for (int c = 0; c < grid.columns; c++) {
            float dx = grid.xStart + c * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);
            drawDotGlow(vg, dx, dy, peakColor, style.peakGlowRadius, style.peakGlowAlpha);
            nvgBeginPath(vg);
            nvgCircle(vg, dx, dy, VFDConfig::DOT_RADIUS);
            nvgFillColor(vg, createAlphaColor(peakColor, style.peakAlpha));
            nvgFill(vg);
        }
    }

    void drawDotGlow(NVGcontext* vg, float x, float y, NVGcolor color, float radius, float alpha) {
        if (alpha <= 0.0f) {
            return;
        }

        NVGpaint paint = nvgRadialGradient(vg, x, y, 0.0f, radius, createAlphaColor(color, alpha),
                                           createAlphaColor(color, 0.0f));
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, radius);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }

    IntensityStyle getIntensityStyle(float level, float peakLevel) {
        IntensityStyle style;
        float levelBrightness = std::sqrt(clamp(level, 0.0f, 1.0f));
        float peakBrightness = std::sqrt(clamp(peakLevel, 0.0f, 1.0f));

        switch (module->intensityMode) {
            case IntensityMode::ALPHA:
                style.levelAlpha = levelBrightness;
                style.peakAlpha = peakBrightness;
                style.activeGlowAlpha = 0.10f + 0.24f * levelBrightness;
                style.peakGlowAlpha = 0.08f + 0.18f * peakBrightness;
                break;
            case IntensityMode::GLOW:
                style.levelAlpha = 0.72f + 0.28f * levelBrightness;
                style.peakAlpha = 0.82f + 0.18f * peakBrightness;
                style.activeGlowAlpha = 0.34f + 0.18f * levelBrightness;
                style.activeGlowRadius = VFDConfig::DOT_RADIUS * 5.0f;
                style.peakGlowAlpha = 0.40f;
                style.peakGlowRadius = VFDConfig::DOT_RADIUS * 4.0f;
                break;
            case IntensityMode::GHOST:
                style.levelAlpha = 0.92f;
                style.peakAlpha = 0.82f;
                style.inactiveAlpha = 0.20f;
                style.activeGlowAlpha = 100.0f / 255.0f;
                style.peakGlowAlpha = 0.12f;
                break;
            case IntensityMode::CLEAN:
                style.inactiveAlpha = 0.50f;
                break;
            case IntensityMode::SOLID:
            default:
                style.activeGlowAlpha = 100.0f / 255.0f;
                break;
        }

        return style;
    }

    // Helper function to create alpha-adjusted color
    NVGcolor createAlphaColor(NVGcolor baseColor, float alpha) {
        return nvgTransRGBA(baseColor, static_cast<unsigned char>(clamp(alpha, 0.0f, 1.0f) * 255.0f));
    }

    // Helper function to get available display height (accounting for labels)
    float getAvailableDisplayHeight() {
        float totalHeight = box.size.y - 3 * VFDConfig::BAND_MARGIN;
        if (module && module->showLabels) {
            totalHeight -= VFDConfig::LABEL_HEIGHT;
        }
        return totalHeight;
    }

    void drawFrequencyLabels(NVGcontext* vg) {
        size_t numBands = VFDConfig::NUM_BANDS;
        const float totalWidth = box.size.x - 2 * VFDConfig::BAND_MARGIN;
        const float bandWidth = totalWidth / numBands;

        // Set up text style
        nvgFontSize(vg, 9.0f);
        nvgFontFaceId(vg, 0);
        nvgFillColor(vg, nvgRGB(180, 180, 180));  // Light gray color
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);

        for (size_t b = 0; b < numBands; b++) {
            float xCenter = VFDConfig::BAND_MARGIN + (b + 0.5f) * bandWidth;
            float yPos = box.size.y - VFDConfig::LABEL_HEIGHT;  // Position in label area

            // Format frequency label
            float freq = VFDConfig::BAND_CENTERS[b];
            char label[16];
            if (freq >= 1000.0f) {
                snprintf(label, sizeof(label), "%.0fk", freq / 1000.0f);
            } else {
                snprintf(label, sizeof(label), "%.0f", freq);
            }

            nvgText(vg, xCenter, yPos, label, NULL);
        }
    }
};

struct VFDSlider : ui::Slider {
    VFDSlider(ParamQuantity* qnt) {
        quantity = qnt;
        box.size.x = 200.0f;
    }
    ~VFDSlider() {}
};

// ============================================================================
//  Module Widget
// ============================================================================
struct SpectrumWidget : ModuleWidget {
    SpectrumWidget(Spectrum* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/VFDFreqAnalyzer.svg"),
                             asset::plugin(pluginInstance, "res/VFDFreqAnalyzer-dark.svg")));

        addDisplay(module);
        addInputs(module);
    }

    void addDisplay(Spectrum* module) {
        VFDCustomDisplay* display = new VFDCustomDisplay();
        display->module = module;
        display->box.pos = Vec(VFDConfig::DISPLAY_X_OFFSET, VFDConfig::DISPLAY_Y_OFFSET);
        display->box.size = Vec(VFDConfig::DISPLAY_WIDTH, VFDConfig::DISPLAY_HEIGHT);
        addChild(display);
    }

    void addInputs(Spectrum* module) {
        const float jackY = VFDConfig::DISPLAY_Y_OFFSET + VFDConfig::DISPLAY_HEIGHT + 18.f;

        addInput(createInputCentered<ThemedPJ301MPort>(Vec(12.f, jackY), module, Spectrum::IN_L_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(12.f + 35.f, jackY), module, Spectrum::IN_R_INPUT));
    }

    void appendContextMenu(Menu* menu) override {
        Spectrum* module = dynamic_cast<Spectrum*>(this->module);
        if (!module) return;

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem(
            "Display Mode",
            {"Dots", "Bars"},
            [=]() { return static_cast<size_t>(module->displayMode); },
            [=](size_t index) { module->displayMode = static_cast<DisplayMode>(index); }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Other visual"));

        menu->addChild(createIndexSubmenuItem(
            "Intensity Mode",
            {"Solid", "Alpha", "Glow", "Ghost", "Clean"},
            [=]() { return static_cast<size_t>(module->intensityMode); },
            [=](size_t index) { module->intensityMode = static_cast<IntensityMode>(index); }));

        menu->addChild(createCheckMenuItem(
            "Show Labels", "", [=]() { return module->showLabels; },
            [=]() { module->showLabels = !module->showLabels; }));

        menu->addChild(createCheckMenuItem(
            "Show Unlit Segments", "", [=]() { return module->showUnlitSegments; },
            [=]() { module->showUnlitSegments = !module->showUnlitSegments; }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem(
            "Theme",
            {"Classic", "Warm", "Cool"},
            [=]() { return static_cast<size_t>(module->currentTheme); },
            [=](size_t index) { module->currentTheme = static_cast<Theme>(index); }));

        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::UPPER_PARAM)));
        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::LOWER_PARAM)));
        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::FALL_DELAY_PARAM)));
        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::PEAK_FALL_DELAY_PARAM)));
    }
};

Model* modelSpectrum = createModel<Spectrum, SpectrumWidget>("Spectrum");

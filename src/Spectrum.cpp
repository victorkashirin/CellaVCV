#include "plugin.hpp"
#include "spectrum/SpectrumAnalyzer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

// ============================================================================
//  Configuration & Constants
// ============================================================================
namespace VFDConfig {
// DSP Constants
static constexpr int NUM_BANDS = cella::spectrum::SpectrumConfig::NUM_BANDS;
static constexpr float MIN_DELAY_TIME = cella::spectrum::SpectrumConfig::MIN_DELAY_TIME;
static constexpr float DENORMAL_THRESHOLD = cella::spectrum::SpectrumConfig::DENORMAL_THRESHOLD;
static constexpr int NUM_AUDIO_CHANNELS = cella::spectrum::SpectrumConfig::NUM_AUDIO_CHANNELS;

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
static constexpr float STEREO_SPLIT_GAP = 1.5f;
static constexpr int MIN_COLUMNS = 3;
static constexpr int MAX_COLUMNS = 100;

// Display Constants - Bars Mode
static constexpr int BAR_SEGMENTS = 30;            // Number of segments in bar mode
static constexpr float BAR_SEGMENT_HEIGHT = 4.5f;  // Height of each bar segment

// Label Constants
static constexpr float LABEL_HEIGHT = 12.0f;  // Space reserved for frequency labels
static constexpr float LABEL_FONT_SIZE = 9.0f;

// Frequency band edges (Hz)
static const std::array<float, NUM_BANDS>& BAND_CENTERS = cella::spectrum::SpectrumConfig::BAND_CENTERS;
}  // namespace VFDConfig

// Display Modes
enum class DisplayMode { DOTS, BARS, COUNT };

// Stereo Modes
enum class StereoMode { MONO, LEFT_RIGHT_SPLIT, COUNT };

// Intensity Modes
enum class IntensityMode { SOLID, ALPHA, GLOW, GHOST, CLEAN, COUNT };

// Theme System
enum class Theme { RED, ORANGE, AMBER, GREEN, LIGHT_BLUE, VINTAGE_BLUE, IVORY, COUNT };

struct ThemeColors {
    NVGcolor active;
    NVGcolor secondary;
    NVGcolor inactive;
    NVGcolor peak;
};

template <typename T>
static inline T clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Predefined themes
static const std::array<ThemeColors, static_cast<size_t>(Theme::COUNT)> THEMES = {{
    // RED
    {nvgRGB(0xFF, 0x30, 0x38), nvgRGB(0xFF, 0x8A, 0x66), nvgRGB(0x26, 0x0A, 0x0C),
     nvgRGB(0xFF, 0xC2, 0x47)},
    // ORANGE
    {nvgRGB(0xFF, 0x6B, 0x18), nvgRGB(0xFF, 0xC1, 0x5A), nvgRGB(0x2A, 0x14, 0x08),
     nvgRGB(0xFF, 0xE6, 0xA6)},
    // AMBER
    {nvgRGB(0xFF, 0xD2, 0x4A), nvgRGB(0xFF, 0x9F, 0x2F), nvgRGB(0x2A, 0x1A, 0x0A),
     nvgRGB(0xFF, 0x5A, 0x36)},
    // GREEN
    {nvgRGB(0x47, 0xFF, 0x87), nvgRGB(0x8F, 0xB8, 0xFF), nvgRGB(0x10, 0x2A, 0x1A),
     nvgRGB(0xFF, 0x87, 0x47)},
    // LIGHT BLUE
    {nvgRGB(0x93, 0xEA, 0xFF), nvgRGB(0x5B, 0x8C, 0xFF), nvgRGB(0x20, 0x20, 0x20),
     nvgRGB(0xFF, 0x30, 0x30)},
    // VINTAGE BLUE
    {nvgRGB(0x6F, 0x9F, 0xD8), nvgRGB(0xB4, 0xC4, 0xDE), nvgRGB(0x10, 0x18, 0x24),
     nvgRGB(0xFF, 0xE0, 0xA3)},
    // IVORY
    {nvgRGB(0xFF, 0xE0, 0xA3), nvgRGB(0xB9, 0xD6, 0xC2), nvgRGB(0x24, 0x1F, 0x18),
     nvgRGB(0xFF, 0x70, 0x43)},
}};

// Theme-based color functions
static size_t getThemeIndex(Theme theme) {
    return static_cast<size_t>(clamp(static_cast<int>(theme), 0, static_cast<int>(Theme::COUNT) - 1));
}

static NVGcolor getActiveColor(Theme theme) { return THEMES[getThemeIndex(theme)].active; }

static NVGcolor getSecondaryColor(Theme theme) { return THEMES[getThemeIndex(theme)].secondary; }

static NVGcolor getInactiveColor(Theme theme) { return THEMES[getThemeIndex(theme)].inactive; }

static NVGcolor getPeakColor(Theme theme) { return THEMES[getThemeIndex(theme)].peak; }

static int getClampedJsonEnumIndex(json_t* rootJ, const char* key, int count, int fallback) {
    json_t* valueJ = json_object_get(rootJ, key);
    if (!json_is_integer(valueJ)) {
        return fallback;
    }
    return clamp(static_cast<int>(json_integer_value(valueJ)), 0, count - 1);
}

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
    cella::spectrum::SpectrumAnalyzer analyzer;
    dsp::RingBuffer<cella::spectrum::SpectrumFrame, 16> displayFrames;

    // Display state
    DisplayMode displayMode = DisplayMode::DOTS;
    StereoMode stereoMode = StereoMode::MONO;
    IntensityMode intensityMode = IntensityMode::SOLID;
    bool showLabels = false;
    bool showUnlitSegments = true;
    Theme currentTheme = Theme::LIGHT_BLUE;

    Spectrum() {
        config(NUM_PARAMS, NUM_INPUTS, 0, 0);
        configureParameters();
        configureInputs();
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

    void process(const ProcessArgs& args) override {
        const bool frameReady = analyzer.process(inputs[IN_L_INPUT].getVoltage(), inputs[IN_R_INPUT].getVoltage(),
                                                 inputs[IN_L_INPUT].isConnected(), inputs[IN_R_INPUT].isConnected(),
                                                 args.sampleRate, params[FALL_DELAY_PARAM].getValue());
        if (frameReady && !displayFrames.full()) {
            displayFrames.push(analyzer.getFrame());
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "displayMode", json_integer(static_cast<int>(displayMode)));
        json_object_set_new(rootJ, "stereoMode", json_integer(static_cast<int>(stereoMode)));
        json_object_set_new(rootJ, "intensityMode", json_integer(static_cast<int>(intensityMode)));
        json_object_set_new(rootJ, "showLabels", json_boolean(showLabels));
        json_object_set_new(rootJ, "showUnlitSegments", json_boolean(showUnlitSegments));
        json_object_set_new(rootJ, "currentTheme", json_integer(static_cast<int>(currentTheme)));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* displayModeJ = json_object_get(rootJ, "displayMode");
        if (displayModeJ) {
            displayMode =
                static_cast<DisplayMode>(getClampedJsonEnumIndex(rootJ, "displayMode", static_cast<int>(DisplayMode::COUNT),
                                                                 static_cast<int>(displayMode)));
        }

        json_t* stereoModeJ = json_object_get(rootJ, "stereoMode");
        if (stereoModeJ) {
            stereoMode =
                static_cast<StereoMode>(getClampedJsonEnumIndex(rootJ, "stereoMode", static_cast<int>(StereoMode::COUNT),
                                                                static_cast<int>(stereoMode)));
        }

        json_t* intensityModeJ = json_object_get(rootJ, "intensityMode");
        if (intensityModeJ) {
            intensityMode = static_cast<IntensityMode>(getClampedJsonEnumIndex(
                rootJ, "intensityMode", static_cast<int>(IntensityMode::COUNT), static_cast<int>(intensityMode)));
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
            currentTheme =
                static_cast<Theme>(getClampedJsonEnumIndex(rootJ, "currentTheme", static_cast<int>(Theme::COUNT),
                                                           static_cast<int>(currentTheme)));
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
    cella::spectrum::SpectrumFrame latestFrame;
    std::array<float, VFDConfig::NUM_BANDS> ghostLevels = {};
    std::array<std::array<float, VFDConfig::NUM_BANDS>, VFDConfig::NUM_AUDIO_CHANNELS> channelGhostLevels = {};
    std::chrono::steady_clock::time_point lastPeakUpdate = std::chrono::steady_clock::now();
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

        while (!module->displayFrames.empty()) {
            cella::spectrum::SpectrumFrame nextFrame = module->displayFrames.shift();
            nextFrame.peaks = latestFrame.peaks;
            nextFrame.channelPeaks = latestFrame.channelPeaks;
            latestFrame = nextFrame;
        }

        nvgSave(args.vg);
        nvgScissor(args.vg, 0.0f, 0.0f, box.size.x, getBandClipBottomY());
        drawBands(args.vg);
        nvgRestore(args.vg);

        if (module->showLabels) {
            nvgSave(args.vg);
            drawFrequencyLabels(args.vg);
            nvgRestore(args.vg);
        }
    }

    void drawBands(NVGcontext* vg) {
        size_t numBands = VFDConfig::NUM_BANDS;
        const float totalWidth = box.size.x - 2 * VFDConfig::BAND_MARGIN;
        const float bandWidth = totalWidth / numBands;

        DisplayRange range(module->params[0].getValue(), module->params[1].getValue());
        updatePeakLevels(range);
        updateGhostLevels(range);

        for (size_t b = 0; b < numBands; b++) {
            drawSingleBand(vg, b, bandWidth, range);
        }
    }

    void drawSingleBand(NVGcontext* vg, size_t bandIndex, float bandWidth, const DisplayRange& range) {
        float xOffset = VFDConfig::BAND_MARGIN + bandIndex * bandWidth;
        float availableWidth = bandWidth - VFDConfig::BAND_MARGIN * 2;

        if (module->stereoMode == StereoMode::LEFT_RIGHT_SPLIT) {
            drawStereoSplitBand(vg, bandIndex, xOffset, availableWidth, range);
            return;
        }

        float level = range.normalizeDb(latestFrame.levels[bandIndex]);
        float peakLevel = latestFrame.peaks[bandIndex];
        float ghostLevel = ghostLevels[bandIndex];
        drawBandMeter(vg, level, peakLevel, ghostLevel, xOffset, availableWidth, getActiveColor(module->currentTheme));
    }

    void drawStereoSplitBand(NVGcontext* vg, size_t bandIndex, float xOffset, float availableWidth,
                             const DisplayRange& range) {
        float channelWidth = std::max((availableWidth - VFDConfig::STEREO_SPLIT_GAP) * 0.5f, 0.0f);

        for (int channel = 0; channel < VFDConfig::NUM_AUDIO_CHANNELS; ++channel) {
            float level = range.normalizeDb(latestFrame.channelLevels[channel][bandIndex]);
            float peakLevel = latestFrame.channelPeaks[channel][bandIndex];
            float ghostLevel = channelGhostLevels[channel][bandIndex];
            float channelXOffset = xOffset + channel * (channelWidth + VFDConfig::STEREO_SPLIT_GAP);
            NVGcolor activeColor =
                channel == 0 ? getActiveColor(module->currentTheme) : getSecondaryColor(module->currentTheme);

            drawBandMeter(vg, level, peakLevel, ghostLevel, channelXOffset, channelWidth, activeColor);
        }
    }

    void drawBandMeter(NVGcontext* vg, float level, float peakLevel, float ghostLevel, float xOffset,
                       float availableWidth, NVGcolor activeColor) {
        if (module->displayMode == DisplayMode::DOTS) {
            drawDotsMode(vg, level, peakLevel, ghostLevel, xOffset, availableWidth, activeColor);
        } else {
            drawBarsMode(vg, level, peakLevel, ghostLevel, xOffset, availableWidth, activeColor);
        }
    }

    void updateGhostLevels(const DisplayRange& range) {
        auto now = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastGhostUpdate).count();
        lastGhostUpdate = now;

        deltaTime = clamp(deltaTime, 0.0f, 0.1f);
        float ghostDecay = std::exp(-deltaTime / 0.9f);

        for (size_t b = 0; b < VFDConfig::NUM_BANDS; ++b) {
            float level = range.normalizeDb(latestFrame.levels[b]);
            updateGhostLevel(ghostLevels[b], level, ghostDecay);

            for (int channel = 0; channel < VFDConfig::NUM_AUDIO_CHANNELS; ++channel) {
                float channelLevel = range.normalizeDb(latestFrame.channelLevels[channel][b]);
                updateGhostLevel(channelGhostLevels[channel][b], channelLevel, ghostDecay);
            }
        }
    }

    void updatePeakLevels(const DisplayRange& range) {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const float deltaTime = std::chrono::duration<float>(now - lastPeakUpdate).count();
        lastPeakUpdate = now;
        const float delay = std::max(VFDConfig::MIN_DELAY_TIME,
                                     module->params[Spectrum::PEAK_FALL_DELAY_PARAM].getValue());
        const float peakDecay = std::exp(-deltaTime / delay);

        for (int band = 0; band < VFDConfig::NUM_BANDS; ++band) {
            updatePeakLevel(latestFrame.peaks[band], range.normalizeDb(latestFrame.levels[band]), peakDecay);
            for (int channel = 0; channel < VFDConfig::NUM_AUDIO_CHANNELS; ++channel) {
                updatePeakLevel(latestFrame.channelPeaks[channel][band],
                                range.normalizeDb(latestFrame.channelLevels[channel][band]), peakDecay);
            }
        }
    }

    void updatePeakLevel(float& peakLevel, float level, float peakDecay) {
        if (level >= peakLevel) {
            peakLevel = level;
        } else {
            peakLevel *= peakDecay;
            if (peakLevel < VFDConfig::DENORMAL_THRESHOLD) {
                peakLevel = 0.0f;
            }
        }
        peakLevel = clamp(peakLevel, 0.0f, 1.0f);
    }

    void updateGhostLevel(float& ghostLevel, float level, float ghostDecay) {
        if (level >= ghostLevel) {
            ghostLevel = level;
        } else {
            ghostLevel *= ghostDecay;
            if (ghostLevel < VFDConfig::DENORMAL_THRESHOLD) {
                ghostLevel = 0.0f;
            }
        }
    }

    void drawDotsMode(NVGcontext* vg, float level, float peakLevel, float ghostLevel, float xOffset,
                      float availableWidth, NVGcolor activeColor) {
        DisplayGrid grid(availableWidth, getAvailableDisplayHeight(),
                         xOffset + VFDConfig::BAND_MARGIN + VFDConfig::DOT_RADIUS);

        IntensityStyle style = getIntensityStyle(level, peakLevel);
        int peakRow = peakLevel > 0.0f ? getPeakRow(grid, peakLevel, level) : -1;

        if (module->showUnlitSegments) {
            drawDotGrid(vg, grid, createAlphaColor(getInactiveColor(module->currentTheme), style.inactiveAlpha));
        }

        if (module->intensityMode == IntensityMode::GHOST && ghostLevel > level) {
            drawGhostDots(vg, grid, level, ghostLevel, peakRow, activeColor);
        }

        if (level > 0.0f) {
            drawActiveDots(vg, grid, level, style, peakRow, activeColor);
        }

        if (peakRow >= 0) {
            drawPeakIndicator(vg, grid, peakRow, style);
        }
    }

    void drawBarsMode(NVGcontext* vg, float level, float peakLevel, float ghostLevel, float xOffset,
                      float availableWidth, NVGcolor activeColor) {
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

        int peakSegment = peakLevel > 0.0f ? getPeakSegment(numSegments, peakLevel, level) : -1;

        if (module->showUnlitSegments) {
            drawBarScaffold(vg, barX, barWidth, startY, numSegments, segmentHeight, segmentSpacing,
                            style.inactiveAlpha);
        }

        if (module->intensityMode == IntensityMode::GHOST && ghostLevel > level) {
            drawGhostBars(vg, barX, barWidth, startY, numSegments, segmentHeight, segmentSpacing, level, ghostLevel,
                          peakSegment, activeColor);
        }

        // Draw active segments
        if (level > 0.0f) {
            int activeSegments = static_cast<int>(std::ceil(level * numSegments));

            for (int i = 0; i < activeSegments && i < numSegments; i++) {
                int segmentIndex = numSegments - 1 - i;  // Start from bottom
                if (segmentIndex == peakSegment) {
                    continue;
                }
                float segY = startY + segmentIndex * (segmentHeight + segmentSpacing);

                drawBarSegmentWithIntensity(vg, barX, segY, barWidth, segmentHeight, activeColor, style.levelAlpha,
                                            activeBarGlowAlpha);
            }
        }

        // Draw peak segment
        if (peakSegment >= 0) {
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

    int getActiveRows(const DisplayGrid& grid, float level) {
        return clamp(static_cast<int>(std::ceil(level * grid.rows)), 0, grid.rows);
    }

    int getPeakRow(const DisplayGrid& grid, float peakLevel, float level) {
        int peakRow = clamp(grid.rows - static_cast<int>(std::ceil(peakLevel * grid.rows)), 0, grid.rows - 1);
        int activeRows = getActiveRows(grid, level);
        if (activeRows > 0) {
            peakRow = std::min(peakRow, grid.rows - activeRows);
        }
        return peakRow;
    }

    int getPeakSegment(int numSegments, float peakLevel, float level) {
        int peakSegment = numSegments - 1 - static_cast<int>(std::floor(peakLevel * numSegments));
        peakSegment = clamp(peakSegment, 0, numSegments - 1);

        int activeSegments = clamp(static_cast<int>(std::ceil(level * numSegments)), 0, numSegments);
        if (activeSegments > 0) {
            peakSegment = std::min(peakSegment, numSegments - activeSegments);
        }
        return peakSegment;
    }

    void drawGhostDots(NVGcontext* vg, const DisplayGrid& grid, float level, float ghostLevel, int skipRow,
                       NVGcolor activeColor) {
        int activeRows = getActiveRows(grid, level);
        int ghostRows = clamp(static_cast<int>(std::ceil(ghostLevel * grid.rows)), 0, grid.rows);
        int firstGhostRow = grid.rows - ghostRows;
        int firstActiveRow = grid.rows - activeRows;
        int trailRows = std::max(firstActiveRow - firstGhostRow, 1);

        for (int c = 0; c < grid.columns; c++) {
            for (int r = firstGhostRow; r < firstActiveRow; r++) {
                if (r == skipRow) {
                    continue;
                }
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
                       float segmentSpacing, float level, float ghostLevel, int skipSegment, NVGcolor activeColor) {
        int activeSegments = clamp(static_cast<int>(std::ceil(level * numSegments)), 0, numSegments);
        int ghostSegments = clamp(static_cast<int>(std::ceil(ghostLevel * numSegments)), 0, numSegments);
        int trailSegments = std::max(ghostSegments - activeSegments, 1);

        for (int i = activeSegments; i < ghostSegments; i++) {
            int segmentIndex = numSegments - 1 - i;
            if (segmentIndex == skipSegment) {
                continue;
            }
            float segY = startY + segmentIndex * (segmentHeight + segmentSpacing);
            float segmentFade = static_cast<float>(ghostSegments - i) / trailSegments;
            drawBarSegment(vg, x, segY, width, segmentHeight,
                           createAlphaColor(activeColor, 0.22f + 0.28f * segmentFade));
        }
    }

    void drawActiveDots(NVGcontext* vg, const DisplayGrid& grid, float level, const IntensityStyle& style, int skipRow,
                        NVGcolor activeColor) {
        int activeRows = getActiveRows(grid, level);
        int firstActiveRow = grid.rows - activeRows;

        for (int c = 0; c < grid.columns; c++) {
            for (int r = firstActiveRow; r < grid.rows; r++) {
                if (r == skipRow) {
                    continue;
                }
                float dy = grid.yStart + r * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);
                float dx = grid.xStart + c * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);
                drawActiveDot(vg, dx, dy, style, activeColor);
            }
        }
    }

    void drawActiveDot(NVGcontext* vg, float x, float y, const IntensityStyle& style, NVGcolor activeColor) {
        if (!module) return;

        drawDotGlow(vg, x, y, activeColor, style.activeGlowRadius, style.activeGlowAlpha);

        // Core dot
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, VFDConfig::DOT_RADIUS);
        nvgFillColor(vg, createAlphaColor(activeColor, style.levelAlpha));
        nvgFill(vg);
    }

    void drawPeakIndicator(NVGcontext* vg, const DisplayGrid& grid, int peakRow, const IntensityStyle& style) {
        NVGcolor peakColor = getPeakColor(module->currentTheme);
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

    float getLabelsTopY() const { return box.size.y - VFDConfig::LABEL_HEIGHT; }

    float getLabelsBottomY() const { return getLabelsTopY() + VFDConfig::LABEL_FONT_SIZE; }

    float getBandClipBottomY() const { return module && module->showLabels ? getLabelsTopY() : getLabelsBottomY(); }

    // Helper function to get available display height (accounting for labels)
    float getAvailableDisplayHeight() const {
        return std::max(getBandClipBottomY() - 3 * VFDConfig::BAND_MARGIN, 0.0f);
    }

    void drawFrequencyLabels(NVGcontext* vg) {
        size_t numBands = VFDConfig::NUM_BANDS;
        const float totalWidth = box.size.x - 2 * VFDConfig::BAND_MARGIN;
        const float bandWidth = totalWidth / numBands;

        // Set up text style
        nvgFontSize(vg, VFDConfig::LABEL_FONT_SIZE);
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
            "Stereo View",
            {"Mono Energy", "L/R Split"},
            [=]() { return static_cast<size_t>(module->stereoMode); },
            [=](size_t index) { module->stereoMode = static_cast<StereoMode>(index); }));

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
            {"Red", "Orange", "Amber", "Green", "Light Blue", "Vintage Blue", "Ivory"},
            [=]() { return static_cast<size_t>(module->currentTheme); },
            [=](size_t index) { module->currentTheme = static_cast<Theme>(index); }));

        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::UPPER_PARAM)));
        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::LOWER_PARAM)));
        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::FALL_DELAY_PARAM)));
        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::PEAK_FALL_DELAY_PARAM)));
    }
};

Model* modelSpectrum = createModel<Spectrum, SpectrumWidget>("Spectrum");

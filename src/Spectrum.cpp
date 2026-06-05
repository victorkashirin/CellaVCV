#include "plugin.hpp"

// ============================================================================
//  Configuration & Constants
// ============================================================================
namespace VFDConfig {
// DSP Constants
static constexpr int FFT_SIZE = 2048;
static constexpr int NUM_BANDS = 12;
static constexpr float INPUT_GAIN = 0.1f;
static constexpr float MIN_DELAY_TIME = 0.001f;
static constexpr float NOISE_FLOOR_DB = -120.0f;
static constexpr float DENORMAL_THRESHOLD = 1e-6f;

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
    float dbLevel = VFDConfig::NOISE_FLOOR_DB;
    float peakLevel = 0.0f;

    void updateLevel(float newDb, float fallDecay) {
        if (newDb >= dbLevel) {
            dbLevel = newDb;  // Fast attack
        } else {
            dbLevel = dbLevel * fallDecay + newDb * (1.0f - fallDecay);
        }

        // Prevent excessive decay
        if (dbLevel < VFDConfig::NOISE_FLOOR_DB) {
            dbLevel = VFDConfig::NOISE_FLOOR_DB;
        }
    }

    void updatePeak(float currentNormalized, float peakDecay) {
        if (currentNormalized >= peakLevel) {
            peakLevel = currentNormalized;
        } else {
            peakLevel *= peakDecay;
            if (peakLevel < VFDConfig::DENORMAL_THRESHOLD) {
                peakLevel = 0.0f;
            }
        }
        peakLevel = clamp(peakLevel, 0.0f, 1.0f);
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
    std::vector<float> capture;
    std::array<SpectrumBand, VFDConfig::NUM_BANDS> bands;
    int writePos = 0;

    // Display state
    DisplayMode displayMode = DisplayMode::DOTS;
    bool alphaMode = false;
    bool showLabels = false;
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
        capture.resize(VFDConfig::FFT_SIZE);

        // Generate Hann window
        for (int i = 0; i < VFDConfig::FFT_SIZE; ++i) {
            window[i] = 0.5f * (1.f - std::cos(2.f * M_PI * i / (VFDConfig::FFT_SIZE - 1)));
        }
    }

    void process(const ProcessArgs& args) override {
        float inputSignal = getInputSignal();
        processWindowedInput(inputSignal);
        updateDecayAndPeaks(args);
        // handleNoInputDecay(args);
    }

    float getInputSignal() {
        float in = 0.f;
        bool lCon = inputs[IN_L_INPUT].isConnected();
        bool rCon = inputs[IN_R_INPUT].isConnected();

        if (lCon) in += inputs[IN_L_INPUT].getVoltage();
        if (rCon) in += inputs[IN_R_INPUT].getVoltage();
        if (lCon && rCon) in *= 0.5f;

        return in * VFDConfig::INPUT_GAIN;
    }

    void processWindowedInput(float input) {
        capture[writePos] = input * window[writePos];
        if (++writePos >= VFDConfig::FFT_SIZE) {
            writePos = 0;
            analyzeFFT();
        }
    }

    void updateDecayAndPeaks(const ProcessArgs& args) {
        DisplayRange range(params[UPPER_PARAM].getValue(), params[LOWER_PARAM].getValue());
        float peakDecay = calculateDecay(args.sampleTime, params[PEAK_FALL_DELAY_PARAM].getValue());

        for (auto& band : bands) {
            float currentNormalized = range.normalizeDb(band.dbLevel);
            band.updatePeak(currentNormalized, peakDecay);
        }
    }

    void handleNoInputDecay(const ProcessArgs& args) {
        if (!inputs[IN_L_INPUT].isConnected() && !inputs[IN_R_INPUT].isConnected()) {
            float fallDecay = calculateDecay(args.sampleTime, params[FALL_DELAY_PARAM].getValue());

            for (auto& band : bands) {
                band.dbLevel *= fallDecay;
                band.dbLevel = std::max(band.dbLevel, VFDConfig::NOISE_FLOOR_DB);
            }
        }
    }

    float calculateDecay(float deltaTime, float delayParam) {
        float delay = std::max(VFDConfig::MIN_DELAY_TIME, delayParam);
        return std::exp(-deltaTime / delay);
    }

    void analyzeFFT() {
        auto fftResult = performFFT();
        std::vector<float> magnitudes = fftResult.first;
        int specBins = fftResult.second;
        auto frequencyEdges = getFrequencyEdges();
        float fallDecay = calculateFallDecay();

        updateBandLevels(magnitudes, frequencyEdges, specBins, fallDecay);
    }

    std::pair<std::vector<float>, int> performFFT() {
        std::vector<float> input(capture);
        std::vector<float> output(VFDConfig::FFT_SIZE);

        fft.rfft(input.data(), output.data());
        fft.scale(output.data());

        const int specBins = VFDConfig::FFT_SIZE / 2 + 1;
        std::vector<float> magnitudes(specBins);

        magnitudes[0] = std::fabs(output[0]);
        magnitudes[VFDConfig::FFT_SIZE / 2] = std::fabs(output[1]);

        for (int k = 1; k < VFDConfig::FFT_SIZE / 2; ++k) {
            float re = output[2 * k];
            float im = output[2 * k + 1];
            magnitudes[k] = std::hypot(re, im);
        }

        return {magnitudes, specBins};
    }

    std::array<float, VFDConfig::NUM_BANDS + 1> getFrequencyEdges() {
        std::array<float, VFDConfig::NUM_BANDS + 1> edges;

        // First edge: geometric mean between first center and a reasonable low
        // frequency
        edges[0] = std::sqrt(VFDConfig::BAND_CENTERS[0] * (VFDConfig::BAND_CENTERS[0] / 2.0f));

        // Middle edges: geometric mean between adjacent centers
        for (int i = 1; i < VFDConfig::NUM_BANDS; ++i) {
            edges[i] = std::sqrt(VFDConfig::BAND_CENTERS[i - 1] * VFDConfig::BAND_CENTERS[i]);
        }

        // Last edge: Nyquist frequency
        edges[VFDConfig::NUM_BANDS] = APP->engine->getSampleRate() * 0.5f;

        return edges;
    }

    float calculateFallDecay() {
        float fallDelay = std::max(VFDConfig::MIN_DELAY_TIME, params[FALL_DELAY_PARAM].getValue());
        float deltaTime = static_cast<float>(VFDConfig::FFT_SIZE) / APP->engine->getSampleRate();
        return std::exp(-deltaTime / fallDelay);
    }

    void updateBandLevels(const std::vector<float>& magnitudes,
                          const std::array<float, VFDConfig::NUM_BANDS + 1>& edges, int specBins, float fallDecay) {
        float sampleRate = APP->engine->getSampleRate();

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

        float sum = 0.f;
        for (int k = binLo; k < binHi; ++k) {
            sum += magnitudes[k];
        }
        return sum / (binHi - binLo);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "displayMode", json_integer(static_cast<int>(displayMode)));
        json_object_set_new(rootJ, "alphaMode", json_boolean(alphaMode));
        json_object_set_new(rootJ, "showLabels", json_boolean(showLabels));
        json_object_set_new(rootJ, "currentTheme", json_integer(static_cast<int>(currentTheme)));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* displayModeJ = json_object_get(rootJ, "displayMode");
        if (displayModeJ) {
            displayMode = static_cast<DisplayMode>(json_integer_value(displayModeJ));
        }

        json_t* alphaModeJ = json_object_get(rootJ, "alphaMode");
        if (alphaModeJ) {
            alphaMode = json_boolean_value(alphaModeJ);
        }

        json_t* showLabelsJ = json_object_get(rootJ, "showLabels");
        if (showLabelsJ) {
            showLabels = json_boolean_value(showLabelsJ);
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

        for (size_t b = 0; b < numBands; b++) {
            drawSingleBand(vg, b, bandWidth, range);
        }
    }

    void drawSingleBand(NVGcontext* vg, size_t bandIndex, float bandWidth, const DisplayRange& range) {
        float level = range.normalizeDb(module->bands[bandIndex].dbLevel);
        float peakLevel = module->bands[bandIndex].peakLevel;

        float xOffset = VFDConfig::BAND_MARGIN + bandIndex * bandWidth;
        float availableWidth = bandWidth - VFDConfig::BAND_MARGIN * 2;

        if (module->displayMode == DisplayMode::DOTS) {
            drawDotsMode(vg, level, peakLevel, xOffset, availableWidth);
        } else {
            drawBarsMode(vg, level, peakLevel, xOffset, availableWidth);
        }
    }

    void drawDotsMode(NVGcontext* vg, float level, float peakLevel, float xOffset, float availableWidth) {
        DisplayGrid grid(availableWidth, getAvailableDisplayHeight(),
                         xOffset + VFDConfig::BAND_MARGIN + VFDConfig::DOT_RADIUS);

        // Calculate alpha for this band
        float levelAlpha = calculateAlpha(level);
        float peakAlpha = calculateAlpha(peakLevel);

        // Draw inactive dots with normal color (not affected by level)
        drawDotGrid(vg, grid, getInactiveColor(module->currentTheme));

        if (level > 0.0f) {
            drawActiveDots(vg, grid, level, levelAlpha);
        }

        if (peakLevel > 0.0f) {
            drawPeakIndicator(vg, grid, peakLevel, peakAlpha);
        }
    }

    void drawBarsMode(NVGcontext* vg, float level, float peakLevel, float xOffset, float availableWidth) {
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

        // Calculate alpha for this band
        float levelAlpha = calculateAlpha(level);
        float peakAlpha = calculateAlpha(peakLevel);

        // Draw all segments (inactive background) with normal color (not affected
        // by level) nvgFillColor(vg, VFDConfig::INACTIVE_COLOR); for (int i = 0; i
        // < numSegments; i++) {
        //     float segY = startY + i * (segmentHeight + segmentSpacing);
        //     nvgBeginPath(vg);
        //     nvgRect(vg, barX, segY, barWidth, segmentHeight);
        //     nvgFill(vg);
        // }

        // Draw active segments
        if (level > 0.0f) {
            int activeSegments = static_cast<int>(std::ceil(level * numSegments));

            for (int i = 0; i < activeSegments && i < numSegments; i++) {
                int segmentIndex = numSegments - 1 - i;  // Start from bottom
                float segY = startY + segmentIndex * (segmentHeight + segmentSpacing);

                // Core active segment with alpha
                nvgBeginPath(vg);
                nvgRect(vg, barX, segY, barWidth, segmentHeight);
                nvgFillColor(vg, createAlphaColor(getActiveColor(module->currentTheme), levelAlpha));
                nvgFill(vg);
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
            nvgBeginPath(vg);
            nvgRect(vg, barX, segY, barWidth, segmentHeight);
            nvgFillColor(vg, createAlphaColor(getPeakColor(module->currentTheme), peakAlpha));
            nvgFill(vg);
        }
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

    void drawActiveDots(NVGcontext* vg, const DisplayGrid& grid, float level, float levelAlpha) {
        float activeHeight = getAvailableDisplayHeight() * level;

        for (int c = 0; c < grid.columns; c++) {
            for (int r = 0; r < grid.rows; r++) {
                float dy = grid.yStart + r * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);

                if (grid.yStart + grid.gridHeight - dy <= activeHeight) {
                    float dx = grid.xStart + c * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);
                    drawActiveDot(vg, dx, dy, levelAlpha);
                }
            }
        }
    }

    void drawActiveDot(NVGcontext* vg, float x, float y, float levelAlpha) {
        if (!module) return;

        // Glow effect with alpha
        NVGcolor activeColor = getActiveColor(module->currentTheme);
        NVGpaint paint = nvgRadialGradient(vg, x, y, 0.0f, VFDConfig::DOT_RADIUS * 3,
                                           nvgTransRGBA(activeColor, static_cast<unsigned char>(levelAlpha * 100)),
                                           nvgTransRGBA(activeColor, 0.0f));
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, 3 * VFDConfig::DOT_RADIUS);
        nvgFillPaint(vg, paint);
        nvgFill(vg);

        // Core dot
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, VFDConfig::DOT_RADIUS);
        nvgFillColor(vg, createAlphaColor(activeColor, levelAlpha));
        nvgFill(vg);
    }

    void drawPeakIndicator(NVGcontext* vg, const DisplayGrid& grid, float peakLevel, float peakAlpha) {
        nvgFillColor(vg, createAlphaColor(getPeakColor(module->currentTheme), peakAlpha));
        int peakRow = grid.rows - ceil(peakLevel * grid.rows);
        float dy = grid.yStart + peakRow * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);

        for (int c = 0; c < grid.columns; c++) {
            float dx = grid.xStart + c * (VFDConfig::DOT_RADIUS * 2 + VFDConfig::DOT_SPACING);
            nvgBeginPath(vg);
            nvgCircle(vg, dx, dy, VFDConfig::DOT_RADIUS);
            nvgFill(vg);
        }
    }

    // Helper function to calculate alpha based on magnitude and alpha mode
    float calculateAlpha(float normalizedLevel) {
        if (!module || !module->alphaMode) {
            return 1.0f;  // Full opacity when alpha mode is off
        }

        // In alpha mode: 0 magnitude = 100% transparency, max magnitude = 0%
        // transparency So alpha = normalizedLevel (0.0 = fully transparent, 1.0 =
        // fully opaque)
        return sqrt(clamp(normalizedLevel, 0.0f, 1.0f));
    }

    // Helper function to create alpha-adjusted color
    NVGcolor createAlphaColor(NVGcolor baseColor, float alpha) {
        return nvgTransRGBA(baseColor, static_cast<unsigned char>(alpha * 255));
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
        menu->addChild(createMenuLabel("Display Mode"));

        menu->addChild(createCheckMenuItem(
            "Dots", "", [=]() { return module->displayMode == DisplayMode::DOTS; },
            [=]() { module->displayMode = DisplayMode::DOTS; }));

        menu->addChild(createCheckMenuItem(
            "Bars", "", [=]() { return module->displayMode == DisplayMode::BARS; },
            [=]() { module->displayMode = DisplayMode::BARS; }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Other visual"));

        menu->addChild(createCheckMenuItem(
            "Alpha Mode", "", [=]() { return module->alphaMode; }, [=]() { module->alphaMode = !module->alphaMode; }));

        menu->addChild(createCheckMenuItem(
            "Show Labels", "", [=]() { return module->showLabels; },
            [=]() { module->showLabels = !module->showLabels; }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Theme"));

        menu->addChild(createCheckMenuItem(
            "Classic", "", [=]() { return module->currentTheme == Theme::CLASSIC; },
            [=]() { module->currentTheme = Theme::CLASSIC; }));
        menu->addChild(createCheckMenuItem(
            "Warm", "", [=]() { return module->currentTheme == Theme::WARM; },
            [=]() { module->currentTheme = Theme::WARM; }));
        menu->addChild(createCheckMenuItem(
            "Cool", "", [=]() { return module->currentTheme == Theme::COOL; },
            [=]() { module->currentTheme = Theme::COOL; }));

        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::UPPER_PARAM)));
        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::LOWER_PARAM)));
        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::FALL_DELAY_PARAM)));
        menu->addChild(new VFDSlider(module->getParamQuantity(Spectrum::PEAK_FALL_DELAY_PARAM)));
    }
};

Model* modelSpectrum = createModel<Spectrum, SpectrumWidget>("Spectrum");

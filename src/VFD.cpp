// Vintage Spectrum Analyzer – single‑file VCV Rack 2.0 module  (dynamic‑range)
// MIT‑licensed example code – drop this file into your plugin's src/ folder as plugin.cpp
// -----------------------------------------------------------------------------
// CHANGE‑LOG 2025‑04‑29 (3rd rev)
// • Added *configurable* display window: two knobs set the **upper** (‑40…0 dB) and
//   **lower** (‑120…‑60 dB) limits. Default window is −30…0 dB as before.
// • Analyzer and renderer now respect those limits when clipping and normalising.
// • UI: two small trim‑knobs appear at the bottom of the module labelled "TOP" and "BOTTOM".
//
// Everything still lives in ONE file for easy drop‑in use.

#include "plugin.hpp"

template <typename T>
static inline T clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static constexpr std::array<const char*, 12> LABELS = {
    "32", "50", "80", "125", "200", "315", "500", "1k", "2k", "4k", "8k", "16k"};

// ============================================================================
//  Module
// ============================================================================
struct VintageSpectrumAnalyzer : Module {
    // ------------------------------------------------------------------
    // Enumerations
    // ------------------------------------------------------------------
    enum ParamIds {
        UPPER_PARAM,  // display ceiling (‑40…0 dB)
        LOWER_PARAM,  // display floor   (‑120…‑60 dB)
        FALL_DELAY_PARAM,      // fall delay time
        PEAK_FALL_DELAY_PARAM, // peak fall delay time
        NUM_PARAMS
    };
    enum InputIds { IN_L_INPUT,
                    IN_R_INPUT,
                    NUM_INPUTS };

    // ------------------------------------------------------------------
    // Constants & parameters
    // ------------------------------------------------------------------
    static constexpr int FFT_SIZE = 2048;  // 46 ms @ 44.1 kHz
    static constexpr int NUM_BANDS = 13;
    static constexpr float INPUT_GAIN = 0.1f;  // Scale ±10 V to roughly ±1 V‑peak

    // Mid‑frequency labels (geometric means of one‑third‑octave edges)

    // ------------------------------------------------------------------
    // DSP state
    // ------------------------------------------------------------------
    dsp::RealFFT fft{FFT_SIZE};
    std::vector<float> window;              // Hann window values
    std::vector<float> capture;             // Rolling capture buffer (windowed signal)
    std::array<float, NUM_BANDS> bandDb{};  // Bar heights in dB (raw, unclamped)
    std::array<float, NUM_BANDS> bandPeaks{}; // Peak levels for each band
    int writePos = 0;                       // Cursor into capture[]

    VintageSpectrumAnalyzer() {
        config(NUM_PARAMS, NUM_INPUTS, 0, 0);

        // Param ranges – users asked upper: ‑40…0 dB, lower: ‑120…‑60 dB
        configParam(UPPER_PARAM, -40.f, 0.f, 0.f, "Top", " dB");
        configParam(LOWER_PARAM, -120.f, -60.f, -30.f, "Bottom", " dB");
        configParam(FALL_DELAY_PARAM, 0.01f, 2.f, 0.5f, "Fall Delay", "s");
        configParam(PEAK_FALL_DELAY_PARAM, 0.01f, 3.f, 1.f, "Peak Fall Delay", "s");

        configInput(IN_L_INPUT, "Left");
        configInput(IN_R_INPUT, "Right");

        window.resize(FFT_SIZE);
        capture.resize(FFT_SIZE);
        for (int i = 0; i < FFT_SIZE; ++i) {
            window[i] = 0.5f * (1.f - std::cos(2.f * M_PI * i / (FFT_SIZE - 1)));
        }
    }

    // ------------------------------------------------------------------
    // Per‑sample processing – fills the window then triggers analysis
    // ------------------------------------------------------------------
    void process(const ProcessArgs& args) override {
        // 1) Mono sum + voltage‑range correction
        float in = 0.f;
        bool lCon = inputs[IN_L_INPUT].isConnected();
        bool rCon = inputs[IN_R_INPUT].isConnected();
        if (lCon) in += inputs[IN_L_INPUT].getVoltage();
        if (rCon) in += inputs[IN_R_INPUT].getVoltage();
        if (lCon && rCon)
            in *= 0.5f;
        in *= INPUT_GAIN;

        // 2) Window & write into circular buffer
        capture[writePos] = in * window[writePos];
        if (++writePos >= FFT_SIZE) {
            writePos = 0;
            analyzeFFT(args.sampleRate);
        }

        // 3) Apply exponential decay to peaks between FFT updates
        float deltaTime = args.sampleTime;
        float peakFallDelay = std::max(0.001f, params[PEAK_FALL_DELAY_PARAM].getValue());
        float peakDecay = std::exp(-deltaTime / peakFallDelay);

        for (int b = 0; b < NUM_BANDS; b++) {
            // Convert current bandDb to normalized range for comparison
            const float topDb = params[UPPER_PARAM].getValue();
            const float bottomDb = params[LOWER_PARAM].getValue();
            const float rangeDb = topDb - bottomDb;
            
            float currentDb = clamp(bandDb[b], bottomDb, topDb);
            float currentLevel = (currentDb - bottomDb) / rangeDb;
            
            // Update peak if current level is higher
            if (currentLevel >= bandPeaks[b]) {
                bandPeaks[b] = currentLevel;
            } else {
                // Apply decay to peak
                bandPeaks[b] *= peakDecay;
                if (bandPeaks[b] < 1e-6f) bandPeaks[b] = 0.f;
            }
            
            // Clamp peak to valid range
            bandPeaks[b] = clamp(bandPeaks[b], 0.f, 1.f);
        }

        // 4) Apply fall delay to main levels when no input
        if (!lCon && !rCon) {
            float fallDelay = std::max(0.001f, params[FALL_DELAY_PARAM].getValue());
            float fallDecay = std::exp(-deltaTime / fallDelay);
            
            for (int b = 0; b < NUM_BANDS; b++) {
                bandDb[b] *= fallDecay;
                // Prevent levels from going too negative
                bandDb[b] = std::max(bandDb[b], -120.0f);
            }
        }
    }

    // ------------------------------------------------------------------
    // FFT analysis (runs once per window)
    // ------------------------------------------------------------------
    void analyzeFFT(float sampleRate) {
        // Prepare input & output buffers for RealFFT
        std::vector<float> input(capture);    // copy current window
        std::vector<float> output(FFT_SIZE);  // RealFFT needs length elements

        // Forward FFT using canonical interleaved layout
        fft.rfft(input.data(), output.data());  // real → complex FFT
        fft.scale(output.data());               // normalise amplitude

        const int specBins = FFT_SIZE / 2 + 1;  // include Nyquist
        std::vector<float> mag(specBins);
        for (int k = 0; k < specBins; ++k) {
            float re = output[2 * k];
            float im = output[2 * k + 1];
            mag[k] = std::sqrt(re * re + im * im);
        }

        // Frequency edges that define the bars (Hz)
        const float edges[NUM_BANDS + 1] = {
            13.f, 25.f, 40.f, 63.f, 100.f, 160.f, 250.f,
            500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f,
            sampleRate * 0.5f  // Nyquist upper edge
        };

        // Decay parameters for level updates
        float fallDelay = std::max(0.001f, params[FALL_DELAY_PARAM].getValue());
        float deltaTime = (float)FFT_SIZE / sampleRate;  // Time between FFT calculations
        float fallDecay = std::exp(-deltaTime / fallDelay);

        for (int b = 0; b < NUM_BANDS; ++b) {
            float fLo = edges[b];
            float fHi = edges[b + 1];
            int binLo = clamp<int>(std::floor(fLo * FFT_SIZE / sampleRate), 0, specBins - 1);
            int binHi = clamp<int>(std::ceil(fHi * FFT_SIZE / sampleRate), binLo + 1, specBins);

            float sum = 0.f;
            for (int k = binLo; k < binHi; ++k)
                sum += mag[k];
            float avgMag = sum / (binHi - binLo);

            float newDb = 20.f * std::log10(avgMag + 1e-12f);

            // Update main level (fast attack, exponential decay)
            if (newDb >= bandDb[b]) {
                bandDb[b] = newDb;  // Fast attack
            } else {
                bandDb[b] = bandDb[b] * fallDecay + newDb * (1.0f - fallDecay);  // Smooth decay
            }
        }
    }
};

struct VFDCustomDisplay : LedDisplay {
    VintageSpectrumAnalyzer* module;
    const float dotRadius = 2.0f;
    const float dotSpacing = 2.0f;
    const NVGcolor activeColor = nvgRGB(0x93, 0xEA, 0xFF);
    const NVGcolor inactiveColor = nvgRGB(0x20, 0x20, 0x20);
    const NVGcolor peakColor = nvgRGB(0xFF, 0x30, 0x30);

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1 || !module) return;

        nvgSave(args.vg);

        // Draw dark background
        // nvgBeginPath(args.vg);
        // nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        // nvgFillColor(args.vg, nvgRGB(0x10, 0x10, 0x10));
        // nvgFill(args.vg);

        size_t numBands = VintageSpectrumAnalyzer::NUM_BANDS;
        if (numBands < 1) {
            nvgRestore(args.vg);
            return;
        }

        const float bandMargin = 3.0f;
        const float totalWidth = box.size.x - 2 * bandMargin;
        const float bandWidth = totalWidth / numBands;

        const float topDb = module->params[0].getValue();
        const float bottomDb = module->params[1].getValue();
        const float rangeDb = topDb - bottomDb;

        for (size_t b = 0; b < numBands; b++) {
            float db = module->bandDb[b];
            db = clamp(db, bottomDb, topDb);
            float norm = (db - bottomDb) / rangeDb;
            float level = norm;
            float peakNorm = module->bandPeaks[b]; // bandPeaks now stores normalized values directly
            float x = bandMargin + b * bandWidth;

            // Calculate dot grid dimensions
            float availableWidth = bandWidth - bandMargin * 2;
            int cols = calculateColumnCount(availableWidth);
            int rows = calculateRowCount(box.size.y - 3 * bandMargin);

            // Center the grid horizontally
            float gridWidth = cols * (dotRadius * 2 + dotSpacing) - dotSpacing;
            float xStart = x + (availableWidth - gridWidth) / 2 + bandMargin;

            // Vertical positioning
            float yStart = 2 * bandMargin + 1.5;
            float gridHeight = rows * (dotRadius * 2 + dotSpacing) - dotSpacing;
            float yStep = (box.size.y - bandMargin * 2 - gridHeight) / (rows - 1);

            // Draw inactive dots
            drawDotGrid(args.vg, xStart, yStart, cols, rows, inactiveColor);

            // Draw active dots
            if (level > 0.0f) {
                float activeHeight = box.size.y * level;
                drawActiveDots(args.vg, xStart, yStart, cols, rows, activeHeight);
            }

            // Draw peak indicator
            if (peakNorm > 0.0f) {
                nvgFillColor(args.vg, peakColor);
                float peakY = box.size.y - (box.size.y * peakNorm);
                int r = rows - ceil(peakNorm * rows);
                float dy = yStart + r * (dotRadius * 2 + dotSpacing);
                for (int c = 0; c < cols; c++) {
                    float dx = xStart + c * (dotRadius * 2 + dotSpacing);

                    nvgBeginPath(args.vg);
                    nvgCircle(args.vg, dx, dy, dotRadius);
                    nvgFill(args.vg);
                }
            }
        }

        nvgRestore(args.vg);
    }

   private:
    int calculateColumnCount(float availableWidth) {
        int cols = static_cast<int>((availableWidth + dotSpacing) /
                                    (dotRadius * 2 + dotSpacing));
        return clamp(cols, 3, 100);
    }

    int calculateRowCount(float availableHeight) {
        int rows = static_cast<int>((availableHeight + dotSpacing) /
                                    (dotRadius * 2 + dotSpacing));
        return rows;
    }

    void drawDotGrid(NVGcontext* vg, float x, float y, int cols, int rows, NVGcolor color) {
        nvgFillColor(vg, color);
        for (int c = 0; c < cols; c++) {
            for (int r = 0; r < rows; r++) {
                float dx = x + c * (dotRadius * 2 + dotSpacing);
                float dy = y + r * (dotRadius * 2 + dotSpacing);
                nvgBeginPath(vg);
                nvgCircle(vg, dx, dy, dotRadius);
                nvgFill(vg);
            }
        }
    }

    void drawActiveDots(NVGcontext* vg, float x, float y, int cols, int rows, float activeHeight) {
        nvgFillColor(vg, activeColor);
        for (int c = 0; c < cols; c++) {
            for (int r = 0; r < rows; r++) {
                float dy = y + r * (dotRadius * 2 + dotSpacing);
                if (box.size.y - dy <= activeHeight) {
                    float dx = x + c * (dotRadius * 2 + dotSpacing);

                    NVGpaint paint = nvgRadialGradient(vg, dx, dy, dotRadius * 0.f, dotRadius * 3, nvgTransRGBA(activeColor, 100), nvgTransRGBA(activeColor, 0.f));

                    nvgBeginPath(vg);
                    nvgCircle(vg, dx, dy, 3 * dotRadius);
                    nvgFillPaint(vg, paint);
                    nvgFill(vg);

                    nvgBeginPath(vg);
                    nvgCircle(vg, dx, dy, dotRadius);
                    nvgFillColor(vg, activeColor);
                    nvgFill(vg);
                }
            }
        }
    }
};

// ============================================================================
//  Display widget – shows bars according to dynamic range
// ============================================================================
struct AnalyzerDisplay : Widget {
    VintageSpectrumAnalyzer* module = nullptr;

    AnalyzerDisplay() {
        // box.size = Vec(280, 140);  // little taller for knobs
    }

    void draw(const DrawArgs& args) override {
        if (!module)
            return;
        nvgSave(args.vg);
        std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));

        // Background glass
        NVGcolor bg = nvgRGB(10, 10, 12);
        nvgBeginPath(args.vg);
        nvgFillColor(args.vg, bg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFill(args.vg);

        const int bars = VintageSpectrumAnalyzer::NUM_BANDS;
        const float margin = 4.f;
        const float labelH = 10.f;
        const float barAreaH = box.size.y - margin * 4 - labelH;  // extra margin for knobs below
        const float barW = (box.size.x - margin * (bars + 1)) / bars;

        const float topDb = module->params[0].getValue();
        const float bottomDb = module->params[1].getValue();
        const float rangeDb = topDb - bottomDb;

        for (int i = 0; i < bars; ++i) {
            float db = module->bandDb[i];
            db = clamp(db, bottomDb, topDb);
            float norm = (db - bottomDb) / rangeDb;
            float barH = norm * barAreaH;
            float x = margin + i * (barW + margin);
            float y = margin + (barAreaH - barH);

            NVGcolor cyan = nvgRGB(0x00, 0xc8, 0xff);
            nvgBeginPath(args.vg);
            nvgFillColor(args.vg, cyan);
            nvgRect(args.vg, x, y, barW, barH);
            nvgFill(args.vg);

            // Draw peak indicator
            float peakNorm = module->bandPeaks[i]; // bandPeaks now stores normalized values directly
            if (peakNorm > 0.0f) {
                float peakH = 2.0f; // Peak indicator height
                float peakY = margin + (barAreaH - peakNorm * barAreaH) - peakH;
                NVGcolor peakColor = nvgRGB(0xff, 0x30, 0x30);
                nvgBeginPath(args.vg);
                nvgFillColor(args.vg, peakColor);
                nvgRect(args.vg, x, peakY, barW, peakH);
                nvgFill(args.vg);
            }

            // frequency label
            nvgFontSize(args.vg, 8.f);
            if (font) nvgFontFaceId(args.vg, font->handle);
            nvgFillColor(args.vg, nvgRGB(200, 200, 200));
            nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            nvgText(args.vg, x + barW * 0.5f, box.size.y - labelH, LABELS[i], nullptr);
        }
        nvgRestore(args.vg);
    }
};

// ============================================================================
//  Module widget (panel + display + knobs)
// ============================================================================
struct VintageSpectrumAnalyzerWidget : ModuleWidget {
    VintageSpectrumAnalyzerWidget(VintageSpectrumAnalyzer* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/VFDFreqAnalyzer.svg"), asset::plugin(pluginInstance, "res/VFDFreqAnalyzer-dark.svg")));

        // Display
        // auto* display = new AnalyzerDisplay();
        // display->module = module;
        // display->box.pos = Vec(0, 25);
        // display->box.size = Vec(496.0, 280.0);
        // addChild(display);

        VFDCustomDisplay* display = new VFDCustomDisplay();
        display->module = module;
        display->box.pos = Vec(0.0, 25.0);
        display->box.size = Vec(496.0, 280.0);
        addChild(display);

        // Knobs – small trimmers at bottom
        const float knobY = 25 + display->box.size.y + 15.f;
        addParam(createParamCentered<Trimpot>(Vec(45, knobY), module, VintageSpectrumAnalyzer::UPPER_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(105, knobY), module, VintageSpectrumAnalyzer::LOWER_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(165, knobY), module, VintageSpectrumAnalyzer::FALL_DELAY_PARAM));
        addParam(createParamCentered<Trimpot>(Vec(225, knobY), module, VintageSpectrumAnalyzer::PEAK_FALL_DELAY_PARAM));
        // labels could be in SVG panel – TOP and BOTTOM

        // Inputs
        const float jackX = 12.f;
        const float jackY0 = knobY + 28.f;
        const float jackDy = 35.f;
        addInput(createInputCentered<PJ301MPort>(Vec(jackX, jackY0), module, VintageSpectrumAnalyzer::IN_L_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(jackX + jackDy, jackY0), module, VintageSpectrumAnalyzer::IN_R_INPUT));
    }
};

Model* modelVintageSpectrumAnalyzer = createModel<VintageSpectrumAnalyzer, VintageSpectrumAnalyzerWidget>("VintageSpectrumAnalyzer");

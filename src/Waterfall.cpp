#include "plugin.hpp"
#include "waterfall/WaterfallAnalyzer.hpp"
#include "waterfall/WaterfallTypes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace cella::waterfall;

namespace {

constexpr float DISPLAY_X = 5.f;
constexpr float DISPLAY_Y = 27.f;
constexpr float DISPLAY_WIDTH = 350.f;
constexpr float DISPLAY_HEIGHT = 278.f;
constexpr float RANGE_DEFAULT_DB = -100.f;
constexpr float RANGE_MIN_DB = -140.f;
constexpr float RANGE_MAX_DB = -40.f;

const char* const CHANNEL_NAMES[] = {"L", "R", "Mono", "Mid", "Side"};
const char* const WINDOW_NAMES[] = {"Hann", "Blackman-Harris", "Flat-top"};
const char* const QUALITY_NAMES[] = {"15 Hz", "30 Hz", "60 Hz"};

int getJsonInt(json_t* root, const char* key, int minimum, int maximum, int fallback) {
    json_t* value = json_object_get(root, key);
    if (!json_is_integer(value)) return fallback;
    return clampValue(static_cast<int>(json_integer_value(value)), minimum, maximum);
}

float getJsonFloat(json_t* root, const char* key, float minimum, float maximum, float fallback) {
    json_t* value = json_object_get(root, key);
    if (!json_is_number(value)) return fallback;
    const float result = static_cast<float>(json_number_value(value));
    if (!std::isfinite(result)) return fallback;
    return clampValue(result, minimum, maximum);
}

std::string frequencyLabel(float frequency) {
    if (frequency >= 1000.f) {
        if (frequency >= 9950.f) return rack::string::f("%.0fk", frequency / 1000.f);
        return rack::string::f("%.1fk", frequency / 1000.f);
    }
    return rack::string::f("%.0f", frequency);
}

std::string noteLabel(float frequency) {
    if (!(frequency > 0.f) || !std::isfinite(frequency)) return "--";
    const float midi = 69.f + 12.f * std::log2(frequency / 440.f);
    const int nearest = static_cast<int>(std::lround(midi));
    const int cents = static_cast<int>(std::lround((midi - nearest) * 100.f));
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int note = nearest % 12;
    if (note < 0) note += 12;
    const int octave = nearest / 12 - 1;
    return rack::string::f("%s%d %+dc", names[note], octave, cents);
}

}  // namespace

struct Waterfall : Module {
    enum ParamIds { MODE_PARAM, RANGE_PARAM, FREEZE_PARAM, CLEAR_PARAM, NUM_PARAMS };
    enum InputIds { LEFT_INPUT, RIGHT_INPUT, FREEZE_INPUT, CLEAR_INPUT, NUM_INPUTS };
    enum LightIds { FREEZE_LIGHT, NUM_LIGHTS };

    WaterfallAnalyzer analyzer;
    dsp::RingBuffer<SpectrumRow, ROW_QUEUE_SIZE> displayRows;
    dsp::SchmittTrigger freezeButtonTrigger;
    dsp::SchmittTrigger freezeInputTrigger;
    dsp::SchmittTrigger clearButtonTrigger;
    dsp::SchmittTrigger clearInputTrigger;

    std::atomic<int> fftSizeSetting{static_cast<int>(FftSize::FFT_4096)};
    std::atomic<int> windowSetting{static_cast<int>(WindowFunction::HANN)};
    std::atomic<int> qualitySetting{static_cast<int>(Quality::NORMAL)};
    std::atomic<int> polyChannelSetting{0};
    std::atomic<int> paletteSetting{static_cast<int>(Palette::HEAT)};
    std::atomic<int> peakHoldSetting{static_cast<int>(PeakHold::DECAY)};
    std::atomic<int> flowSetting{static_cast<int>(FlowDirection::UP)};
    std::atomic<float> viewMinimum{0.f};
    std::atomic<float> viewMaximum{1.f};
    std::atomic<bool> frozen{false};
    std::atomic<uint64_t> clearGeneration{0};
    std::atomic<uint64_t> activeConfigGeneration{1};
#ifndef NDEBUG
    std::atomic<uint64_t> droppedRows{0};
#endif

    int appliedFftSize = -1;
    int appliedWindow = -1;
    int appliedQuality = -1;
    int appliedChannelMode = -1;
    int appliedPolyChannel = -1;
    float appliedSampleRate = 0.f;
    uint64_t configGeneration = 1;

    Waterfall() {
        config(NUM_PARAMS, NUM_INPUTS, 0, NUM_LIGHTS);
        configSwitch(MODE_PARAM, 0.f, static_cast<float>(static_cast<int>(ChannelMode::COUNT) - 1),
                     static_cast<float>(ChannelMode::MONO), "Channel", {"Left", "Right", "Mono", "Mid", "Side"});
        configParam(RANGE_PARAM, RANGE_MIN_DB, RANGE_MAX_DB, RANGE_DEFAULT_DB, "Display floor", " dBFS");
        configButton(FREEZE_PARAM, "Freeze");
        configButton(CLEAR_PARAM, "Clear");
        configInput(LEFT_INPUT, "Left");
        configInput(RIGHT_INPUT, "Right");
        configInput(FREEZE_INPUT, "Freeze trigger");
        configInput(CLEAR_INPUT, "Clear trigger");
    }

    void process(const ProcessArgs& args) override {
        const bool freezeButtonEvent = freezeButtonTrigger.process(params[FREEZE_PARAM].getValue());
        const bool freezePortEvent = freezeInputTrigger.process(inputs[FREEZE_INPUT].getVoltage());
        const bool freezeEvent = freezeButtonEvent || freezePortEvent;
        if (freezeEvent) frozen.store(!frozen.load(std::memory_order_relaxed), std::memory_order_relaxed);

        const bool clearButtonEvent = clearButtonTrigger.process(params[CLEAR_PARAM].getValue());
        const bool clearPortEvent = clearInputTrigger.process(inputs[CLEAR_INPUT].getVoltage());
        const bool clearEvent = clearButtonEvent || clearPortEvent;
        if (clearEvent) clearGeneration.fetch_add(1, std::memory_order_release);
        lights[FREEZE_LIGHT].setBrightness(frozen.load(std::memory_order_relaxed) ? 1.f : 0.f);

        const int fftSize =
            clampValue(fftSizeSetting.load(std::memory_order_relaxed), 0, static_cast<int>(FftSize::COUNT) - 1);
        const int window =
            clampValue(windowSetting.load(std::memory_order_relaxed), 0, static_cast<int>(WindowFunction::COUNT) - 1);
        const int quality =
            clampValue(qualitySetting.load(std::memory_order_relaxed), 0, static_cast<int>(Quality::COUNT) - 1);
        const int channelMode = clampValue(static_cast<int>(std::lround(params[MODE_PARAM].getValue())), 0,
                                           static_cast<int>(ChannelMode::COUNT) - 1);
        const int polyChannel = clampValue(polyChannelSetting.load(std::memory_order_relaxed), 0, 15);

        if (fftSize != appliedFftSize || window != appliedWindow || quality != appliedQuality ||
            channelMode != appliedChannelMode || polyChannel != appliedPolyChannel || args.sampleRate != appliedSampleRate) {
            WaterfallConfig newConfig;
            newConfig.fftSize = static_cast<FftSize>(fftSize);
            newConfig.window = static_cast<WindowFunction>(window);
            newConfig.quality = static_cast<Quality>(quality);
            newConfig.channelMode = static_cast<ChannelMode>(channelMode);
            newConfig.polyChannel = polyChannel;
            newConfig.sampleRate = args.sampleRate;
            newConfig.generation = ++configGeneration;
            analyzer.configure(newConfig);
            activeConfigGeneration.store(configGeneration, std::memory_order_release);
            appliedFftSize = fftSize;
            appliedWindow = window;
            appliedQuality = quality;
            appliedChannelMode = channelMode;
            appliedPolyChannel = polyChannel;
            appliedSampleRate = args.sampleRate;
        }

        const bool leftConnected = inputs[LEFT_INPUT].isConnected();
        const bool rightConnected = inputs[RIGHT_INPUT].isConnected();
        const float left = leftConnected ? inputs[LEFT_INPUT].getVoltage(polyChannel) : 0.f;
        const float right = rightConnected ? inputs[RIGHT_INPUT].getVoltage(polyChannel) : 0.f;
        const float mixed =
            mixInputVoltages(left, right, leftConnected, rightConnected, static_cast<ChannelMode>(channelMode));

        SpectrumRow row;
        if (analyzer.processSample(mixed * VOLTAGE_TO_FULL_SCALE, row)) {
            if (!displayRows.full()) {
                displayRows.push(row);
            }
#ifndef NDEBUG
            else {
                droppedRows.fetch_add(1, std::memory_order_relaxed);
            }
#endif
        }
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "fftSize", json_integer(fftSizeSetting.load()));
        json_object_set_new(root, "window", json_integer(windowSetting.load()));
        json_object_set_new(root, "quality", json_integer(qualitySetting.load()));
        json_object_set_new(root, "polyChannel", json_integer(polyChannelSetting.load()));
        json_object_set_new(root, "palette", json_integer(paletteSetting.load()));
        json_object_set_new(root, "peakHold", json_integer(peakHoldSetting.load()));
        json_object_set_new(root, "flow", json_integer(flowSetting.load()));
        json_object_set_new(root, "viewMinimum", json_real(viewMinimum.load()));
        json_object_set_new(root, "viewMaximum", json_real(viewMaximum.load()));
        return root;
    }

    void dataFromJson(json_t* root) override {
        fftSizeSetting.store(getJsonInt(root, "fftSize", 0, static_cast<int>(FftSize::COUNT) - 1,
                                        static_cast<int>(FftSize::FFT_4096)));
        windowSetting.store(getJsonInt(root, "window", 0, static_cast<int>(WindowFunction::COUNT) - 1,
                                       static_cast<int>(WindowFunction::HANN)));
        qualitySetting.store(getJsonInt(root, "quality", 0, static_cast<int>(Quality::COUNT) - 1,
                                        static_cast<int>(Quality::NORMAL)));
        polyChannelSetting.store(getJsonInt(root, "polyChannel", 0, 15, 0));
        paletteSetting.store(
            getJsonInt(root, "palette", 0, static_cast<int>(Palette::COUNT) - 1, static_cast<int>(Palette::HEAT)));
        peakHoldSetting.store(getJsonInt(root, "peakHold", 0, static_cast<int>(PeakHold::COUNT) - 1,
                                         static_cast<int>(PeakHold::DECAY)));
        flowSetting.store(getJsonInt(root, "flow", 0, static_cast<int>(FlowDirection::COUNT) - 1,
                                     static_cast<int>(FlowDirection::UP)));
        float minimum = getJsonFloat(root, "viewMinimum", 0.f, 0.99f, 0.f);
        float maximum = getJsonFloat(root, "viewMaximum", 0.01f, 1.f, 1.f);
        if (maximum - minimum < 0.01f) {
            minimum = 0.f;
            maximum = 1.f;
        }
        viewMinimum.store(minimum);
        viewMaximum.store(maximum);
    }
};

namespace {

struct WaterfallRenderer {
    GLuint program = 0;
    GLuint historyTexture = 0;
    GLuint traceTexture = 0;
    GLint historyLocation = -1;
    GLint traceLocation = -1;
    GLint headLocation = -1;
    GLint flowLocation = -1;
    GLint viewLocation = -1;
    GLint rangeLocation = -1;
    GLint paletteLocation = -1;
    GLint peakHoldLocation = -1;
    bool initializationAttempted = false;

    static std::string loadResource(const std::string& path) {
        const std::vector<uint8_t> bytes = system::readFile(asset::plugin(pluginInstance, path));
        return std::string(bytes.begin(), bytes.end());
    }

    static GLuint compile(GLenum type, const std::string& source, const char* label) {
        GLuint shader = glCreateShader(type);
        const GLchar* text = source.c_str();
        glShaderSource(shader, 1, &text, NULL);
        glCompileShader(shader);
        GLint success = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (success == GL_TRUE) return shader;
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<GLchar> log(static_cast<size_t>(std::max(length, 1)));
        glGetShaderInfoLog(shader, length, NULL, log.data());
        WARN("Waterfall %s shader compilation failed: %s", label, log.data());
        glDeleteShader(shader);
        return 0;
    }

    bool initialize() {
        if (program && historyTexture && traceTexture) return true;
        if (initializationAttempted) return false;
        initializationAttempted = true;
        try {
            GLuint vertex = compile(GL_VERTEX_SHADER, loadResource("res/shaders/waterfall_gl.vert"), "vertex");
            GLuint fragment = compile(GL_FRAGMENT_SHADER, loadResource("res/shaders/waterfall_gl.frag"), "fragment");
            if (!vertex || !fragment) {
                if (vertex) glDeleteShader(vertex);
                if (fragment) glDeleteShader(fragment);
                return false;
            }

            GLuint candidate = glCreateProgram();
            glAttachShader(candidate, vertex);
            glAttachShader(candidate, fragment);
            glLinkProgram(candidate);
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            GLint linked = GL_FALSE;
            glGetProgramiv(candidate, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE) {
                GLint length = 0;
                glGetProgramiv(candidate, GL_INFO_LOG_LENGTH, &length);
                std::vector<GLchar> log(static_cast<size_t>(std::max(length, 1)));
                glGetProgramInfoLog(candidate, length, NULL, log.data());
                WARN("Waterfall shader link failed: %s", log.data());
                glDeleteProgram(candidate);
                return false;
            }

            program = candidate;
            historyLocation = glGetUniformLocation(program, "uHistory");
            traceLocation = glGetUniformLocation(program, "uTrace");
            headLocation = glGetUniformLocation(program, "uHead");
            flowLocation = glGetUniformLocation(program, "uFlow");
            viewLocation = glGetUniformLocation(program, "uView");
            rangeLocation = glGetUniformLocation(program, "uRange");
            paletteLocation = glGetUniformLocation(program, "uPalette");
            peakHoldLocation = glGetUniformLocation(program, "uPeakHold");

            glGenTextures(1, &historyTexture);
            glBindTexture(GL_TEXTURE_2D, historyTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, NUM_FREQUENCY_CELLS, HISTORY_ROWS, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);

            glGenTextures(1, &traceTexture);
            glBindTexture(GL_TEXTURE_2D, traceTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, NUM_FREQUENCY_CELLS, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            return true;
        } catch (const std::exception& exception) {
            WARN("Waterfall shader resources could not be loaded: %s", exception.what());
            return false;
        }
    }

    void destroy() {
        if (traceTexture) glDeleteTextures(1, &traceTexture);
        if (historyTexture) glDeleteTextures(1, &historyTexture);
        if (program) glDeleteProgram(program);
        traceTexture = 0;
        historyTexture = 0;
        program = 0;
        initializationAttempted = false;
    }
};

struct WaterfallDisplay : widget::OpenGlWidget {
    Waterfall* module = NULL;
    WaterfallRenderer renderer;
    std::array<SpectrumRow, HISTORY_ROWS> history;
    std::array<float, NUM_FREQUENCY_CELLS> currentTrace;
    std::array<float, NUM_FREQUENCY_CELLS> peakTrace;
    std::array<bool, HISTORY_ROWS> dirtyRows;
    int head = -1;
    int historyCount = 0;
    uint64_t currentGeneration = 0;
    uint64_t seenClearGeneration = 0;
    uint64_t seenModuleConfigGeneration = 0;
    bool traceDirty = true;
    SpectrumRow latestMetadata;

    WaterfallDisplay() {
        currentTrace.fill(INTERNAL_FLOOR_DB);
        peakTrace.fill(INTERNAL_FLOOR_DB);
        dirtyRows.fill(true);
        for (int row = 0; row < HISTORY_ROWS; ++row) {
            history[static_cast<size_t>(row)].dbTenths.fill(quantizeDb(INTERNAL_FLOOR_DB));
        }
    }

    void onContextCreate(const ContextCreateEvent& event) override {
        widget::OpenGlWidget::onContextCreate(event);
        renderer.program = 0;
        renderer.historyTexture = 0;
        renderer.traceTexture = 0;
        renderer.initializationAttempted = false;
        dirtyRows.fill(true);
        traceDirty = true;
    }

    void onContextDestroy(const ContextDestroyEvent& event) override {
        renderer.destroy();
        widget::OpenGlWidget::onContextDestroy(event);
    }

    void clearHistory() {
        head = -1;
        historyCount = 0;
        currentGeneration = 0;
        latestMetadata = SpectrumRow();
        currentTrace.fill(INTERNAL_FLOOR_DB);
        peakTrace.fill(INTERNAL_FLOOR_DB);
        for (int row = 0; row < HISTORY_ROWS; ++row) {
            history[static_cast<size_t>(row)] = SpectrumRow();
            history[static_cast<size_t>(row)].dbTenths.fill(quantizeDb(INTERNAL_FLOOR_DB));
        }
        dirtyRows.fill(true);
        traceDirty = true;
    }

    void addRow(const SpectrumRow& row) {
        if (currentGeneration != 0 && row.configGeneration != currentGeneration) clearHistory();
        currentGeneration = row.configGeneration;

        float elapsedSeconds = 0.f;
        if (historyCount > 0 && latestMetadata.sampleRate == row.sampleRate &&
            row.rowEndSample >= latestMetadata.rowEndSample) {
            elapsedSeconds =
                static_cast<float>(row.rowEndSample - latestMetadata.rowEndSample) / std::max(row.sampleRate, 1.f);
        }
        if (!(elapsedSeconds > 0.f)) {
            elapsedSeconds = 1.f / rowsPerSecond(static_cast<Quality>(
                                             clampValue(module ? module->qualitySetting.load() : 1, 0, 2)));
        }

        const PeakHold peakMode = static_cast<PeakHold>(
            clampValue(module ? module->peakHoldSetting.load() : static_cast<int>(PeakHold::DECAY), 0,
                       static_cast<int>(PeakHold::COUNT) - 1));
        for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
            const float value = dequantizeDb(row.dbTenths[static_cast<size_t>(cell)]);
            currentTrace[static_cast<size_t>(cell)] = value;
            if (peakMode == PeakHold::OFF) {
                peakTrace[static_cast<size_t>(cell)] = INTERNAL_FLOOR_DB;
            } else if (peakMode == PeakHold::INFINITE) {
                peakTrace[static_cast<size_t>(cell)] = std::max(peakTrace[static_cast<size_t>(cell)], value);
            } else {
                peakTrace[static_cast<size_t>(cell)] =
                    std::max(value, peakTrace[static_cast<size_t>(cell)] - 12.f * elapsedSeconds);
            }
        }

        head = (head + 1) % HISTORY_ROWS;
        history[static_cast<size_t>(head)] = row;
        historyCount = std::min(historyCount + 1, HISTORY_ROWS);
        latestMetadata = row;
        dirtyRows[static_cast<size_t>(head)] = true;
        traceDirty = true;
    }

    void drainRows() {
        if (!module) return;
        const uint64_t clear = module->clearGeneration.load(std::memory_order_acquire);
        if (clear != seenClearGeneration) {
            seenClearGeneration = clear;
            clearHistory();
        }
        const uint64_t moduleGeneration = module->activeConfigGeneration.load(std::memory_order_acquire);
        if (seenModuleConfigGeneration != 0 && moduleGeneration != seenModuleConfigGeneration) clearHistory();
        seenModuleConfigGeneration = moduleGeneration;

        const bool isFrozen = module->frozen.load(std::memory_order_relaxed);
        while (!module->displayRows.empty()) {
            const SpectrumRow row = module->displayRows.shift();
            if (!isFrozen) addRow(row);
        }
    }

    void seedPreview() {
        if (historyCount > 0) return;
        const float sampleRate = 48000.f;
        for (int rowIndex = 0; rowIndex < HISTORY_ROWS; ++rowIndex) {
            SpectrumRow row;
            row.sampleRate = sampleRate;
            row.fftSize = 4096;
            row.effectiveHopSize = 1024;
            row.configGeneration = 1;
            row.rowEndSample = static_cast<uint64_t>((rowIndex + 1) * 1600);
            row.sourceAnalysisSample = row.rowEndSample;
            for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
                const float x = static_cast<float>(cell) / (NUM_FREQUENCY_CELLS - 1);
                const float ridge = 0.20f + 0.22f * std::sin(rowIndex * 0.055f);
                const float ridge2 = 0.64f + 0.08f * std::sin(rowIndex * 0.031f);
                const float energy = std::max(std::exp(-900.f * (x - ridge) * (x - ridge)),
                                              0.7f * std::exp(-500.f * (x - ridge2) * (x - ridge2)));
                row.dbTenths[static_cast<size_t>(cell)] = quantizeDb(-105.f + energy * 91.f);
            }
            addRow(row);
        }
    }

    bool sampleAt(float fullFrequency, float age, float& db, SpectrumRow& metadata) const {
        if (historyCount <= 0 || head < 0) return false;
        const int ageRows = clampValue(static_cast<int>(std::lround(age * (HISTORY_ROWS - 1))), 0,
                                       HISTORY_ROWS - 1);
        if (ageRows >= historyCount) return false;
        const int rowIndex = (head - ageRows + HISTORY_ROWS) % HISTORY_ROWS;
        const int cell = clampValue(static_cast<int>(std::lround(fullFrequency * (NUM_FREQUENCY_CELLS - 1))), 0,
                                    NUM_FREQUENCY_CELLS - 1);
        metadata = history[static_cast<size_t>(rowIndex)];
        db = dequantizeDb(metadata.dbTenths[static_cast<size_t>(cell)]);
        return true;
    }

    void uploadDirtyData() {
        std::array<unsigned char, NUM_FREQUENCY_CELLS * 4> pixels;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, renderer.historyTexture);
        for (int row = 0; row < HISTORY_ROWS; ++row) {
            if (!dirtyRows[static_cast<size_t>(row)]) continue;
            for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
                const unsigned char encoded =
                    encodeDb(dequantizeDb(history[static_cast<size_t>(row)].dbTenths[static_cast<size_t>(cell)]));
                const size_t offset = static_cast<size_t>(cell * 4);
                pixels[offset] = encoded;
                pixels[offset + 1] = encoded;
                pixels[offset + 2] = encoded;
                pixels[offset + 3] = 255;
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, NUM_FREQUENCY_CELLS, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                            pixels.data());
            dirtyRows[static_cast<size_t>(row)] = false;
        }

        if (traceDirty) {
            for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
                const size_t offset = static_cast<size_t>(cell * 4);
                pixels[offset] = encodeDb(currentTrace[static_cast<size_t>(cell)]);
                pixels[offset + 1] = encodeDb(peakTrace[static_cast<size_t>(cell)]);
                pixels[offset + 2] = 0;
                pixels[offset + 3] = 255;
            }
            glBindTexture(GL_TEXTURE_2D, renderer.traceTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NUM_FREQUENCY_CELLS, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                            pixels.data());
            traceDirty = false;
        }
    }

    static void drawQuad() {
        glBegin(GL_QUADS);
        glTexCoord2f(0.f, 0.f);
        glVertex2f(-1.f, -1.f);
        glTexCoord2f(1.f, 0.f);
        glVertex2f(1.f, -1.f);
        glTexCoord2f(1.f, 1.f);
        glVertex2f(1.f, 1.f);
        glTexCoord2f(0.f, 1.f);
        glVertex2f(-1.f, 1.f);
        glEnd();
    }

    void drawFallback() {
        glUseProgram(0);
        glBegin(GL_QUADS);
        glColor3f(0.005f, 0.008f, 0.012f);
        glVertex2f(-1.f, -1.f);
        glVertex2f(1.f, -1.f);
        glColor3f(0.02f, 0.025f, 0.032f);
        glVertex2f(1.f, 1.f);
        glVertex2f(-1.f, 1.f);
        glEnd();
    }

    void drawFramebuffer() override {
        if (module)
            drainRows();
        else
            seedPreview();

        GLint oldProgram = 0;
        GLint oldActiveTexture = 0;
        GLint oldTexture0 = 0;
        GLint oldTexture1 = 0;
        GLint oldUnpackAlignment = 4;
        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
        glActiveTexture(GL_TEXTURE1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture1);
        glPushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT | GL_VIEWPORT_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT);

        const math::Vec framebuffer = getFramebufferSize();
        glViewport(0, 0, static_cast<GLsizei>(framebuffer.x), static_cast<GLsizei>(framebuffer.y));
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);

        if (renderer.initialize()) {
            uploadDirtyData();
            const int flow = module ? clampValue(module->flowSetting.load(), 0, 3) : 0;
            const int palette = module ? clampValue(module->paletteSetting.load(), 0, 2) : 0;
            const int peakHold = module ? clampValue(module->peakHoldSetting.load(), 0, 2) : 1;
            float viewLow = module ? module->viewMinimum.load() : 0.f;
            float viewHigh = module ? module->viewMaximum.load() : 1.f;
            const float floorDb = module ? module->params[Waterfall::RANGE_PARAM].getValue() : RANGE_DEFAULT_DB;

            glUseProgram(renderer.program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, renderer.historyTexture);
            glUniform1i(renderer.historyLocation, 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, renderer.traceTexture);
            glUniform1i(renderer.traceLocation, 1);
            glUniform1f(renderer.headLocation, static_cast<float>(std::max(head, 0)));
            glUniform1i(renderer.flowLocation, flow);
            glUniform2f(renderer.viewLocation, viewLow, viewHigh);
            glUniform2f(renderer.rangeLocation, floorDb, 0.f);
            glUniform1i(renderer.paletteLocation, palette);
            glUniform1i(renderer.peakHoldLocation, peakHold);
            drawQuad();
        } else {
            drawFallback();
        }

        glPopAttrib();
        glUseProgram(static_cast<GLuint>(oldProgram));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture1));
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
        glPixelStorei(GL_UNPACK_ALIGNMENT, oldUnpackAlignment);
    }

    void drawPreviewNanoVg(const DrawArgs& args) {
        nvgSave(args.vg);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
        nvgFillPaint(args.vg, nvgLinearGradient(args.vg, 0.f, 0.f, box.size.x, box.size.y,
                                                nvgRGB(40, 5, 52), nvgRGB(2, 4, 7)));
        nvgFill(args.vg);

        nvgBeginPath(args.vg);
        for (int y = 0; y < 90; ++y) {
            const float age = static_cast<float>(y) / 89.f;
            const float ridge = 0.2f + 0.18f * std::sin(age * 14.f);
            const float x = ridge * box.size.x;
            const float displayY = age * box.size.y;
            if (y == 0)
                nvgMoveTo(args.vg, x, displayY);
            else
                nvgLineTo(args.vg, x, displayY);
        }
        nvgStrokeColor(args.vg, nvgRGB(255, 126, 45));
        nvgStrokeWidth(args.vg, 5.f);
        nvgStroke(args.vg);
        nvgBeginPath(args.vg);
        for (int y = 0; y < 90; ++y) {
            const float age = static_cast<float>(y) / 89.f;
            const float x = (0.64f + 0.08f * std::sin(age * 8.f)) * box.size.x;
            const float displayY = age * box.size.y;
            if (y == 0)
                nvgMoveTo(args.vg, x, displayY);
            else
                nvgLineTo(args.vg, x, displayY);
        }
        nvgStrokeColor(args.vg, nvgRGB(240, 54, 95));
        nvgStrokeWidth(args.vg, 3.f);
        nvgStroke(args.vg);
        nvgRestore(args.vg);
    }

    void draw(const DrawArgs& args) override {
        if (!module && args.fb) {
            drawPreviewNanoVg(args);
            return;
        }
        widget::OpenGlWidget::draw(args);
    }
};

struct WaterfallOverlay : TransparentWidget {
    Waterfall* module = NULL;
    WaterfallDisplay* display = NULL;
    bool hovered = false;
    bool draggingLegend = false;
    math::Vec cursor;

    FlowDirection flow() const {
        return static_cast<FlowDirection>(
            clampValue(module ? module->flowSetting.load() : 0, 0, static_cast<int>(FlowDirection::COUNT) - 1));
    }

    float viewLow() const { return module ? module->viewMinimum.load() : 0.f; }
    float viewHigh() const { return module ? module->viewMaximum.load() : 1.f; }

    LogicalPoint logicalAt(math::Vec position) const {
        const float x = position.x / std::max(box.size.x, 1.f);
        const float yBottom = 1.f - position.y / std::max(box.size.y, 1.f);
        return logicalFromScreen(flow(), x, yBottom);
    }

    float fullFrequencyCoordinate(const LogicalPoint& logical) const {
        return viewLow() + logical.frequency * (viewHigh() - viewLow());
    }

    float nyquist() const {
        if (display && display->latestMetadata.sampleRate > 0.f) return display->latestMetadata.sampleRate * 0.5f;
        return 24000.f;
    }

    float frequencyFromFullCoordinate(float coordinate) const {
        return MIN_FREQUENCY_HZ * std::pow(nyquist() / MIN_FREQUENCY_HZ, clampValue(coordinate, 0.f, 1.f));
    }

    void drawStatus(const DrawArgs& args) {
        const int mode = module ? clampValue(static_cast<int>(std::lround(module->params[Waterfall::MODE_PARAM].getValue())),
                                             0, 4)
                                : 2;
        const int fft = module ? clampValue(module->fftSizeSetting.load(), 0, 4) : 2;
        const int window = module ? clampValue(module->windowSetting.load(), 0, 2) : 0;
        const int quality = module ? clampValue(module->qualitySetting.load(), 0, 2) : 1;
        const float range = module ? module->params[Waterfall::RANGE_PARAM].getValue() : RANGE_DEFAULT_DB;
        const bool frozen = module && module->frozen.load();
        float overlap = 75.f;
        if (display && display->latestMetadata.fftSize > 0) {
            overlap =
                clampValue(100.f * (1.f - static_cast<float>(display->latestMetadata.effectiveHopSize) /
                                              display->latestMetadata.fftSize),
                           0.f, 75.f);
        }
        const std::string status = rack::string::f(
            "%s  %d %s  %s  %.0f%% ov  %.0f..0 dBFS%s", CHANNEL_NAMES[mode], FFT_SIZES[fft],
            WINDOW_NAMES[window], QUALITY_NAMES[quality], overlap, range, frozen ? "  FROZEN" : "");

        nvgFontSize(args.vg, 9.f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(args.vg, frozen ? nvgRGB(255, 196, 92) : nvgRGB(186, 198, 202));
        nvgText(args.vg, 7.f, 5.f, status.c_str(), NULL);
    }

    void drawGrid(const DrawArgs& args) {
        static const float guides[] = {20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f};
        const float low = viewLow();
        const float high = viewHigh();
        const float logRange = std::log(nyquist() / MIN_FREQUENCY_HZ);
        if (!(logRange > 0.f) || high <= low) return;

        nvgStrokeWidth(args.vg, 0.6f);
        nvgStrokeColor(args.vg, nvgRGBA(190, 205, 210, 42));
        nvgFillColor(args.vg, nvgRGBA(190, 205, 210, 150));
        nvgFontSize(args.vg, 8.f);
        for (size_t i = 0; i < sizeof(guides) / sizeof(guides[0]); ++i) {
            if (guides[i] > nyquist()) continue;
            const float full = std::log(guides[i] / MIN_FREQUENCY_HZ) / logRange;
            const float visible = (full - low) / (high - low);
            if (visible < 0.f || visible > 1.f) continue;
            nvgBeginPath(args.vg);
            if (isVerticalFlow(flow())) {
                const float x = visible * box.size.x;
                nvgMoveTo(args.vg, x, 18.f);
                nvgLineTo(args.vg, x, box.size.y);
                nvgStroke(args.vg);
                nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
                const std::string label = frequencyLabel(guides[i]);
                nvgText(args.vg, x, box.size.y - 2.f, label.c_str(), NULL);
            } else {
                const float y = (1.f - visible) * box.size.y;
                nvgMoveTo(args.vg, 0.f, y);
                nvgLineTo(args.vg, box.size.x - 10.f, y);
                nvgStroke(args.vg);
                nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                const std::string label = frequencyLabel(guides[i]);
                nvgText(args.vg, box.size.x - 3.f, y, label.c_str(), NULL);
            }
        }

        nvgStrokeColor(args.vg, nvgRGBA(190, 205, 210, 26));
        for (int division = 1; division < 4; ++division) {
            const float age = division / 4.f;
            float x = 0.f;
            float yBottom = 0.f;
            screenFromLogical(flow(), LogicalPoint(0.f, age), x, yBottom);
            nvgBeginPath(args.vg);
            if (isVerticalFlow(flow())) {
                const float y = (1.f - yBottom) * box.size.y;
                nvgMoveTo(args.vg, 0.f, y);
                nvgLineTo(args.vg, box.size.x, y);
            } else {
                const float displayX = x * box.size.x;
                nvgMoveTo(args.vg, displayX, 18.f);
                nvgLineTo(args.vg, displayX, box.size.y);
            }
            nvgStroke(args.vg);
        }
    }

    void drawLegend(const DrawArgs& args) {
        const float floorDb = module ? module->params[Waterfall::RANGE_PARAM].getValue() : RANGE_DEFAULT_DB;
        const float x = box.size.x - 7.f;
        const float top = 22.f;
        const float height = 68.f;
        const int selectedPalette = module ? clampValue(module->paletteSetting.load(), 0, 2) : 0;
        NVGcolor lowColor = nvgRGB(5, 3, 18);
        NVGcolor highColor = nvgRGB(255, 224, 88);
        if (selectedPalette == static_cast<int>(Palette::GRAYSCALE)) {
            lowColor = nvgRGB(3, 3, 3);
            highColor = nvgRGB(248, 248, 248);
        } else if (selectedPalette == static_cast<int>(Palette::VIRIDIS)) {
            lowColor = nvgRGB(68, 1, 84);
            highColor = nvgRGB(253, 231, 37);
        }
        NVGpaint gradient = nvgLinearGradient(args.vg, x, top + height, x, top, lowColor, highColor);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, x, top, 4.f, height);
        nvgFillPaint(args.vg, gradient);
        nvgFill(args.vg);
        nvgFontSize(args.vg, 7.f);
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
        nvgFillColor(args.vg, nvgRGBA(220, 224, 225, 170));
        nvgText(args.vg, x - 2.f, top, "0", NULL);
        const std::string floor = rack::string::f("%.0f", floorDb);
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
        nvgText(args.vg, x - 2.f, top + height, floor.c_str(), NULL);
    }

    void drawCursor(const DrawArgs& args) {
        if (!hovered || !display) return;
        const LogicalPoint logical = logicalAt(cursor);
        const float full = fullFrequencyCoordinate(logical);
        const float frequency = frequencyFromFullCoordinate(full);
        float db = 0.f;
        SpectrumRow row;
        const bool valid = display->sampleAt(full, logical.age, db, row);

        nvgStrokeWidth(args.vg, 0.8f);
        nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 150));
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, cursor.x, 18.f);
        nvgLineTo(args.vg, cursor.x, box.size.y);
        nvgMoveTo(args.vg, 0.f, cursor.y);
        nvgLineTo(args.vg, box.size.x, cursor.y);
        nvgStroke(args.vg);

        std::string text;
        if (valid) {
            float ageSeconds = 0.f;
            if (display->latestMetadata.rowEndSample >= row.rowEndSample && row.sampleRate > 0.f) {
                ageSeconds =
                    static_cast<float>(display->latestMetadata.rowEndSample - row.rowEndSample) / row.sampleRate;
            }
            text = rack::string::f("%s Hz  %s  %.1f dBFS  -%.2fs", frequencyLabel(frequency).c_str(),
                                   noteLabel(frequency).c_str(), db, ageSeconds);
        } else {
            text = rack::string::f("%s Hz  %s  no history", frequencyLabel(frequency).c_str(),
                                   noteLabel(frequency).c_str());
        }

        nvgFontSize(args.vg, 9.f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        float bounds[4];
        nvgTextBounds(args.vg, cursor.x + 8.f, cursor.y + 8.f, text.c_str(), NULL, bounds);
        float textX = cursor.x + 8.f;
        float textY = cursor.y + 8.f;
        if (bounds[2] > box.size.x - 4.f) textX = cursor.x - (bounds[2] - bounds[0]) - 8.f;
        if (textY > box.size.y - 18.f) textY = cursor.y - 17.f;
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, textX - 3.f, textY - 2.f, bounds[2] - bounds[0] + 6.f, 14.f, 2.f);
        nvgFillColor(args.vg, nvgRGBA(0, 0, 0, 190));
        nvgFill(args.vg);
        nvgFillColor(args.vg, nvgRGB(236, 239, 240));
        nvgText(args.vg, textX, textY, text.c_str(), NULL);
    }

    void draw(const DrawArgs& args) override {
        nvgSave(args.vg);
        drawGrid(args);
        drawStatus(args);
        drawLegend(args);
        drawCursor(args);
        nvgRestore(args.vg);
    }

    void onHover(const event::Hover& event) override {
        hovered = true;
        cursor = event.pos;
        event.consume(this);
    }

    void onLeave(const event::Leave& event) override {
        hovered = false;
        TransparentWidget::onLeave(event);
    }

    void onButton(const event::Button& event) override {
        if (event.action == GLFW_PRESS && event.button == GLFW_MOUSE_BUTTON_LEFT &&
            (event.mods & RACK_MOD_MASK) == 0) {
            cursor = event.pos;
            draggingLegend = event.pos.x >= box.size.x - 14.f && event.pos.y >= 18.f && event.pos.y <= 102.f;
            event.consume(this);
        }
    }

    void onDragMove(const event::DragMove& event) override {
        if (!module) return;
        const float zoom = APP->scene->rackScroll->zoomWidget->zoom;
        cursor.x += event.mouseDelta.x / zoom;
        cursor.y += event.mouseDelta.y / zoom;
        cursor.x = clampValue(cursor.x, 0.f, box.size.x);
        cursor.y = clampValue(cursor.y, 0.f, box.size.y);
        if (draggingLegend) {
            const float next = module->params[Waterfall::RANGE_PARAM].getValue() -
                               event.mouseDelta.y / std::max(zoom, 0.01f) * 0.4f;
            module->params[Waterfall::RANGE_PARAM].setValue(clampValue(next, RANGE_MIN_DB, RANGE_MAX_DB));
            return;
        }

        const float low = module->viewMinimum.load();
        const float high = module->viewMaximum.load();
        const float span = high - low;
        const float axisDelta = isVerticalFlow(flow()) ? event.mouseDelta.x / (zoom * box.size.x)
                                                       : -event.mouseDelta.y / (zoom * box.size.y);
        float nextLow = low - axisDelta * span;
        nextLow = clampValue(nextLow, 0.f, 1.f - span);
        module->viewMinimum.store(nextLow);
        module->viewMaximum.store(nextLow + span);
    }

    void onDragEnd(const event::DragEnd& event) override {
        draggingLegend = false;
        TransparentWidget::onDragEnd(event);
    }

    void onHoverScroll(const event::HoverScroll& event) override {
        if (!module) return;
        const LogicalPoint logical = logicalAt(event.pos);
        const float low = module->viewMinimum.load();
        const float high = module->viewMaximum.load();
        const float span = high - low;
        const float anchor = low + logical.frequency * span;
        const float factor = std::pow(1.0015f, event.scrollDelta.y);
        const float nextSpan = clampValue(span * factor, 0.03f, 1.f);
        float nextLow = anchor - logical.frequency * nextSpan;
        nextLow = clampValue(nextLow, 0.f, 1.f - nextSpan);
        module->viewMinimum.store(nextLow);
        module->viewMaximum.store(nextLow + nextSpan);
        event.consume(this);
    }

    void onDoubleClick(const event::DoubleClick& event) override {
        if (!module) return;
        module->viewMinimum.store(0.f);
        module->viewMaximum.store(1.f);
        event.consume(this);
    }
};

struct WaterfallBezel : TransparentWidget {
    void draw(const DrawArgs& args) override {
        nvgSave(args.vg);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f);
        nvgStrokeColor(args.vg, nvgRGB(14, 17, 19));
        nvgStrokeWidth(args.vg, 2.f);
        nvgStroke(args.vg);
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 2.f, 2.f);
        nvgLineTo(args.vg, box.size.x - 2.f, 2.f);
        nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 40));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);
        nvgRestore(args.vg);
    }
};

struct WaterfallPanelLabels : TransparentWidget {
    void draw(const DrawArgs& args) override {
        nvgSave(args.vg);
        nvgFillColor(args.vg, settings::preferDarkPanels ? nvgRGB(247, 197, 173) : nvgRGB(236, 237, 241));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(args.vg, 13.f);
        nvgTextLetterSpacing(args.vg, 2.f);
        nvgText(args.vg, box.size.x * 0.5f, 13.f, "WATERFALL", NULL);
        nvgTextLetterSpacing(args.vg, 0.f);
        nvgFontSize(args.vg, 7.5f);
        const char* labels[] = {"L", "R", "FREEZE IN", "CLEAR IN", "MODE", "RANGE", "FREEZE", "CLEAR"};
        const float positions[] = {24.f, 65.f, 106.f, 147.f, 198.f, 245.f, 296.f, 337.f};
        for (int index = 0; index < 8; ++index) {
            nvgText(args.vg, positions[index], 316.f, labels[index], NULL);
        }
        nvgFontSize(args.vg, 7.f);
        nvgTextLetterSpacing(args.vg, 2.f);
        nvgText(args.vg, box.size.x * 0.5f, 376.f, "CELLA", NULL);
        nvgRestore(args.vg);
    }
};

}  // namespace

struct WaterfallWidget : ModuleWidget {
    WaterfallWidget(Waterfall* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Waterfall.svg"),
                             asset::plugin(pluginInstance, "res/Waterfall-dark.svg")));

        WaterfallPanelLabels* panelLabels = new WaterfallPanelLabels;
        panelLabels->box.size = Vec(360.f, 380.f);
        addChild(panelLabels);

        WaterfallDisplay* display = new WaterfallDisplay;
        display->module = module;
        display->box.pos = Vec(DISPLAY_X, DISPLAY_Y);
        display->box.size = Vec(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        addChild(display);

        WaterfallBezel* bezel = new WaterfallBezel;
        bezel->box.pos = display->box.pos;
        bezel->box.size = display->box.size;
        addChild(bezel);

        WaterfallOverlay* overlay = new WaterfallOverlay;
        overlay->module = module;
        overlay->display = display;
        overlay->box.pos = display->box.pos;
        overlay->box.size = display->box.size;
        addChild(overlay);

        const float controlY = 340.f;
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(24.f, controlY), module, Waterfall::LEFT_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(65.f, controlY), module, Waterfall::RIGHT_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(106.f, controlY), module, Waterfall::FREEZE_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(147.f, controlY), module, Waterfall::CLEAR_INPUT));
        addParam(createParamCentered<RoundSmallBlackKnob>(Vec(198.f, controlY), module, Waterfall::MODE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(Vec(245.f, controlY), module, Waterfall::RANGE_PARAM));
        addParam(createParamCentered<LEDButton>(Vec(296.f, controlY), module, Waterfall::FREEZE_PARAM));
        addChild(createLightCentered<MediumLight<YellowLight>>(Vec(296.f, controlY), module, Waterfall::FREEZE_LIGHT));
        addParam(createParamCentered<VCVButton>(Vec(337.f, controlY), module, Waterfall::CLEAR_PARAM));
    }

    void appendContextMenu(Menu* menu) override {
        Waterfall* waterfall = dynamic_cast<Waterfall*>(module);
        if (!waterfall) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Analysis"));
        menu->addChild(createIndexSubmenuItem(
            "FFT size", {"1024", "2048", "4096", "8192", "16384"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->fftSizeSetting.load(), 0, 4)); },
            [=](size_t value) { waterfall->fftSizeSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "Window", {"Hann", "Blackman-Harris", "Flat-top"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->windowSetting.load(), 0, 2)); },
            [=](size_t value) { waterfall->windowSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "History rate", {"Economy · 15 rows/s", "Normal · 30 rows/s", "High · 60 rows/s"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->qualitySetting.load(), 0, 2)); },
            [=](size_t value) { waterfall->qualitySetting.store(static_cast<int>(value)); }));

        std::vector<std::string> channels;
        for (int channel = 1; channel <= 16; ++channel) channels.push_back(rack::string::f("Channel %d", channel));
        menu->addChild(createIndexSubmenuItem(
            "Polyphonic voice", channels,
            [=]() { return static_cast<size_t>(clampValue(waterfall->polyChannelSetting.load(), 0, 15)); },
            [=](size_t value) { waterfall->polyChannelSetting.store(static_cast<int>(value)); }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Display"));
        menu->addChild(createIndexSubmenuItem(
            "Flow", {"Up", "Down", "Left", "Right"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->flowSetting.load(), 0, 3)); },
            [=](size_t value) { waterfall->flowSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "Palette", {"Heat", "Grayscale", "Viridis (color-blind safe)"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->paletteSetting.load(), 0, 2)); },
            [=](size_t value) { waterfall->paletteSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "Peak trace", {"Off", "Decay", "Infinite hold"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->peakHoldSetting.load(), 0, 2)); },
            [=](size_t value) { waterfall->peakHoldSetting.store(static_cast<int>(value)); }));
        menu->addChild(createMenuItem("Reset frequency zoom", "", [=]() {
            waterfall->viewMinimum.store(0.f);
            waterfall->viewMaximum.store(1.f);
        }));
#ifndef NDEBUG
        menu->addChild(createMenuLabel(
            rack::string::f("Dropped display rows: %llu",
                            static_cast<unsigned long long>(waterfall->droppedRows.load()))));
#endif
    }
};

Model* modelWaterfall = createModel<Waterfall, WaterfallWidget>("Waterfall");

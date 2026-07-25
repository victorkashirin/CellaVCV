#include "plugin.hpp"
#include "waterfall/HistoryTimeline.hpp"
#include "waterfall/WaterfallAnalyzer.hpp"
#include "waterfall/WaterfallPalettes.hpp"
#include "waterfall/WaterfallPresentation.hpp"
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

constexpr float DISPLAY_X = 0.f;
constexpr float DISPLAY_Y = 26.f;
constexpr float DISPLAY_WIDTH = 360.f;
constexpr float DISPLAY_HEIGHT = 280.f;
constexpr float RANGE_DEFAULT_DB = -100.f;
constexpr float RANGE_MIN_DB = -140.f;
constexpr float RANGE_MAX_DB = -40.f;

int getJsonInt(json_t* root, const char* key, int minimum, int maximum, int fallback) {
    json_t* value = json_object_get(root, key);
    if (!json_is_integer(value)) return fallback;
    return clampValue(static_cast<int>(json_integer_value(value)), minimum, maximum);
}

float getJsonFloat(json_t* root, const char* key, float minimum, float maximum, float fallback) {
    json_t* value = json_object_get(root, key);
    if (!json_is_number(value)) return fallback;
    const float result = static_cast<float>(json_number_value(value));
    return std::isfinite(result) ? clampValue(result, minimum, maximum) : fallback;
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
    return rack::string::f("%s%d %+03dc", names[note], nearest / 12 - 1, cents);
}

std::string timeLabel(float age, float span) {
    if (age < 0.0005f) return span < 2.f ? "0 ms" : "0 s";
    if (span < 2.f) return rack::string::f("-%d ms", static_cast<int>(std::lround(age * 1000.f)));
    return rack::string::f("-%.1f s", age);
}

}  // namespace

struct Waterfall : Module {
    enum ParamIds { MODE_PARAM, RANGE_PARAM, FREEZE_PARAM, CLEAR_PARAM, NUM_PARAMS };
    enum InputIds { LEFT_INPUT, RIGHT_INPUT, FREEZE_INPUT, MARK_INPUT, CLEAR_INPUT, NUM_INPUTS };
    enum LightIds { FREEZE_LIGHT, NUM_LIGHTS };

    WaterfallAnalyzer analyzer;
    dsp::RingBuffer<SpectrumRow, ROW_QUEUE_SIZE> displayRows;
    dsp::RingBuffer<MarkerEvent, MARKER_QUEUE_SIZE> markerEvents;
    dsp::SchmittTrigger freezeButtonTrigger;
    dsp::SchmittTrigger freezeInputTrigger;
    dsp::SchmittTrigger markInputTrigger;
    dsp::SchmittTrigger clearButtonTrigger;
    dsp::SchmittTrigger clearInputTrigger;

    std::atomic<int> fftSizeSetting{static_cast<int>(FftSize::FFT_4096)};
    std::atomic<int> windowSetting{static_cast<int>(WindowFunction::HANN)};
    std::atomic<int> fftOverlapSetting{static_cast<int>(FftOverlap::PERCENT_75)};
    std::atomic<int> qualitySetting{static_cast<int>(Quality::NORMAL)};
    std::atomic<int> polyChannelSetting{0};
    std::atomic<int> paletteSetting{static_cast<int>(Palette::HEAT)};
    std::atomic<int> peakHoldSetting{static_cast<int>(PeakHold::DECAY)};
    std::atomic<int> flowSetting{static_cast<int>(FlowDirection::UP)};
    std::atomic<int> renderingStyleSetting{static_cast<int>(RenderingStyle::SMOOTH)};
    std::atomic<int> liveTraceSetting{static_cast<int>(LiveTraceMode::LINE)};
    std::atomic<int> frequencyScaleSetting{static_cast<int>(FrequencyScaleMode::COMBINED)};
    std::atomic<int> frequencySmoothingSetting{static_cast<int>(FrequencySmoothing::NONE)};
    std::atomic<int> temporalSmoothingSetting{static_cast<int>(TemporalSmoothing::OFF)};
    std::atomic<int> historyDurationSetting{static_cast<int>(HistoryDuration::SECONDS_8)};
    std::atomic<bool> showMarkersSetting{true};
    std::atomic<float> timeSpanSetting{8.f};
    std::atomic<float> viewMinimum{0.f};
    std::atomic<float> viewMaximum{1.f};
    std::atomic<bool> frozen{false};
    std::atomic<uint64_t> clearGeneration{0};
    std::atomic<uint64_t> activeConfigGeneration{1};
#ifndef NDEBUG
    std::atomic<uint64_t> droppedRows{0};
    std::atomic<uint64_t> droppedMarkers{0};
#endif

    int appliedFftSize = -1;
    int appliedWindow = -1;
    int appliedFftOverlap = -1;
    int appliedQuality = -1;
    int appliedChannelMode = -1;
    int appliedPolyChannel = -1;
    float appliedSampleRate = 0.f;
    uint64_t configGeneration = 1;
    uint64_t timelineSample = 0;
    uint32_t markerSequence = 0;

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
        configInput(MARK_INPUT, "Marker trigger");
        configInput(CLEAR_INPUT, "Clear trigger");
    }

    void process(const ProcessArgs& args) override {
        const int fftSize =
            clampValue(fftSizeSetting.load(std::memory_order_relaxed), 0, static_cast<int>(FftSize::COUNT) - 1);
        const int window =
            clampValue(windowSetting.load(std::memory_order_relaxed), 0, static_cast<int>(WindowFunction::COUNT) - 1);
        const int fftOverlap = clampValue(fftOverlapSetting.load(std::memory_order_relaxed), 0,
                                          static_cast<int>(FftOverlap::COUNT) - 1);
        const int quality =
            clampValue(qualitySetting.load(std::memory_order_relaxed), 0, static_cast<int>(Quality::COUNT) - 1);
        const int channelMode = clampValue(static_cast<int>(std::lround(params[MODE_PARAM].getValue())), 0,
                                           static_cast<int>(ChannelMode::COUNT) - 1);
        const int polyChannel = clampValue(polyChannelSetting.load(std::memory_order_relaxed), 0, 15);
        const bool sampleRateChanged = appliedSampleRate != 0.f && args.sampleRate != appliedSampleRate;
        const bool analysisChanged =
            fftSize != appliedFftSize || window != appliedWindow || fftOverlap != appliedFftOverlap ||
            channelMode != appliedChannelMode || polyChannel != appliedPolyChannel || sampleRateChanged;
        const bool qualityChanged = quality != appliedQuality;
        if (analysisChanged || qualityChanged || appliedSampleRate == 0.f) {
            if (analysisChanged) {
                ++configGeneration;
                if (sampleRateChanged) timelineSample = 0;
            }
            WaterfallConfig next;
            next.fftSize = static_cast<FftSize>(fftSize);
            next.window = static_cast<WindowFunction>(window);
            next.fftOverlap = static_cast<FftOverlap>(fftOverlap);
            next.quality = static_cast<Quality>(quality);
            next.channelMode = static_cast<ChannelMode>(channelMode);
            next.polyChannel = polyChannel;
            next.sampleRate = args.sampleRate;
            next.generation = configGeneration;
            analyzer.configure(next);
            activeConfigGeneration.store(configGeneration, std::memory_order_release);
            appliedFftSize = fftSize;
            appliedWindow = window;
            appliedFftOverlap = fftOverlap;
            appliedQuality = quality;
            appliedChannelMode = channelMode;
            appliedPolyChannel = polyChannel;
            appliedSampleRate = args.sampleRate;
        }

        ++timelineSample;
        const bool freezeEvent = freezeButtonTrigger.process(params[FREEZE_PARAM].getValue()) ||
                                 freezeInputTrigger.process(inputs[FREEZE_INPUT].getVoltage());
        if (freezeEvent) frozen.store(!frozen.load(std::memory_order_relaxed), std::memory_order_relaxed);
        const bool clearEvent = clearButtonTrigger.process(params[CLEAR_PARAM].getValue()) ||
                                clearInputTrigger.process(inputs[CLEAR_INPUT].getVoltage());
        if (clearEvent) clearGeneration.fetch_add(1, std::memory_order_release);
        if (markInputTrigger.process(inputs[MARK_INPUT].getVoltage())) {
            MarkerEvent marker;
            marker.timelineSample = timelineSample;
            marker.sampleRate = args.sampleRate;
            marker.configGeneration = configGeneration;
            marker.sequence = ++markerSequence;
            if (!markerEvents.full())
                markerEvents.push(marker);
#ifndef NDEBUG
            else
                droppedMarkers.fetch_add(1, std::memory_order_relaxed);
#endif
        }
        lights[FREEZE_LIGHT].setBrightness(frozen.load(std::memory_order_relaxed) ? 1.f : 0.f);

        const bool leftConnected = inputs[LEFT_INPUT].isConnected();
        const bool rightConnected = inputs[RIGHT_INPUT].isConnected();
        const float left = leftConnected ? inputs[LEFT_INPUT].getVoltage(polyChannel) : 0.f;
        const float right = rightConnected ? inputs[RIGHT_INPUT].getVoltage(polyChannel) : 0.f;
        const float mixed =
            mixInputVoltages(left, right, leftConnected, rightConnected, static_cast<ChannelMode>(channelMode));
        SpectrumRow row;
        if (analyzer.processSample(mixed * VOLTAGE_TO_FULL_SCALE, timelineSample, row)) {
            if (!displayRows.full())
                displayRows.push(row);
#ifndef NDEBUG
            else
                droppedRows.fetch_add(1, std::memory_order_relaxed);
#endif
        }
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "fftSize", json_integer(fftSizeSetting.load()));
        json_object_set_new(root, "window", json_integer(windowSetting.load()));
        json_object_set_new(root, "fftOverlap", json_integer(fftOverlapSetting.load()));
        json_object_set_new(root, "quality", json_integer(qualitySetting.load()));
        json_object_set_new(root, "polyChannel", json_integer(polyChannelSetting.load()));
        json_object_set_new(root, "palette", json_integer(paletteSetting.load()));
        json_object_set_new(root, "peakHold", json_integer(peakHoldSetting.load()));
        json_object_set_new(root, "flow", json_integer(flowSetting.load()));
        json_object_set_new(root, "renderingStyle", json_integer(renderingStyleSetting.load()));
        json_object_set_new(root, "liveTrace", json_integer(liveTraceSetting.load()));
        json_object_set_new(root, "frequencyScale", json_integer(frequencyScaleSetting.load()));
        json_object_set_new(root, "frequencySmoothing", json_integer(frequencySmoothingSetting.load()));
        json_object_set_new(root, "temporalSmoothing", json_integer(temporalSmoothingSetting.load()));
        json_object_set_new(root, "historyDuration", json_integer(historyDurationSetting.load()));
        json_object_set_new(root, "showMarkers", json_boolean(showMarkersSetting.load()));
        json_object_set_new(root, "timeSpan", json_real(timeSpanSetting.load()));
        json_object_set_new(root, "viewMinimum", json_real(viewMinimum.load()));
        json_object_set_new(root, "viewMaximum", json_real(viewMaximum.load()));
        return root;
    }

    void dataFromJson(json_t* root) override {
        fftSizeSetting.store(getJsonInt(root, "fftSize", 0, static_cast<int>(FftSize::COUNT) - 1,
                                        static_cast<int>(FftSize::FFT_4096)));
        windowSetting.store(getJsonInt(root, "window", 0, static_cast<int>(WindowFunction::COUNT) - 1,
                                       static_cast<int>(WindowFunction::HANN)));
        fftOverlapSetting.store(getJsonInt(root, "fftOverlap", 0, static_cast<int>(FftOverlap::COUNT) - 1,
                                           static_cast<int>(FftOverlap::PERCENT_75)));
        qualitySetting.store(getJsonInt(root, "quality", 0, static_cast<int>(Quality::COUNT) - 1,
                                        static_cast<int>(Quality::NORMAL)));
        polyChannelSetting.store(getJsonInt(root, "polyChannel", 0, 15, 0));
        paletteSetting.store(
            getJsonInt(root, "palette", 0, static_cast<int>(Palette::COUNT) - 1, static_cast<int>(Palette::HEAT)));
        peakHoldSetting.store(getJsonInt(root, "peakHold", 0, static_cast<int>(PeakHold::COUNT) - 1,
                                         static_cast<int>(PeakHold::DECAY)));
        flowSetting.store(getJsonInt(root, "flow", 0, static_cast<int>(FlowDirection::COUNT) - 1,
                                     static_cast<int>(FlowDirection::UP)));
        renderingStyleSetting.store(getJsonInt(root, "renderingStyle", 0, static_cast<int>(RenderingStyle::COUNT) - 1,
                                               static_cast<int>(RenderingStyle::SMOOTH)));
        liveTraceSetting.store(getJsonInt(root, "liveTrace", 0, static_cast<int>(LiveTraceMode::COUNT) - 1,
                                         static_cast<int>(LiveTraceMode::LINE)));
        frequencyScaleSetting.store(getJsonInt(root, "frequencyScale", 0,
                                               static_cast<int>(FrequencyScaleMode::COUNT) - 1,
                                               static_cast<int>(FrequencyScaleMode::COMBINED)));
        frequencySmoothingSetting.store(getJsonInt(root, "frequencySmoothing", 0,
                                                   static_cast<int>(FrequencySmoothing::COUNT) - 1,
                                                   static_cast<int>(FrequencySmoothing::NONE)));
        temporalSmoothingSetting.store(getJsonInt(root, "temporalSmoothing", 0,
                                                  static_cast<int>(TemporalSmoothing::COUNT) - 1,
                                                  static_cast<int>(TemporalSmoothing::OFF)));
        historyDurationSetting.store(getJsonInt(root, "historyDuration", 0,
                                                static_cast<int>(HistoryDuration::COUNT) - 1,
                                                static_cast<int>(HistoryDuration::SECONDS_8)));
        json_t* showMarkers = json_object_get(root, "showMarkers");
        showMarkersSetting.store(json_is_boolean(showMarkers) ? json_is_true(showMarkers) : true);
        const int duration = historyDurationSeconds(static_cast<HistoryDuration>(historyDurationSetting.load()));
        timeSpanSetting.store(getJsonFloat(root, "timeSpan", 0.25f, static_cast<float>(duration),
                                           static_cast<float>(duration)));
        float minimum = getJsonFloat(root, "viewMinimum", 0.f, 0.99f, 0.f);
        float maximum = getJsonFloat(root, "viewMaximum", 0.01f, 1.f, 1.f);
        if (maximum - minimum < 0.01f) {
            minimum = 0.f;
            maximum = 1.f;
        }
        viewMinimum.store(minimum);
        viewMaximum.store(maximum);
        frozen.store(false);
        clearGeneration.fetch_add(1, std::memory_order_release);
    }
};

namespace {

struct WaterfallRenderer {
    GLuint program = 0;
    GLuint historyTexture = 0;
    GLuint traceTexture = 0;
    GLuint lookupTexture = 0;
    GLuint paletteTexture = 0;
    int allocatedRows = 0;
    int uploadedPalette = -1;
    GLint historyLocation = -1;
    GLint traceLocation = -1;
    GLint lookupLocation = -1;
    GLint rowsLocation = -1;
    GLint flowLocation = -1;
    GLint viewLocation = -1;
    GLint rangeLocation = -1;
    GLint paletteLocation = -1;
    GLint peakHoldLocation = -1;
    GLint liveTraceLocation = -1;
    GLint styleLocation = -1;
    GLint logicalPixelLocation = -1;
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

    bool initialize(int requestedRows) {
        if (!program) {
            if (initializationAttempted) return false;
            initializationAttempted = true;
            try {
                GLuint vertex = compile(GL_VERTEX_SHADER, loadResource("res/shaders/waterfall_gl.vert"), "vertex");
                GLuint fragment =
                    compile(GL_FRAGMENT_SHADER, loadResource("res/shaders/waterfall_gl.frag"), "fragment");
                if (!vertex || !fragment) return false;
                program = glCreateProgram();
                glAttachShader(program, vertex);
                glAttachShader(program, fragment);
                glLinkProgram(program);
                glDeleteShader(vertex);
                glDeleteShader(fragment);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked != GL_TRUE) {
                    glDeleteProgram(program);
                    program = 0;
                    return false;
                }
                historyLocation = glGetUniformLocation(program, "uHistory");
                traceLocation = glGetUniformLocation(program, "uTrace");
                lookupLocation = glGetUniformLocation(program, "uTimeLookup");
                rowsLocation = glGetUniformLocation(program, "uRows");
                flowLocation = glGetUniformLocation(program, "uFlow");
                viewLocation = glGetUniformLocation(program, "uView");
                rangeLocation = glGetUniformLocation(program, "uRange");
                paletteLocation = glGetUniformLocation(program, "uPalette");
                peakHoldLocation = glGetUniformLocation(program, "uPeakHold");
                liveTraceLocation = glGetUniformLocation(program, "uLiveTrace");
                styleLocation = glGetUniformLocation(program, "uRenderingStyle");
                logicalPixelLocation = glGetUniformLocation(program, "uLogicalPixel");
                glGenTextures(1, &historyTexture);
                glGenTextures(1, &traceTexture);
                glGenTextures(1, &lookupTexture);
                glGenTextures(1, &paletteTexture);

                glBindTexture(GL_TEXTURE_2D, traceTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, NUM_FREQUENCY_CELLS, 1, 0, GL_RGBA,
                             GL_UNSIGNED_SHORT, NULL);

                glBindTexture(GL_TEXTURE_2D, lookupTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TIME_LOOKUP_SIZE, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

                glBindTexture(GL_TEXTURE_2D, paletteTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, PALETTE_LUT_SIZE, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            } catch (const std::exception& exception) {
                WARN("Waterfall shader resources could not be loaded: %s", exception.what());
                return false;
            }
        }
        if (allocatedRows != requestedRows) {
            glBindTexture(GL_TEXTURE_2D, historyTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8, NUM_FREQUENCY_CELLS, requestedRows, 0, GL_LUMINANCE,
                         GL_UNSIGNED_BYTE, NULL);
            allocatedRows = requestedRows;
        }
        return program && historyTexture && traceTexture && lookupTexture && paletteTexture;
    }

    void uploadPalette(Palette palette) {
        const int index = clampValue(static_cast<int>(palette), 0, static_cast<int>(Palette::COUNT) - 1);
        if (uploadedPalette == index) return;
        const std::array<unsigned char, PALETTE_LUT_SIZE * 4> lut =
            buildPaletteLut(static_cast<Palette>(index));
        glBindTexture(GL_TEXTURE_2D, paletteTexture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PALETTE_LUT_SIZE, 1, GL_RGBA, GL_UNSIGNED_BYTE, lut.data());
        uploadedPalette = index;
    }

    void destroy() {
        if (paletteTexture) glDeleteTextures(1, &paletteTexture);
        if (lookupTexture) glDeleteTextures(1, &lookupTexture);
        if (traceTexture) glDeleteTextures(1, &traceTexture);
        if (historyTexture) glDeleteTextures(1, &historyTexture);
        if (program) glDeleteProgram(program);
        program = historyTexture = traceTexture = lookupTexture = paletteTexture = 0;
        allocatedRows = 0;
        uploadedPalette = -1;
        initializationAttempted = false;
    }
};

struct WaterfallDisplay : widget::OpenGlWidget {
    Waterfall* module = NULL;
    WaterfallRenderer renderer;
    HistoryTimeline timeline;
    FrequencySmoothingKernel frequencyKernel;
    TemporalPowerSmoother temporalSmoother;
    std::vector<SpectrumRow> derivedRows;
    std::vector<bool> dirtyRows;
    std::array<float, NUM_FREQUENCY_CELLS> currentTrace;
    std::array<float, NUM_FREQUENCY_CELLS> peakTrace;
    uint64_t seenClearGeneration = 0;
    uint64_t seenModuleConfigGeneration = 0;
    int appliedCapacity = 0;
    int appliedFrequencySmoothing = -1;
    int appliedTemporalSmoothing = -1;
    int appliedPeakHold = -1;
    float appliedSmoothingSampleRate = 48000.f;
    bool traceDirty = true;
    bool lookupDirty = true;
    SpectrumRow latestMetadata;

    WaterfallDisplay() {
        currentTrace.fill(INTERNAL_FLOOR_DB);
        peakTrace.fill(INTERNAL_FLOOR_DB);
        frequencyKernel.configure(FrequencySmoothing::NONE);
        temporalSmoother.configure(TemporalSmoothing::OFF);
        resizeCaches(timeline.capacity());
    }

    void resizeCaches(int capacity) {
        derivedRows.resize(static_cast<size_t>(capacity));
        dirtyRows.assign(static_cast<size_t>(capacity), true);
        appliedCapacity = capacity;
        lookupDirty = true;
    }

    void onContextCreate(const ContextCreateEvent& event) override {
        widget::OpenGlWidget::onContextCreate(event);
        renderer = WaterfallRenderer();
        dirtyRows.assign(dirtyRows.size(), true);
        traceDirty = lookupDirty = true;
    }

    void onContextDestroy(const ContextDestroyEvent& event) override {
        renderer.destroy();
        widget::OpenGlWidget::onContextDestroy(event);
    }

    void clearHistory() {
        timeline.clear();
        temporalSmoother.reset();
        latestMetadata = SpectrumRow();
        currentTrace.fill(INTERNAL_FLOOR_DB);
        peakTrace.fill(INTERNAL_FLOOR_DB);
        for (size_t i = 0; i < derivedRows.size(); ++i) {
            derivedRows[i] = SpectrumRow();
            derivedRows[i].dbTenths.fill(quantizeDb(INTERNAL_FLOOR_DB));
        }
        dirtyRows.assign(dirtyRows.size(), true);
        traceDirty = lookupDirty = true;
    }

    void rebuildDerived() {
        temporalSmoother.configure(static_cast<TemporalSmoothing>(appliedTemporalSmoothing));
        currentTrace.fill(INTERNAL_FLOOR_DB);
        peakTrace.fill(INTERNAL_FLOOR_DB);
        const SpectrumRow* previousRaw = NULL;
        for (int ordered = 0; ordered < timeline.size(); ++ordered) {
            const int physical = timeline.physicalFromOldest(ordered);
            const SpectrumRow* raw = timeline.physicalRow(physical);
            SpectrumRow frequencySmoothed;
            frequencyKernel.apply(*raw, frequencySmoothed);
            temporalSmoother.process(frequencySmoothed, derivedRows[static_cast<size_t>(physical)]);
            float elapsed = 0.f;
            if (previousRaw && previousRaw->sampleRate == raw->sampleRate &&
                raw->rowEndSample >= previousRaw->rowEndSample)
                elapsed = static_cast<float>(raw->rowEndSample - previousRaw->rowEndSample) / raw->sampleRate;
            updateTraces(derivedRows[static_cast<size_t>(physical)], elapsed);
            previousRaw = raw;
        }
        dirtyRows.assign(dirtyRows.size(), true);
        traceDirty = lookupDirty = true;
    }

    void syncSettings() {
        const int quality = clampValue(module ? module->qualitySetting.load() : 1, 0, 2);
        const int durationIndex =
            clampValue(module ? module->historyDurationSetting.load() : 2, 0,
                       static_cast<int>(HistoryDuration::COUNT) - 1);
        const float retained =
            static_cast<float>(historyDurationSeconds(static_cast<HistoryDuration>(durationIndex)));
        int desired = historyRowCapacity(retained, rowsPerSecond(static_cast<Quality>(quality)));
        GLint maximumTexture = 2048;
        if (renderer.program) glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTexture);
        desired = std::min(desired, std::max(static_cast<int>(maximumTexture), 4));
        timeline.setExpectedRowsPerSecond(rowsPerSecond(static_cast<Quality>(quality)));
        timeline.setRetainedDuration(std::min(retained, (desired - 2.f) / rowsPerSecond(static_cast<Quality>(quality))));
        if (desired != timeline.capacity()) {
            timeline.setCapacity(desired);
            resizeCaches(desired);
            renderer.allocatedRows = 0;
            rebuildDerived();
        }
        if (module) {
            timeline.setVisibleSpan(module->timeSpanSetting.load());
            module->timeSpanSetting.store(timeline.visibleSpan());
        }
        const int frequencyMode =
            clampValue(module ? module->frequencySmoothingSetting.load() : 0, 0,
                       static_cast<int>(FrequencySmoothing::COUNT) - 1);
        const int temporalMode =
            clampValue(module ? module->temporalSmoothingSetting.load() : 0, 0,
                       static_cast<int>(TemporalSmoothing::COUNT) - 1);
        if (frequencyMode != appliedFrequencySmoothing || temporalMode != appliedTemporalSmoothing) {
            appliedFrequencySmoothing = frequencyMode;
            appliedTemporalSmoothing = temporalMode;
            frequencyKernel.configure(static_cast<FrequencySmoothing>(frequencyMode), appliedSmoothingSampleRate);
            rebuildDerived();
        }
        const int peak =
            clampValue(module ? module->peakHoldSetting.load() : 1, 0, static_cast<int>(PeakHold::COUNT) - 1);
        if (peak != appliedPeakHold) {
            appliedPeakHold = peak;
            rebuildDerived();
        }
    }

    void updateTraces(const SpectrumRow& row, float elapsedSeconds) {
        const PeakHold peakMode = static_cast<PeakHold>(
            clampValue(module ? module->peakHoldSetting.load() : static_cast<int>(PeakHold::DECAY), 0,
                       static_cast<int>(PeakHold::COUNT) - 1));
        for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
            const float value = dequantizeDb(row.dbTenths[static_cast<size_t>(cell)]);
            currentTrace[static_cast<size_t>(cell)] = value;
            if (peakMode == PeakHold::OFF)
                peakTrace[static_cast<size_t>(cell)] = INTERNAL_FLOOR_DB;
            else if (peakMode == PeakHold::INFINITE)
                peakTrace[static_cast<size_t>(cell)] = std::max(peakTrace[static_cast<size_t>(cell)], value);
            else
                peakTrace[static_cast<size_t>(cell)] =
                    std::max(value, peakTrace[static_cast<size_t>(cell)] - 12.f * elapsedSeconds);
        }
        traceDirty = true;
    }

    void addRow(const SpectrumRow& row) {
        if (row.sampleRate != appliedSmoothingSampleRate) {
            appliedSmoothingSampleRate = row.sampleRate;
            frequencyKernel.configure(static_cast<FrequencySmoothing>(
                                          std::max(appliedFrequencySmoothing, 0)),
                                      appliedSmoothingSampleRate);
            if (!timeline.empty()) rebuildDerived();
        }
        float elapsed = 0.f;
        if (const SpectrumRow* newest = timeline.newestRow()) {
            if (newest->sampleRate == row.sampleRate && row.rowEndSample >= newest->rowEndSample)
                elapsed = static_cast<float>(row.rowEndSample - newest->rowEndSample) / row.sampleRate;
        }
        const int physical = timeline.addRow(row);
        SpectrumRow frequencySmoothed;
        frequencyKernel.apply(row, frequencySmoothed);
        temporalSmoother.process(frequencySmoothed, derivedRows[static_cast<size_t>(physical)]);
        updateTraces(derivedRows[static_cast<size_t>(physical)], elapsed);
        latestMetadata = row;
        dirtyRows[static_cast<size_t>(physical)] = true;
        lookupDirty = true;
    }

    void drainQueues() {
        if (!module) return;
        syncSettings();
        const uint64_t clear = module->clearGeneration.load(std::memory_order_acquire);
        if (clear != seenClearGeneration) {
            seenClearGeneration = clear;
            clearHistory();
            while (!module->displayRows.empty()) module->displayRows.shift();
            while (!module->markerEvents.empty()) module->markerEvents.shift();
            return;
        }
        const uint64_t generation = module->activeConfigGeneration.load(std::memory_order_acquire);
        if (seenModuleConfigGeneration != 0 && generation != seenModuleConfigGeneration) clearHistory();
        seenModuleConfigGeneration = generation;
        const bool frozen = module->frozen.load(std::memory_order_relaxed);
        while (!module->displayRows.empty()) {
            const SpectrumRow row = module->displayRows.shift();
            if (!frozen && row.configGeneration == generation) addRow(row);
        }
        while (!module->markerEvents.empty()) {
            const MarkerEvent marker = module->markerEvents.shift();
            if (marker.configGeneration == generation) timeline.addMarker(marker);
        }
    }

    void seedPreview() {
        if (!timeline.empty()) return;
        for (int index = 0; index < timeline.capacity(); ++index) {
            SpectrumRow row;
            row.sampleRate = 48000.f;
            row.fftSize = 4096;
            row.effectiveHopSize = 1024;
            row.displayRowsPerSecond = 30;
            row.configGeneration = 1;
            row.rowEndSample = static_cast<uint64_t>((index + 1) * 1600);
            row.sourceAnalysisSample = row.rowEndSample;
            for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
                const float x = static_cast<float>(cell) / (NUM_FREQUENCY_CELLS - 1);
                const float ridge = 0.20f + 0.22f * std::sin(index * 0.055f);
                const float ridge2 = 0.64f + 0.08f * std::sin(index * 0.031f);
                const float energy = std::max(std::exp(-900.f * (x - ridge) * (x - ridge)),
                                              0.7f * std::exp(-500.f * (x - ridge2) * (x - ridge2)));
                row.dbTenths[static_cast<size_t>(cell)] = quantizeDb(-105.f + energy * 91.f);
            }
            addRow(row);
        }
    }

    bool sampleAt(float fullFrequency, float age, float& displayedDb, float& rawDb, SpectrumRow& metadata) const {
        const TimelineSelection selected = timeline.lookup(age);
        if (!selected.valid) return false;
        const int cell = clampValue(static_cast<int>(std::lround(fullFrequency * (NUM_FREQUENCY_CELLS - 1))), 0,
                                    NUM_FREQUENCY_CELLS - 1);
        const SpectrumRow* rawOlder = timeline.physicalRow(selected.olderPhysical);
        const SpectrumRow* rawNewer = timeline.physicalRow(selected.newerPhysical);
        const SpectrumRow& shownOlder = derivedRows[static_cast<size_t>(selected.olderPhysical)];
        const SpectrumRow& shownNewer = derivedRows[static_cast<size_t>(selected.newerPhysical)];
        rawDb = interpolateNoOvershoot(dequantizeDb(rawOlder->dbTenths[static_cast<size_t>(cell)]),
                                       dequantizeDb(rawNewer->dbTenths[static_cast<size_t>(cell)]),
                                       selected.interpolationValid ? selected.fraction : 0.f);
        displayedDb = interpolateNoOvershoot(dequantizeDb(shownOlder.dbTenths[static_cast<size_t>(cell)]),
                                             dequantizeDb(shownNewer.dbTenths[static_cast<size_t>(cell)]),
                                             selected.interpolationValid ? selected.fraction : 0.f);
        metadata = selected.fraction < 0.5f ? *rawOlder : *rawNewer;
        return true;
    }

    void uploadDirtyData() {
        std::array<unsigned char, NUM_FREQUENCY_CELLS> rowBytes;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, renderer.historyTexture);
        for (int row = 0; row < timeline.capacity(); ++row) {
            if (!dirtyRows[static_cast<size_t>(row)]) continue;
            for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell)
                rowBytes[static_cast<size_t>(cell)] =
                    encodeDb(dequantizeDb(derivedRows[static_cast<size_t>(row)].dbTenths[static_cast<size_t>(cell)]));
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, NUM_FREQUENCY_CELLS, 1, GL_LUMINANCE, GL_UNSIGNED_BYTE,
                            rowBytes.data());
            dirtyRows[static_cast<size_t>(row)] = false;
        }
        if (traceDirty) {
            std::array<unsigned short, NUM_FREQUENCY_CELLS * 4> traceBytes;
            for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
                const size_t offset = static_cast<size_t>(cell * 4);
                traceBytes[offset] = encodeDb16(currentTrace[static_cast<size_t>(cell)]);
                traceBytes[offset + 1] = encodeDb16(peakTrace[static_cast<size_t>(cell)]);
                traceBytes[offset + 2] = 0;
                traceBytes[offset + 3] = 65535;
            }
            glBindTexture(GL_TEXTURE_2D, renderer.traceTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NUM_FREQUENCY_CELLS, 1, GL_RGBA, GL_UNSIGNED_SHORT,
                            traceBytes.data());
            traceDirty = false;
        }
        if (lookupDirty) {
            const std::array<unsigned char, TIME_LOOKUP_SIZE * 4> lookup = timeline.buildLookup();
            glBindTexture(GL_TEXTURE_2D, renderer.lookupTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TIME_LOOKUP_SIZE, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                            lookup.data());
            lookupDirty = false;
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

    void drawFramebuffer() override {
        if (module)
            drainQueues();
        else
            seedPreview();

        GLint oldProgram = 0, oldActiveTexture = 0, oldTexture0 = 0, oldTexture1 = 0, oldTexture2 = 0,
              oldTexture3 = 0;
        GLint oldUnpackAlignment = 4;
        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
        glActiveTexture(GL_TEXTURE1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture1);
        glActiveTexture(GL_TEXTURE2);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture2);
        glActiveTexture(GL_TEXTURE3);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture3);
        glPushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT | GL_VIEWPORT_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT);
        const math::Vec framebuffer = getFramebufferSize();
        glViewport(0, 0, static_cast<GLsizei>(framebuffer.x), static_cast<GLsizei>(framebuffer.y));
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);

        if (renderer.initialize(timeline.capacity())) {
            if (renderer.allocatedRows == timeline.capacity() && dirtyRows.size() != static_cast<size_t>(timeline.capacity()))
                resizeCaches(timeline.capacity());
            uploadDirtyData();
            const int palette =
                clampValue(module ? module->paletteSetting.load() : static_cast<int>(Palette::HEAT), 0,
                           static_cast<int>(Palette::COUNT) - 1);
            renderer.uploadPalette(static_cast<Palette>(palette));
            glUseProgram(renderer.program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, renderer.historyTexture);
            glUniform1i(renderer.historyLocation, 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, renderer.traceTexture);
            glUniform1i(renderer.traceLocation, 1);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, renderer.lookupTexture);
            glUniform1i(renderer.lookupLocation, 2);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, renderer.paletteTexture);
            glUniform1i(renderer.paletteLocation, 3);
            glUniform1f(renderer.rowsLocation, static_cast<float>(timeline.capacity()));
            glUniform1i(renderer.flowLocation, module ? clampValue(module->flowSetting.load(), 0, 3) : 0);
            glUniform2f(renderer.viewLocation, module ? module->viewMinimum.load() : 0.f,
                        module ? module->viewMaximum.load() : 1.f);
            glUniform2f(renderer.rangeLocation,
                        module ? module->params[Waterfall::RANGE_PARAM].getValue() : RANGE_DEFAULT_DB, 0.f);
            glUniform1i(renderer.peakHoldLocation, module ? clampValue(module->peakHoldSetting.load(), 0, 2) : 1);
            glUniform1i(renderer.liveTraceLocation, module ? clampValue(module->liveTraceSetting.load(), 0, 2) : 1);
            glUniform1i(renderer.styleLocation,
                        module ? clampValue(module->renderingStyleSetting.load(), 0, 1) : 1);
            glUniform2f(renderer.logicalPixelLocation, 1.f / std::max(framebuffer.x, 1.f),
                        1.f / std::max(framebuffer.y, 1.f));
            drawQuad();
        } else {
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
        glPopAttrib();
        glUseProgram(static_cast<GLuint>(oldProgram));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture1));
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture2));
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture3));
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
        glPixelStorei(GL_UNPACK_ALIGNMENT, oldUnpackAlignment);
    }

    void drawPreviewNanoVg(const DrawArgs& args) {
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
        nvgFillPaint(args.vg, nvgLinearGradient(args.vg, 0.f, 0.f, box.size.x, box.size.y,
                                                nvgRGB(40, 5, 52), nvgRGB(2, 4, 7)));
        nvgFill(args.vg);
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
    bool draggingTime = false;
    math::Vec cursor;

    FlowDirection flow() const {
        return static_cast<FlowDirection>(
            clampValue(module ? module->flowSetting.load() : 0, 0, static_cast<int>(FlowDirection::COUNT) - 1));
    }
    float viewLow() const { return module ? module->viewMinimum.load() : 0.f; }
    float viewHigh() const { return module ? module->viewMaximum.load() : 1.f; }
    LogicalPoint logicalAt(math::Vec position) const {
        return logicalFromScreen(flow(), position.x / std::max(box.size.x, 1.f),
                                 1.f - position.y / std::max(box.size.y, 1.f));
    }
    float fullFrequencyCoordinate(const LogicalPoint& logical) const {
        return viewLow() + logical.frequency * (viewHigh() - viewLow());
    }
    float nyquist() const {
        return display && display->latestMetadata.sampleRate > 0.f ? display->latestMetadata.sampleRate * 0.5f
                                                                  : 24000.f;
    }
    float frequencyFromFullCoordinate(float coordinate) const {
        return MIN_FREQUENCY_HZ * std::pow(nyquist() / MIN_FREQUENCY_HZ, clampValue(coordinate, 0.f, 1.f));
    }
    bool inTimeGutter(math::Vec position) const {
        return isVerticalFlow(flow()) ? position.x < 40.f : position.y > box.size.y - 18.f;
    }

    void drawFrequencyGuide(const DrawArgs& args, float frequency, const std::string& label, bool secondary,
                            float& lastLabelPosition) {
        if (frequency < MIN_FREQUENCY_HZ || frequency > nyquist()) return;
        const float full = std::log(frequency / MIN_FREQUENCY_HZ) / std::log(nyquist() / MIN_FREQUENCY_HZ);
        const float visible = (full - viewLow()) / (viewHigh() - viewLow());
        if (visible < 0.f || visible > 1.f) return;
        const float position = visible * (isVerticalFlow(flow()) ? box.size.x : box.size.y);
        nvgStrokeColor(args.vg, secondary ? nvgRGBA(155, 184, 194, 24) : nvgRGBA(190, 205, 210, 44));
        nvgStrokeWidth(args.vg, secondary ? 0.45f : 0.7f);
        nvgBeginPath(args.vg);
        if (isVerticalFlow(flow())) {
            nvgMoveTo(args.vg, position, 0.f);
            nvgLineTo(args.vg, position, box.size.y);
        } else {
            const float y = box.size.y - position;
            nvgMoveTo(args.vg, 0.f, y);
            nvgLineTo(args.vg, box.size.x, y);
        }
        nvgStroke(args.vg);
        if (label.empty() || std::fabs(position - lastLabelPosition) < 31.f) return;
        lastLabelPosition = position;
        nvgFillColor(args.vg, secondary ? nvgRGBA(180, 198, 204, 95) : nvgRGBA(190, 205, 210, 155));
        nvgFontSize(args.vg, 8.f);
        if (isVerticalFlow(flow())) {
            float labelX = position;
            int alignment = NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM;
            if (position < 15.f) {
                labelX = 2.f;
                alignment = NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM;
            } else if (position > box.size.x - 15.f) {
                labelX = box.size.x - 2.f;
                alignment = NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM;
            }
            nvgTextAlign(args.vg, alignment);
            nvgText(args.vg, labelX, box.size.y - 2.f, label.c_str(), NULL);
        } else {
            float labelY = box.size.y - position;
            int alignment = NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE;
            if (labelY < 7.f) {
                labelY = 2.f;
                alignment = NVG_ALIGN_RIGHT | NVG_ALIGN_TOP;
            } else if (labelY > box.size.y - 7.f) {
                labelY = box.size.y - 2.f;
                alignment = NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM;
            }
            nvgTextAlign(args.vg, alignment);
            nvgText(args.vg, box.size.x - 3.f, labelY, label.c_str(), NULL);
        }
    }

    void drawGrid(const DrawArgs& args) {
        if (!(nyquist() > MIN_FREQUENCY_HZ) || viewHigh() <= viewLow()) return;
        const FrequencyScaleMode scale = static_cast<FrequencyScaleMode>(
            clampValue(module ? module->frequencyScaleSetting.load() : 3, 0,
                       static_cast<int>(FrequencyScaleMode::COUNT) - 1));
        static const float hzGuides[] = {20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f};
        static const float octaveGuides[] = {31.25f, 62.5f, 125.f, 250.f, 500.f,
                                            1000.f, 2000.f, 4000.f, 8000.f, 16000.f};
        float last = -1000.f;
        if (scale == FrequencyScaleMode::HZ || scale == FrequencyScaleMode::COMBINED) {
            for (size_t i = 0; i < sizeof(hzGuides) / sizeof(hzGuides[0]); ++i)
                drawFrequencyGuide(args, hzGuides[i], frequencyLabel(hzGuides[i]), false, last);
        }
        if (scale == FrequencyScaleMode::OCTAVES || scale == FrequencyScaleMode::COMBINED) {
            if (scale == FrequencyScaleMode::OCTAVES) last = -1000.f;
            for (size_t i = 0; i < sizeof(octaveGuides) / sizeof(octaveGuides[0]); ++i)
                drawFrequencyGuide(args, octaveGuides[i],
                                   scale == FrequencyScaleMode::OCTAVES ? frequencyLabel(octaveGuides[i]) : "",
                                   scale == FrequencyScaleMode::COMBINED, last);
        }
        if (scale == FrequencyScaleMode::MUSICAL) {
            const float frequencyAxisPixels = isVerticalFlow(flow()) ? box.size.x : box.size.y;
            const float semitonePixels = frequencyAxisPixels / std::max((viewHigh() - viewLow()) *
                                                                            std::log2(nyquist() / MIN_FREQUENCY_HZ) *
                                                                            12.f,
                                                                        1.f);
            for (int midi = 12; midi <= 132; ++midi) {
                if (midi % 12 != 0 && semitonePixels < 18.f) continue;
                const float frequency = 440.f * std::pow(2.f, (midi - 69) / 12.f);
                std::string label;
                if (midi % 12 == 0) label = rack::string::f("C%d", midi / 12 - 1);
                drawFrequencyGuide(args, frequency, label, midi % 12 != 0, last);
            }
        }
        drawTimeRuler(args);
    }

    void drawTimeRuler(const DrawArgs& args) {
        if (!display) return;
        const float pixels = isVerticalFlow(flow()) ? box.size.y : box.size.x;
        const std::vector<TimeTick> ticks = display->timeline.makeTicks(48.f, pixels);
        nvgFontSize(args.vg, 7.5f);
        for (size_t i = 0; i < ticks.size(); ++i) {
            float x = 0.f, yBottom = 0.f;
            screenFromLogical(flow(), LogicalPoint(0.f, ticks[i].normalizedAge), x, yBottom);
            const float y = (1.f - yBottom) * box.size.y;
            nvgStrokeColor(args.vg, nvgRGBA(190, 205, 210, 28));
            nvgBeginPath(args.vg);
            if (isVerticalFlow(flow())) {
                nvgMoveTo(args.vg, 0.f, y);
                nvgLineTo(args.vg, box.size.x, y);
                float labelY = y;
                int alignment = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE;
                if (y < 7.f) {
                    labelY = 2.f;
                    alignment = NVG_ALIGN_LEFT | NVG_ALIGN_TOP;
                } else if (y > box.size.y - 7.f) {
                    labelY = box.size.y - 2.f;
                    alignment = NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM;
                }
                nvgTextAlign(args.vg, alignment);
                nvgFillColor(args.vg, nvgRGBA(190, 205, 210, 135));
                const std::string label = timeLabel(ticks[i].ageSeconds, display->timeline.visibleSpan());
                nvgText(args.vg, 2.f, labelY, label.c_str(), NULL);
            } else {
                const float displayX = x * box.size.x;
                nvgMoveTo(args.vg, displayX, 0.f);
                nvgLineTo(args.vg, displayX, box.size.y);
                float labelX = displayX;
                int alignment = NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM;
                if (displayX < 15.f) {
                    labelX = 2.f;
                    alignment = NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM;
                } else if (displayX > box.size.x - 15.f) {
                    labelX = box.size.x - 2.f;
                    alignment = NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM;
                }
                nvgTextAlign(args.vg, alignment);
                nvgFillColor(args.vg, nvgRGBA(190, 205, 210, 135));
                const std::string label = timeLabel(ticks[i].ageSeconds, display->timeline.visibleSpan());
                nvgText(args.vg, labelX, box.size.y - 1.f, label.c_str(), NULL);
            }
            nvgStroke(args.vg);
        }
    }

    void drawMarkers(const DrawArgs& args) {
        if (!display || (module && !module->showMarkersSetting.load())) return;
        const std::vector<MarkerEvent>& markers = display->timeline.markers();
        for (size_t i = 0; i < markers.size(); ++i) {
            const float age = display->timeline.normalizedAgeForSample(markers[i].timelineSample, markers[i].sampleRate);
            if (age < 0.f || age > 1.f) continue;
            float x = 0.f, yBottom = 0.f;
            screenFromLogical(flow(), LogicalPoint(0.f, age), x, yBottom);
            const float y = (1.f - yBottom) * box.size.y;
            const NVGcolor color = markers[i].sequence % 2 ? nvgRGBA(255, 193, 72, 210) : nvgRGBA(71, 224, 255, 210);
            nvgStrokeColor(args.vg, color);
            nvgStrokeWidth(args.vg, 1.15f);
            nvgBeginPath(args.vg);
            if (isVerticalFlow(flow())) {
                nvgMoveTo(args.vg, 0.f, y);
                nvgLineTo(args.vg, box.size.x, y);
                nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM);
                nvgFillColor(args.vg, color);
                const std::string label = rack::string::f("M%02u", markers[i].sequence);
                nvgText(args.vg, box.size.x - 3.f, y - 1.f, label.c_str(), NULL);
            } else {
                const float displayX = x * box.size.x;
                nvgMoveTo(args.vg, displayX, 0.f);
                nvgLineTo(args.vg, displayX, box.size.y);
                nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nvgFillColor(args.vg, color);
                const std::string label = rack::string::f("M%02u", markers[i].sequence);
                const float labelX = std::min(displayX + 2.f, box.size.x - 24.f);
                nvgText(args.vg, labelX, 2.f, label.c_str(), NULL);
            }
            nvgStroke(args.vg);
        }
    }

    void drawCursor(const DrawArgs& args) {
        if (!hovered || !display) return;
        const LogicalPoint logical = logicalAt(cursor);
        const float full = fullFrequencyCoordinate(logical);
        const float frequency = frequencyFromFullCoordinate(full);
        float displayed = 0.f, raw = 0.f;
        SpectrumRow row;
        const bool valid = display->sampleAt(full, logical.age, displayed, raw, row);
        nvgStrokeWidth(args.vg, 0.8f);
        nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 150));
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, cursor.x, 0.f);
        nvgLineTo(args.vg, cursor.x, box.size.y);
        nvgMoveTo(args.vg, 0.f, cursor.y);
        nvgLineTo(args.vg, box.size.x, cursor.y);
        nvgStroke(args.vg);
        std::string text;
        if (valid) {
            text = rack::string::f("%s Hz  %s  %.1f dBFS", frequencyLabel(frequency).c_str(),
                                   noteLabel(frequency).c_str(), displayed);
        } else {
            text = rack::string::f("%s Hz  %s  gap", frequencyLabel(frequency).c_str(),
                                   noteLabel(frequency).c_str());
        }
        nvgFontSize(args.vg, 9.f);
        float bounds[4];
        nvgTextBounds(args.vg, 0.f, 0.f, text.c_str(), NULL, bounds);
        float maximumBounds[4];
        nvgTextBounds(args.vg, 0.f, 0.f, "20.0k Hz  A#10 +00c  -160.0 dBFS", NULL, maximumBounds);
        const float tooltipWidth = std::max(bounds[2] - bounds[0], maximumBounds[2] - maximumBounds[0]);
        float textX = cursor.x + 8.f, textY = cursor.y + 8.f;
        if (textX + tooltipWidth > box.size.x - 4.f) textX = cursor.x - tooltipWidth - 8.f;
        textX = clampValue(textX, 4.f, std::max(4.f, box.size.x - tooltipWidth - 4.f));
        if (textY > box.size.y - 18.f) textY = cursor.y - 17.f;
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, textX - 3.f, textY - 2.f, tooltipWidth + 6.f, 14.f, 2.f);
        nvgFillColor(args.vg, nvgRGBA(0, 0, 0, 190));
        nvgFill(args.vg);
        nvgFillColor(args.vg, nvgRGB(236, 239, 240));
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(args.vg, textX, textY, text.c_str(), NULL);
    }

    void draw(const DrawArgs& args) override {
        nvgSave(args.vg);
        drawGrid(args);
        drawMarkers(args);
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
        if (event.action == GLFW_PRESS && event.button == GLFW_MOUSE_BUTTON_LEFT) {
            cursor = event.pos;
            draggingTime = inTimeGutter(event.pos) || (event.mods & GLFW_MOD_SHIFT);
            event.consume(this);
        }
    }
    void onDragMove(const event::DragMove& event) override {
        if (!module || !display) return;
        const float zoom = APP->scene->rackScroll->zoomWidget->zoom;
        cursor += event.mouseDelta.div(zoom);
        cursor.x = clampValue(cursor.x, 0.f, box.size.x);
        cursor.y = clampValue(cursor.y, 0.f, box.size.y);
        if (draggingTime) {
            const float axisDelta = isVerticalFlow(flow()) ? event.mouseDelta.y / (zoom * box.size.y)
                                                           : -event.mouseDelta.x / (zoom * box.size.x);
            display->timeline.pan(axisDelta * display->timeline.visibleSpan());
            display->lookupDirty = true;
        } else {
            const float low = module->viewMinimum.load(), high = module->viewMaximum.load(), span = high - low;
            const float axisDelta = isVerticalFlow(flow()) ? event.mouseDelta.x / (zoom * box.size.x)
                                                           : -event.mouseDelta.y / (zoom * box.size.y);
            float nextLow = clampValue(low - axisDelta * span, 0.f, 1.f - span);
            module->viewMinimum.store(nextLow);
            module->viewMaximum.store(nextLow + span);
        }
    }
    void onDragEnd(const event::DragEnd& event) override {
        draggingTime = false;
        TransparentWidget::onDragEnd(event);
    }
    void onHoverScroll(const event::HoverScroll& event) override {
        if (!module || !display) return;
        const LogicalPoint logical = logicalAt(event.pos);
        const bool timeGesture = inTimeGutter(event.pos) || (APP->window->getMods() & GLFW_MOD_SHIFT);
        const float factor = std::pow(1.0015f, event.scrollDelta.y);
        if (timeGesture) {
            display->timeline.zoom(factor, logical.age);
            module->timeSpanSetting.store(display->timeline.visibleSpan());
            display->lookupDirty = true;
        } else {
            const float low = module->viewMinimum.load(), high = module->viewMaximum.load(), span = high - low;
            const float anchor = low + logical.frequency * span;
            const float nextSpan = clampValue(span * factor, 0.03f, 1.f);
            const float nextLow = clampValue(anchor - logical.frequency * nextSpan, 0.f, 1.f - nextSpan);
            module->viewMinimum.store(nextLow);
            module->viewMaximum.store(nextLow + nextSpan);
        }
        event.consume(this);
    }
    void onDoubleClick(const event::DoubleClick& event) override {
        if (!module || !display) return;
        if (inTimeGutter(cursor)) {
            display->timeline.returnToLive();
            module->timeSpanSetting.store(display->timeline.visibleSpan());
            display->lookupDirty = true;
        } else {
            module->viewMinimum.store(0.f);
            module->viewMaximum.store(1.f);
        }
        event.consume(this);
    }
};

struct WaterfallBezel : TransparentWidget {
    void draw(const DrawArgs& args) override {
        const float width = box.size.x;
        const float height = box.size.y;
        nvgSave(args.vg);

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.f, -0.5f);
        nvgLineTo(args.vg, width, -0.5f);
        nvgStrokeColor(args.vg, nvgRGBAf(0.f, 0.f, 0.f, 0.24f));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.f, height + 0.5f);
        nvgLineTo(args.vg, width, height + 0.5f);
        nvgStrokeColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, 0.25f));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.f, 2.5f);
        nvgLineTo(args.vg, width, 2.5f);
        nvgStrokeColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, 0.20f));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.f, height - 2.5f);
        nvgLineTo(args.vg, width, height - 2.5f);
        nvgStrokeColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, 0.20f));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgBeginPath(args.vg);
        nvgRect(args.vg, 1.f, 1.f, std::max(width - 2.f, 0.f), std::max(height - 2.f, 0.f));
        nvgStrokeColor(args.vg, nvgRGB(0x12, 0x12, 0x12));
        nvgStrokeWidth(args.vg, 2.f);
        nvgStroke(args.vg);

        nvgRestore(args.vg);
    }
};

struct WaterfallPanelLabels : TransparentWidget {
    void draw(const DrawArgs& args) override {
        nvgFillColor(args.vg, settings::preferDarkPanels ? nvgRGB(247, 197, 173) : nvgRGB(236, 237, 241));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(args.vg, 13.f);
        nvgTextLetterSpacing(args.vg, 2.f);
        nvgText(args.vg, box.size.x * 0.5f, 13.f, "WATERFALL", NULL);
        nvgTextLetterSpacing(args.vg, 0.f);
        nvgFontSize(args.vg, 6.7f);
        const char* labels[] = {"L", "R", "FREEZE", "MARK", "CLEAR", "RANGE", "FREEZE", "CLEAR"};
        const float positions[] = {20.f, 58.f, 96.f, 134.f, 172.f, 228.f, 283.f, 338.f};
        for (int index = 0; index < 8; ++index) nvgText(args.vg, positions[index], 316.f, labels[index], NULL);
        nvgFontSize(args.vg, 7.f);
        nvgTextLetterSpacing(args.vg, 2.f);
        nvgText(args.vg, box.size.x * 0.5f, 376.f, "CELLA", NULL);
    }
};

}  // namespace

struct WaterfallWidget : ModuleWidget {
    WaterfallWidget(Waterfall* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/Waterfall.svg"),
                             asset::plugin(pluginInstance, "res/Waterfall-dark.svg")));
        WaterfallPanelLabels* labels = new WaterfallPanelLabels;
        labels->box.size = Vec(360.f, 380.f);
        addChild(labels);
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

        const float y = 340.f;
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(20.f, y), module, Waterfall::LEFT_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(58.f, y), module, Waterfall::RIGHT_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(96.f, y), module, Waterfall::FREEZE_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(134.f, y), module, Waterfall::MARK_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(172.f, y), module, Waterfall::CLEAR_INPUT));
        addParam(createParamCentered<RoundSmallBlackKnob>(Vec(228.f, y), module, Waterfall::RANGE_PARAM));
        addParam(createParamCentered<LEDButton>(Vec(283.f, y), module, Waterfall::FREEZE_PARAM));
        addChild(createLightCentered<MediumLight<YellowLight>>(Vec(283.f, y), module, Waterfall::FREEZE_LIGHT));
        addParam(createParamCentered<VCVButton>(Vec(338.f, y), module, Waterfall::CLEAR_PARAM));
    }

    void appendContextMenu(Menu* menu) override {
        Waterfall* waterfall = dynamic_cast<Waterfall*>(module);
        if (!waterfall) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Analysis"));
        menu->addChild(createIndexSubmenuItem(
            "Channel mode", {"Left", "Right", "Mono", "Mid", "Side"},
            [=]() {
                return static_cast<size_t>(
                    clampValue(static_cast<int>(std::lround(waterfall->params[Waterfall::MODE_PARAM].getValue())),
                               0, static_cast<int>(ChannelMode::COUNT) - 1));
            },
            [=](size_t value) {
                waterfall->params[Waterfall::MODE_PARAM].setValue(
                    static_cast<float>(clampValue(static_cast<int>(value), 0,
                                                  static_cast<int>(ChannelMode::COUNT) - 1)));
            }));
        menu->addChild(createIndexSubmenuItem(
            "FFT size", {"1024", "2048", "4096", "8192", "16384"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->fftSizeSetting.load(), 0, 4)); },
            [=](size_t value) { waterfall->fftSizeSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "Window", {"Hann", "Blackman-Harris", "Flat-top"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->windowSetting.load(), 0, 2)); },
            [=](size_t value) { waterfall->windowSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "FFT overlap", {"None", "25%", "50%", "75%", "87.5%", "93.75%"},
            [=]() {
                return static_cast<size_t>(
                    clampValue(waterfall->fftOverlapSetting.load(), 0, static_cast<int>(FftOverlap::COUNT) - 1));
            },
            [=](size_t value) { waterfall->fftOverlapSetting.store(static_cast<int>(value)); }));
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
        menu->addChild(createMenuLabel("Presentation"));
        menu->addChild(createIndexSubmenuItem(
            "Rendering", {"Precise", "Smooth"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->renderingStyleSetting.load(), 0, 1)); },
            [=](size_t value) { waterfall->renderingStyleSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "Live trace", {"Off", "Line", "Line + Fill"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->liveTraceSetting.load(), 0, 2)); },
            [=](size_t value) { waterfall->liveTraceSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "Peak trace", {"Off", "Decay", "Infinite hold"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->peakHoldSetting.load(), 0, 2)); },
            [=](size_t value) { waterfall->peakHoldSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "Frequency scale", {"Hz", "Octaves", "Musical", "Combined"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->frequencyScaleSetting.load(), 0, 3)); },
            [=](size_t value) { waterfall->frequencyScaleSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "Frequency smoothing", {"None", "1/48 octave", "1/24 octave", "1/12 octave", "1/6 octave", "1/3 octave"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->frequencySmoothingSetting.load(), 0, 5)); },
            [=](size_t value) { waterfall->frequencySmoothingSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "Temporal smoothing", {"Off", "Fast · 25/250 ms", "Medium · 100/700 ms", "Slow · 300/1500 ms"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->temporalSmoothingSetting.load(), 0, 3)); },
            [=](size_t value) { waterfall->temporalSmoothingSetting.store(static_cast<int>(value)); }));
        menu->addChild(createIndexSubmenuItem(
            "History duration", {"2 seconds", "4 seconds", "8 seconds", "16 seconds", "30 seconds"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->historyDurationSetting.load(), 0, 4)); },
            [=](size_t value) {
                waterfall->historyDurationSetting.store(static_cast<int>(value));
                waterfall->timeSpanSetting.store(static_cast<float>(
                    historyDurationSeconds(static_cast<HistoryDuration>(static_cast<int>(value)))));
            }));
        menu->addChild(createBoolMenuItem("Show markers", "",
                                          [=]() { return waterfall->showMarkersSetting.load(); },
                                          [=](bool value) { waterfall->showMarkersSetting.store(value); }));
        menu->addChild(createIndexSubmenuItem(
            "Flow", {"Up", "Down", "Left", "Right"},
            [=]() { return static_cast<size_t>(clampValue(waterfall->flowSetting.load(), 0, 3)); },
            [=](size_t value) { waterfall->flowSetting.store(static_cast<int>(value)); }));
        const Palette selectedPalette = static_cast<Palette>(
            clampValue(waterfall->paletteSetting.load(), 0, static_cast<int>(Palette::COUNT) - 1));
        menu->addChild(createSubmenuItem(
            "Palette", paletteDefinition(selectedPalette).name, [=](Menu* paletteMenu) {
                paletteMenu->addChild(createCheckMenuItem(
                    paletteDefinition(Palette::HEAT).name, "",
                    [=]() { return waterfall->paletteSetting.load() == static_cast<int>(Palette::HEAT); },
                    [=]() { waterfall->paletteSetting.store(static_cast<int>(Palette::HEAT)); },
                    false, true));
                paletteMenu->addChild(new MenuSeparator);
                for (const PaletteMenuGroup& group : paletteMenuGroups()) {
                    const std::vector<Palette> palettes = group.palettes;
                    std::string activeName;
                    for (Palette palette : palettes) {
                        if (palette == static_cast<Palette>(waterfall->paletteSetting.load()))
                            activeName = paletteDefinition(palette).name;
                    }
                    paletteMenu->addChild(createSubmenuItem(
                        group.name, activeName, [=](Menu* groupMenu) {
                            for (Palette palette : palettes) {
                                groupMenu->addChild(createCheckMenuItem(
                                    paletteDefinition(palette).name, "",
                                    [=]() {
                                        return waterfall->paletteSetting.load() ==
                                               static_cast<int>(palette);
                                    },
                                    [=]() {
                                        waterfall->paletteSetting.store(static_cast<int>(palette));
                                    },
                                    false, true));
                            }
                        }));
                }
            }));
        menu->addChild(createMenuItem("Reset frequency zoom", "", [=]() {
            waterfall->viewMinimum.store(0.f);
            waterfall->viewMaximum.store(1.f);
        }));
#ifndef NDEBUG
        menu->addChild(createMenuLabel(rack::string::f(
            "Dropped rows / markers: %llu / %llu",
            static_cast<unsigned long long>(waterfall->droppedRows.load()),
            static_cast<unsigned long long>(waterfall->droppedMarkers.load()))));
#endif
    }
};

Model* modelWaterfall = createModel<Waterfall, WaterfallWidget>("Waterfall");

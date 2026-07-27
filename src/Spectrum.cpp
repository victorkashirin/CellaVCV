#include "plugin.hpp"
#include "spectrum/HistoryTimeline.hpp"
#include "spectrum/SpectrumAnalyzer.hpp"
#include "spectrum/SpectrumPalettes.hpp"
#include "spectrum/SpectrumPresentation.hpp"
#include "spectrum/SpectrumTypes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace cella::spectrum;

namespace {

constexpr float DISPLAY_X = 0.f;
constexpr float DISPLAY_Y = 26.f;
constexpr float DISPLAY_WIDTH = 360.f;
constexpr float DISPLAY_HEIGHT = 280.f;
constexpr int DEFAULT_PANEL_WIDTH_HP = 24;
constexpr int MIN_PANEL_WIDTH_HP = 24;
constexpr int MAX_PANEL_WIDTH_HP = 64;
constexpr float RANGE_DEFAULT_DB = -100.f;
constexpr float RANGE_MIN_DB = -140.f;
constexpr float RANGE_MAX_DB = -40.f;
constexpr float VERTICAL_TIME_GUTTER = 40.f;
constexpr float HORIZONTAL_TIME_GUTTER = 18.f;
constexpr float GRID_TOP_INSET = 0.f;
constexpr float GRID_BOTTOM_INSET = 3.f;

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

struct Spectrum : Module {
    enum ParamIds { MODE_PARAM, RANGE_PARAM, FREEZE_PARAM, CLEAR_PARAM, SPEED_PARAM, NUM_PARAMS };
    enum InputIds { LEFT_INPUT, RIGHT_INPUT, FREEZE_INPUT, MARK_INPUT, NUM_INPUTS };
    enum LightIds { FREEZE_LIGHT, NUM_LIGHTS };

    SpectrumAnalyzer analyzer;
    dsp::RingBuffer<SpectrumRow, ROW_QUEUE_SIZE> displayRows;
    dsp::RingBuffer<MarkerEvent, MARKER_QUEUE_SIZE> markerEvents;
    dsp::SchmittTrigger freezeButtonTrigger;
    dsp::SchmittTrigger freezeInputTrigger;
    dsp::SchmittTrigger markInputTrigger;
    dsp::SchmittTrigger clearButtonTrigger;

    std::atomic<int> fftSizeSetting{static_cast<int>(FftSize::FFT_4096)};
    std::atomic<int> windowSetting{static_cast<int>(WindowFunction::HANN)};
    std::atomic<int> fftOverlapSetting{static_cast<int>(FftOverlap::PERCENT_75)};
    std::atomic<int> qualitySetting{static_cast<int>(Quality::HIGH)};
    std::atomic<int> analysisModeSetting{static_cast<int>(AnalysisMode::CLASSIC)};
    std::atomic<int> polyChannelSetting{0};
    std::atomic<int> paletteSetting{static_cast<int>(Palette::INFERNO)};
    std::atomic<int> peakHoldSetting{static_cast<int>(PeakHold::OFF)};
    std::atomic<int> flowSetting{static_cast<int>(FlowDirection::LEFT)};
    std::atomic<int> renderingStyleSetting{static_cast<int>(RenderingStyle::PRECISE)};
    std::atomic<int> liveTraceSetting{static_cast<int>(LiveTraceMode::OFF)};
    std::atomic<int> frequencyScaleSetting{static_cast<int>(FrequencyScaleMode::HZ)};
    std::atomic<int> frequencyBinsSetting{static_cast<int>(FrequencyBinScale::LOGARITHMIC)};
    std::atomic<int> frequencySmoothingSetting{static_cast<int>(FrequencySmoothing::NONE)};
    std::atomic<bool> showFrequencyTicksSetting{true};
    std::atomic<bool> showTimeTicksSetting{true};
    std::atomic<float> frequencyGridOpacitySetting{1.f};
    std::atomic<float> timeGridOpacitySetting{1.f};
    std::atomic<bool> showMarkersSetting{true};
    std::atomic<float> markerOpacitySetting{0.82f};
    std::atomic<float> viewMinimum{0.f};
    std::atomic<float> viewMaximum{1.f};
    std::atomic<bool> displayOnlyModeSetting{false};
    std::atomic<bool> frozen{false};
    std::atomic<bool> freezeToggleRequested{false};
    std::atomic<uint64_t> clearGeneration{0};
    std::atomic<uint64_t> rowAcceptanceBoundarySample{0};
    std::atomic<uint64_t> activeConfigGeneration{1};
#ifndef NDEBUG
    std::atomic<uint64_t> droppedRows{0};
    std::atomic<uint64_t> droppedMarkers{0};
#endif

    int appliedFftSize = -1;
    int appliedWindow = -1;
    int appliedFftOverlap = -1;
    int appliedQuality = -1;
    int appliedAnalysisMode = -1;
    int appliedChannelMode = -1;
    int appliedFrequencyBins = -1;
    int appliedPolyChannel = -1;
    float appliedSampleRate = 0.f;
    uint64_t configGeneration = 1;
    uint64_t timelineSample = 0;
    uint32_t markerSequence = 0;
    int panelWidth = DEFAULT_PANEL_WIDTH_HP;

    Spectrum() {
        config(NUM_PARAMS, NUM_INPUTS, 0, NUM_LIGHTS);
        configSwitch(MODE_PARAM, 0.f, static_cast<float>(static_cast<int>(ChannelMode::COUNT) - 1),
                     static_cast<float>(ChannelMode::MONO), "Channel", {"Left", "Right", "Mono", "Mid", "Side"});
        configParam(RANGE_PARAM, RANGE_MIN_DB, RANGE_MAX_DB, RANGE_DEFAULT_DB, "Display floor", " dBFS");
        configButton(FREEZE_PARAM, "Freeze");
        configButton(CLEAR_PARAM, "Clear");
        configParam(SPEED_PARAM, std::log2(MIN_HISTORY_SPEED), std::log2(MAX_HISTORY_SPEED), 0.f,
                    "History speed", "×", 2.f);
        configInput(LEFT_INPUT, "Left");
        configInput(RIGHT_INPUT, "Right");
        configInput(FREEZE_INPUT, "Freeze trigger");
        configInput(MARK_INPUT, "Marker trigger");
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
        const int analysisMode =
            clampValue(analysisModeSetting.load(std::memory_order_relaxed), 0,
                       static_cast<int>(AnalysisMode::COUNT) - 1);
        const int channelMode = clampValue(static_cast<int>(std::lround(params[MODE_PARAM].getValue())), 0,
                                           static_cast<int>(ChannelMode::COUNT) - 1);
        const int frequencyBins =
            clampValue(frequencyBinsSetting.load(std::memory_order_relaxed), 0,
                       static_cast<int>(FrequencyBinScale::COUNT) - 1);
        const int polyChannel = clampValue(polyChannelSetting.load(std::memory_order_relaxed), 0, 15);
        const bool sampleRateChanged = appliedSampleRate != 0.f && args.sampleRate != appliedSampleRate;
        const bool analysisChanged =
            fftSize != appliedFftSize || window != appliedWindow || fftOverlap != appliedFftOverlap ||
            analysisMode != appliedAnalysisMode ||
            channelMode != appliedChannelMode || frequencyBins != appliedFrequencyBins ||
            polyChannel != appliedPolyChannel || sampleRateChanged;
        const bool qualityChanged = quality != appliedQuality;
        if (analysisChanged || qualityChanged || appliedSampleRate == 0.f) {
            if (analysisChanged) {
                ++configGeneration;
                if (sampleRateChanged) {
                    timelineSample = 0;
                    rowAcceptanceBoundarySample.store(0,
                                                      std::memory_order_release);
                }
            }
            SpectrumConfig next;
            next.fftSize = static_cast<FftSize>(fftSize);
            next.window = static_cast<WindowFunction>(window);
            next.fftOverlap = static_cast<FftOverlap>(fftOverlap);
            next.quality = static_cast<Quality>(quality);
            next.channelMode = static_cast<ChannelMode>(channelMode);
            next.analysisMode = static_cast<AnalysisMode>(analysisMode);
            next.frequencyBins = static_cast<FrequencyBinScale>(frequencyBins);
            next.polyChannel = polyChannel;
            next.sampleRate = args.sampleRate;
            next.generation = configGeneration;
            analyzer.configure(next);
            activeConfigGeneration.store(configGeneration, std::memory_order_release);
            appliedFftSize = fftSize;
            appliedWindow = window;
            appliedFftOverlap = fftOverlap;
            appliedQuality = quality;
            appliedAnalysisMode = analysisMode;
            appliedChannelMode = channelMode;
            appliedFrequencyBins = frequencyBins;
            appliedPolyChannel = polyChannel;
            appliedSampleRate = args.sampleRate;
        }

        ++timelineSample;
        const bool freezeEvent =
            freezeButtonTrigger.process(params[FREEZE_PARAM].getValue()) ||
            freezeInputTrigger.process(inputs[FREEZE_INPUT].getVoltage()) ||
            freezeToggleRequested.exchange(false, std::memory_order_acq_rel);
        if (freezeEvent) {
            const bool nowFrozen = !frozen.load(std::memory_order_relaxed);
            frozen.store(nowFrozen, std::memory_order_relaxed);
            if (!nowFrozen)
                rowAcceptanceBoundarySample.store(timelineSample,
                                                  std::memory_order_release);
            analyzer.discardPending(timelineSample);
        }
        const bool clearEvent = clearButtonTrigger.process(params[CLEAR_PARAM].getValue());
        if (clearEvent) {
            clearGeneration.fetch_add(1, std::memory_order_release);
            rowAcceptanceBoundarySample.store(timelineSample,
                                              std::memory_order_release);
            analyzer.discardPending(timelineSample);
        }
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
            if (!frozen.load(std::memory_order_relaxed) && !displayRows.full())
                displayRows.push(row);
#ifndef NDEBUG
            else if (!frozen.load(std::memory_order_relaxed))
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
        json_object_set_new(root, "analysisMode", json_integer(analysisModeSetting.load()));
        json_object_set_new(root, "polyChannel", json_integer(polyChannelSetting.load()));
        json_object_set_new(root, "palette", json_integer(paletteSetting.load()));
        json_object_set_new(root, "peakHold", json_integer(peakHoldSetting.load()));
        json_object_set_new(root, "flow", json_integer(flowSetting.load()));
        json_object_set_new(root, "renderingStyle", json_integer(renderingStyleSetting.load()));
        json_object_set_new(root, "liveTrace", json_integer(liveTraceSetting.load()));
        json_object_set_new(root, "frequencyScale", json_integer(frequencyScaleSetting.load()));
        json_object_set_new(root, "frequencyBins", json_integer(frequencyBinsSetting.load()));
        json_object_set_new(root, "frequencySmoothing", json_integer(frequencySmoothingSetting.load()));
        json_object_set_new(root, "showFrequencyTicks", json_boolean(showFrequencyTicksSetting.load()));
        json_object_set_new(root, "showTimeTicks", json_boolean(showTimeTicksSetting.load()));
        json_object_set_new(root, "frequencyGridOpacity", json_real(frequencyGridOpacitySetting.load()));
        json_object_set_new(root, "timeGridOpacity", json_real(timeGridOpacitySetting.load()));
        json_object_set_new(root, "showMarkers", json_boolean(showMarkersSetting.load()));
        json_object_set_new(root, "markerOpacity", json_real(markerOpacitySetting.load()));
        json_object_set_new(root, "viewMinimum", json_real(viewMinimum.load()));
        json_object_set_new(root, "viewMaximum", json_real(viewMaximum.load()));
        json_object_set_new(root, "displayOnlyMode",
                            json_boolean(displayOnlyModeSetting.load()));
        json_object_set_new(root, "panelWidth", json_integer(panelWidth));
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
                                        static_cast<int>(Quality::HIGH)));
        analysisModeSetting.store(
            getJsonInt(root, "analysisMode", 0, static_cast<int>(AnalysisMode::COUNT) - 1,
                       static_cast<int>(AnalysisMode::CLASSIC)));
        polyChannelSetting.store(getJsonInt(root, "polyChannel", 0, 15, 0));
        paletteSetting.store(
            getJsonInt(root, "palette", 0, static_cast<int>(Palette::COUNT) - 1,
                       static_cast<int>(Palette::INFERNO)));
        peakHoldSetting.store(getJsonInt(root, "peakHold", 0, static_cast<int>(PeakHold::COUNT) - 1,
                                         static_cast<int>(PeakHold::OFF)));
        flowSetting.store(getJsonInt(root, "flow", 0, static_cast<int>(FlowDirection::COUNT) - 1,
                                     static_cast<int>(FlowDirection::LEFT)));
        renderingStyleSetting.store(getJsonInt(root, "renderingStyle", 0, static_cast<int>(RenderingStyle::COUNT) - 1,
                                               static_cast<int>(RenderingStyle::PRECISE)));
        liveTraceSetting.store(getJsonInt(root, "liveTrace", 0, static_cast<int>(LiveTraceMode::COUNT) - 1,
                                         static_cast<int>(LiveTraceMode::OFF)));
        frequencyScaleSetting.store(getJsonInt(root, "frequencyScale", 0,
                                               static_cast<int>(FrequencyScaleMode::COUNT) - 1,
                                               static_cast<int>(FrequencyScaleMode::HZ)));
        frequencyBinsSetting.store(getJsonInt(root, "frequencyBins", 0,
                                              static_cast<int>(FrequencyBinScale::COUNT) - 1,
                                              static_cast<int>(FrequencyBinScale::LOGARITHMIC)));
        frequencySmoothingSetting.store(getJsonInt(root, "frequencySmoothing", 0,
                                                   static_cast<int>(FrequencySmoothing::COUNT) - 1,
                                                   static_cast<int>(FrequencySmoothing::NONE)));
        json_t* showFrequencyTicks = json_object_get(root, "showFrequencyTicks");
        showFrequencyTicksSetting.store(
            json_is_boolean(showFrequencyTicks) ? json_is_true(showFrequencyTicks) : true);
        json_t* showTimeTicks = json_object_get(root, "showTimeTicks");
        showTimeTicksSetting.store(json_is_boolean(showTimeTicks) ? json_is_true(showTimeTicks) : true);
        frequencyGridOpacitySetting.store(
            getJsonFloat(root, "frequencyGridOpacity", 0.f, 1.f, 1.f));
        timeGridOpacitySetting.store(getJsonFloat(root, "timeGridOpacity", 0.f, 1.f, 1.f));
        json_t* showMarkers = json_object_get(root, "showMarkers");
        showMarkersSetting.store(json_is_boolean(showMarkers) ? json_is_true(showMarkers) : true);
        markerOpacitySetting.store(getJsonFloat(root, "markerOpacity", 0.f, 1.f, 0.82f));
        float minimum = getJsonFloat(root, "viewMinimum", 0.f, 0.99f, 0.f);
        float maximum = getJsonFloat(root, "viewMaximum", 0.01f, 1.f, 1.f);
        if (maximum - minimum < 0.01f) {
            minimum = 0.f;
            maximum = 1.f;
        }
        viewMinimum.store(minimum);
        viewMaximum.store(maximum);
        json_t* displayOnlyMode = json_object_get(root, "displayOnlyMode");
        displayOnlyModeSetting.store(
            json_is_boolean(displayOnlyMode) && json_is_true(displayOnlyMode));
        panelWidth =
            getJsonInt(root, "panelWidth", MIN_PANEL_WIDTH_HP, MAX_PANEL_WIDTH_HP,
                       DEFAULT_PANEL_WIDTH_HP);
        frozen.store(false);
        clearGeneration.fetch_add(1, std::memory_order_release);
    }
};

namespace {

struct MarkerOpacityQuantity : Quantity {
    Spectrum* module = NULL;

    explicit MarkerOpacityQuantity(Spectrum* module) : module(module) {}

    void setValue(float value) override {
        if (module)
            module->markerOpacitySetting.store(clampValue(value, getMinValue(), getMaxValue()) * 0.01f);
    }
    float getValue() override {
        return module ? module->markerOpacitySetting.load() * 100.f : getDefaultValue();
    }
    float getMinValue() override { return 0.f; }
    float getMaxValue() override { return 100.f; }
    float getDefaultValue() override { return 82.f; }
    std::string getLabel() override { return "Marker opacity"; }
    std::string getUnit() override { return "%"; }
};

struct MarkerOpacitySlider : ui::Slider {
    explicit MarkerOpacitySlider(Spectrum* module) {
        quantity = new MarkerOpacityQuantity(module);
        box.size.x = 200.f;
    }
    ~MarkerOpacitySlider() override { delete quantity; }
};

struct GridOpacityQuantity : Quantity {
    std::atomic<float>* setting = NULL;
    std::string label;

    GridOpacityQuantity(std::atomic<float>* setting, const std::string& label)
        : setting(setting), label(label) {}

    void setValue(float value) override {
        if (setting) setting->store(clampValue(value, getMinValue(), getMaxValue()) * 0.01f);
    }
    float getValue() override { return setting ? setting->load() * 100.f : getDefaultValue(); }
    float getMinValue() override { return 0.f; }
    float getMaxValue() override { return 100.f; }
    float getDefaultValue() override { return 100.f; }
    std::string getLabel() override { return label; }
    std::string getUnit() override { return "%"; }
};

struct GridOpacitySlider : ui::Slider {
    GridOpacitySlider(std::atomic<float>* setting, const std::string& label) {
        quantity = new GridOpacityQuantity(setting, label);
        box.size.x = 200.f;
    }
    ~GridOpacitySlider() override { delete quantity; }
};

struct NonClosingCheckMenuItem : MenuItem {
    std::function<bool()> checked;
    std::function<void()> action;

    void step() override {
        rightText = CHECKMARK(checked());
        MenuItem::step();
    }

    void onAction(const event::Action& event) override {
        action();
        event.unconsume();
    }
};

MenuItem* createNonClosingCheckMenuItem(const std::string& text,
                                        std::function<bool()> checked,
                                        std::function<void()> action) {
    NonClosingCheckMenuItem* item = createMenuItem<NonClosingCheckMenuItem>(text);
    item->checked = checked;
    item->action = action;
    return item;
}

MenuItem* createNonClosingBoolMenuItem(const std::string& text,
                                       std::function<bool()> getter,
                                       std::function<void(bool)> setter) {
    return createNonClosingCheckMenuItem(text, getter, [=]() { setter(!getter()); });
}

struct NonClosingIndexSubmenuItem : MenuItem {
    std::function<size_t()> getter;
    std::function<void(size_t)> setter;
    std::vector<std::string> labels;

    void step() override {
        const size_t index = getter();
        rightText = (index < labels.size() ? labels[index] : "") + "  " + RIGHT_ARROW;
        MenuItem::step();
    }

    Menu* createChildMenu() override {
        Menu* menu = new Menu;
        for (size_t i = 0; i < labels.size(); ++i) {
            menu->addChild(createNonClosingCheckMenuItem(
                labels[i], [=]() { return getter() == i; }, [=]() { setter(i); }));
        }
        return menu;
    }
};

MenuItem* createNonClosingIndexSubmenuItem(const std::string& text,
                                           const std::vector<std::string>& labels,
                                           std::function<size_t()> getter,
                                           std::function<void(size_t)> setter) {
    NonClosingIndexSubmenuItem* item =
        createMenuItem<NonClosingIndexSubmenuItem>(text);
    item->getter = getter;
    item->setter = setter;
    item->labels = labels;
    return item;
}

struct SpectrumRenderer {
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
        WARN("Spectrum %s shader compilation failed: %s", label, log.data());
        glDeleteShader(shader);
        return 0;
    }

    bool initialize(int requestedRows) {
        if (!program) {
            if (initializationAttempted) return false;
            initializationAttempted = true;
            try {
                GLuint vertex = compile(GL_VERTEX_SHADER, loadResource("res/shaders/spectrum_gl.vert"), "vertex");
                GLuint fragment =
                    compile(GL_FRAGMENT_SHADER, loadResource("res/shaders/spectrum_gl.frag"), "fragment");
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
                WARN("Spectrum shader resources could not be loaded: %s", exception.what());
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

struct SpectrumDisplay : widget::OpenGlWidget {
    Spectrum* module = NULL;
    SpectrumRenderer renderer;
    HistoryTimeline timeline;
    FrequencySmoothingKernel frequencyKernel;
    std::vector<SpectrumRow> derivedRows;
    std::vector<bool> dirtyRows;
    std::array<float, NUM_FREQUENCY_CELLS> currentTrace;
    std::array<float, NUM_FREQUENCY_CELLS> peakTrace;
    uint64_t seenClearGeneration = 0;
    uint64_t seenModuleConfigGeneration = 0;
    int appliedCapacity = 0;
    int appliedFrequencySmoothing = -1;
    int appliedFrequencyBins = -1;
    int appliedPeakHold = -1;
    float appliedHistorySpeed = -1.f;
    float appliedFrequencySampleRate = 48000.f;
    bool traceDirty = true;
    bool lookupDirty = true;
    SpectrumRow latestMetadata;

    SpectrumDisplay() {
        currentTrace.fill(INTERNAL_FLOOR_DB);
        peakTrace.fill(INTERNAL_FLOOR_DB);
        frequencyKernel.configure(FrequencySmoothing::NONE, 48000.f,
                                  FrequencyBinScale::LOGARITHMIC);
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
        renderer = SpectrumRenderer();
        dirtyRows.assign(dirtyRows.size(), true);
        traceDirty = lookupDirty = true;
    }

    void onContextDestroy(const ContextDestroyEvent& event) override {
        renderer.destroy();
        widget::OpenGlWidget::onContextDestroy(event);
    }

    void clearHistory() {
        timeline.clear();
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
        currentTrace.fill(INTERNAL_FLOOR_DB);
        peakTrace.fill(INTERNAL_FLOOR_DB);
        const SpectrumRow* previousRaw = NULL;
        for (int ordered = 0; ordered < timeline.size(); ++ordered) {
            const int physical = timeline.physicalFromOldest(ordered);
            const SpectrumRow* raw = timeline.physicalRow(physical);
            frequencyKernel.apply(*raw, derivedRows[static_cast<size_t>(physical)]);
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
        const int quality =
            clampValue(module ? module->qualitySetting.load() : static_cast<int>(Quality::HIGH), 0, 2);
        const float historySpeed =
            module ? clampValue(std::exp2(module->params[Spectrum::SPEED_PARAM].getValue()),
                                MIN_HISTORY_SPEED, MAX_HISTORY_SPEED)
                   : 1.f;
        const float retained = historyDurationForSpeed(historySpeed);
        const bool historySpeedChanged = std::fabs(historySpeed - appliedHistorySpeed) > 1e-5f;
        appliedHistorySpeed = historySpeed;
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
        if (historySpeedChanged) timeline.setVisibleSpan(retained);
        const int frequencyMode =
            clampValue(module ? module->frequencySmoothingSetting.load() : 0, 0,
                       static_cast<int>(FrequencySmoothing::COUNT) - 1);
        const int frequencyBins =
            clampValue(module ? module->frequencyBinsSetting.load() : 0, 0,
                       static_cast<int>(FrequencyBinScale::COUNT) - 1);
        if (frequencyMode != appliedFrequencySmoothing || frequencyBins != appliedFrequencyBins) {
            appliedFrequencySmoothing = frequencyMode;
            appliedFrequencyBins = frequencyBins;
            frequencyKernel.configure(static_cast<FrequencySmoothing>(frequencyMode),
                                      appliedFrequencySampleRate,
                                      static_cast<FrequencyBinScale>(frequencyBins));
            rebuildDerived();
        }
        const int peak =
            clampValue(module ? module->peakHoldSetting.load() : static_cast<int>(PeakHold::OFF), 0,
                       static_cast<int>(PeakHold::COUNT) - 1);
        if (peak != appliedPeakHold) {
            appliedPeakHold = peak;
            rebuildDerived();
        }
    }

    void updateTraces(const SpectrumRow& row, float elapsedSeconds) {
        const PeakHold peakMode = static_cast<PeakHold>(
            clampValue(module ? module->peakHoldSetting.load() : static_cast<int>(PeakHold::OFF), 0,
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
        if (row.sampleRate != appliedFrequencySampleRate) {
            appliedFrequencySampleRate = row.sampleRate;
            frequencyKernel.configure(static_cast<FrequencySmoothing>(
                                          std::max(appliedFrequencySmoothing, 0)),
                                      appliedFrequencySampleRate,
                                      static_cast<FrequencyBinScale>(
                                          std::max(appliedFrequencyBins, 0)));
            if (!timeline.empty()) rebuildDerived();
        }
        float elapsed = 0.f;
        if (const SpectrumRow* newest = timeline.newestRow()) {
            if (newest->sampleRate == row.sampleRate && row.rowEndSample >= newest->rowEndSample)
                elapsed = static_cast<float>(row.rowEndSample - newest->rowEndSample) / row.sampleRate;
        }
        const int physical = timeline.addRow(row);
        frequencyKernel.apply(row, derivedRows[static_cast<size_t>(physical)]);
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
        const uint64_t acceptanceBoundary =
            module->rowAcceptanceBoundarySample.load(std::memory_order_acquire);
        while (!module->displayRows.empty()) {
            const SpectrumRow row = module->displayRows.shift();
            if (!frozen && row.configGeneration == generation &&
                row.rowEndSample > acceptanceBoundary)
                addRow(row);
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
                clampValue(module ? module->paletteSetting.load() : static_cast<int>(Palette::INFERNO), 0,
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
            glUniform1i(renderer.flowLocation,
                        module ? clampValue(module->flowSetting.load(), 0, 3)
                               : static_cast<int>(FlowDirection::LEFT));
            glUniform2f(renderer.viewLocation, module ? module->viewMinimum.load() : 0.f,
                        module ? module->viewMaximum.load() : 1.f);
            glUniform2f(renderer.rangeLocation,
                        module ? module->params[Spectrum::RANGE_PARAM].getValue() : RANGE_DEFAULT_DB, 0.f);
            glUniform1i(renderer.peakHoldLocation,
                        module ? clampValue(module->peakHoldSetting.load(), 0, 2)
                               : static_cast<int>(PeakHold::OFF));
            glUniform1i(renderer.liveTraceLocation,
                        module ? clampValue(module->liveTraceSetting.load(), 0, 2)
                               : static_cast<int>(LiveTraceMode::OFF));
            glUniform1i(renderer.styleLocation,
                        module ? clampValue(module->renderingStyleSetting.load(), 0, 1)
                               : static_cast<int>(RenderingStyle::PRECISE));
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
        }
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        // Rack dims the normal module layer before drawing its light layer.
        // Composite the OpenGL framebuffer as a light so Spectrum remains
        // luminous when the rack brightness is reduced.
        if (layer == 1) {
            widget::OpenGlWidget::draw(args);
        }
        widget::OpenGlWidget::drawLayer(args, layer);
    }
};

struct SpectrumOverlay : TransparentWidget {
    struct LabelBounds {
        float left;
        float top;
        float right;
        float bottom;

        LabelBounds(float left, float top, float right, float bottom)
            : left(left), top(top), right(right), bottom(bottom) {}
    };

    Spectrum* module = NULL;
    SpectrumDisplay* display = NULL;
    bool hovered = false;
    bool draggingTime = false;
    bool layoutTogglePressed = false;
    math::Vec cursor;
    std::vector<LabelBounds> frequencyLabelBounds;

    FlowDirection flow() const {
        return static_cast<FlowDirection>(
            clampValue(module ? module->flowSetting.load() : static_cast<int>(FlowDirection::LEFT), 0,
                       static_cast<int>(FlowDirection::COUNT) - 1));
    }
    float viewLow() const { return module ? module->viewMinimum.load() : 0.f; }
    float viewHigh() const { return module ? module->viewMaximum.load() : 1.f; }
    float presentationScale() const {
        const float widthScale = box.size.x / DISPLAY_WIDTH;
        const float heightScale = box.size.y / DISPLAY_HEIGHT;
        return clampValue(std::min(widthScale, heightScale), 1.f, 2.5f);
    }
    LogicalPoint logicalAt(math::Vec position) const {
        return logicalFromScreen(flow(), position.x / std::max(box.size.x, 1.f),
                                 1.f - position.y / std::max(box.size.y, 1.f));
    }
    float fullFrequencyCoordinate(const LogicalPoint& logical) const {
        return viewLow() + logical.frequency * (viewHigh() - viewLow());
    }
    float maximumFrequency() const {
        const float sampleRate =
            display && display->latestMetadata.sampleRate > 0.f ? display->latestMetadata.sampleRate : 48000.f;
        return displayMaximumFrequency(sampleRate);
    }
    FrequencyBinScale frequencyBins() const {
        return static_cast<FrequencyBinScale>(
            clampValue(module ? module->frequencyBinsSetting.load() : 0, 0,
                       static_cast<int>(FrequencyBinScale::COUNT) - 1));
    }
    float frequencyFromFullCoordinate(float coordinate) const {
        return frequencyHzForCoordinate(coordinate, maximumFrequency(), frequencyBins());
    }
    bool inTimeGutter(math::Vec position) const {
        const float scale = presentationScale();
        return isVerticalFlow(flow()) ? position.x < VERTICAL_TIME_GUTTER * scale
                                      : position.y > box.size.y - HORIZONTAL_TIME_GUTTER * scale;
    }
    float layoutControlScale() const {
        return clampValue(presentationScale(), 1.f, 1.5f);
    }
    math::Rect layoutControlBox() const {
        const float scale = layoutControlScale();
        const float size = 26.f * scale;
        const float horizontalMargin = 18.f * scale;
        const float verticalMargin = 5.f * scale;
        return math::Rect(
            Vec(std::max(0.f, box.size.x - size - horizontalMargin),
                verticalMargin),
            Vec(size, size));
    }

    LabelBounds textBounds(const DrawArgs& args, float x, float y, const std::string& label) const {
        float bounds[4] = {};
        nvgTextBounds(args.vg, x, y, label.c_str(), NULL, bounds);
        const float LABEL_CLEARANCE = presentationScale();
        return {bounds[0] - LABEL_CLEARANCE, bounds[1] - LABEL_CLEARANCE,
                bounds[2] + LABEL_CLEARANCE, bounds[3] + LABEL_CLEARANCE};
    }

    bool overlapsFrequencyLabel(const LabelBounds& bounds) const {
        for (const LabelBounds& frequencyBounds : frequencyLabelBounds) {
            if (bounds.left <= frequencyBounds.right && bounds.right >= frequencyBounds.left &&
                bounds.top <= frequencyBounds.bottom && bounds.bottom >= frequencyBounds.top)
                return true;
        }
        return false;
    }

    void drawFrequencyGuide(const DrawArgs& args, float frequency, const std::string& label, bool secondary,
                            float& lastLabelPosition) {
        if (frequency < MIN_FREQUENCY_HZ || frequency > maximumFrequency()) return;
        const float scale = presentationScale();
        const float full =
            frequencyCoordinateForHz(frequency, maximumFrequency(), frequencyBins());
        const float visible = (full - viewLow()) / (viewHigh() - viewLow());
        if (visible < 0.f || visible > 1.f) return;
        const float position = visible * (isVerticalFlow(flow()) ? box.size.x : box.size.y);
        const float gridOpacity =
            clampValue(module ? module->frequencyGridOpacitySetting.load() : 1.f, 0.f, 1.f);
        const int gridAlpha =
            static_cast<int>(std::lround((secondary ? 36.f : 64.f) * gridOpacity));
        nvgStrokeColor(args.vg, secondary ? nvgRGBA(155, 184, 194, gridAlpha)
                                         : nvgRGBA(190, 205, 210, gridAlpha));
        nvgStrokeWidth(args.vg, secondary ? 0.45f : 0.7f);
        nvgBeginPath(args.vg);
        if (isVerticalFlow(flow())) {
            nvgMoveTo(args.vg, position, GRID_TOP_INSET);
            nvgLineTo(args.vg, position, box.size.y - GRID_BOTTOM_INSET);
        } else {
            const float y =
                clampValue(box.size.y - position, GRID_TOP_INSET, box.size.y - GRID_BOTTOM_INSET);
            nvgMoveTo(args.vg, 0.f, y);
            nvgLineTo(args.vg, box.size.x, y);
        }
        nvgStroke(args.vg);
        if (label.empty() || std::fabs(position - lastLabelPosition) < 31.f * scale) return;
        lastLabelPosition = position;
        nvgFillColor(args.vg, secondary ? nvgRGBA(180, 198, 204, 95) : nvgRGBA(190, 205, 210, 155));
        nvgFontSize(args.vg, 8.f * scale);
        if (isVerticalFlow(flow())) {
            float labelX = position;
            const bool labelsAtTop = flow() == FlowDirection::UP;
            const int verticalAlignment = labelsAtTop ? NVG_ALIGN_TOP : NVG_ALIGN_BOTTOM;
            int alignment = NVG_ALIGN_CENTER | verticalAlignment;
            if (position < 15.f * scale) {
                labelX = 2.f * scale;
                alignment = NVG_ALIGN_LEFT | verticalAlignment;
            } else if (position > box.size.x - 15.f * scale) {
                labelX = box.size.x - 2.f * scale;
                alignment = NVG_ALIGN_RIGHT | verticalAlignment;
            }
            nvgTextAlign(args.vg, alignment);
            const float labelY =
                labelsAtTop ? GRID_TOP_INSET + 2.f * scale
                            : box.size.y - GRID_BOTTOM_INSET - 2.f * scale;
            frequencyLabelBounds.push_back(textBounds(args, labelX, labelY, label));
            nvgText(args.vg, labelX, labelY, label.c_str(), NULL);
        } else {
            float labelY =
                clampValue(box.size.y - position, GRID_TOP_INSET, box.size.y - GRID_BOTTOM_INSET);
            const bool labelsAtLeft = flow() == FlowDirection::LEFT;
            const int horizontalAlignment = labelsAtLeft ? NVG_ALIGN_LEFT : NVG_ALIGN_RIGHT;
            int alignment = horizontalAlignment | NVG_ALIGN_MIDDLE;
            if (labelY < GRID_TOP_INSET + 7.f * scale) {
                labelY = GRID_TOP_INSET + 2.f * scale;
                alignment = horizontalAlignment | NVG_ALIGN_TOP;
            } else if (labelY > box.size.y - GRID_BOTTOM_INSET - 7.f * scale) {
                labelY = box.size.y - GRID_BOTTOM_INSET - 2.f * scale;
                alignment = horizontalAlignment | NVG_ALIGN_BOTTOM;
            }
            nvgTextAlign(args.vg, alignment);
            const float labelX = labelsAtLeft ? 3.f * scale : box.size.x - 3.f * scale;
            frequencyLabelBounds.push_back(textBounds(args, labelX, labelY, label));
            nvgText(args.vg, labelX, labelY, label.c_str(), NULL);
        }
    }

    void drawGrid(const DrawArgs& args) {
        frequencyLabelBounds.clear();
        if ((!module || module->showFrequencyTicksSetting.load()) &&
            maximumFrequency() > MIN_FREQUENCY_HZ && viewHigh() > viewLow()) {
            const FrequencyScaleMode scale = static_cast<FrequencyScaleMode>(
                clampValue(module ? module->frequencyScaleSetting.load() : static_cast<int>(FrequencyScaleMode::HZ), 0,
                           static_cast<int>(FrequencyScaleMode::COUNT) - 1));
            static const float hzGuides[] = {
                20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f};
            static const float octaveGuides[] = {
                31.25f, 62.5f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f};
            float last = -1000.f;
            if (scale == FrequencyScaleMode::HZ) {
                for (size_t i = 0; i < sizeof(hzGuides) / sizeof(hzGuides[0]); ++i)
                    drawFrequencyGuide(args, hzGuides[i], frequencyLabel(hzGuides[i]), false, last);
            }
            if (scale == FrequencyScaleMode::OCTAVES) {
                for (size_t i = 0; i < sizeof(octaveGuides) / sizeof(octaveGuides[0]); ++i)
                    drawFrequencyGuide(args, octaveGuides[i], frequencyLabel(octaveGuides[i]), false, last);
            }
            if (scale == FrequencyScaleMode::MUSICAL) {
                const float frequencyAxisPixels =
                    isVerticalFlow(flow()) ? box.size.x : box.size.y;
                const float lowFrequency = frequencyFromFullCoordinate(viewLow());
                const float highFrequency = frequencyFromFullCoordinate(viewHigh());
                const float visibleSemitones =
                    12.f * std::log2(std::max(highFrequency / lowFrequency, 1.0001f));
                const float semitonePixels =
                    frequencyAxisPixels / std::max(visibleSemitones, 1.f);
                for (int midi = 12; midi <= 132; ++midi) {
                    if (midi % 12 != 0 && semitonePixels < 18.f) continue;
                    const float frequency = 440.f * std::pow(2.f, (midi - 69) / 12.f);
                    std::string label;
                    if (midi % 12 == 0) label = rack::string::f("C%d", midi / 12 - 1);
                    drawFrequencyGuide(args, frequency, label, midi % 12 != 0, last);
                }
            }
        }
        if (!module || module->showTimeTicksSetting.load()) drawTimeRuler(args);
    }

    void drawTimeRuler(const DrawArgs& args) {
        if (!display) return;
        const float scale = presentationScale();
        const float pixels = isVerticalFlow(flow()) ? box.size.y : box.size.x;
        const std::vector<TimeTick> ticks = display->timeline.makeTicks(48.f * scale, pixels);
        nvgFontSize(args.vg, 7.5f * scale);
        for (size_t i = 0; i < ticks.size(); ++i) {
            float x = 0.f, yBottom = 0.f;
            screenFromLogical(flow(), LogicalPoint(0.f, ticks[i].normalizedAge), x, yBottom);
            const float y = clampValue((1.f - yBottom) * box.size.y, GRID_TOP_INSET,
                                       box.size.y - GRID_BOTTOM_INSET);
            const float gridOpacity =
                clampValue(module ? module->timeGridOpacitySetting.load() : 1.f, 0.f, 1.f);
            const int gridAlpha = static_cast<int>(std::lround(48.f * gridOpacity));
            nvgStrokeColor(args.vg, nvgRGBA(190, 205, 210, gridAlpha));
            nvgBeginPath(args.vg);
            if (isVerticalFlow(flow())) {
                nvgMoveTo(args.vg, 0.f, y);
                nvgLineTo(args.vg, box.size.x, y);
                float labelY = y;
                int alignment = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE;
                if (y < GRID_TOP_INSET + 7.f * scale) {
                    labelY = GRID_TOP_INSET + 2.f * scale;
                    alignment = NVG_ALIGN_LEFT | NVG_ALIGN_TOP;
                } else if (y > box.size.y - GRID_BOTTOM_INSET - 7.f * scale) {
                    labelY = box.size.y - GRID_BOTTOM_INSET - 2.f * scale;
                    alignment = NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM;
                }
                nvgTextAlign(args.vg, alignment);
                nvgFillColor(args.vg, nvgRGBA(190, 205, 210, 135));
                const std::string label =
                    timeLabel(ticks[i].ageSeconds, display->timeline.visibleSpan());
                if (!overlapsFrequencyLabel(textBounds(args, 2.f, labelY, label))) {
                    nvgText(args.vg, 2.f, labelY, label.c_str(), NULL);
                }
            } else {
                const float displayX = x * box.size.x;
                nvgMoveTo(args.vg, displayX, GRID_TOP_INSET);
                nvgLineTo(args.vg, displayX, box.size.y - GRID_BOTTOM_INSET);
                float labelX = displayX;
                int alignment = NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM;
                if (displayX < 15.f * scale) {
                    labelX = 2.f * scale;
                    alignment = NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM;
                } else if (displayX > box.size.x - 15.f * scale) {
                    labelX = box.size.x - 2.f * scale;
                    alignment = NVG_ALIGN_RIGHT | NVG_ALIGN_BOTTOM;
                }
                nvgTextAlign(args.vg, alignment);
                nvgFillColor(args.vg, nvgRGBA(190, 205, 210, 135));
                const std::string label =
                    timeLabel(ticks[i].ageSeconds, display->timeline.visibleSpan());
                if (!overlapsFrequencyLabel(
                        textBounds(args, labelX, box.size.y - GRID_BOTTOM_INSET - scale, label))) {
                    nvgText(args.vg, labelX, box.size.y - GRID_BOTTOM_INSET - scale,
                            label.c_str(), NULL);
                }
            }
            nvgStroke(args.vg);
        }
    }

    void drawMarkers(const DrawArgs& args) {
        if (!display || (module && !module->showMarkersSetting.load())) return;
        const float opacity = clampValue(module ? module->markerOpacitySetting.load() : 0.82f, 0.f, 1.f);
        const int alpha = static_cast<int>(std::lround(255.f * opacity));
        if (alpha <= 0) return;
        const std::vector<MarkerEvent>& markers = display->timeline.markers();
        for (size_t i = 0; i < markers.size(); ++i) {
            const float age = display->timeline.normalizedAgeForSample(markers[i].timelineSample, markers[i].sampleRate);
            if (age < 0.f || age > 1.f) continue;
            float x = 0.f, yBottom = 0.f;
            screenFromLogical(flow(), LogicalPoint(0.f, age), x, yBottom);
            const float y = (1.f - yBottom) * box.size.y;
            const NVGcolor color =
                markers[i].sequence % 2 ? nvgRGBA(255, 193, 72, alpha)
                                        : nvgRGBA(71, 224, 255, alpha);
            nvgStrokeColor(args.vg, color);
            nvgStrokeWidth(args.vg, 1.15f);
            nvgBeginPath(args.vg);
            if (isVerticalFlow(flow())) {
                nvgMoveTo(args.vg, 0.f, y);
                nvgLineTo(args.vg, box.size.x, y);
            } else {
                const float displayX = x * box.size.x;
                nvgMoveTo(args.vg, displayX, 0.f);
                nvgLineTo(args.vg, displayX, box.size.y);
            }
            nvgStroke(args.vg);
        }
    }

    void drawCursor(const DrawArgs& args) {
        if (!hovered || !display) return;
        const float scale = presentationScale();
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
        nvgFontSize(args.vg, 9.f * scale);
        float bounds[4];
        nvgTextBounds(args.vg, 0.f, 0.f, text.c_str(), NULL, bounds);
        float maximumBounds[4];
        nvgTextBounds(args.vg, 0.f, 0.f, "20.0k Hz  A#10 +00c  -160.0 dBFS", NULL, maximumBounds);
        const float tooltipWidth = std::max(bounds[2] - bounds[0], maximumBounds[2] - maximumBounds[0]);
        const float edgePadding = 4.f * scale;
        float textX = cursor.x + 8.f * scale, textY = cursor.y + 8.f * scale;
        if (textX + tooltipWidth > box.size.x - edgePadding)
            textX = cursor.x - tooltipWidth - 8.f * scale;
        textX = clampValue(textX, edgePadding,
                           std::max(edgePadding, box.size.x - tooltipWidth - edgePadding));
        if (textY > box.size.y - 18.f * scale) textY = cursor.y - 17.f * scale;
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, textX - 3.f * scale, textY - 2.f * scale,
                       tooltipWidth + 6.f * scale, 14.f * scale, 2.f * scale);
        nvgFillColor(args.vg, nvgRGBA(0, 0, 0, 190));
        nvgFill(args.vg);
        nvgFillColor(args.vg, nvgRGB(236, 239, 240));
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(args.vg, textX, textY, text.c_str(), NULL);
    }

    void drawLayoutControl(const DrawArgs& args) {
        if (!hovered || !module) return;
        const math::Rect control = layoutControlBox();
        const float scale = layoutControlScale();
        const bool overControl = control.contains(cursor);
        const bool displayOnly = module->displayOnlyModeSetting.load();

        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, control.pos.x, control.pos.y,
                       control.size.x, control.size.y, 3.f * scale);
        nvgFillColor(args.vg,
                     nvgRGBA(0, 0, 0, overControl ? 190 : 125));
        nvgFill(args.vg);

        nvgStrokeColor(
            args.vg,
            displayOnly ? nvgRGBA(105, 225, 255, overControl ? 255 : 210)
                        : nvgRGBA(235, 241, 243, overControl ? 245 : 190));
        nvgStrokeWidth(args.vg, 1.35f * scale);
        nvgLineCap(args.vg, NVG_SQUARE);

        const float outerInset = 6.f * scale;
        const float arm = 5.f * scale;
        const float left = control.pos.x + outerInset;
        const float top = control.pos.y + outerInset;
        const float right = control.pos.x + control.size.x - outerInset;
        const float bottom = control.pos.y + control.size.y - outerInset;
        nvgBeginPath(args.vg);
        if (!displayOnly) {
            nvgMoveTo(args.vg, left + arm, top);
            nvgLineTo(args.vg, left, top);
            nvgLineTo(args.vg, left, top + arm);
            nvgMoveTo(args.vg, right - arm, top);
            nvgLineTo(args.vg, right, top);
            nvgLineTo(args.vg, right, top + arm);
            nvgMoveTo(args.vg, left + arm, bottom);
            nvgLineTo(args.vg, left, bottom);
            nvgLineTo(args.vg, left, bottom - arm);
            nvgMoveTo(args.vg, right - arm, bottom);
            nvgLineTo(args.vg, right, bottom);
            nvgLineTo(args.vg, right, bottom - arm);
        } else {
            nvgMoveTo(args.vg, left, top + arm);
            nvgLineTo(args.vg, left + arm, top + arm);
            nvgLineTo(args.vg, left + arm, top);
            nvgMoveTo(args.vg, right, top + arm);
            nvgLineTo(args.vg, right - arm, top + arm);
            nvgLineTo(args.vg, right - arm, top);
            nvgMoveTo(args.vg, left, bottom - arm);
            nvgLineTo(args.vg, left + arm, bottom - arm);
            nvgLineTo(args.vg, left + arm, bottom);
            nvgMoveTo(args.vg, right, bottom - arm);
            nvgLineTo(args.vg, right - arm, bottom - arm);
            nvgLineTo(args.vg, right - arm, bottom);
        }
        nvgStroke(args.vg);
    }

    void drawOverlay(const DrawArgs& args) {
        nvgSave(args.vg);
        drawGrid(args);
        drawMarkers(args);
        drawCursor(args);
        drawLayoutControl(args);
        nvgRestore(args.vg);
    }

    void draw(const DrawArgs& args) override {
        if (!module && args.fb) {
            drawOverlay(args);
        }
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            drawOverlay(args);
        }
        TransparentWidget::drawLayer(args, layer);
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
            if (module && layoutControlBox().contains(event.pos)) {
                layoutTogglePressed = true;
                draggingTime = false;
                module->displayOnlyModeSetting.store(
                    !module->displayOnlyModeSetting.load());
                event.consume(this);
                return;
            }
            draggingTime = inTimeGutter(event.pos) || (event.mods & GLFW_MOD_SHIFT);
            event.consume(this);
        }
    }
    void onDragMove(const event::DragMove& event) override {
        if (layoutTogglePressed) return;
        if (!module || !display) return;
        const float zoom = std::max(getAbsoluteZoom(), 1e-6f);
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
        layoutTogglePressed = false;
        draggingTime = false;
        TransparentWidget::onDragEnd(event);
    }
    void onHoverScroll(const event::HoverScroll& event) override {
        if (!module || !display || !(APP->window->getMods() & GLFW_MOD_SHIFT)) {
            TransparentWidget::onHoverScroll(event);
            return;
        }
        const LogicalPoint logical = logicalAt(event.pos);
        const float factor = std::pow(1.0015f, event.scrollDelta.y);
        const float low = module->viewMinimum.load(), high = module->viewMaximum.load(), span = high - low;
        const float anchor = low + logical.frequency * span;
        const float nextSpan = clampValue(span * factor, 0.03f, 1.f);
        const float nextLow = clampValue(anchor - logical.frequency * nextSpan, 0.f, 1.f - nextSpan);
        module->viewMinimum.store(nextLow);
        module->viewMaximum.store(nextLow + nextSpan);
        event.consume(this);
    }
    void onDoubleClick(const event::DoubleClick& event) override {
        if (!module || !display) return;
        if (inTimeGutter(cursor)) {
            display->timeline.returnToLive();
            display->lookupDirty = true;
        } else {
            module->viewMinimum.store(0.f);
            module->viewMaximum.store(1.f);
        }
        event.consume(this);
    }
};

struct SpectrumBezel : TransparentWidget {
    void drawBezel(const DrawArgs& args) {
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
        nvgMoveTo(args.vg, 0.f, height - 1.f);
        nvgLineTo(args.vg, width, height - 1.f);
        nvgStrokeColor(args.vg, nvgRGB(0x12, 0x12, 0x12));
        nvgStrokeWidth(args.vg, 2.f);
        nvgStroke(args.vg);

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.f, height - 2.5f);
        nvgLineTo(args.vg, width, height - 2.5f);
        nvgStrokeColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, 0.20f));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgRestore(args.vg);
    }

    void draw(const DrawArgs& args) override {
        if (args.fb) {
            drawBezel(args);
        }
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            drawBezel(args);
        }
        TransparentWidget::drawLayer(args, layer);
    }
};

struct SpectrumPanelArtwork : Widget {
    std::shared_ptr<window::Svg> lightSvg;
    std::shared_ptr<window::Svg> darkSvg;
    bool displayOnly = false;

    static void drawShape(NVGcontext* vg, NSVGimage* source,
                          NSVGshape* sourceShape) {
        // svgDraw() draws an entire linked list. Shallow-copy the image and
        // one shape so the source SVG remains immutable and cached.
        NSVGimage image = *source;
        NSVGshape shape = *sourceShape;
        shape.next = NULL;
        image.shapes = &shape;
        window::svgDraw(vg, &image);
    }

    static NVGcolor svgColor(unsigned int color) {
        return nvgRGBA((color >> 0) & 0xff, (color >> 8) & 0xff,
                       (color >> 16) & 0xff, (color >> 24) & 0xff);
    }

    static bool drawSolidBackground(NVGcontext* vg, const NSVGshape* shape,
                                    const Vec& size) {
        if (shape->fill.type != NSVG_PAINT_COLOR) return false;
        nvgBeginPath(vg);
        nvgRect(vg, 0.f, 0.f, size.x, size.y);
        nvgFillColor(vg, svgColor(shape->fill.color));
        nvgFill(vg);
        return true;
    }

    static bool drawHorizontalLine(NVGcontext* vg, const NSVGshape* shape,
                                   float startX, float endX,
                                   float heightScale) {
        if (shape->stroke.type != NSVG_PAINT_COLOR) return false;
        const float y =
            (shape->bounds[1] + shape->bounds[3]) * 0.5f * heightScale;
        nvgBeginPath(vg);
        nvgMoveTo(vg, startX, y);
        nvgLineTo(vg, endX, y);
        nvgStrokeColor(vg, svgColor(shape->stroke.color));
        nvgStrokeWidth(vg, shape->strokeWidth * heightScale);
        if (shape->strokeLineCap == NSVG_CAP_ROUND)
            nvgLineCap(vg, NVG_ROUND);
        else if (shape->strokeLineCap == NSVG_CAP_SQUARE)
            nvgLineCap(vg, NVG_SQUARE);
        else
            nvgLineCap(vg, NVG_BUTT);
        nvgStroke(vg);
        return true;
    }

    static bool isBackground(const NSVGshape* shape, const Vec& svgSize) {
        return shape->bounds[0] <= 0.f && shape->bounds[1] <= 0.f &&
               shape->bounds[2] >= svgSize.x &&
               shape->bounds[3] >= svgSize.y;
    }

    static bool isHeaderRule(const NSVGshape* shape, const Vec& svgSize) {
        return shape->bounds[0] <= 0.f &&
               shape->bounds[2] >= svgSize.x &&
               std::abs(shape->bounds[3] - shape->bounds[1]) < 0.1f &&
               shape->bounds[1] < 40.f;
    }

    void draw(const DrawArgs& args) override {
        const std::shared_ptr<window::Svg>& svg =
            settings::preferDarkPanels ? darkSvg : lightSvg;
        if (!svg || !svg->handle) return;
        const Vec svgSize = svg->getSize();
        if (svgSize.x <= 0.f || svgSize.y <= 0.f) return;

        const float centerOffset = (box.size.x - svgSize.x) * 0.5f;
        const float heightScale = box.size.y / svgSize.y;

        for (NSVGshape* shape = svg->handle->shapes; shape;
             shape = shape->next) {
            nvgSave(args.vg);

            if (isBackground(shape, svgSize)) {
                if (!drawSolidBackground(args.vg, shape, box.size)) {
                    nvgScale(args.vg, box.size.x / svgSize.x, heightScale);
                    drawShape(args.vg, svg->handle, shape);
                }
                nvgRestore(args.vg);
                continue;
            }
            if (displayOnly) {
                nvgRestore(args.vg);
                continue;
            }
            if (isHeaderRule(shape, svgSize)) {
                if (!drawHorizontalLine(args.vg, shape, 0.f, box.size.x,
                                        heightScale)) {
                    nvgScale(args.vg, box.size.x / svgSize.x, heightScale);
                    drawShape(args.vg, svg->handle, shape);
                }
                nvgRestore(args.vg);
                continue;
            } else {
                float offsetX = 0.f;
                const float minY = shape->bounds[1];

                if (minY < 30.f || minY > 360.f) {
                    // Center the SPECTRUM title and CELLA footer as groups.
                    offsetX = centerOffset;
                }

                nvgTranslate(args.vg, offsetX, 0.f);
                nvgScale(args.vg, 1.f, heightScale);
            }

            drawShape(args.vg, svg->handle, shape);
            nvgRestore(args.vg);
        }
    }
};

struct SpectrumPanel : Widget {
    widget::FramebufferWidget* framebuffer = NULL;
    SpectrumPanelArtwork* artwork = NULL;
    PanelBorder* border = NULL;
    Vec renderedSize;
    bool renderedDark = false;
    bool displayOnly = false;
    bool renderedDisplayOnly = false;

    SpectrumPanel() {
        box.size = Vec(DISPLAY_WIDTH, RACK_GRID_HEIGHT);

        framebuffer = new widget::FramebufferWidget;
        addChild(framebuffer);

        artwork = new SpectrumPanelArtwork;
        artwork->lightSvg =
            window::Svg::load(asset::plugin(pluginInstance, "res/Spectrum.svg"));
        artwork->darkSvg =
            window::Svg::load(asset::plugin(pluginInstance, "res/Spectrum-dark.svg"));
        framebuffer->addChild(artwork);

        border = new PanelBorder;
        framebuffer->addChild(border);
    }

    void step() override {
        framebuffer->oversample = APP->window->pixelRatio < 2.f ? 2.f : 1.f;
        const bool dark = settings::preferDarkPanels;
        if (renderedSize != box.size || renderedDark != dark ||
            renderedDisplayOnly != displayOnly) {
            renderedSize = box.size;
            renderedDark = dark;
            renderedDisplayOnly = displayOnly;
            framebuffer->setSize(box.size);
            artwork->setSize(box.size);
            artwork->displayOnly = displayOnly;
            border->setSize(box.size);
            framebuffer->setDirty();
        }
        Widget::step();
    }
};

struct SpectrumResizeHandle : OpaqueWidget {
    Spectrum* module = NULL;
    bool right = false;
    Vec dragPosition;
    Rect originalBox;

    SpectrumResizeHandle() {
        box.size = Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT);
    }

    void onDragStart(const event::DragStart& event) override {
        if (event.button != GLFW_MOUSE_BUTTON_LEFT || !module) return;
        dragPosition = APP->scene->rack->getMousePos();
        ModuleWidget* moduleWidget = getAncestorOfType<ModuleWidget>();
        if (moduleWidget) originalBox = moduleWidget->box;
    }

    void onDragMove(const event::DragMove& event) override {
        if (!module) return;
        ModuleWidget* moduleWidget = getAncestorOfType<ModuleWidget>();
        if (!moduleWidget) return;

        const Vec currentDragPosition = APP->scene->rack->getMousePos();
        const float deltaX = currentDragPosition.x - dragPosition.x;
        Rect newBox = originalBox;
        const Rect oldBox = moduleWidget->box;
        const float minimumWidth = MIN_PANEL_WIDTH_HP * RACK_GRID_WIDTH;
        const float maximumWidth = MAX_PANEL_WIDTH_HP * RACK_GRID_WIDTH;
        if (right) {
            newBox.size.x =
                clampValue(newBox.size.x + deltaX, minimumWidth, maximumWidth);
        } else {
            newBox.size.x =
                clampValue(newBox.size.x - deltaX, minimumWidth, maximumWidth);
        }
        newBox.size.x =
            std::round(newBox.size.x / RACK_GRID_WIDTH) * RACK_GRID_WIDTH;
        if (!right)
            newBox.pos.x = originalBox.pos.x + originalBox.size.x - newBox.size.x;

        moduleWidget->box = newBox;
        if (!APP->scene->rack->requestModulePos(moduleWidget, newBox.pos))
            moduleWidget->box = oldBox;
        module->panelWidth = clampValue(
            static_cast<int>(std::lround(moduleWidget->box.size.x / RACK_GRID_WIDTH)),
            MIN_PANEL_WIDTH_HP, MAX_PANEL_WIDTH_HP);
    }

};

struct SpectrumView : Widget {
    SpectrumDisplay* display = NULL;
    SpectrumBezel* bezel = NULL;
    SpectrumOverlay* overlay = NULL;

    explicit SpectrumView(Spectrum* module) {
        display = new SpectrumDisplay;
        display->module = module;
        addChild(display);

        bezel = new SpectrumBezel;
        addChild(bezel);

        overlay = new SpectrumOverlay;
        overlay->module = module;
        overlay->display = display;
        addChild(overlay);

        layout();
    }

    void layout() {
        if (display) {
            display->setPosition(Vec());
            display->setSize(box.size);
        }
        if (bezel) {
            bezel->setPosition(Vec());
            bezel->setSize(box.size);
        }
        if (overlay) {
            overlay->setPosition(Vec());
            overlay->setSize(box.size);
        }
    }

    void onResize(const event::Resize& event) override {
        layout();
        Widget::onResize(event);
    }
};

struct SpectrumFloatingWindow : OpaqueWidget {
    static constexpr float MARGIN = 8.f;
    static constexpr float HEADER_HEIGHT = 32.f;
    static constexpr float CLOSE_SIZE = 22.f;
    static constexpr float RESIZE_SIZE = 18.f;
    static constexpr float MINIMUM_WIDTH = 420.f;
    static constexpr float MINIMUM_HEIGHT = 280.f;

    enum class Interaction { NONE, MOVE, RESIZE };

    WeakPtr<Widget> originalParent;
    math::Rect originalBox;
    Spectrum* module = NULL;
    SpectrumView* view = NULL;
    Interaction interaction = Interaction::NONE;
    bool closing = false;

    ~SpectrumFloatingWindow() override {
        restoreView();
    }

    math::Rect closeBox() const {
        return math::Rect(
            Vec(std::max(MARGIN, box.size.x - MARGIN - CLOSE_SIZE), 5.f),
            Vec(CLOSE_SIZE, CLOSE_SIZE));
    }

    math::Rect resizeBox() const {
        return math::Rect(
            Vec(std::max(0.f, box.size.x - RESIZE_SIZE),
                std::max(0.f, box.size.y - RESIZE_SIZE)),
            Vec(RESIZE_SIZE, RESIZE_SIZE));
    }

    math::Rect headerBox() const {
        return math::Rect(Vec(), Vec(box.size.x, HEADER_HEIGHT));
    }

    void layout() {
        if (!view) return;
        const float width = std::max(1.f, box.size.x - 2.f * MARGIN);
        const float height =
            std::max(1.f, box.size.y - HEADER_HEIGHT - MARGIN);
        view->setPosition(Vec(MARGIN, HEADER_HEIGHT));
        view->setSize(Vec(width, height));
    }

    void restoreView() {
        if (!view) return;
        Widget* parent = originalParent.get();
        if (!parent) return;
        if (view->parent) view->parent->removeChild(view);
        parent->addChild(view);
        view->setBox(originalBox);
        view = NULL;
    }

    void close() {
        if (closing) return;
        closing = true;
        restoreView();
        requestDelete();
    }

    void clampToScene() {
        if (!APP || !APP->scene) return;
        const Vec sceneSize = APP->scene->box.size;
        const Vec minimumSize(
            std::min(MINIMUM_WIDTH, std::max(sceneSize.x, 1.f)),
            std::min(MINIMUM_HEIGHT, std::max(sceneSize.y, 1.f)));
        const Vec maximumSize(std::max(sceneSize.x, minimumSize.x),
                              std::max(sceneSize.y, minimumSize.y));
        Vec size(clampValue(box.size.x, minimumSize.x, maximumSize.x),
                 clampValue(box.size.y, minimumSize.y, maximumSize.y));
        Vec position(
            clampValue(box.pos.x, 0.f, std::max(0.f, sceneSize.x - size.x)),
            clampValue(box.pos.y, 0.f, std::max(0.f, sceneSize.y - size.y)));
        setSize(size);
        setPosition(position);
    }

    void step() override {
        clampToScene();
        OpaqueWidget::step();
    }

    void onResize(const event::Resize& event) override {
        layout();
        OpaqueWidget::onResize(event);
    }

    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 5.f);
        nvgFillColor(args.vg, nvgRGBA(3, 5, 8, 250));
        nvgFill(args.vg);

        nvgBeginPath(args.vg);
        nvgRect(args.vg, MARGIN, HEADER_HEIGHT,
                std::max(0.f, box.size.x - 2.f * MARGIN),
                std::max(0.f, box.size.y - HEADER_HEIGHT - MARGIN));
        nvgStrokeColor(args.vg, nvgRGBA(190, 205, 210, 90));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgFontSize(args.vg, 13.f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, nvgRGBA(225, 232, 235, 220));
        nvgText(args.vg, MARGIN, HEADER_HEIGHT * 0.5f, "SPECTRUM", NULL);
        nvgFontSize(args.vg, 9.f);
        nvgFillColor(args.vg, nvgRGBA(180, 194, 200, 150));
        nvgText(args.vg, MARGIN + 82.f, HEADER_HEIGHT * 0.5f,
                "FLOATING VIEW  ·  DRAG TITLE TO MOVE  ·  SPACE TO FREEZE",
                NULL);

        const math::Rect close = closeBox();
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, close.pos.x, close.pos.y, close.size.x,
                       close.size.y, 3.f);
        nvgFillColor(args.vg, nvgRGBA(255, 255, 255, 18));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGBA(225, 232, 235, 175));
        nvgStrokeWidth(args.vg, 1.2f);
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, close.pos.x + 6.f, close.pos.y + 6.f);
        nvgLineTo(args.vg, close.pos.x + close.size.x - 6.f,
                  close.pos.y + close.size.y - 6.f);
        nvgMoveTo(args.vg, close.pos.x + close.size.x - 6.f,
                  close.pos.y + 6.f);
        nvgLineTo(args.vg, close.pos.x + 6.f,
                  close.pos.y + close.size.y - 6.f);
        nvgStroke(args.vg);

        nvgStrokeColor(args.vg, nvgRGBA(225, 232, 235, 90));
        nvgStrokeWidth(args.vg, 1.f);
        for (int line = 0; line < 3; ++line) {
            const float offset = 5.f + line * 4.f;
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, box.size.x - offset, box.size.y - 2.f);
            nvgLineTo(args.vg, box.size.x - 2.f, box.size.y - offset);
            nvgStroke(args.vg);
        }

        OpaqueWidget::draw(args);
        // The view normally receives Rack's light-layer pass through its
        // module container. This floating scene child must issue it itself.
        Widget::drawLayer(args, 1);
    }

    void onButton(const event::Button& event) override {
        if (event.action == GLFW_PRESS &&
            event.button == GLFW_MOUSE_BUTTON_LEFT) {
            if (closeBox().contains(event.pos)) {
                close();
                event.consume(this);
                return;
            }
            if (resizeBox().contains(event.pos)) {
                interaction = Interaction::RESIZE;
                event.consume(this);
                return;
            }
            if (headerBox().contains(event.pos)) {
                interaction = Interaction::MOVE;
                event.consume(this);
                return;
            }
        }
        OpaqueWidget::onButton(event);
    }

    void onDragMove(const event::DragMove& event) override {
        if (event.button != GLFW_MOUSE_BUTTON_LEFT ||
            interaction == Interaction::NONE)
            return;
        const float zoom = std::max(getAbsoluteZoom(), 1e-6f);
        const Vec delta = event.mouseDelta.div(zoom);
        if (interaction == Interaction::MOVE)
            setPosition(box.pos.plus(delta));
        else
            setSize(box.size.plus(delta));
        clampToScene();
    }

    void onDragEnd(const event::DragEnd& event) override {
        interaction = Interaction::NONE;
        OpaqueWidget::onDragEnd(event);
    }

    void onHoverKey(const event::HoverKey& event) override {
        if (event.action == GLFW_PRESS) {
            if (event.key == GLFW_KEY_SPACE && module) {
                module->freezeToggleRequested.store(true,
                                                    std::memory_order_release);
                event.consume(this);
                return;
            }
            if (event.key == GLFW_KEY_ESCAPE) {
                close();
                event.consume(this);
                return;
            }
        }
        OpaqueWidget::onHoverKey(event);
    }
};

}  // namespace

struct SpectrumWidget : ModuleWidget {
    struct BottomItem {
        Widget* widget = NULL;
        int slot = 0;
        bool port = false;

        BottomItem(Widget* widget, int slot, bool port)
            : widget(widget), slot(slot), port(port) {}
    };

    SpectrumPanel* spectrumPanel = NULL;
    SpectrumView* spectrumView = NULL;
    SpectrumResizeHandle* rightHandle = NULL;
    std::vector<BottomItem> bottomItems;
    WeakPtr<SpectrumFloatingWindow> floatingWindow;

    SpectrumWidget(Spectrum* module) {
        setModule(module);
        spectrumPanel = new SpectrumPanel;
        setPanel(spectrumPanel);
        if (module)
            box.size.x = module->panelWidth * RACK_GRID_WIDTH;

        spectrumView = new SpectrumView(module);
        spectrumView->setBox(
            math::Rect(Vec(DISPLAY_X, DISPLAY_Y), Vec(box.size.x, DISPLAY_HEIGHT)));
        addChild(spectrumView);

        constexpr float y = 329.5f;
        addBottomInput(
            createInputCentered<ThemedPJ301MPort>(Vec(0.f, y), module,
                                                  Spectrum::LEFT_INPUT),
            0);
        addBottomInput(
            createInputCentered<ThemedPJ301MPort>(Vec(0.f, y), module,
                                                  Spectrum::RIGHT_INPUT),
            1);
        addBottomInput(
            createInputCentered<ThemedPJ301MPort>(Vec(0.f, y), module,
                                                  Spectrum::MARK_INPUT),
            2);
        addBottomParam(
            createParamCentered<RoundSmallBlackKnob>(Vec(0.f, y), module,
                                                     Spectrum::RANGE_PARAM),
            3);
        addBottomParam(
            createParamCentered<RoundSmallBlackKnob>(Vec(0.f, y), module,
                                                     Spectrum::SPEED_PARAM),
            4);
        addBottomInput(
            createInputCentered<ThemedPJ301MPort>(Vec(0.f, y), module,
                                                  Spectrum::FREEZE_INPUT),
            5);
        addBottomParam(
            createParamCentered<LEDButton>(Vec(0.f, y), module,
                                           Spectrum::FREEZE_PARAM),
            6);
        addBottomChild(
            createLightCentered<MediumLight<YellowLight>>(
                Vec(0.f, y), module, Spectrum::FREEZE_LIGHT),
            6);
        addBottomParam(
            createParamCentered<VCVButton>(Vec(0.f, y), module,
                                           Spectrum::CLEAR_PARAM),
            7);

        SpectrumResizeHandle* leftHandle = new SpectrumResizeHandle;
        leftHandle->module = module;
        addChild(leftHandle);

        rightHandle = new SpectrumResizeHandle;
        rightHandle->module = module;
        rightHandle->right = true;
        addChild(rightHandle);

        layout();
    }

    ~SpectrumWidget() override {
        if (SpectrumFloatingWindow* floating = floatingWindow.get()) floating->close();
    }

    void addBottomChild(Widget* widget, int slot) {
        addChild(widget);
        bottomItems.push_back({widget, slot, false});
    }

    void addBottomInput(PortWidget* widget, int slot) {
        addInput(widget);
        bottomItems.push_back({widget, slot, true});
    }

    void addBottomParam(ParamWidget* widget, int slot) {
        addParam(widget);
        bottomItems.push_back({widget, slot, false});
    }

    void layout() {
        Spectrum* spectrum = dynamic_cast<Spectrum*>(module);
        const bool displayOnly =
            spectrum && spectrum->displayOnlyModeSetting.load();
        if (spectrumPanel) {
            spectrumPanel->displayOnly = displayOnly;
            spectrumPanel->setSize(box.size);
        }
        if (spectrumView) {
            if (spectrumView->bezel)
                spectrumView->bezel->setVisible(!displayOnly);
            if (spectrumView->parent == this) {
                if (displayOnly) {
                    constexpr float portRailWidth = 45.f;
                    spectrumView->setBox(math::Rect(
                        Vec(portRailWidth, 1.f),
                        Vec(std::max(1.f, box.size.x - portRailWidth - 1.f),
                            RACK_GRID_HEIGHT - 2.f)));
                } else {
                    spectrumView->setBox(
                        math::Rect(Vec(DISPLAY_X, DISPLAY_Y),
                                   Vec(box.size.x, DISPLAY_HEIGHT)));
                }
            }
        }
        int portIndex = 0;
        for (const BottomItem& item : bottomItems) {
            if (!item.widget) continue;
            item.widget->setVisible(!displayOnly || item.port);
            float centerX = 22.5f + 45.f * item.slot;
            float centerY = 329.5f;
            if (displayOnly && item.port) {
                centerX = 22.5f;
                centerY = 67.f + 82.f * portIndex;
                ++portIndex;
            }
            item.widget->box.pos.x = centerX - item.widget->box.size.x * 0.5f;
            item.widget->box.pos.y = centerY - item.widget->box.size.y * 0.5f;
        }
        if (rightHandle)
            rightHandle->box.pos.x = box.size.x - rightHandle->box.size.x;
    }

    void step() override {
        Spectrum* spectrum = dynamic_cast<Spectrum*>(module);
        if (spectrum)
            box.size.x = spectrum->panelWidth * RACK_GRID_WIDTH;
        layout();
        ModuleWidget::step();
    }

    void openFloatingWindow() {
        if (!spectrumView || floatingWindow || !APP || !APP->scene ||
            !spectrumView->parent)
            return;

        SpectrumFloatingWindow* floating = new SpectrumFloatingWindow;
        floating->originalParent = spectrumView->parent;
        floating->originalBox = spectrumView->box;
        floating->module = dynamic_cast<Spectrum*>(module);
        floating->view = spectrumView;

        spectrumView->parent->removeChild(spectrumView);
        floating->addChild(spectrumView);

        const Vec sceneSize = APP->scene->box.size;
        const Vec size(
            clampValue(sceneSize.x * 0.62f, SpectrumFloatingWindow::MINIMUM_WIDTH,
                       std::max(SpectrumFloatingWindow::MINIMUM_WIDTH,
                                sceneSize.x - 32.f)),
            clampValue(sceneSize.y * 0.62f,
                       SpectrumFloatingWindow::MINIMUM_HEIGHT,
                       std::max(SpectrumFloatingWindow::MINIMUM_HEIGHT,
                                sceneSize.y - 32.f)));
        floating->setBox(math::Rect(
            Vec(std::max(0.f, (sceneSize.x - size.x) * 0.5f),
                std::max(0.f, (sceneSize.y - size.y) * 0.5f)),
            size));
        APP->scene->addChild(floating);
        floatingWindow = floating;
    }

    void appendContextMenu(Menu* menu) override {
        Spectrum* spectrum = dynamic_cast<Spectrum*>(module);
        if (!spectrum) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem("Open floating window", "", [this]() {
            openFloatingWindow();
        }));
        menu->addChild(createNonClosingBoolMenuItem(
            "Display only",
            [=]() { return spectrum->displayOnlyModeSetting.load(); },
            [=](bool value) {
                spectrum->displayOnlyModeSetting.store(value);
            }));
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Analysis"));
        menu->addChild(createNonClosingIndexSubmenuItem(
            "Analysis mode",
            {"Classic", "T-F Reassigned"},
            [=]() {
                return static_cast<size_t>(
                    clampValue(spectrum->analysisModeSetting.load(), 0,
                               static_cast<int>(AnalysisMode::COUNT) - 1));
            },
            [=](size_t value) {
                spectrum->analysisModeSetting.store(static_cast<int>(value));
            }));
        menu->addChild(createNonClosingIndexSubmenuItem(
            "Channel mode", {"Left", "Right", "Mono", "Mid", "Side"},
            [=]() {
                return static_cast<size_t>(
                    clampValue(static_cast<int>(std::lround(spectrum->params[Spectrum::MODE_PARAM].getValue())),
                               0, static_cast<int>(ChannelMode::COUNT) - 1));
            },
            [=](size_t value) {
                spectrum->params[Spectrum::MODE_PARAM].setValue(
                    static_cast<float>(clampValue(static_cast<int>(value), 0,
                                                  static_cast<int>(ChannelMode::COUNT) - 1)));
            }));
        menu->addChild(createNonClosingIndexSubmenuItem(
            "FFT size", {"1024", "2048", "4096", "8192", "16384"},
            [=]() { return static_cast<size_t>(clampValue(spectrum->fftSizeSetting.load(), 0, 4)); },
            [=](size_t value) { spectrum->fftSizeSetting.store(static_cast<int>(value)); }));
        menu->addChild(createNonClosingIndexSubmenuItem(
            "FFT overlap", {"None", "25%", "50%", "75%", "87.5%", "93.75%"},
            [=]() {
                return static_cast<size_t>(
                    clampValue(spectrum->fftOverlapSetting.load(), 0, static_cast<int>(FftOverlap::COUNT) - 1));
            },
            [=](size_t value) { spectrum->fftOverlapSetting.store(static_cast<int>(value)); }));
        menu->addChild(createNonClosingIndexSubmenuItem(
            "Window", {"Hann", "Blackman-Harris", "Flat-top"},
            [=]() { return static_cast<size_t>(clampValue(spectrum->windowSetting.load(), 0, 2)); },
            [=](size_t value) { spectrum->windowSetting.store(static_cast<int>(value)); }));
        menu->addChild(createNonClosingIndexSubmenuItem(
            "Time detail", {"Economy · 15 rows/s", "Normal · 30 rows/s", "High · 60 rows/s"},
            [=]() { return static_cast<size_t>(clampValue(spectrum->qualitySetting.load(), 0, 2)); },
            [=](size_t value) { spectrum->qualitySetting.store(static_cast<int>(value)); }));
        std::vector<std::string> channels;
        for (int channel = 1; channel <= 16; ++channel) channels.push_back(rack::string::f("Channel %d", channel));
        menu->addChild(createNonClosingIndexSubmenuItem(
            "Polyphonic voice", channels,
            [=]() { return static_cast<size_t>(clampValue(spectrum->polyChannelSetting.load(), 0, 15)); },
            [=](size_t value) { spectrum->polyChannelSetting.store(static_cast<int>(value)); }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Presentation"));
        menu->addChild(createNonClosingIndexSubmenuItem(
            "Flow", {"Up", "Down", "Left", "Right"},
            [=]() { return static_cast<size_t>(clampValue(spectrum->flowSetting.load(), 0, 3)); },
            [=](size_t value) { spectrum->flowSetting.store(static_cast<int>(value)); }));
        const Palette selectedPalette = static_cast<Palette>(
            clampValue(spectrum->paletteSetting.load(), 0, static_cast<int>(Palette::COUNT) - 1));
        menu->addChild(createSubmenuItem(
            "Palette", paletteDefinition(selectedPalette).name, [=](Menu* paletteMenu) {
                for (const PaletteMenuGroup& group : paletteMenuGroups()) {
                    const std::vector<Palette> palettes = group.palettes;
                    std::string activeName;
                    for (Palette palette : palettes) {
                        if (palette == static_cast<Palette>(spectrum->paletteSetting.load()))
                            activeName = paletteDefinition(palette).name;
                    }
                    paletteMenu->addChild(createSubmenuItem(
                        group.name, activeName, [=](Menu* groupMenu) {
                            for (Palette palette : palettes) {
                                groupMenu->addChild(createNonClosingCheckMenuItem(
                                    paletteDefinition(palette).name,
                                    [=]() {
                                        return spectrum->paletteSetting.load() ==
                                               static_cast<int>(palette);
                                    },
                                    [=]() {
                                        spectrum->paletteSetting.store(static_cast<int>(palette));
                                    }));
                            }
                        }));
                }
            }));
        menu->addChild(createNonClosingIndexSubmenuItem(
            "Rendering", {"Precise", "Smooth"},
            [=]() { return static_cast<size_t>(clampValue(spectrum->renderingStyleSetting.load(), 0, 1)); },
            [=](size_t value) { spectrum->renderingStyleSetting.store(static_cast<int>(value)); }));
        menu->addChild(createSubmenuItem("Trace", "", [=](Menu* traceMenu) {
            traceMenu->addChild(createNonClosingIndexSubmenuItem(
                "Live", {"Off", "Line", "Line + Fill"},
                [=]() { return static_cast<size_t>(clampValue(spectrum->liveTraceSetting.load(), 0, 2)); },
                [=](size_t value) { spectrum->liveTraceSetting.store(static_cast<int>(value)); }));
            traceMenu->addChild(createNonClosingIndexSubmenuItem(
                "Peak", {"Off", "Decay", "Infinite hold"},
                [=]() { return static_cast<size_t>(clampValue(spectrum->peakHoldSetting.load(), 0, 2)); },
                [=](size_t value) { spectrum->peakHoldSetting.store(static_cast<int>(value)); }));
        }));
        menu->addChild(createSubmenuItem("Ticks", "", [=](Menu* ticksMenu) {
            ticksMenu->addChild(createSubmenuItem(
                "Frequency", spectrum->showFrequencyTicksSetting.load() ? "On" : "Off",
                [=](Menu* frequencyTicksMenu) {
                    frequencyTicksMenu->addChild(createNonClosingBoolMenuItem(
                        "On",
                        [=]() { return spectrum->showFrequencyTicksSetting.load(); },
                        [=](bool value) { spectrum->showFrequencyTicksSetting.store(value); }));
                    frequencyTicksMenu->addChild(new GridOpacitySlider(
                        &spectrum->frequencyGridOpacitySetting, "Frequency grid opacity"));
                }));
            ticksMenu->addChild(createSubmenuItem(
                "Time", spectrum->showTimeTicksSetting.load() ? "On" : "Off",
                [=](Menu* timeTicksMenu) {
                    timeTicksMenu->addChild(createNonClosingBoolMenuItem(
                        "On",
                        [=]() { return spectrum->showTimeTicksSetting.load(); },
                        [=](bool value) { spectrum->showTimeTicksSetting.store(value); }));
                    timeTicksMenu->addChild(new GridOpacitySlider(
                        &spectrum->timeGridOpacitySetting, "Time grid opacity"));
                }));
        }));
        menu->addChild(createSubmenuItem("Frequency", "", [=](Menu* frequencyMenu) {
            frequencyMenu->addChild(createNonClosingIndexSubmenuItem(
                "Scale", {"Hz", "Octaves", "Musical"},
                [=]() {
                    return static_cast<size_t>(
                        clampValue(spectrum->frequencyScaleSetting.load(), 0,
                                   static_cast<int>(FrequencyScaleMode::COUNT) - 1));
                },
                [=](size_t value) {
                    spectrum->frequencyScaleSetting.store(static_cast<int>(value));
                }));
            frequencyMenu->addChild(createNonClosingIndexSubmenuItem(
                "Smoothing",
                {"None", "1/48 octave", "1/24 octave", "1/12 octave", "1/6 octave", "1/3 octave"},
                [=]() {
                    return static_cast<size_t>(
                        clampValue(spectrum->frequencySmoothingSetting.load(), 0, 5));
                },
                [=](size_t value) {
                    spectrum->frequencySmoothingSetting.store(static_cast<int>(value));
                }));
            frequencyMenu->addChild(createNonClosingIndexSubmenuItem(
                "Bins", {"Log", "Linear", "Mel"},
                [=]() {
                    return static_cast<size_t>(
                        clampValue(spectrum->frequencyBinsSetting.load(), 0,
                                   static_cast<int>(FrequencyBinScale::COUNT) - 1));
                },
                [=](size_t value) {
                    spectrum->frequencyBinsSetting.store(static_cast<int>(value));
                    spectrum->viewMinimum.store(0.f);
                    spectrum->viewMaximum.store(1.f);
                }));
            frequencyMenu->addChild(new MenuSeparator);
            frequencyMenu->addChild(createMenuItem("Reset zoom", "", [=]() {
                spectrum->viewMinimum.store(0.f);
                spectrum->viewMaximum.store(1.f);
            }));
        }));
        menu->addChild(createSubmenuItem("Markers", "", [=](Menu* markersMenu) {
            markersMenu->addChild(createNonClosingBoolMenuItem(
                "Show markers",
                [=]() { return spectrum->showMarkersSetting.load(); },
                [=](bool value) { spectrum->showMarkersSetting.store(value); }));
            markersMenu->addChild(new MarkerOpacitySlider(spectrum));
        }));
    }
};

Model* modelSpectrum = createModel<Spectrum, SpectrumWidget>("Spectrum");

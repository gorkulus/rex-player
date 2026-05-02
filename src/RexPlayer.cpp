#include "plugin.hpp"

#include "velociloops.h"
#include <osdialog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxVoices = PORT_MAX_CHANNELS;
constexpr int kOverviewBins = 768;
constexpr int kFadeSamples = 64;
constexpr float kRackAudioVolts = 5.f;

static std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

static std::string midiNoteName(int note) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int octave = note / 12 - 1;
    int pc = note % 12;
    if (pc < 0) pc += 12;
    std::ostringstream os;
    os << names[pc] << octave << " / MIDI " << note;
    return os.str();
}

struct RexSlice {
    int index = 0;
    int sampleStart = 0;
    int sampleLength = 0;
    int renderedFrames = 0;
    int flags = 0;
    int ppqPos = 0;
    std::vector<float> left;
    std::vector<float> right;
};

struct RexBuffer {
    uint64_t generation = 0;
    std::string path;
    std::string displayName;
    int channels = 0;
    int sampleRate = 44100;
    int totalFrames = 0;
    int ppqLength = 0;
    float tempoBpm = 0.f;
    std::vector<RexSlice> slices;
    std::vector<float> overview;
    std::vector<float> slicePositions;
};

struct VLHandle {
    VLFile file = nullptr;
    ~VLHandle() {
        if (file) {
            vl_close(file);
        }
    }
};

static std::shared_ptr<RexBuffer> loadRexBufferFromFile(const std::string& path, uint64_t generation, std::string& error) {
    VLError err = VL_OK;
    VLHandle handle;
    handle.file = vl_open(path.c_str(), &err);
    if (!handle.file) {
        error = std::string("vl_open failed: ") + vl_error_string(err);
        return nullptr;
    }

    VLFileInfo info = {};
    err = vl_get_info(handle.file, &info);
    if (err != VL_OK) {
        error = std::string("vl_get_info failed: ") + vl_error_string(err);
        return nullptr;
    }
    if (info.channels != 1 && info.channels != 2) {
        error = "Unsupported REX channel count: " + std::to_string(info.channels);
        return nullptr;
    }
    if (info.slice_count <= 0) {
        error = "REX file has no playable slices";
        return nullptr;
    }

    auto buffer = std::make_shared<RexBuffer>();
    buffer->generation = generation;
    buffer->path = path;
    buffer->displayName = baseName(path);
    buffer->channels = info.channels;
    buffer->sampleRate = info.sample_rate > 0 ? info.sample_rate : 44100;
    buffer->totalFrames = std::max(1, info.total_frames);
    buffer->ppqLength = info.ppq_length;
    buffer->tempoBpm = info.tempo > 0 ? static_cast<float>(info.tempo) / 1000.f : 0.f;
    buffer->overview.assign(kOverviewBins, 0.f);
    buffer->slices.reserve(static_cast<size_t>(info.slice_count));
    buffer->slicePositions.reserve(static_cast<size_t>(info.slice_count));

    int inferredTotalFrames = buffer->totalFrames;

    for (int i = 0; i < info.slice_count; ++i) {
        VLSliceInfo si = {};
        err = vl_get_slice_info(handle.file, i, &si);
        if (err != VL_OK) {
            error = std::string("vl_get_slice_info failed at slice ") + std::to_string(i) + ": " + vl_error_string(err);
            return nullptr;
        }

        const int32_t renderedFrames = vl_get_slice_frame_count(handle.file, i);
        if (renderedFrames <= 0) {
            error = "Invalid rendered frame count at slice " + std::to_string(i);
            return nullptr;
        }

        RexSlice slice;
        slice.index = i;
        slice.sampleStart = std::max(0, si.sample_start);
        slice.sampleLength = std::max(0, si.sample_length);
        slice.renderedFrames = renderedFrames;
        slice.flags = si.flags;
        slice.ppqPos = si.ppq_pos;
        slice.left.assign(static_cast<size_t>(renderedFrames), 0.f);
        slice.right.assign(static_cast<size_t>(renderedFrames), 0.f);

        int32_t written = 0;
        err = vl_decode_slice(handle.file, i, slice.left.data(), slice.right.data(), 0, renderedFrames, &written);
        if (err != VL_OK || written != renderedFrames) {
            error = std::string("vl_decode_slice failed at slice ") + std::to_string(i) + ": " + vl_error_string(err);
            return nullptr;
        }

        inferredTotalFrames = std::max(inferredTotalFrames, slice.sampleStart + std::max(slice.sampleLength, slice.renderedFrames));
        buffer->slices.push_back(std::move(slice));
    }

    buffer->totalFrames = std::max(1, inferredTotalFrames);
    for (const RexSlice& slice : buffer->slices) {
        buffer->slicePositions.push_back(clampf(static_cast<float>(slice.sampleStart) / static_cast<float>(buffer->totalFrames), 0.f, 1.f));
        const int displayFrames = std::min(slice.sampleLength > 0 ? slice.sampleLength : slice.renderedFrames, slice.renderedFrames);
        for (int j = 0; j < displayFrames; ++j) {
            const int absFrame = slice.sampleStart + j;
            const int bin = std::max(0, std::min(kOverviewBins - 1, static_cast<int>((static_cast<int64_t>(absFrame) * kOverviewBins) / buffer->totalFrames)));
            const float peak = std::max(std::fabs(slice.left[static_cast<size_t>(j)]), std::fabs(slice.right[static_cast<size_t>(j)]));
            buffer->overview[static_cast<size_t>(bin)] = std::max(buffer->overview[static_cast<size_t>(bin)], peak);
        }
    }

    float maxPeak = 0.f;
    for (float p : buffer->overview) {
        maxPeak = std::max(maxPeak, p);
    }
    if (maxPeak > 0.f) {
        for (float& p : buffer->overview) {
            p = clampf(p / maxPeak, 0.f, 1.f);
        }
    }

    return buffer;
}

struct Voice {
    bool active = false;
    int slice = 0;
    float pos = 0.f;
    float inc = 1.f;
    int fadeIn = 0;
    int fadeInTotal = 1;
    int fadeOut = 0;
    int fadeOutTotal = 1;

    void clear() {
        active = false;
        slice = 0;
        pos = 0.f;
        inc = 1.f;
        fadeIn = 0;
        fadeOut = 0;
        fadeInTotal = 1;
        fadeOutTotal = 1;
    }
};

static void renderVoice(Voice& voice, const RexBuffer& buffer, float& outL, float& outR) {
    if (!voice.active || voice.slice < 0 || voice.slice >= static_cast<int>(buffer.slices.size())) {
        voice.active = false;
        return;
    }

    const RexSlice& slice = buffer.slices[static_cast<size_t>(voice.slice)];
    if (slice.renderedFrames <= 1 || voice.pos >= static_cast<float>(slice.renderedFrames - 1)) {
        voice.clear();
        return;
    }

    const int i0 = std::max(0, std::min(slice.renderedFrames - 1, static_cast<int>(voice.pos)));
    const int i1 = std::min(slice.renderedFrames - 1, i0 + 1);
    const float frac = voice.pos - static_cast<float>(i0);
    const float l = slice.left[static_cast<size_t>(i0)] + (slice.left[static_cast<size_t>(i1)] - slice.left[static_cast<size_t>(i0)]) * frac;
    const float r = slice.right[static_cast<size_t>(i0)] + (slice.right[static_cast<size_t>(i1)] - slice.right[static_cast<size_t>(i0)]) * frac;

    float gain = 1.f;
    if (voice.fadeIn > 0) {
        gain *= 1.f - static_cast<float>(voice.fadeIn) / static_cast<float>(std::max(1, voice.fadeInTotal));
        voice.fadeIn--;
    }
    if (voice.fadeOut > 0) {
        gain *= static_cast<float>(voice.fadeOut) / static_cast<float>(std::max(1, voice.fadeOutTotal));
        voice.fadeOut--;
        if (voice.fadeOut <= 0) {
            voice.clear();
        }
    }

    outL += l * gain;
    outR += r * gain;

    if (voice.active) {
        voice.pos += voice.inc;
        if (voice.pos >= static_cast<float>(slice.renderedFrames - 1)) {
            voice.clear();
        }
    }
}

} // namespace

struct RexPlayer : Module {
    enum ParamId { PARAMS_LEN };
    enum InputId { SLICE_INPUT, PITCH_INPUT, TRIG_INPUT, STEP_TRIG_INPUT, INPUTS_LEN };
    enum OutputId { LEFT_OUTPUT, RIGHT_OUTPUT, OUTPUTS_LEN };
    enum LightId { STATUS_LIGHT, POLY_LIGHT, LIGHTS_LEN };

    std::shared_ptr<const RexBuffer> buffer;
    std::atomic<uint64_t> nextGeneration{1};
    uint64_t processGeneration = 0;

    std::atomic<int> baseMidiNote{36}; // C2. Local rx2-to-midi uses 36+i for REX slices.
    std::atomic<int> selectedSlice{0};
    std::atomic<int> lastTriggeredSlice{-1};
    std::atomic<int> activeVoices{0};
    std::atomic<bool> polyMode{false};
    std::atomic<float> playhead01{-1.f};

    std::string lastPath;
    std::string lastStatus = "No REX loaded";

    std::array<Voice, kMaxVoices> voices;
    std::array<Voice, kMaxVoices> tails;
    std::array<dsp::SchmittTrigger, kMaxVoices> trigTriggers;
    std::array<dsp::SchmittTrigger, kMaxVoices> stepTriggers;
    int stepSlice = 0;
    int polyVoiceCursor = 0;
    int lastVoiceIndex = 0;

    RexPlayer() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configInput(SLICE_INPUT, "Slice select V/Oct");
        configInput(PITCH_INPUT, "Playback pitch V/Oct");
        configInput(TRIG_INPUT, "Trigger current slice");
        configInput(STEP_TRIG_INPUT, "Trigger current slice and step");
        configOutput(LEFT_OUTPUT, "Left master");
        configOutput(RIGHT_OUTPUT, "Right master");
        configLight(STATUS_LIGHT, "REX loaded");
        configLight(POLY_LIGHT, "Polyphonic voice mode");
    }

    std::shared_ptr<const RexBuffer> getBuffer() const {
        return std::atomic_load_explicit(&buffer, std::memory_order_acquire);
    }

    void clearVoices() {
        for (Voice& v : voices) v.clear();
        for (Voice& t : tails) t.clear();
        activeVoices.store(0, std::memory_order_relaxed);
        playhead01.store(-1.f, std::memory_order_relaxed);
    }

    bool loadFile(const std::string& path) {
        std::string error;
        const uint64_t generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
        std::shared_ptr<RexBuffer> loaded = loadRexBufferFromFile(path, generation, error);
        if (!loaded) {
            lastStatus = error;
            WARN("RexRack: failed to load %s: %s", path.c_str(), error.c_str());
            return false;
        }

        lastPath = path;
        std::ostringstream status;
        status << loaded->displayName << " — " << loaded->slices.size() << " slices, " << loaded->sampleRate << " Hz";
        if (loaded->tempoBpm > 0.f) {
            status << ", " << loaded->tempoBpm << " BPM";
        }
        lastStatus = status.str();
        stepSlice = 0;
        selectedSlice.store(0, std::memory_order_relaxed);
        lastTriggeredSlice.store(-1, std::memory_order_relaxed);
        std::atomic_store_explicit(&buffer, std::static_pointer_cast<const RexBuffer>(loaded), std::memory_order_release);
        return true;
    }

    void chooseAndLoadFile() {
        osdialog_filters* filters = osdialog_filters_parse("REX/RX2:rx2,rex,rcy;All files:*");
        char* path = osdialog_file(OSDIALOG_OPEN, nullptr, nullptr, filters);
        osdialog_filters_free(filters);
        if (!path) {
            return;
        }
        std::string selected = path;
        std::free(path);
        loadFile(selected);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "path", json_string(lastPath.c_str()));
        json_object_set_new(rootJ, "baseMidiNote", json_integer(baseMidiNote.load(std::memory_order_relaxed)));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* baseJ = json_object_get(rootJ, "baseMidiNote");
        if (baseJ) {
            baseMidiNote.store(static_cast<int>(json_integer_value(baseJ)), std::memory_order_relaxed);
        }
        json_t* pathJ = json_object_get(rootJ, "path");
        if (pathJ) {
            const char* path = json_string_value(pathJ);
            if (path && std::strlen(path) > 0) {
                loadFile(path);
            }
        }
    }

    int sliceFromVoltage(const RexBuffer& b, float volts) const {
        const int note = static_cast<int>(std::round(60.f + volts * 12.f)); // Rack V/OCT: 0V = C4/MIDI 60
        return std::max(0, std::min(static_cast<int>(b.slices.size()) - 1, note - baseMidiNote.load(std::memory_order_relaxed)));
    }

    int sliceForChannel(const RexBuffer& b, int c, int channelCount) {
        if (inputs[SLICE_INPUT].isConnected()) {
            return sliceFromVoltage(b, inputs[SLICE_INPUT].getPolyVoltage(static_cast<uint8_t>(c)));
        }
        if (channelCount > 1) {
            return (stepSlice + c) % static_cast<int>(b.slices.size());
        }
        return std::max(0, std::min(static_cast<int>(b.slices.size()) - 1, stepSlice));
    }

    void copyVoiceToTail(int index) {
        if (index < 0 || index >= kMaxVoices || !voices[static_cast<size_t>(index)].active) {
            return;
        }
        tails[static_cast<size_t>(index)] = voices[static_cast<size_t>(index)];
        tails[static_cast<size_t>(index)].fadeIn = 0;
        tails[static_cast<size_t>(index)].fadeOut = kFadeSamples;
        tails[static_cast<size_t>(index)].fadeOutTotal = kFadeSamples;
        voices[static_cast<size_t>(index)].clear();
    }

    void chokeAllToTails() {
        for (int i = 0; i < kMaxVoices; ++i) {
            copyVoiceToTail(i);
        }
    }

    void triggerSlice(const RexBuffer& b, int slice, float pitchVolts, bool usePolyMode, int voiceCount) {
        if (b.slices.empty()) return;
        slice = std::max(0, std::min(static_cast<int>(b.slices.size()) - 1, slice));
        const float pitch = std::pow(2.f, pitchVolts);
        const float sampleRate = APP ? APP->engine->getSampleRate() : 44100.f;
        const float inc = (static_cast<float>(b.sampleRate) / std::max(1.f, sampleRate)) * pitch;

        int voiceIndex = 0;
        if (usePolyMode) {
            voiceCount = std::max(1, std::min(kMaxVoices, voiceCount));
            voiceIndex = polyVoiceCursor % voiceCount;
            polyVoiceCursor = (polyVoiceCursor + 1) % voiceCount;
            // If the voice pool wraps onto an active voice, fade it out instead of hard stealing.
            copyVoiceToTail(voiceIndex);
        }
        else {
            chokeAllToTails();
            voiceIndex = 0;
            polyVoiceCursor = 0;
        }

        Voice& voice = voices[static_cast<size_t>(voiceIndex)];
        voice.clear();
        voice.active = true;
        voice.slice = slice;
        voice.pos = 0.f;
        voice.inc = inc;
        voice.fadeIn = kFadeSamples;
        voice.fadeInTotal = kFadeSamples;
        lastVoiceIndex = voiceIndex;
        selectedSlice.store(slice, std::memory_order_relaxed);
        lastTriggeredSlice.store(slice, std::memory_order_relaxed);
    }

    void process(const ProcessArgs& args) override {
        std::shared_ptr<const RexBuffer> b = getBuffer();
        if (!b || b->slices.empty()) {
            outputs[LEFT_OUTPUT].setVoltage(0.f);
            outputs[RIGHT_OUTPUT].setVoltage(0.f);
            lights[STATUS_LIGHT].setBrightnessSmooth(0.f, args.sampleTime);
            lights[POLY_LIGHT].setBrightnessSmooth(0.f, args.sampleTime);
            return;
        }

        if (processGeneration != b->generation) {
            processGeneration = b->generation;
            clearVoices();
            stepSlice = 0;
            polyVoiceCursor = 0;
        }

        const int triggerChannels = std::max(inputs[TRIG_INPUT].getChannels(), inputs[STEP_TRIG_INPUT].getChannels());
        const int selectorChannels = inputs[SLICE_INPUT].getChannels();
        const int pitchChannels = inputs[PITCH_INPUT].getChannels();
        const int channelCount = std::max(1, std::min(kMaxVoices, std::max(triggerChannels, std::max(selectorChannels, pitchChannels))));
        const bool usePolyMode = channelCount > 1;
        polyMode.store(usePolyMode, std::memory_order_relaxed);

        int shownSlice = sliceForChannel(*b, 0, channelCount);
        selectedSlice.store(shownSlice, std::memory_order_relaxed);

        for (int c = 0; c < channelCount; ++c) {
            const float trigV = inputs[TRIG_INPUT].isConnected() ? inputs[TRIG_INPUT].getPolyVoltage(static_cast<uint8_t>(c)) : 0.f;
            const float stepV = inputs[STEP_TRIG_INPUT].isConnected() ? inputs[STEP_TRIG_INPUT].getPolyVoltage(static_cast<uint8_t>(c)) : 0.f;
            const bool trig = trigTriggers[static_cast<size_t>(c)].process(trigV, 0.1f, 2.f);
            const bool step = stepTriggers[static_cast<size_t>(c)].process(stepV, 0.1f, 2.f);
            if (!trig && !step) {
                continue;
            }
            const int slice = sliceForChannel(*b, c, channelCount);
            const float pitchV = inputs[PITCH_INPUT].isConnected() ? inputs[PITCH_INPUT].getPolyVoltage(static_cast<uint8_t>(c)) : 0.f;
            triggerSlice(*b, slice, pitchV, usePolyMode, channelCount);
            if (step) {
                stepSlice = (stepSlice + 1) % static_cast<int>(b->slices.size());
            }
        }

        float outL = 0.f;
        float outR = 0.f;
        int active = 0;
        for (Voice& tail : tails) {
            renderVoice(tail, *b, outL, outR);
            if (tail.active) active++;
        }
        for (Voice& voice : voices) {
            renderVoice(voice, *b, outL, outR);
            if (voice.active) active++;
        }

        activeVoices.store(active, std::memory_order_relaxed);
        outputs[LEFT_OUTPUT].setVoltage(clampf(outL * kRackAudioVolts, -10.f, 10.f));
        outputs[RIGHT_OUTPUT].setVoltage(clampf(outR * kRackAudioVolts, -10.f, 10.f));
        lights[STATUS_LIGHT].setBrightnessSmooth(1.f, args.sampleTime);
        lights[POLY_LIGHT].setBrightnessSmooth(usePolyMode ? 1.f : 0.f, args.sampleTime);

        float ph = -1.f;
        const Voice* pv = nullptr;
        if (lastVoiceIndex >= 0 && lastVoiceIndex < kMaxVoices && voices[static_cast<size_t>(lastVoiceIndex)].active) {
            pv = &voices[static_cast<size_t>(lastVoiceIndex)];
        }
        else {
            for (const Voice& voice : voices) {
                if (voice.active) {
                    pv = &voice;
                    break;
                }
            }
        }
        if (pv && pv->slice >= 0 && pv->slice < static_cast<int>(b->slices.size())) {
            const RexSlice& slice = b->slices[static_cast<size_t>(pv->slice)];
            ph = clampf((static_cast<float>(slice.sampleStart) + pv->pos) / static_cast<float>(std::max(1, b->totalFrames)), 0.f, 1.f);
        }
        playhead01.store(ph, std::memory_order_relaxed);
    }
};

struct RexPanel : OpaqueWidget {
    void draw(const DrawArgs& args) override {
        NVGcontext* vg = args.vg;
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(vg, nvgRGB(13, 14, 18));
        nvgFill(vg);

        // Subtle panel gradient-ish vertical bands.
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, box.size.x, 36.f);
        nvgFillColor(vg, nvgRGB(26, 28, 36));
        nvgFill(vg);

        nvgBeginPath(vg);
        nvgMoveTo(vg, 0, 36.f);
        nvgLineTo(vg, box.size.x, 36.f);
        nvgStrokeColor(vg, nvgRGB(70, 78, 96));
        nvgStrokeWidth(vg, 1.f);
        nvgStroke(vg);

        auto font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (font) {
            nvgFontFaceId(vg, font->handle);
        }
        nvgFillColor(vg, nvgRGB(235, 241, 255));
        nvgFontSize(vg, 22.f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, box.size.x * 0.5f, 18.f, "REX PLAYER", nullptr);

        nvgFontSize(vg, 10.f);
        nvgFillColor(vg, nvgRGB(118, 203, 255));
        nvgText(vg, box.size.x * 0.5f, box.size.y - 14.f, "VelociLoops / RexRack", nullptr);
    }
};

struct RexWaveformDisplay : OpaqueWidget {
    RexPlayer* module = nullptr;

    explicit RexWaveformDisplay(RexPlayer* module) : module(module) {}

    void drawText(NVGcontext* vg, const char* text, float x, float y, float size, NVGcolor color, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP) {
        auto font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
        if (font) {
            nvgFontFaceId(vg, font->handle);
        }
        nvgFontSize(vg, size);
        nvgTextAlign(vg, align);
        nvgFillColor(vg, color);
        nvgText(vg, x, y, text, nullptr);
    }

    void draw(const DrawArgs& args) override {
        NVGcontext* vg = args.vg;
        const float w = box.size.x;
        const float h = box.size.y;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, 0, 0, w, h, 6.f);
        nvgFillColor(vg, nvgRGB(4, 6, 10));
        nvgFill(vg);
        nvgStrokeColor(vg, nvgRGB(62, 74, 98));
        nvgStrokeWidth(vg, 1.f);
        nvgStroke(vg);

        nvgBeginPath(vg);
        nvgMoveTo(vg, 8.f, h * 0.5f);
        nvgLineTo(vg, w - 8.f, h * 0.5f);
        nvgStrokeColor(vg, nvgRGBA(88, 94, 112, 110));
        nvgStrokeWidth(vg, 1.f);
        nvgStroke(vg);

        if (!module) {
            drawText(vg, "Browser preview", 10.f, 10.f, 12.f, nvgRGB(180, 190, 210));
            return;
        }

        std::shared_ptr<const RexBuffer> b = module->getBuffer();
        if (!b || b->slices.empty()) {
            drawText(vg, "Right-click module > Load REX/RX2", 12.f, 12.f, 13.f, nvgRGB(185, 198, 220));
            drawText(vg, "Slice V/OCT: first slice C2 by default", 12.f, 33.f, 10.f, nvgRGB(120, 134, 160));
            drawText(vg, "TRIG fires, STEP fires+advances", 12.f, 49.f, 10.f, nvgRGB(120, 134, 160));
            return;
        }

        const float left = 8.f;
        const float right = w - 8.f;
        const float mid = h * 0.5f + 8.f;
        const float amp = h * 0.33f;
        const float span = right - left;

        if (!b->overview.empty()) {
            nvgBeginPath(vg);
            for (size_t i = 0; i < b->overview.size(); ++i) {
                const float x = left + (static_cast<float>(i) / static_cast<float>(b->overview.size() - 1)) * span;
                const float y0 = mid - b->overview[i] * amp;
                const float y1 = mid + b->overview[i] * amp;
                nvgMoveTo(vg, x, y0);
                nvgLineTo(vg, x, y1);
            }
            nvgStrokeColor(vg, nvgRGB(76, 202, 255));
            nvgStrokeWidth(vg, 1.f);
            nvgStroke(vg);
        }

        for (float pos : b->slicePositions) {
            const float x = left + clampf(pos, 0.f, 1.f) * span;
            nvgBeginPath(vg);
            nvgMoveTo(vg, x, 23.f);
            nvgLineTo(vg, x, h - 8.f);
            nvgStrokeColor(vg, nvgRGBA(255, 210, 86, 115));
            nvgStrokeWidth(vg, 1.f);
            nvgStroke(vg);
        }

        const int selected = module->selectedSlice.load(std::memory_order_relaxed);
        if (selected >= 0 && selected < static_cast<int>(b->slicePositions.size())) {
            const float x = left + clampf(b->slicePositions[static_cast<size_t>(selected)], 0.f, 1.f) * span;
            nvgBeginPath(vg);
            nvgMoveTo(vg, x, 20.f);
            nvgLineTo(vg, x, h - 6.f);
            nvgStrokeColor(vg, nvgRGB(255, 112, 176));
            nvgStrokeWidth(vg, 2.f);
            nvgStroke(vg);
        }

        const float ph = module->playhead01.load(std::memory_order_relaxed);
        if (ph >= 0.f) {
            const float x = left + clampf(ph, 0.f, 1.f) * span;
            nvgBeginPath(vg);
            nvgMoveTo(vg, x, 6.f);
            nvgLineTo(vg, x, h - 6.f);
            nvgStrokeColor(vg, nvgRGB(112, 255, 152));
            nvgStrokeWidth(vg, 2.f);
            nvgStroke(vg);
        }

        const std::string title = b->displayName;
        drawText(vg, title.c_str(), 10.f, 8.f, 11.f, nvgRGB(232, 238, 255));

        std::ostringstream meta;
        meta << b->slices.size() << " slices  " << b->sampleRate << " Hz";
        if (b->tempoBpm > 0.f) {
            meta << "  " << static_cast<int>(std::round(b->tempoBpm)) << " BPM";
        }
        drawText(vg, meta.str().c_str(), 10.f, h - 20.f, 10.f, nvgRGB(164, 178, 206));

        std::ostringstream note;
        note << "base " << midiNoteName(module->baseMidiNote.load(std::memory_order_relaxed));
        drawText(vg, note.str().c_str(), w - 10.f, h - 20.f, 10.f, nvgRGB(164, 178, 206), NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    }
};

struct RexPlayerWidget : ModuleWidget {
    RexPlayerWidget(RexPlayer* module) {
        setModule(module);
        box.size = Vec(20 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);

        RexPanel* panel = new RexPanel();
        panel->box.size = box.size;
        addChild(panel);

        addChild(createWidget<ScrewSilver>(Vec(12, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 27, 0)));
        addChild(createWidget<ScrewSilver>(Vec(12, RACK_GRID_HEIGHT - 15)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 27, RACK_GRID_HEIGHT - 15)));

        RexWaveformDisplay* display = new RexWaveformDisplay(module);
        display->box.pos = Vec(14.f, 48.f);
        display->box.size = Vec(box.size.x - 28.f, 126.f);
        addChild(display);

        // Inputs
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14, 72)), module, RexPlayer::SLICE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(38, 72)), module, RexPlayer::PITCH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(62, 72)), module, RexPlayer::TRIG_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(86, 72)), module, RexPlayer::STEP_TRIG_INPUT));

        // Outputs
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(38, 106)), module, RexPlayer::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(62, 106)), module, RexPlayer::RIGHT_OUTPUT));

        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(86, 105)), module, RexPlayer::STATUS_LIGHT));
        addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(91, 105)), module, RexPlayer::POLY_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        RexPlayer* m = dynamic_cast<RexPlayer*>(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Rex Rack"));
        menu->addChild(createMenuItem("Load REX/RX2/RCY...", "", [m]() {
            if (m) m->chooseAndLoadFile();
        }));
        menu->addChild(createMenuItem("Reload current file", "", [m]() {
            if (m && !m->lastPath.empty()) m->loadFile(m->lastPath);
        }, !m || m->lastPath.empty()));

        menu->addChild(new MenuSeparator);
        menu->addChild(createSubmenuItem("First slice MIDI note", midiNoteName(m ? m->baseMidiNote.load(std::memory_order_relaxed) : 36), [m](Menu* menu) {
            const int choices[] = {24, 36, 48, 60};
            for (int note : choices) {
                menu->addChild(createCheckMenuItem(midiNoteName(note), "", [m, note]() {
                    return m && m->baseMidiNote.load(std::memory_order_relaxed) == note;
                }, [m, note]() {
                    if (m) m->baseMidiNote.store(note, std::memory_order_relaxed);
                }));
            }
        }, !m));

        if (m) {
            menu->addChild(new MenuSeparator);
            menu->addChild(createMenuLabel(m->lastStatus));
        }
    }
};

Model* modelRexPlayer = createModel<RexPlayer, RexPlayerWidget>("RexPlayer");

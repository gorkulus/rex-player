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
#include <exception>
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
constexpr int kRexPpqPerBar = 15360;
constexpr int kRexPpqPerQuarter = kRexPpqPerBar / 4;
constexpr int kClockPpq = kRexPpqPerQuarter / 4; // One x4/16th-note clock pulse.
constexpr float kSeqTriggerSeconds = 1e-3f;

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
    std::vector<int> sequenceOrder;
    std::vector<double> sequenceGateLengths;
    int sequenceLengthPpq = 1;
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

    buffer->sequenceOrder.reserve(buffer->slices.size());
    for (size_t i = 0; i < buffer->slices.size(); ++i) {
        buffer->sequenceOrder.push_back(static_cast<int>(i));
    }
    std::sort(buffer->sequenceOrder.begin(), buffer->sequenceOrder.end(), [&buffer](int a, int b) {
        const RexSlice& sa = buffer->slices[static_cast<size_t>(a)];
        const RexSlice& sb = buffer->slices[static_cast<size_t>(b)];
        if (sa.ppqPos != sb.ppqPos) return sa.ppqPos < sb.ppqPos;
        return sa.index < sb.index;
    });

    int sequenceLength = buffer->ppqLength;
    for (const RexSlice& slice : buffer->slices) {
        sequenceLength = std::max(sequenceLength, slice.ppqPos + 1);
    }
    buffer->sequenceLengthPpq = std::max(1, sequenceLength);
    buffer->sequenceGateLengths.assign(buffer->sequenceOrder.size(), static_cast<double>(kClockPpq));
    for (size_t orderIndex = 0; orderIndex < buffer->sequenceOrder.size(); ++orderIndex) {
        const int curSlice = buffer->sequenceOrder[orderIndex];
        const int cur = std::max(0, std::min(buffer->sequenceLengthPpq - 1, buffer->slices[static_cast<size_t>(curSlice)].ppqPos));
        double gate = static_cast<double>(buffer->sequenceLengthPpq);
        for (size_t step = 1; step <= buffer->sequenceOrder.size(); ++step) {
            const size_t nextIndex = (orderIndex + step) % buffer->sequenceOrder.size();
            const int nextSlice = buffer->sequenceOrder[nextIndex];
            int next = std::max(0, std::min(buffer->sequenceLengthPpq - 1, buffer->slices[static_cast<size_t>(nextSlice)].ppqPos));
            if (nextIndex <= orderIndex) next += buffer->sequenceLengthPpq;
            const int delta = next - cur;
            if (delta > 0) {
                gate = static_cast<double>(delta);
                break;
            }
        }
        buffer->sequenceGateLengths[orderIndex] = std::max(1.0, gate);
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
    enum ParamId { RUN_PARAM, PARAMS_LEN };
    enum InputId { SLICE_INPUT, PITCH_INPUT, TRIG_INPUT, STEP_TRIG_INPUT, CLOCK_INPUT, RESET_INPUT, RUN_INPUT, INPUTS_LEN };
    enum OutputId { LEFT_OUTPUT, RIGHT_OUTPUT, SEQ_VOCT_OUTPUT, SEQ_TRIG_OUTPUT, SEQ_GATE_OUTPUT, OUTPUTS_LEN };
    enum LightId { STATUS_LIGHT, POLY_LIGHT, RUN_LIGHT, LIGHTS_LEN };

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
    dsp::SchmittTrigger clockTrigger;
    dsp::SchmittTrigger resetTrigger;
    dsp::SchmittTrigger runTrigger;
    dsp::PulseGenerator seqTrigPulse;
    int stepSlice = 0;
    int polyVoiceCursor = 0;
    int lastVoiceIndex = 0;

    bool seqClockSeen = false;
    bool clockHasLastEdge = false;
    double seqSamplesSinceClock = 0.0;
    double seqSamplesPerClock = 0.0;
    int64_t seqClockTick = 0;
    double seqAbsPpq = 0.0;
    int64_t seqLoop = 0;
    size_t nextSeqOrderIndex = 0;
    int currentSeqSlice = 0;
    double seqGateRemainingPpq = 0.0;
    bool wasRunning = true;

    RexPlayer() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configSwitch(RUN_PARAM, 0.f, 1.f, 1.f, "Run clocked sequence", {"Stopped", "Running"});
        configInput(SLICE_INPUT, "Slice select V/Oct");
        configInput(PITCH_INPUT, "Playback pitch V/Oct");
        configInput(TRIG_INPUT, "Trigger current slice");
        configInput(STEP_TRIG_INPUT, "Trigger current slice and step");
        configInput(CLOCK_INPUT, "Clock x4 / 16th notes");
        configInput(RESET_INPUT, "Reset sequence to first slice");
        configInput(RUN_INPUT, "Run toggle trigger");
        configOutput(LEFT_OUTPUT, "Left master");
        configOutput(RIGHT_OUTPUT, "Right master");
        configOutput(SEQ_VOCT_OUTPUT, "Clocked slice V/Oct");
        configOutput(SEQ_TRIG_OUTPUT, "Clocked slice trigger");
        configOutput(SEQ_GATE_OUTPUT, "Clocked slice gate");
        configLight(STATUS_LIGHT, "REX loaded");
        configLight(POLY_LIGHT, "Polyphonic voice mode");
        configLight(RUN_LIGHT, "Sequence running");
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
        std::shared_ptr<RexBuffer> loaded;
        try {
            loaded = loadRexBufferFromFile(path, generation, error);
        }
        catch (const std::exception& e) {
            error = std::string("load failed: ") + e.what();
        }
        catch (...) {
            error = "load failed: unknown exception";
        }
        if (!loaded) {
            lastStatus = error;
            WARN("SoundVisions-REXRack: failed to load %s: %s", path.c_str(), error.c_str());
            return false;
        }

        lastPath = path;
        std::ostringstream status;
        status << loaded->displayName << " — " << loaded->slices.size() << " slices, " << loaded->sampleRate << " Hz";
        if (loaded->tempoBpm > 0.f) {
            status << ", " << loaded->tempoBpm << " BPM";
        }
        lastStatus = status.str();
        // Do not touch process-owned step/voice state from the UI thread here.
        // The audio thread resets it when it observes the new buffer generation.
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

    int clampSlice(const RexBuffer& b, int slice) const {
        if (b.slices.empty()) return 0;
        return std::max(0, std::min(static_cast<int>(b.slices.size()) - 1, slice));
    }

    float voltageFromSlice(int slice) const {
        const int note = baseMidiNote.load(std::memory_order_relaxed) + slice;
        return (static_cast<float>(note) - 60.f) / 12.f; // Inverse of Rack V/OCT 0V = C4/MIDI 60.
    }

    int sliceForChannel(const RexBuffer& b, int c, int channelCount, bool useSequencerNormal = false) {
        if (inputs[SLICE_INPUT].isConnected()) {
            return sliceFromVoltage(b, inputs[SLICE_INPUT].getPolyVoltage(static_cast<uint8_t>(c)));
        }
        if (useSequencerNormal && seqClockSeen) {
            return clampSlice(b, currentSeqSlice);
        }
        if (channelCount > 1) {
            return (stepSlice + c) % static_cast<int>(b.slices.size());
        }
        return clampSlice(b, stepSlice);
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

    int sequenceLengthPpq(const RexBuffer& b) const {
        return std::max(1, b.sequenceLengthPpq);
    }

    int orderedSliceIndex(const RexBuffer& b, size_t orderIndex) const {
        if (b.sequenceOrder.empty()) return 0;
        orderIndex = std::min(orderIndex, b.sequenceOrder.size() - 1);
        return clampSlice(b, b.sequenceOrder[orderIndex]);
    }

    int eventPpq(const RexBuffer& b, size_t orderIndex) const {
        if (b.sequenceOrder.empty()) return 0;
        const int len = sequenceLengthPpq(b);
        const int slice = orderedSliceIndex(b, orderIndex);
        return std::max(0, std::min(len - 1, b.slices[static_cast<size_t>(slice)].ppqPos));
    }

    double gateLengthPpqForOrder(const RexBuffer& b, size_t orderIndex) const {
        if (b.sequenceGateLengths.empty()) return static_cast<double>(kClockPpq);
        orderIndex = std::min(orderIndex, b.sequenceGateLengths.size() - 1);
        return std::max(1.0, b.sequenceGateLengths[orderIndex]);
    }

    void resetSequencer(const RexBuffer& b, bool forgetClockPhase) {
        seqAbsPpq = 0.0;
        seqLoop = 0;
        seqClockTick = 0;
        nextSeqOrderIndex = 0;
        seqGateRemainingPpq = 0.0;
        currentSeqSlice = orderedSliceIndex(b, 0);
        if (forgetClockPhase) {
            seqClockSeen = false;
        }
    }

    size_t orderIndexForSlice(const RexBuffer& b, int slice) const {
        slice = clampSlice(b, slice);
        for (size_t i = 0; i < b.sequenceOrder.size(); ++i) {
            if (b.sequenceOrder[i] == slice) return i;
        }
        return 0;
    }

    void cueSequencerToSlice(const RexBuffer& b, int slice) {
        if (b.sequenceOrder.empty()) return;
        const int len = sequenceLengthPpq(b);
        const size_t orderIndex = orderIndexForSlice(b, slice);
        const int64_t currentLoop = std::max<int64_t>(0, static_cast<int64_t>(std::floor(seqAbsPpq / static_cast<double>(len))));
        const double eventAbs = static_cast<double>(currentLoop * static_cast<int64_t>(len) + eventPpq(b, orderIndex));
        const bool clockPeriodKnown = seqSamplesPerClock > 1.0;

        currentSeqSlice = orderedSliceIndex(b, orderIndex);
        selectedSlice.store(currentSeqSlice, std::memory_order_relaxed);

        if (!clockPeriodKnown) {
            // The external trigger already fired this slice. Without a measured clock
            // period, we cannot continue off-grid REX timing yet, so do not arm the
            // sequencer or hold a gate until the clock has learned an interval.
            seqClockSeen = false;
            seqGateRemainingPpq = 0.0;
            return;
        }

        seqAbsPpq = eventAbs;
        seqGateRemainingPpq = std::max(1.0, gateLengthPpqForOrder(b, orderIndex));
        nextSeqOrderIndex = orderIndex + 1;
        seqLoop = currentLoop;
        if (nextSeqOrderIndex >= b.sequenceOrder.size()) {
            nextSeqOrderIndex = 0;
            seqLoop = currentLoop + 1;
        }
        seqClockTick = static_cast<int64_t>(std::floor(seqAbsPpq / static_cast<double>(kClockPpq)));
        seqClockSeen = true;
    }

    void fireSequencedSlice(const RexBuffer& b, size_t orderIndex, bool usePolyMode, int voiceCount) {
        const int seqSlice = orderedSliceIndex(b, orderIndex);
        currentSeqSlice = seqSlice;
        selectedSlice.store(seqSlice, std::memory_order_relaxed);
        seqTrigPulse.trigger(kSeqTriggerSeconds);
        seqGateRemainingPpq = std::max(1.0, gateLengthPpqForOrder(b, orderIndex));

        // Internal normaling: with no trigger cable, the clocked sequence triggers playback.
        // A patched trigger input breaks this normal, while the sequence outputs still emit.
        if (!inputs[TRIG_INPUT].isConnected()) {
            const int playbackSlice = inputs[SLICE_INPUT].isConnected() ? sliceFromVoltage(b, inputs[SLICE_INPUT].getVoltage()) : seqSlice;
            const float pitchV = inputs[PITCH_INPUT].isConnected() ? inputs[PITCH_INPUT].getVoltage() : 0.f;
            triggerSlice(b, playbackSlice, pitchV, usePolyMode, voiceCount);
        }
    }

    void fireSequenceEventsThrough(const RexBuffer& b, double targetAbsPpq, bool usePolyMode, int voiceCount) {
        if (b.sequenceOrder.empty()) return;
        const int len = sequenceLengthPpq(b);
        int guard = 0;
        while (guard++ < static_cast<int>(b.sequenceOrder.size()) * 4) {
            if (nextSeqOrderIndex >= b.sequenceOrder.size()) {
                nextSeqOrderIndex = 0;
                seqLoop++;
            }
            const double eventAbs = static_cast<double>(seqLoop * static_cast<int64_t>(len) + eventPpq(b, nextSeqOrderIndex));
            if (eventAbs > targetAbsPpq + 1e-6) {
                break;
            }
            const size_t firedOrderIndex = nextSeqOrderIndex;
            fireSequencedSlice(b, firedOrderIndex, usePolyMode, voiceCount);
            nextSeqOrderIndex++;
        }
    }

    void advanceSequencerBy(const RexBuffer& b, double deltaPpq, bool usePolyMode, int voiceCount) {
        if (deltaPpq <= 0.0 || b.sequenceOrder.empty()) return;
        const double target = seqAbsPpq + deltaPpq;
        const int len = sequenceLengthPpq(b);
        int guard = 0;

        while (guard++ < static_cast<int>(b.sequenceOrder.size()) * 4) {
            if (nextSeqOrderIndex >= b.sequenceOrder.size()) {
                nextSeqOrderIndex = 0;
                seqLoop++;
            }

            const double eventAbs = static_cast<double>(seqLoop * static_cast<int64_t>(len) + eventPpq(b, nextSeqOrderIndex));
            if (eventAbs > target + 1e-6) {
                break;
            }

            const double elapsedToEvent = std::max(0.0, eventAbs - seqAbsPpq);
            if (seqGateRemainingPpq > 0.0 && elapsedToEvent > 0.0) {
                seqGateRemainingPpq = std::max(0.0, seqGateRemainingPpq - elapsedToEvent);
            }
            seqAbsPpq = std::max(seqAbsPpq, eventAbs);

            const size_t firedOrderIndex = nextSeqOrderIndex;
            fireSequencedSlice(b, firedOrderIndex, usePolyMode, voiceCount);
            nextSeqOrderIndex++;
        }

        if (target > seqAbsPpq) {
            const double elapsed = target - seqAbsPpq;
            if (seqGateRemainingPpq > 0.0) {
                seqGateRemainingPpq = std::max(0.0, seqGateRemainingPpq - elapsed);
            }
            seqAbsPpq = target;
        }
    }

    bool isSequenceRunning() {
        return params[RUN_PARAM].getValue() > 0.5f;
    }

    void processSequencer(const RexBuffer& b, const ProcessArgs& args, bool usePolyMode, int voiceCount) {
        if (inputs[RUN_INPUT].isConnected() && runTrigger.process(inputs[RUN_INPUT].getVoltage(), 0.1f, 2.f)) {
            params[RUN_PARAM].setValue(isSequenceRunning() ? 0.f : 1.f);
        }

        const bool running = isSequenceRunning();
        if (running && !wasRunning) {
            resetSequencer(b, true);
        }
        wasRunning = running;

        if (inputs[RESET_INPUT].isConnected() && resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f)) {
            resetSequencer(b, true);
        }

        const bool clockConnected = inputs[CLOCK_INPUT].isConnected();
        if (!clockConnected) {
            clockHasLastEdge = false;
            seqClockSeen = false;
            seqSamplesSinceClock = 0.0;
            seqSamplesPerClock = 0.0;
            seqGateRemainingPpq = 0.0;
            return;
        }

        const bool clockEdge = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f);
        if (clockEdge) {
            if (clockHasLastEdge && seqSamplesSinceClock > 1.0) {
                seqSamplesPerClock = seqSamplesSinceClock;
            }
            clockHasLastEdge = true;
            seqSamplesSinceClock = 0.0;
        }

        const bool clockPeriodKnown = seqSamplesPerClock > 1.0;
        const double staleLimit = clockPeriodKnown ? std::max(2.0, seqSamplesPerClock * 2.5) : 0.0;
        const bool clockFresh = clockEdge || !clockPeriodKnown || seqSamplesSinceClock <= staleLimit;
        if (!clockFresh) {
            // Do not free-run forever from an old tempo when the clock stops.
            seqClockSeen = false;
            clockHasLastEdge = false;
            seqSamplesPerClock = 0.0;
            seqSamplesSinceClock = 0.0;
            seqGateRemainingPpq = 0.0;
            return;
        }

        if (running && clockEdge) {
            if (!seqClockSeen) {
                if (clockPeriodKnown) {
                    // Start on a real clock edge once tempo is known. On the first ever
                    // incoming pulse we wait one pulse so off-grid REX slices are not
                    // emitted late/bunched before an interval has been measured.
                    seqClockSeen = true;
                    seqClockTick = 0;
                    seqAbsPpq = 0.0;
                    seqLoop = 0;
                    nextSeqOrderIndex = 0;
                    fireSequenceEventsThrough(b, 0.0, usePolyMode, voiceCount);
                }
            }
            else {
                seqClockTick++;
                const double target = static_cast<double>(seqClockTick) * static_cast<double>(kClockPpq);
                if (target > seqAbsPpq) {
                    advanceSequencerBy(b, target - seqAbsPpq, usePolyMode, voiceCount);
                }
                else {
                    seqAbsPpq = target;
                }
            }
        }
        else if (running && seqClockSeen && clockPeriodKnown) {
            advanceSequencerBy(b, static_cast<double>(kClockPpq) / seqSamplesPerClock, usePolyMode, voiceCount);
        }

        seqSamplesSinceClock += 1.0;

        if (!running) {
            seqGateRemainingPpq = 0.0;
        }
    }

    void process(const ProcessArgs& args) override {
        std::shared_ptr<const RexBuffer> b = getBuffer();
        if (!b || b->slices.empty()) {
            outputs[LEFT_OUTPUT].setVoltage(0.f);
            outputs[RIGHT_OUTPUT].setVoltage(0.f);
            outputs[SEQ_VOCT_OUTPUT].setVoltage(0.f);
            outputs[SEQ_TRIG_OUTPUT].setVoltage(0.f);
            outputs[SEQ_GATE_OUTPUT].setVoltage(0.f);
            lights[STATUS_LIGHT].setBrightnessSmooth(0.f, args.sampleTime);
            lights[POLY_LIGHT].setBrightnessSmooth(0.f, args.sampleTime);
            lights[RUN_LIGHT].setBrightnessSmooth(0.f, args.sampleTime);
            return;
        }

        if (processGeneration != b->generation) {
            processGeneration = b->generation;
            clearVoices();
            stepSlice = 0;
            polyVoiceCursor = 0;
            resetSequencer(*b, true);
        }

        const int triggerChannels = std::max(inputs[TRIG_INPUT].getChannels(), inputs[STEP_TRIG_INPUT].getChannels());
        const int selectorChannels = inputs[SLICE_INPUT].getChannels();
        const int pitchChannels = inputs[PITCH_INPUT].getChannels();
        const int channelCount = std::max(1, std::min(kMaxVoices, std::max(triggerChannels, std::max(selectorChannels, pitchChannels))));
        const bool usePolyMode = channelCount > 1;
        polyMode.store(usePolyMode, std::memory_order_relaxed);

        processSequencer(*b, args, usePolyMode, channelCount);

        int shownSlice = sliceForChannel(*b, 0, channelCount, true);
        selectedSlice.store(shownSlice, std::memory_order_relaxed);

        bool anyStepTrigger = false;
        for (int c = 0; c < channelCount; ++c) {
            const float trigV = inputs[TRIG_INPUT].isConnected() ? inputs[TRIG_INPUT].getPolyVoltage(static_cast<uint8_t>(c)) : 0.f;
            const float stepV = inputs[STEP_TRIG_INPUT].isConnected() ? inputs[STEP_TRIG_INPUT].getPolyVoltage(static_cast<uint8_t>(c)) : 0.f;
            const bool trig = trigTriggers[static_cast<size_t>(c)].process(trigV, 0.1f, 2.f);
            const bool step = stepTriggers[static_cast<size_t>(c)].process(stepV, 0.1f, 2.f);
            if (!trig && !step) {
                continue;
            }
            const int slice = sliceForChannel(*b, c, channelCount, true);
            const float pitchV = inputs[PITCH_INPUT].isConnected() ? inputs[PITCH_INPUT].getPolyVoltage(static_cast<uint8_t>(c)) : 0.f;
            triggerSlice(*b, slice, pitchV, usePolyMode, channelCount);
            if (trig && inputs[TRIG_INPUT].isConnected()) {
                cueSequencerToSlice(*b, slice);
            }
            if (step) {
                anyStepTrigger = true;
            }
        }
        if (anyStepTrigger) {
            stepSlice = (stepSlice + 1) % static_cast<int>(b->slices.size());
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
        outputs[SEQ_VOCT_OUTPUT].setVoltage(voltageFromSlice(clampSlice(*b, currentSeqSlice)));
        outputs[SEQ_TRIG_OUTPUT].setVoltage(seqTrigPulse.process(args.sampleTime) ? 10.f : 0.f);
        outputs[SEQ_GATE_OUTPUT].setVoltage(seqGateRemainingPpq > 0.0 ? 10.f : 0.f);
        lights[STATUS_LIGHT].setBrightnessSmooth(1.f, args.sampleTime);
        lights[POLY_LIGHT].setBrightnessSmooth(usePolyMode ? 1.f : 0.f, args.sampleTime);
        lights[RUN_LIGHT].setBrightnessSmooth(isSequenceRunning() ? 1.f : 0.f, args.sampleTime);

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

struct RexPanel : TransparentWidget {
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

        // Output section: muted Rack-dark-mode grey with dark legends drawn above it.
        const Vec outPos = mm2px(Vec(7.f, 96.f));
        const Vec outSize = mm2px(Vec(90.f, 27.f));
        nvgBeginPath(vg);
        nvgRoundedRect(vg, outPos.x, outPos.y, outSize.x, outSize.y, 7.f);
        nvgFillColor(vg, nvgRGB(108, 120, 132));
        nvgFill(vg);
        nvgStrokeColor(vg, nvgRGB(156, 172, 188));
        nvgStrokeWidth(vg, 1.f);
        nvgStroke(vg);

        // Visual normaling hints from playback inputs to their sequenced outputs.
        const float normY1 = mm2px(Vec(0.f, 92.f)).y;
        const float normY2 = mm2px(Vec(0.f, 97.f)).y;
        const float normXs[] = {mm2px(Vec(14.f, 0.f)).x, mm2px(Vec(30.f, 0.f)).x};
        for (float x : normXs) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, x, normY1);
            nvgLineTo(vg, x, normY2);
            nvgStrokeColor(vg, nvgRGB(62, 98, 128));
            nvgStrokeWidth(vg, 1.6f);
            nvgLineCap(vg, NVG_ROUND);
            nvgStroke(vg);
        }

        auto font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        if (font) {
            nvgFontFaceId(vg, font->handle);
        }
        nvgFillColor(vg, nvgRGB(235, 241, 255));
        nvgFontSize(vg, 18.f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, box.size.x * 0.5f, 18.f, "REX PLAYER", nullptr);

        nvgFontSize(vg, 10.f);
        nvgFillColor(vg, nvgRGB(118, 203, 255));
        nvgText(vg, box.size.x * 0.5f, box.size.y - 14.f, "Sound Visions", nullptr);
    }
};

struct RexWaveformDisplay : TransparentWidget {
    RexPlayer* module = nullptr;
    bool titleHovered = false;
    bool titleWasTruncated = false;

    explicit RexWaveformDisplay(RexPlayer* module) : module(module) {}

    void setTextFont(NVGcontext* vg, float size) {
        auto font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
        if (font) {
            nvgFontFaceId(vg, font->handle);
        }
        nvgFontSize(vg, size);
    }

    float measureTextWidth(NVGcontext* vg, const std::string& text) {
        float bounds[4] = {};
        nvgTextBounds(vg, 0.f, 0.f, text.c_str(), nullptr, bounds);
        return bounds[2] - bounds[0];
    }

    std::string truncateText(NVGcontext* vg, const std::string& text, float maxWidth) {
        if (text.empty() || measureTextWidth(vg, text) <= maxWidth) {
            return text;
        }
        const std::string ellipsis = "...";
        if (measureTextWidth(vg, ellipsis) > maxWidth) {
            return ellipsis;
        }
        size_t lo = 0;
        size_t hi = text.size();
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            const std::string candidate = text.substr(0, mid) + ellipsis;
            if (measureTextWidth(vg, candidate) <= maxWidth) {
                lo = mid;
            }
            else {
                hi = mid - 1;
            }
        }
        return text.substr(0, lo) + ellipsis;
    }

    void drawText(NVGcontext* vg, const char* text, float x, float y, float size, NVGcolor color, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP) {
        setTextFont(vg, size);
        nvgTextAlign(vg, align);
        nvgFillColor(vg, color);
        nvgText(vg, x, y, text, nullptr);
    }

    void drawTitleTooltip(NVGcontext* vg, const std::string& title, float w) {
        const float x = 8.f;
        const float y = 27.f;
        const float pad = 6.f;
        const float textW = w - 2.f * (x + pad);
        setTextFont(vg, 10.5f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        float bounds[4] = {};
        nvgTextBoxBounds(vg, x + pad, y + pad, textW, title.c_str(), nullptr, bounds);
        const float h = clampf(bounds[3] - bounds[1] + pad * 2.f, 24.f, 64.f);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, w - 2.f * x, h, 4.f);
        nvgFillColor(vg, nvgRGBA(12, 18, 26, 235));
        nvgFill(vg);
        nvgStrokeColor(vg, nvgRGB(74, 96, 124));
        nvgStrokeWidth(vg, 1.f);
        nvgStroke(vg);

        nvgSave(vg);
        nvgIntersectScissor(vg, x + pad, y + pad, textW, h - 2.f * pad);
        nvgFillColor(vg, nvgRGB(232, 238, 255));
        nvgTextBox(vg, x + pad, y + pad, textW, title.c_str(), nullptr);
        nvgRestore(vg);
    }

    void onHover(const HoverEvent& e) override {
        const bool inTitle = e.pos.x >= 8.f && e.pos.x <= box.size.x - 8.f && e.pos.y >= 4.f && e.pos.y <= 23.f;
        titleHovered = titleWasTruncated && inTitle;
        if (titleHovered) {
            e.consume(this);
        }
    }

    void onLeave(const LeaveEvent& e) override {
        titleHovered = false;
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
            titleWasTruncated = false;
            titleHovered = false;
            drawText(vg, "Browser preview", 10.f, 10.f, 12.f, nvgRGB(180, 190, 210));
            return;
        }

        std::shared_ptr<const RexBuffer> b = module->getBuffer();
        if (!b || b->slices.empty()) {
            titleWasTruncated = false;
            titleHovered = false;
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
            nvgStrokeColor(vg, nvgRGBA(30, 112, 200, 150));
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
        setTextFont(vg, 11.f);
        const std::string visibleTitle = truncateText(vg, title, w - 20.f);
        titleWasTruncated = visibleTitle != title;
        if (!titleWasTruncated) {
            titleHovered = false;
        }
        drawText(vg, visibleTitle.c_str(), 10.f, 8.f, 11.f, nvgRGB(232, 238, 255));

        std::ostringstream meta;
        meta << b->slices.size() << " slices  " << b->sampleRate << " Hz";
        if (b->tempoBpm > 0.f) {
            meta << "  " << static_cast<int>(std::round(b->tempoBpm)) << " BPM";
        }
        drawText(vg, meta.str().c_str(), 10.f, h - 20.f, 10.f, nvgRGB(164, 178, 206));

        std::ostringstream note;
        note << "base " << midiNoteName(module->baseMidiNote.load(std::memory_order_relaxed));
        drawText(vg, note.str().c_str(), w - 10.f, h - 20.f, 10.f, nvgRGB(164, 178, 206), NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);

        if (titleHovered && titleWasTruncated) {
            drawTitleTooltip(vg, title, w);
        }
    }
};

struct RexTextLabel : TransparentWidget {
    std::string text;
    float fontSize = 8.f;
    NVGcolor color = nvgRGB(170, 184, 210);
    int align = NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE;

    RexTextLabel(std::string text, float fontSize = 8.f, NVGcolor color = nvgRGB(170, 184, 210))
        : text(std::move(text)), fontSize(fontSize), color(color) {}

    void draw(const DrawArgs& args) override {
        auto font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
        if (font) {
            nvgFontFaceId(args.vg, font->handle);
        }
        nvgFontSize(args.vg, fontSize);
        nvgTextAlign(args.vg, align);
        nvgFillColor(args.vg, color);
        nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, text.c_str(), nullptr);
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

        auto addCenteredLabel = [this](const std::string& text, Vec mm, float size = 7.2f, NVGcolor color = nvgRGB(170, 184, 210)) {
            RexTextLabel* label = new RexTextLabel(text, size, color);
            const Vec center = mm2px(mm);
            label->box.pos = Vec(center.x - 18.f, center.y - 5.f);
            label->box.size = Vec(36.f, 10.f);
            addChild(label);
        };

        addParam(createParamCentered<CKSS>(mm2px(Vec(95, 12)), module, RexPlayer::RUN_PARAM));
        addCenteredLabel("RUN", Vec(95, 19), 7.2f, nvgRGB(116, 255, 150));

        const float xA = 14.f;
        const float xB = 30.f;
        const float xC = 46.f;
        const float xPitch = 78.f;
        const float xL = 82.f;
        const float xR = 92.f;
        const float row1Y = 70.f;
        const float row2Y = 86.f;
        const float row3Y = 108.f;
        const NVGcolor inputLabel = nvgRGB(170, 184, 210);
        const NVGcolor outputLabel = nvgRGB(24, 30, 38);

        // Sequencer utility inputs
        addCenteredLabel("CLK", Vec(xA, row1Y - 7.f), 7.2f, inputLabel);
        addCenteredLabel("RST", Vec(xB, row1Y - 7.f), 7.2f, inputLabel);
        addCenteredLabel("RUN", Vec(xC, row1Y - 7.f), 7.2f, inputLabel);
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xA, row1Y)), module, RexPlayer::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xB, row1Y)), module, RexPlayer::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xC, row1Y)), module, RexPlayer::RUN_INPUT));

        // Playback inputs
        addCenteredLabel("SLICE", Vec(xA, row2Y - 7.f), 7.2f, inputLabel);
        addCenteredLabel("TRIG", Vec(xB, row2Y - 7.f), 7.2f, inputLabel);
        addCenteredLabel("STEP", Vec(xC, row2Y - 7.f), 7.2f, inputLabel);
        addCenteredLabel("PITCH", Vec(xPitch, row2Y - 7.f), 7.2f, inputLabel);
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xA, row2Y)), module, RexPlayer::SLICE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xB, row2Y)), module, RexPlayer::TRIG_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xC, row2Y)), module, RexPlayer::STEP_TRIG_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xPitch, row2Y)), module, RexPlayer::PITCH_INPUT));

        // Outputs
        addCenteredLabel("SLICE", Vec(xA, row3Y - 7.f), 7.2f, outputLabel);
        addCenteredLabel("TRIG", Vec(xB, row3Y - 7.f), 7.2f, outputLabel);
        addCenteredLabel("GATE", Vec(xC, row3Y - 7.f), 7.2f, outputLabel);
        addCenteredLabel("L", Vec(xL, row3Y - 7.f), 7.2f, outputLabel);
        addCenteredLabel("R", Vec(xR, row3Y - 7.f), 7.2f, outputLabel);
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xA, row3Y)), module, RexPlayer::SEQ_VOCT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xB, row3Y)), module, RexPlayer::SEQ_TRIG_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xC, row3Y)), module, RexPlayer::SEQ_GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xL, row3Y)), module, RexPlayer::LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xR, row3Y)), module, RexPlayer::RIGHT_OUTPUT));

        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(xL, 119.f)), module, RexPlayer::STATUS_LIGHT));
        addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec((xL + xR) * 0.5f, 119.f)), module, RexPlayer::POLY_LIGHT));
        addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(xR, 119.f)), module, RexPlayer::RUN_LIGHT));
    }

    void appendContextMenu(Menu* menu) override {
        RexPlayer* m = dynamic_cast<RexPlayer*>(module);
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Sound Visions / REX Player"));
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

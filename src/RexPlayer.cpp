#include "plugin.hpp"

struct RexPlayer : Module {
    enum ParamId { PARAMS_LEN };
    enum InputId { SLICE_INPUT, PITCH_INPUT, TRIG_INPUT, STEP_TRIG_INPUT, INPUTS_LEN };
    enum OutputId { LEFT_OUTPUT, RIGHT_OUTPUT, OUTPUTS_LEN };
    enum LightId { STATUS_LIGHT, LIGHTS_LEN };

    RexPlayer() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configInput(SLICE_INPUT, "Slice V/Oct");
        configInput(PITCH_INPUT, "Pitch V/Oct");
        configInput(TRIG_INPUT, "Trigger slice");
        configInput(STEP_TRIG_INPUT, "Trigger slice and step");
        configOutput(LEFT_OUTPUT, "Left master");
        configOutput(RIGHT_OUTPUT, "Right master");
        configLight(STATUS_LIGHT, "Loaded");
    }

    void process(const ProcessArgs& args) override {
        outputs[LEFT_OUTPUT].setVoltage(0.f);
        outputs[RIGHT_OUTPUT].setVoltage(0.f);
    }
};

struct RexPlayerWidget : ModuleWidget {
    RexPlayerWidget(RexPlayer* module) {
        setModule(module);
        box.size = Vec(20 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);
    }
};

Model* modelRexPlayer = createModel<RexPlayer, RexPlayerWidget>("RexPlayer");

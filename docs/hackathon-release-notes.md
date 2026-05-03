# Rex Rack hackathon release notes

Rex Rack is a VCV Rack 2 plugin for REX/RX2 breakbeat slicing inside a modular patch.

It started as a simple REX slice player and quickly turned into something more useful: a clocked REX sequencer that exposes slice timing as patchable CV.

## Highlights

- Load `.rx2`, `.rex`, and `.rcy` files.
- See waveform overview, slice points, selected slice, and playback position.
- Trigger slices from V/Oct + trigger/gate sources.
- Repitch slices musically with V/Oct tracking.
- Drive playback from a 16th-note clock using the REX file's original timing.
- Break out REX timing as V/Oct, trigger, and gate outputs.
- Use internal normaling for instant clocked playback, or patch the sequencer outputs through other modules for modular breakbeat chaos.

## Suggested release blurb

Rex Rack brings REX/RX2 loop slicing into VCV Rack. Load a breakbeat, clock it, trigger individual slices, repitch them with V/Oct, or use the original REX timing as a modular sequencer. The module outputs sequenced slice CV, triggers, and gates, so the loop can drive the rest of your patch instead of sitting behind it as static audio.

Built for the hackathon with VelociLoops.

## Demo patch idea

1. Clock `CLK` from Entrian Sequencers or any 16th-note clock.
2. Reset `RST` from the transport/bar reset.
3. Leave `SLICE` and `TRIG` unpatched for normaled playback.
4. Patch `SEQ` through a switch or offset, then back to `SLICE`.
5. Patch sequenced `TRIG` through probability/ratchets, then back to `TRIG`.
6. Modulate `PITCH` with a sequencer or keyboard CV.
7. Send `L/R` through filters, wavefolders, delays, and compressors.

That patch shows the core idea: REX timing as modular control, not just sample playback.

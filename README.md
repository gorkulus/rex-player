# Rex Rack

A one-shot VCV Rack 2 plugin prototype for triggering REX/RX2 slices directly in Rack.

## Module: REX Player

- Loads `.rx2`, `.rex`, and `.rcy` files using vendored VelociLoops.
- Displays an overview waveform, slice markers, selected slice, and playback cursor.
- Slice V/Oct input maps Rack V/OCT pitches to MIDI note numbers (`0V = C4/MIDI 60`). Default first slice is C2/MIDI 36, matching the local rx2-to-midi/REX convention. Change in context menu.
- Pitch V/Oct input repitches playback (`0V = native rate`, `+1V = octave up`, `-1V = octave down`).
- Trigger input fires the current slice.
- Step trigger fires current slice then advances the internal slice pointer.
- Mono mode chokes active playback with a short crossfade to avoid clicks.
- Polyphonic input cables enable a voice pool; channel count sets voice count, and triggers distribute round-robin through the pool.
- L/R master outputs.

## Build

```bash
make RACK_DIR=/home/hermes/Projects/shared/_sdks/Rack-SDK-2.6.6
```

## Install for local Rack

```bash
make dist RACK_DIR=/home/hermes/Projects/shared/_sdks/Rack-SDK-2.6.6
rsync -a --delete dist/RexRack/ /home/hermes/.local/share/Rack2/plugins-lin-x64/RexRack/
```

## Smoke-test REX decoding

```bash
g++ -std=c++17 -O2 -Ithird_party/VelociLoops/include tools/rex_probe.cpp third_party/VelociLoops/src/velociloops.cpp -o build/rex_probe
./build/rex_probe /path/to/file.rx2
```

## Notes

This is a hackathon/product-spike prototype: no extra audio dependencies beyond Rack SDK and vendored VelociLoops.

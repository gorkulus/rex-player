# Rex Rack manual

Rex Rack plays REX-family sliced loops inside VCV Rack 2.

The module has two main personalities:

1. A slice sampler. Select a slice with V/Oct, trigger it, and optionally repitch it.
2. A clocked REX sequencer. Feed it a 16th-note clock and it follows the original timing stored in the REX file.

These two modes are not separate switches. They are connected by normaling. If you leave the slice and trigger inputs unpatched, the internal clocked sequencer drives playback. If you patch those inputs, your external patch takes over playback while the sequence outputs keep running.

## Loading a file

Right-click the module and choose `Load REX/RX2/RCY...`.

Supported extensions:

- `.rx2`
- `.rex`
- `.rcy`

The module stores the selected file path in the Rack patch. If the file moves, reload it from the context menu.

File decoding currently happens on the UI thread. Normal files load quickly, but a huge or damaged file may briefly pause Rack's interface while it is parsed.

## Display

The display shows:

- waveform overview in blue
- slice markers in yellow
- selected slice in pink
- playback cursor in green
- file name, slice count, sample rate, and detected tempo when available

## Pitch and slice mapping

Rack V/Oct convention is used: `0V = C4 / MIDI 60`.

The `SLICE` input maps notes upward from the configured first slice note. The default first slice is C2 / MIDI 36.

Example with default C2:

| Note | MIDI | Slice |
| --- | ---: | ---: |
| C2 | 36 | 0 |
| C#2 | 37 | 1 |
| D2 | 38 | 2 |
| D#2 | 39 | 3 |

Change the base note from the module context menu: `First slice MIDI note`.

The `PITCH` input repitches playback independently:

- `0V` = native rate
- `+1V` = one octave up
- `-1V` = one octave down

This makes musical transposition easy with normal MIDI-to-CV or sequencer V/Oct sources.

## Inputs

### CLK

Clock input for internal REX timing playback.

Patch a 16th-note clock here. In Entrian-style terms, this is x4 clocking: four pulses per quarter note, sixteen pulses per 4/4 bar.

REX timing uses 15360 PPQ ticks per bar internally, so each 16th-note clock advances 960 REX PPQ ticks.

The module measures the first real clock interval before it can interpolate off-grid slice events. After it has seen a full clock period, it can fire REX slices between clock pulses instead of quantizing every slice to the next 16th.

### RST

Reset trigger. Resets the internal sequence position to the first slice.

Use this with your transport reset or bar reset source.

### RUN

Run trigger. A rising trigger toggles the run state.

There is also a RUN switch on the panel. The switch defaults on.

### SLICE

V/Oct slice select.

If no cable is patched here, the internal clocked sequencer's slice V/Oct is normaled to this input.

If a cable is patched here, external slice selection takes over playback. The sequenced V/Oct output still runs.

### PITCH

V/Oct repitch input.

This does not choose the slice. It changes playback speed/pitch for the triggered slice.

### TRIG

Trigger current slice.

If no cable is patched here, the internal clocked sequencer's trigger output is normaled to this input.

If a cable is patched here, external triggers take over playback. The sequenced trigger output still runs.

### STEP

Trigger current slice, then advance the internal step pointer.

This is useful for simple one-trigger-per-slice stepping that does not care about the original REX timing.

## Outputs

### SEQ

Sequenced V/Oct output. This emits the slice note selected by the internal REX timing sequencer.

Patch it elsewhere if you want another module to follow the REX slice order, or patch/process it before bringing it back to `SLICE`.

### TRIG

Sequenced trigger output. This emits a short 10V pulse when the internal sequencer reaches a slice event.

### GATE

Sequenced gate output. This stays high from a slice start until the next REX slice position or loop wrap.

### L / R

Stereo audio outputs.

## Normaled clocked playback patch

For the simplest loop playback:

1. Load a REX file.
2. Patch a 16th-note clock to `CLK`.
3. Patch reset to `RST` if available.
4. Leave `SLICE` and `TRIG` unpatched.
5. Patch `L` and `R` to a mixer.

The internal sequencer now drives slice choice and triggers playback.

## Breakout patch

For modular abuse:

1. Patch clock to `CLK`.
2. Patch `SEQ` through a quantizer, switch, offset, sample-and-hold, or logic patch.
3. Patch the processed result back into `SLICE`.
4. Patch `TRIG` or `GATE` through clock dividers, Bernoulli gates, ratchets, probability, or logic.
5. Patch the processed trigger back into the playback `TRIG` input.

The sequence outputs keep following the REX file even when you break the internal normaling.

## MIDI-file style playback

If you exported slice MIDI from the same REX file:

1. Send the MIDI V/Oct lane to `SLICE`.
2. Send the MIDI gate/trigger lane to `TRIG`.
3. Set the first-slice MIDI note to match the export convention.
4. Patch audio outputs.

This is the mode tested with Entrian Melody and a matching REX-derived MIDI file.

## Voice behavior

Default behavior is mono choke mode. A new slice trigger fades out current playback quickly before starting the next slice. This keeps classic chopped-break behavior and avoids clicky hard cuts.

If relevant input cables are polyphonic, Rex Rack uses a voice pool. Triggers are distributed round-robin across the available channels. This lets slices overlap instead of choking every previous slice.

## Known limitations

- File loading is synchronous and can pause the Rack UI while parsing.
- No drag-and-drop file loading yet; use the context menu.
- No per-slice output expander yet.
- No polished SVG panel yet. Current labels and graphics are drawn in code.
- Only Linux has been built/tested so far in this repo.
- REX parsing depends on VelociLoops. Weird/corrupt files may fail to load.

## Troubleshooting

### The module does not appear in Rack

Check Rack's log:

```bash
grep -nEi 'RexRack|Rex Rack|REX Rack|Could not load plugin' ~/.local/share/Rack2/log.txt
```

Rack 2 requires the plugin manifest version to start with `2.`. If the log says the plugin ABI version does not match, rebuild/package/reinstall the current version.

### The module appears but will not move

This was fixed in 2.0.2. Restart Rack and make sure the installed manifest says 2.0.2 or newer.

### Clocked playback does not start instantly

The module needs to see a real clock interval before it knows the tempo. Send clock continuously, then reset to the bar if needed.

### Slices are offset by octaves

Change `First slice MIDI note` in the context menu. Default is C2 / MIDI 36.

# Changelog

This plugin uses Rack-compatible version numbers. For Rack 2, the manifest version must start with `2.`.

## 2.0.8 - REX Player slug and GitHub metadata

- Rename the pre-release plugin slug/package so it matches the final REX Player name.
- Fill GitHub URL metadata for `gorkulus/rex-player`.
- Set public author string to `gorkulus with Hermes Agent`.
- Update docs to say `Hermes Agent creative hackathon` on first mention.

## 2.0.7 - RUN switch alignment

- Move the manual RUN switch to the first-row fourth grid position, directly above the PITCH input.

## 2.0.6 - panel output-section refinement

- Make the output-section panel shorter, darker, borderless, and return its legends to light text.
- Move the PITCH input into the fourth grid position on the playback-input row.
- Move the manual RUN switch and its label below the waveform display, aligned with the top jack labels.
- Remove the decorative panel screws for a cleaner faceplate.
- Make the waveform playhead indicator thinner.

## 2.0.5 - final pre-release polish

- Refine pre-release plugin slug/package naming for VCV conventions.
- Remove the `External` Rack browser tag.
- Tighten lower-panel jack layout and output-section visuals.
- Rename the sequenced V/Oct output panel label from `SEQ` to `SLICE`.
- Update waveform slice marker color and truncate long REX filenames with a hover tooltip.

## 2.0.4 - Sound Visions naming

- Set VCV brand to `Sound Visions`.
- Rename public module display name to `REX Player`.
- Remove the `Polyphonic` Rack browser tag while keeping the optional poly-cable voice-pool behavior documented.

## 2.0.3 - hackathon release docs

- Switch project license metadata to MIT for a simple permissive release with explicit warranty/liability disclaimer.
- Add user manual, release checklist, legal/licensing notes, and third-party notices.
- Include docs and notices in Rack distribution packages.

## 2.0.2 - drag fix

- Fixed module dragging by making visual-only panel, waveform, and label widgets transparent to Rack mouse events.

## 2.0.1 - clocked REX sequencing

- Added 4x / 16th-note clock input.
- Added reset input.
- Added run switch and run trigger input.
- Added sequenced V/Oct, trigger, and gate outputs.
- Added internal normaling from sequenced V/Oct/trigger to the playback inputs.
- Added code-drawn panel labels.

## 2.0.0 - Rack 2 ABI fix

- Changed plugin manifest version to `2.x.x` so Rack 2 will load the plugin.
- Improved browser discoverability with interim brand/module display names and Rack tags.

## 0.1.0 - initial prototype

- Initial REX/RX2/RCY slice player using vendored VelociLoops.
- Waveform display, slice markers, selected slice marker, and playhead.
- V/Oct slice select, independent V/Oct pitch input, trigger, step trigger, and stereo output.
- Mono choke mode and polyphonic voice pool mode.

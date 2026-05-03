# Changelog

Rex Rack uses Rack-compatible version numbers. For Rack 2, the manifest version must start with `2.`.

## 2.0.3 - hackathon release docs

- Switch project license metadata to MIT for a simple permissive release with explicit warranty/liability disclaimer.
- Add user manual, release checklist, legal/licensing notes, and third-party notices.
- Include docs and notices in Rack distribution packages.

## 2.0.2 - drag fix

- Fixed module dragging by making visual-only panel, waveform, and label widgets transparent to Rack mouse events.

## 2.0.1 - clocked REX sequencing

- Added 16th-note clock input.
- Added reset input.
- Added run switch and run trigger input.
- Added sequenced V/Oct, trigger, and gate outputs.
- Added internal normaling from sequenced V/Oct/trigger to the playback inputs.
- Added code-drawn panel labels.

## 2.0.0 - Rack 2 ABI fix

- Changed plugin manifest version to `2.x.x` so Rack 2 will load the plugin.
- Improved browser discoverability with `Rex Rack` brand, `REX Rack Player` module name, and Rack tags.

## 0.1.0 - initial prototype

- Initial REX/RX2/RCY slice player using vendored VelociLoops.
- Waveform display, slice markers, selected slice marker, and playhead.
- V/Oct slice select, independent V/Oct pitch input, trigger, step trigger, and stereo output.
- Mono choke mode and polyphonic voice pool mode.

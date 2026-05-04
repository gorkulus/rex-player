# Release checklist

This checklist is for the Hermes Agent creative hackathon release and for a later VCV Library submission.

## Recommended release posture

Release Sound Visions REX Player as a free open-source Rack 2 plugin under MIT.

Why MIT:

- It is permissive and familiar.
- It lets people use, fork, modify, and redistribute the plugin.
- It has an explicit "as is" warranty and liability disclaimer.
- It is compatible with VelociLoops' Unlicense/public-domain dedication.
- VCV's plugin licensing notes allow non-GPL open-source licenses such as MIT for free Rack plugins under the non-commercial plugin exception.

Do not sell the plugin through a non-GPL license without talking to VCV. Rack plugin commercial licensing is a separate VCV issue, not a VelociLoops issue.

## Before publishing the repo

- [x] Decide the public repo URL: https://github.com/gorkulus/rex-player
- [x] Fill `pluginUrl`, `manualUrl`, `sourceUrl`, and `changelogUrl` in `plugin.json`.
- [x] Confirm public author string is `gorkulus with Hermes Agent`; VCV brand remains `Sound Visions`.
- [x] Keep plugin slug as `SoundVisions-REXPlayer`. Do not change it after public release or Rack patch compatibility breaks.
- [ ] Confirm module slug remains `REXPlayer` and public module name is `REX Player`.
- [ ] Confirm license is `MIT` in `plugin.json`.
- [ ] Keep VelociLoops license in `third_party/VelociLoops/LICENSE`.
- [ ] Keep `THIRD_PARTY_NOTICES.md` in the package.
- [ ] Do not include any REX sample files in the repo unless they are explicitly redistributable.
- [ ] Add screenshots or a short demo GIF/video if time allows.

## Build verification

From the repo root:

```bash
python3 -m json.tool plugin.json >/dev/null
git diff --check
make clean RACK_DIR=/path/to/Rack-SDK
make RACK_DIR=/path/to/Rack-SDK
make dist RACK_DIR=/path/to/Rack-SDK
```

Smoke-test plugin loading:

```bash
LD_LIBRARY_PATH=/path/to/Rack-SDK python3 - <<'PY'
import ctypes, os
sdk = '/path/to/Rack-SDK'
plugin = 'dist/SoundVisions-REXPlayer/plugin.so'
ctypes.CDLL(os.path.join(sdk, 'libRack.so'), mode=ctypes.RTLD_GLOBAL)
ctypes.CDLL(plugin)
print('dlopen ok')
PY
```

Smoke-test REX decoding:

```bash
mkdir -p build
g++ -std=c++17 -O2 -Ithird_party/VelociLoops/include tools/rex_probe.cpp third_party/VelociLoops/src/velociloops.cpp -o build/rex_probe
./build/rex_probe /path/to/test-file.rx2
```

Install locally and restart Rack:

```bash
rsync -a --delete dist/SoundVisions-REXPlayer/ ~/.local/share/Rack2/plugins-lin-x64/SoundVisions-REXPlayer/
```

Then test manually in Rack:

- [ ] Browser search finds brand `Sound Visions` / module `REX Player`.
- [ ] Module can be dragged around normally.
- [ ] Context menu loads `.rx2`.
- [ ] Waveform, slice markers, selected marker, and playhead render.
- [ ] Manual `SLICE` + `TRIG` playback works.
- [ ] `PITCH` repitches by octave per volt.
- [ ] `STEP` trigger plays and advances.
- [ ] Clock-only normaled playback works with a 4x / 16th-note clock into `CLK`.
- [ ] `RST` resets to the first slice.
- [ ] `RUN` switch/input start and stop sequence playback.
- [ ] `SLICE`, `TRIG`, and `GATE` outputs work when patched externally.
- [ ] External cables to `SLICE` and `TRIG` break normaling but sequence outputs continue.
- [ ] Stereo output is sane and does not clip horribly on typical loops.

## GitHub release

- [ ] Commit all source/docs/metadata changes.
- [ ] Make sure generated artifacts are ignored and not committed:
  - `build/`
  - `dist/`
  - `plugin.so`
  - `*.vcvplugin`
- [ ] Tag the release:

```bash
git tag v2.0.9
git push origin master --tags
```

- [ ] Attach `dist/SoundVisions-REXPlayer-2.0.9-lin-x64.vcvplugin` to the GitHub release if doing a manual binary release.
- [ ] Include release notes from `CHANGELOG.md`.

## VCV Library submission later

VCV Library's open-source plugin process, as documented in `VCVRack/library`, is:

1. Push the source code to a public repo.
2. Create exactly one issue in https://github.com/VCVRack/library/issues.
3. Title the issue with the plugin slug: `SoundVisions-REXPlayer`.
4. Post the source repo URL.
5. For updates, bump `plugin.json` version, push a commit, and comment with:
   - new version
   - exact commit hash, not just a branch name

VCV also requires plugins to follow their Plugin Ethics Guidelines. Sound Visions REX Player should be fine if we keep our own brand/panel identity and avoid implying endorsement by Propellerhead/Reason Studios or VCV.

## Nice-to-have before a wider release

- [ ] Background file loading so a weird file cannot pause the UI.
- [ ] Better panel art/SVG.
- [ ] Tooltips for each port/control.
- [ ] Cross-platform builds: Linux, macOS, Windows.
- [ ] A tiny test corpus of legally redistributable or synthetic REX/RX2 files, if we can make or obtain one.
- [ ] A short demo patch and demo video.
- [ ] More explicit version/help text in the module context menu.

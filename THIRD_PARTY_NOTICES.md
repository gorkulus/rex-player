# Third-party notices

## VelociLoops

Rex Rack vendors VelociLoops by kunitoki.

- Upstream: https://github.com/kunitoki/VelociLoops
- Local copy: `third_party/VelociLoops/`
- License: The Unlicense
- License file: `third_party/VelociLoops/LICENSE`

VelociLoops is compiled directly into `plugin.so`. Users do not need to install VelociLoops separately.

## VCV Rack SDK

Rex Rack builds against the VCV Rack SDK and links to Rack at runtime. Rack itself is not vendored in this repository or distributed inside the plugin package.

See VCV's plugin licensing notes before distributing through the VCV Library or selling a Rack plugin: https://vcvrack.com/manual/PluginLicensing

## REX / RX2 / RCY format note

Rex Rack reads user-supplied REX-family files. It does not include Propellerhead/Reason Studios software, SDK code, or sample content.

REX and REX2 are file formats associated with Propellerhead/Reason Studios. Rex Rack is an independent project and is not endorsed by Propellerhead/Reason Studios.

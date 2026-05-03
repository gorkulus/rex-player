# Legal and licensing notes

This is a practical release note, not legal advice. If this becomes a paid product or a major public project, ask a lawyer or VCV support before shipping.

## Recommended license

Use MIT for Rex Rack.

MIT is the best fit for this release because it is short, familiar, permissive, and includes an explicit "as is" warranty/liability disclaimer. It lets people use, copy, modify, publish, distribute, sublicense, and sell copies of Rex Rack's own software. Anyone distributing Rex Rack as a Rack plugin still needs to respect Rack/VCV licensing and distribution rules.

VelociLoops is released under The Unlicense. That is compatible with MIT in practice because VelociLoops grants broad rights to copy, modify, publish, use, compile, sell, and distribute the software, with its own no-warranty disclaimer.

The repo should keep:

- `LICENSE` for Rex Rack's MIT license.
- `third_party/VelociLoops/LICENSE` for VelociLoops.
- `THIRD_PARTY_NOTICES.md` to make the vendored dependency obvious.

## VCV Rack licensing

Rack is GPLv3+, but VCV publishes a Non-Commercial Plugin License Exception that allows free Rack plugins to use other open-source licenses, including MIT, BSD, and CC0.

That means a free open-source MIT release is a normal path for a Rack plugin.

If anyone sells Rex Rack as a Rack plugin under non-GPL terms later, they should contact VCV about a commercial plugin license. That is about Rack plugin distribution/linking and VCV's terms, not about VelociLoops.

## VelociLoops and REX/RX2

VelociLoops describes itself as a clean, dependency-free implementation for reading and writing REX2/RX2 files, including DWOP decode/encode. It is open source and released under The Unlicense.

For Rex Rack, we vendor VelociLoops source code and compile it into the plugin binary. We do not ship any Propellerhead/Reason Studios SDK, source code, binaries, or sample content.

Reverse engineering laws vary by jurisdiction. Interoperability-oriented reverse engineering is often treated differently from copying proprietary code, but it is not something I can guarantee legally. The practical risk looks low for a free open-source plugin that:

- uses a clean open-source implementation,
- ships no proprietary code,
- ships no copyrighted REX sample library,
- does not claim to be official,
- uses REX/RX2 descriptively for compatibility rather than claiming official product status,
- includes clear trademark/non-endorsement language.

## Trademark / naming

Keep the product name `Rex Rack`, not `REX2 Rack`, `ReCycle Rack`, `Propellerhead Rack`, or anything that looks official.

Use REX/RX2 descriptively in docs because users need to know what files are supported. Add a simple disclaimer:

> REX and REX2 are file formats associated with Propellerhead/Reason Studios. Rex Rack is an independent project and is not endorsed by Propellerhead/Reason Studios.

Avoid using Propellerhead/Reason Studios logos, product artwork, or panel layouts.

## Samples and demo content

Do not include commercial breakbeats or REX sample packs in the repo or release package unless their license explicitly allows redistribution.

For demo videos, playing your own local files is fine. For downloadable demo patches, either:

- omit the sample file and tell users to load their own REX/RX2 file, or
- include only public-domain/CC0/self-authored sample material with clear attribution.

## Liability posture

The MIT license already says the software is provided "as is" with no warranty and no liability for claims or damages.

Keep that license text in every source/binary distribution. Do not make promises like "safe for live performance" or "guaranteed compatible with all RX2 files." Say what we tested, not what we guarantee.

## My call

Ship the hackathon release under MIT, include the VelociLoops Unlicense notice, keep the trademark disclaimer, and avoid bundling any copyrighted sample content.

That gives users maximum freedom and gives us the clearest no-warranty/no-liability posture without leaning on public-domain dedication quirks.

# Licensing of the Cemu Remote GamePad fork

Modification notice: this fork was modified for Cemu Remote GamePad on
2026-10-27. Changes include the Vulkan/AX/VPAD adapter, embedded GameStream host,
pairing/settings dialog, lifecycle fixes and Windows borderless fullscreen.
Original copyright and license notices are retained.

## Cemu and the enabled combination

The original Cemu source remains available under **MPL-2.0** in [LICENSE.txt](LICENSE.txt).
Original MIT/BSD and other third-party files retain their own licenses.

When Cemu is combined with the embedded GPL-3.0 transport, this fork distributes
the **combined work under GPL-3.0-only**, with the full text in
[extensions/gamepad/LICENSE.txt](extensions/gamepad/LICENSE.txt). To the extent
permitted by MPL-2.0 section 3.3, MPL-covered files in this combination are
**additionally available under GPL-3.0-only**, while their original MPL-2.0
availability and notices are preserved. This notice does not replace Cemu's
license or claim that GPL-only upstream transport can be relicensed as MPL.

The Cemu baseline uses MPL-2.0 and no secondary-license incompatibility notice
was found in its `src/` files. Materials with separate licenses retain those
terms. New Cemu-side adapters, SDK interfaces and Windows GUI files are offered
under `MPL-2.0 OR GPL-3.0-only` for this combination. Additions to existing
MPL-covered files remain available under the MPL as well as under the secondary
license for the combined work. Changes to the Dear ImGui Vulkan backend retain
its MIT license and upstream notices.

The plugin runtime, native host replacements, manager and packaging scripts
under `extensions/gamepad/` are GPL-3.0-only unless a file or the scope exceptions
above state otherwise. Sunshine-derived material remains GPL-3.0-only;
other dependencies retain the licenses listed in
[THIRD_PARTY_NOTICES.md](extensions/gamepad/THIRD_PARTY_NOTICES.md).

The plugin shares callable interfaces, data structures and GPU resources with
Cemu. A stable C ABI, optional loading and a separate DLL are engineering
boundaries, not a special linking exception from any upstream copyright holder.

## Distribution requirements

- Preserve the license texts, copyright/attribution notices and dated
  modification notices. Do not describe this fork as an official Cemu release.
- Provide the actual modified source used for a distributed build, including
  build/installation scripts, the source generator, interface definitions and
  the locked dependency sources needed for that build. Identify the exact
  fork commit; an unchanged upstream Cemu source link is not enough.
- Before offering binary downloads, fulfill GPL-3.0 section 6 and MPL-2.0
  section 3.2. A practical network-distribution route is to offer the complete
  Corresponding Source alongside each binary at equivalent access. A GitHub
  auto-generated archive omits submodule contents and is not automatically a
  complete build-source bundle. Mere pointers to moving upstream branches do
  not establish that the source obligation has been met.
- Preserve the licenses of statically linked dependencies and any required
  source/notice obligations. The install rules collect notices from the
  pinned dependencies and vcpkg packages; check the actual release contents.
- Do not add restrictions that prevent recipients from exercising their
  MPL/GPL rights. No warranty is provided under those licenses.

This source publication does not include a binary Release, games, firmware,
keys, paired-client credentials or personal runtime data. License grants do not
grant trademark rights or imply upstream endorsement.

## Primary references

- [MPL-2.0, sections 3.1–3.4 and 2.3](https://www.mozilla.org/en-US/MPL/2.0/)
- [Mozilla MPL FAQ, questions 8–14](https://www.mozilla.org/en-US/MPL/2.0/FAQ/)
- [Mozilla MPL-in-GPL developer guidelines](https://www.mozilla.org/en-US/MPL/2.0/combining-mpl-and-gpl/)
- [GNU GPL FAQ: combined programs and plugins](https://www.gnu.org/licenses/gpl-faq.en.html#GPLPlugins)
- [GPL-3.0, sections 5–6](https://www.gnu.org/licenses/gpl-3.0.en.html)

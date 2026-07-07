# Third-Party Licences / Dependency Audit

This records the licences of third-party components used by the SHD Systems
Installer Kit, and the obligations they impose. **SHD Systems Ltd's own code** is
licensed per [LICENSING.md](LICENSING.md) (AGPLv3 or commercial); the components
below are **not** owned by SHD and keep their own licences.

_Last audited: 2026-07-07._

## Summary

| Component | Where used | Licence | Copyleft? | AGPL-compatible? | Action needed |
|---|---|---|---|---|---|
| **Qt 6** (Widgets, Svg, Core, Gui + platform plugins) | Linked by `AppSetup` GUI; runtime DLLs bundled by `windeployqt` in `pack.ps1` | **LGPL-3.0** (or Qt commercial) | Weak (library) | ✅ Yes | **LGPL obligations — see below** |
| Windows API (Win32) | `bootstrap.cpp` stub | OS platform (no separate licence) | — | ✅ | None |
| `tar` (Windows built-in) | Bundle extraction at runtime | Ships with Windows | — | ✅ | None (relies on OS-provided binary) |

No copyleft-incompatible dependencies were found. There is **no** GPL/AGPL library
that would conflict with re-licensing SHD's own code, and there is no bundled
compression library requiring separate attribution (the stub links only Win32).

## Qt (LGPL-3.0) — obligations when you distribute

The installer GUI links Qt and the generated installers **bundle the Qt runtime
DLLs**. That is distribution of Qt binaries, so LGPL-3.0 applies to the Qt parts
regardless of how SHD's own code is licensed. To comply:

1. **Dynamic linking** — Qt must remain dynamically linked (Qt DLLs, as
   `windeployqt` produces), so a user can replace the Qt libraries. Do **not**
   statically link Qt under LGPL.
2. **Provide the LGPL text and Qt licence notice** with distributed installers.
3. **Offer the Qt source** (or your modified Qt source, if you modify Qt) to
   recipients, per LGPL-3.0 §4/§5.
4. **Allow re-linking** — recipients must be able to relink your app against a
   modified Qt.

If you (or a commercial licensee) need to **statically link Qt**, ship to
**locked-down devices** where re-linking isn't possible, or otherwise cannot meet
LGPL, you must obtain a **commercial Qt licence from The Qt Company**. SHD's own
commercial licence does **not** cover Qt.

> ⚠️ Confirm the exact Qt module list and versions that `windeployqt` bundles for
> your build, and generate a per-release Qt licence/attribution bundle. This audit
> should be re-run whenever dependencies change.

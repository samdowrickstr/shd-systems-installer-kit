# PlannerDay Installer Kit

A small, reusable **Windows installer** for Qt/C++ desktop apps. It produces a
single self-contained `Installer.exe` (no runtime dependencies) with a clean,
branded install / update / repair / uninstall UI — driven entirely by a JSON
config, so you never touch the C++ to ship a new product.

Built and maintained by **PlannerDay Ltd**. Provided under the MIT licence
(see [LICENSE](LICENSE)) — free to use, including commercially, provided the
copyright/attribution notice is retained.

## What you get

- **One-file installer** — a native Win32 stub embeds the whole bundle (your app
  exes + the Qt runtime + this setup GUI) as a `tar.gz` payload, unpacks to a
  temp dir and runs the setup GUI. No external files.
- **Per-user install** to `%LocalAppData%\Programs\<App>` — no admin/UAC prompt.
- **Multiple apps** in one installer, each with its own checkbox and shortcuts.
- **Install / Update / Repair / Uninstall** flows with SemVer-aware upgrade
  detection, running-process handling, desktop + Start-Menu shortcuts and an
  Add/Remove Programs entry.
- **Portable `.zip`** output (app only, run in place) alongside the installer.

## How it works

Everything product-specific lives in a JSON file — nothing is hard-coded:

```
configure.ps1  ──writes──▶  installer.json  ──read by──▶  pack.ps1  ──▶  Installer.exe
                                                              │
                                     embeds config.json + logo + icon into AppSetup.exe
                                     stages your built exes + Qt runtime, tars it,
                                     links it into the single-file stub
```

At runtime `AppSetup.exe` reads its embedded `config.json` (product name,
version, accent colour, the list of apps, …) — so the same compiled GUI serves
every project.

## Prerequisites

- **Qt 6** (mingw kit) + the matching **MinGW** toolchain (`g++`, `windres`).
- **CMake 3.21+** and `tar.exe` (ships with Windows 10+).
- Your app already **built** (this kit packages binaries; it doesn't build them).

## Quick start

1. Configure a project interactively:

   ```powershell
   pwsh configure.ps1
   ```

   It asks for the product name, version, logo (`.svg`), icon (`.ico`), the app
   binaries to bundle and your Qt/MinGW paths, then writes `installer.json`.

2. Build the installer:

   ```powershell
   pwsh pack.ps1
   ```

   Outputs to the configured `dist/` folder:
   - `<Prefix>-v<version>-Installer.exe` — the one-file installer
   - `<Prefix>-Installer.exe` — latest alias
   - `<Prefix>-v<version>-portable.zip` — the app only (optional)

Prefer to edit config by hand? Copy `installer.example.json` to `installer.json`
and adjust it, then run `pack.ps1`.

## `installer.json`

| Key | Purpose |
|-----|---------|
| `product.appName` | Canonical name (install folder, registry entry). |
| `product.displayName` | Title shown in the header (may omit brand if in the logo). |
| `product.subtitle` | Small caps line under the title (e.g. `SETUP`). |
| `product.publisher` | Company name (Add/Remove Programs). |
| `product.registryKey` | Leaf under `…\Uninstall\<key>`. No spaces. |
| `product.accentColor` | Hex accent for buttons, checks, progress. |
| `product.logo` / `product.icon` | Branding: `.svg` logo, `.ico` icon. |
| `version.file` / `version.literal` | Read `version.txt`, or a fixed string. |
| `version.appendBuildMetadata` | Append `+build.<timestamp>` (default true). |
| `apps[]` | `exe`, `name`, `description`, `default`, optional `qmlDir`. |
| `payload.sources[]` | Files/folders to copy into the bundle (your built exes). |
| `payload.windeployqt` | Run `windeployqt` to bundle the Qt runtime. |
| `qt.dir` / `qt.mingw` | Qt kit and MinGW `bin` paths. |
| `output.namePrefix` / `distDir` / `portableZip` | Output naming + options. |

All relative paths are resolved against the folder containing `installer.json`.

## Layout

- `src/bootstrap.cpp` — native Win32 single-file stub (embeds + unpacks payload).
- `src/main.cpp`, `src/setupwindow.{h,cpp}` — the config-driven Qt setup GUI.
- `resources/` — `setup.qrc`, `logo.svg`, `check.svg`, `app.ico`, manifest, `.rc`.
- `CMakeLists.txt` — builds `AppSetup.exe`.
- `configure.ps1` — the setup wizard. `pack.ps1` — the packager.

## Licence & attribution

© 2026 PlannerDay Ltd. Licensed under the MIT Licence. You may use, modify and
redistribute this kit (including in commercial products) as long as the
copyright and permission notice in [LICENSE](LICENSE) is preserved.

The example configuration references STR (Subsea Technology and Rentals Ltd)
products purely as a worked example; STR's branding assets remain STR's property.

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

Run from a **Windows PowerShell** prompt in the repo folder (use the `.\` prefix).
If scripts are blocked by execution policy, prefix with
`powershell -ExecutionPolicy Bypass -File`.

1. Configure a project interactively:

   ```powershell
   .\configure.ps1
   ```

   It asks for the product name, version, logo (`.svg`), icon (`.ico`), the app
   binaries to bundle and your Qt/MinGW paths, then writes `installer.json`.

2. Build the installer:

   ```powershell
   .\pack.ps1
   ```

   Outputs to the configured `dist/` folder:
   - `<Prefix>-v<version>-Installer.exe` — the one-file installer
   - `<Prefix>-Installer.exe` — latest alias
   - `<Prefix>-v<version>-portable.zip` — the app only (optional)

Prefer to edit config by hand? Copy `installer.example.json` to `installer.json`
and adjust it, then run `.\pack.ps1`.

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
| `payload.windeployqt` | Run `windeployqt` to bundle the Qt runtime (Qt apps). |
| `payload.autoBundleDlls` | Auto-detect & copy non-system DLL dependencies (any native app). |
| `payload.dllSearchDirs[]` | Extra folders to resolve DLLs from when `autoBundleDlls` is on. |
| `qt.dir` / `qt.mingw` | Qt kit and MinGW `bin` paths (also used as DLL search dirs). |
| `output.namePrefix` / `distDir` / `portableZip` | Output naming + options. |
| `signing.enabled` | Authenticode-sign the installer (and payload) with `signtool`. |
| `signing.certFile` / `certPasswordEnv` | `.pfx` path + the **env var** holding its password. |
| `signing.thumbprint` | Alternative to `certFile`: use a cert from the Windows store. |
| `signing.dlib` / `signing.metadata` | Azure Trusted Signing (signtool Dlib dispatcher): `Azure.CodeSigning.Dlib.dll` + metadata JSON. |
| `signing.timestampUrl` / `signPayload` / `signtool` | RFC-3161 timestamp URL; sign app exes too; optional signtool path. |

All relative paths are resolved against the folder containing `installer.json`.

## Does it only work for Qt?

No. The **installer GUI** (`AppSetup.exe`) is a Qt app, so building the kit
itself needs Qt + MinGW. But the **payload can be any Windows software** - the
installer just unpacks and copies whatever you stage in `payload.sources`, then
writes shortcuts and an uninstall entry. Nothing about the runtime install is
Qt-specific.

**Bundling dependencies:**

- **Qt apps** - set `payload.windeployqt: true`. `windeployqt` is the only
  reliable way to gather Qt's runtime-loaded plugins (`platforms/qwindows.dll`,
  image formats) and QML modules, which a dependency scan cannot see.
- **Any native app (incl. non-Qt)** - set `payload.autoBundleDlls: true`. The
  packer runs `objdump -p` on each staged binary, recursively resolves the
  import table, and copies every **non-system** DLL it finds (searching
  `payload.dllSearchDirs`, the stage folder, `qt.dir\bin` and `qt.mingw`).
  Windows system DLLs and `api-ms-*` / `ext-ms-*` stubs are skipped.
- You can enable **both**: `windeployqt` for plugins/QML, `autoBundleDlls` to
  sweep up any remaining compiler-runtime DLLs.

> Auto-detection sees only statically-imported DLLs. Plugins your app loads at
> runtime (via `LoadLibrary`, Qt plugins, etc.) must be added to
> `payload.sources` explicitly.

## Integrating with your app's build

`pack.ps1` is a plain script, so you can call it from CI or your build system
after your app compiles. For example, from CMake:

```cmake
add_custom_target(installer
    COMMAND powershell -ExecutionPolicy Bypass -File
            "${CMAKE_SOURCE_DIR}/../STR-Installer-Kit/pack.ps1"
            -Config "${CMAKE_SOURCE_DIR}/installer.json"
    DEPENDS my_app
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/../STR-Installer-Kit"
    COMMENT "Building the standalone installer")
```

Then `cmake --build . --target installer` produces the single-file
`Installer.exe`. The same one-line invocation works in a GitHub Actions /
Azure Pipelines job on a Windows runner.

## Optional hardening

- **Code signing** - Windows SmartScreen warns on unsigned installers. Set
  `signing.enabled: true` in `installer.json` and point `signing.certFile` at a
  `.pfx` (put its password in the env var named by `signing.certPasswordEnv` -
  never in the JSON), or use `signing.thumbprint` for a cert already in your
  Windows store. `pack.ps1` then signs the payload exes and the final
  `Installer.exe` with `signtool` (from the Windows 10/11 SDK), timestamped via
  `signing.timestampUrl`.
- **Azure Trusted Signing** - fully supported: set `signing.dlib` to
  `Azure.CodeSigning.Dlib.dll` and `signing.metadata` to your account metadata
  JSON (Endpoint / CodeSigningAccountName / CertificateProfileName), use
  `timestampUrl: http://timestamp.acs.microsoft.com`, and provide Azure auth via
  the environment (`AZURE_TENANT_ID` / `AZURE_CLIENT_ID` / `AZURE_CLIENT_SECRET`,
  or `az login`). `pack.ps1` invokes `signtool ... /dlib <dll> /dmdf <json>`.
- **CI releases** - `examples/release.yml` is a ready-to-copy GitHub Actions
  workflow: on every `v*` tag it installs Qt, builds your app, packs a (signed)
  installer, and attaches the `Installer.exe` to the GitHub Release. Copy it
  into your app repo as `.github/workflows/release.yml` and add the
  `CERT_PFX_BASE64` / `CERT_PASSWORD` secrets for signing.

## Layout

- `src/bootstrap.cpp` — native Win32 single-file stub (embeds + unpacks payload).
- `src/main.cpp`, `src/setupwindow.{h,cpp}` — the config-driven Qt setup GUI.
- `resources/` — `setup.qrc`, `logo.svg`, `check.svg`, `app.ico`, manifest, `.rc`.
- `CMakeLists.txt` — builds `AppSetup.exe`.
- `configure.ps1` — the setup wizard. `pack.ps1` — the packager.- `examples/release.yml` - GitHub Actions release workflow template.
## Licence & attribution

© 2026 PlannerDay Ltd. Licensed under the MIT Licence. You may use, modify and
redistribute this kit (including in commercial products) as long as the
copyright and permission notice in [LICENSE](LICENSE) is preserved.

The example configuration references STR (Subsea Technology and Rentals Ltd)
products purely as a worked example; STR's branding assets remain STR's property.

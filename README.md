<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

<h1 align="center">
  <br>
  <img src="android/BachataS4/app/src/main/play_store_512.png" alt="BachataS4" width="180">
  <br>
  <b>BachataS4</b>
  <br>
</h1>

<p align="center">
  PlayStation 4 emulator frontend for Android (ARM64), built around
  <a href="https://github.com/shadps4-emu/shadPS4">shadPS4</a>.
</p>

<p align="center">
  <a href="https://github.com/JICA98/Bachata-S4/issues">Issues</a>
  ·
  <a href="documents/android-building.md">Android build</a>
  ·
  <a href="https://shadps4.net/">shadPS4 website</a>
</p>

---

## What is this?

**BachataS4** is an Android app that runs the [shadPS4](https://github.com/shadps4-emu/shadPS4) emulator on **arm64-v8a** devices (API 31+) using:

- **box64** for x86_64 → ARM64 translation  
- a managed **glibc / rootfs** runtime  
- Vulkan (device drivers or user-installed Turnip packages)  
- an integrated X11 + ALSA bridge for the desktop-oriented emulator  

This repository is a fork that includes both the **emulator core** and the **Android frontend** under `android/BachataS4`.

> [!IMPORTANT]
> Early development. Expect bugs, incomplete game support, and frequent changes.  
> Only use firmware, games, and content you have a legal right to run.

---

## Compatibility

Community-tested compatibility results are published on the
[Bachata S4 compatibility website](https://jica98.github.io/Bachata-S4/#games),
with evidence-backed reports stored in
[`compatibility-site/data/games.json`](compatibility-site/data/games.json).

Every report belongs to one official GitHub release, one physical Android device, and one
selected graphics driver. Results are added with
[`.agents/skills/bachata-compatibility/SKILL.md`](.agents/skills/bachata-compatibility/SKILL.md).

<!-- compatibility-status-table -->
| Status | Reports |
|---|---|
| `playable` | 0 |
| `ingame` | 1 |
| `menus` | 0 |
| `boots` | 0 |
| `nothing` | 0 |
<!-- compatibility-status-table -->

Links:
- [Compatibility website](https://jica98.github.io/Bachata-S4/#games)
- [Play Store](https://play.google.com/store/apps/details?id=com.bachatas4.android)

---

## Features

- Open-source PS4 emulation wrapper for Android  
- Native Vulkan path (device GPU / optional custom Turnip drivers)  
- Library management and session UI  
- Configurable touch controls and controller mapping  
- Two distribution flavors:
  - **playstore** — runtime packaged into the APK  
  - **fdroid** — runtime downloaded on first setup (smaller APK; needs network once)  

---

## Requirements

| Item | Notes |
|------|--------|
| Device | **arm64-v8a**, Android **12+** (API 31) |
| GPU | Working Vulkan (Adreno recommended for custom Turnip import) |
| Content | User-owned legal dumps / homebrew only — nothing is bundled |

Turnip drivers are **never** shipped inside the APK. Install them after setup from the trusted feed or a local ZIP. See [documents/android-building.md](documents/android-building.md).

---

## Building (Android)

Full steps and verification are in **[documents/android-building.md](documents/android-building.md)**.

Prerequisites (summary): Linux/WSL2 x86-64, JDK 17+, Node.js 20+, Android SDK 37, NDK `30.0.14904198`, CMake 3.22.1, Ninja, AArch64 cross toolchain.

```bash
# From repository root — package managed runtime (playstore / local embeds)
git submodule update --init --recursive --jobs 8
runtime/scripts/build-shadps4-x86_64.sh
runtime/scripts/build-box64-host.sh
node runtime/scripts/package-runtime.mjs
node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json

# APK
cd android/BachataS4
./gradlew test lintDebug assemblePlaystoreDebug   # embeds runtime.zip
# or
./gradlew assembleFdroidRelease                   # DOWNLOAD_RUNTIME=true
```

F-Droid builds use the **fdroid** product flavor and do not require a pre-packaged `runtime.zip` in the tree.

Store listing text and graphics live under [`fastlane/metadata/android/`](fastlane/metadata/android/).

---

## Desktop shadPS4 (upstream core)

The C++ emulator core still builds for **Windows**, **Linux**, and **macOS**. Desktop docs:

| Platform | Guide |
|----------|--------|
| Docker | [documents/building-docker.md](documents/building-docker.md) |
| Windows | [documents/building-windows.md](documents/building-windows.md) |
| Linux | [documents/building-linux.md](documents/building-linux.md) |
| macOS | [documents/building-macos.md](documents/building-macos.md) |

Upstream project: [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4) · [Quickstart](https://github.com/shadps4-emu/shadPS4/wiki/I.-Quick-start-%5BUsers%5D) · [Game compatibility](https://github.com/shadps4-compatibility/shadps4-game-compatibility)

---

## Runtime & third-party notices

Managed runtime components (box64, glibc packages, winlator-derived pieces, etc.) are pinned under `runtime/locks/` and described in [NOTICE.android-runtime.md](NOTICE.android-runtime.md).

---

## Contributing & issues

- Issues: https://github.com/JICA98/Bachata-S4/issues  
- Android / BachataS4 changes: open a PR against this fork  
- Core emulator contributions may also track [upstream shadPS4](https://github.com/shadps4-emu/shadPS4)  

Please read [CONTRIBUTING.md](CONTRIBUTING.md) where applicable.

---

## License

[GPL-2.0-or-later](LICENSE) — same family as shadPS4.  
Additional third-party licenses for the Android runtime are listed in [NOTICE.android-runtime.md](NOTICE.android-runtime.md).

---

## Credits

- **shadPS4** team and contributors — emulator core  
- **box64**, **Winlator**, and other runtime dependencies — see locks and NOTICE  
- Logo: `android/BachataS4/app/src/main/play_store_512.png` (also Fastlane icon / `assets/logo.png`)

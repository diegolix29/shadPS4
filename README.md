<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

<div align="center">

<a href="https://play.google.com/store/apps/details?id=com.bachatas4.android">
  <img src="assets/feature_graphic.png" alt="Bachata S4 — PlayStation 4 emulation on Android" width="100%">
</a>

<br><br>

<img src="assets/logo.png" alt="Bachata S4 logo" width="150">

# Bachata S4

### Explore PlayStation 4 emulation on Android.

**An experimental, open-source PS4 emulator for ARM64 Android devices — powered by shadPS4, FEX and Vulkan, with transparent device-specific compatibility results.**

<br>

<a href="https://play.google.com/store/apps/details?id=com.bachatas4.android">
  <img src="https://play.google.com/intl/en_us/badges/static/images/badges/en_badge_web_generic.png" alt="Get Bachata S4 on Google Play" height="80">
</a>

<br>

[![Latest release](https://img.shields.io/github/v/release/JICA98/Bachata-S4?style=for-the-badge&sort=semver&label=LATEST)](https://github.com/JICA98/Bachata-S4/releases/latest)
[![GitHub downloads](https://img.shields.io/github/downloads/JICA98/Bachata-S4/total?style=for-the-badge&label=GITHUB%20DOWNLOADS)](https://github.com/JICA98/Bachata-S4/releases)
[![Compatibility](https://img.shields.io/badge/COMPATIBILITY-LIVE%20RESULTS-5b67f1?style=for-the-badge)](https://jica98.github.io/Bachata-S4/#games)

[![Android 12+](https://img.shields.io/badge/Android-12%2B-3DDC84?style=flat-square&logo=android&logoColor=white)](https://play.google.com/store/apps/details?id=com.bachatas4.android)
[![ARM64](https://img.shields.io/badge/ABI-arm64--v8a-232F3E?style=flat-square)](#requirements)
[![FEX backend](https://img.shields.io/badge/Guest%20backend-FEX-7B61FF?style=flat-square)](#how-it-works)
[![Vulkan](https://img.shields.io/badge/Graphics-Vulkan-AC162C?style=flat-square&logo=vulkan&logoColor=white)](#how-it-works)
[![License](https://img.shields.io/github/license/JICA98/Bachata-S4?style=flat-square)](LICENSE)
[![Stars](https://img.shields.io/github/stars/JICA98/Bachata-S4?style=flat-square)](https://github.com/JICA98/Bachata-S4/stargazers)
[![Issues](https://img.shields.io/github/issues/JICA98/Bachata-S4?style=flat-square)](https://github.com/JICA98/Bachata-S4/issues)
[![Last commit](https://img.shields.io/github/last-commit/JICA98/Bachata-S4?style=flat-square)](https://github.com/JICA98/Bachata-S4/commits/main)

<br>

[**Get the supported Google Play build**](https://play.google.com/store/apps/details?id=com.bachatas4.android)
&nbsp;&nbsp;•&nbsp;&nbsp;
[**Check game compatibility**](https://jica98.github.io/Bachata-S4/#games)
&nbsp;&nbsp;•&nbsp;&nbsp;
[**View releases**](https://github.com/JICA98/Bachata-S4/releases)
&nbsp;&nbsp;•&nbsp;&nbsp;
[**Report an issue**](https://github.com/JICA98/Bachata-S4/issues)

</div>

> [!IMPORTANT]
> Bachata S4 is in **early experimental development**. Compatibility and performance vary by game, emulator release, phone, Android version and graphics driver. No games, firmware, keys or copyrighted system files are included.

---

## Why Bachata S4?

| | |
|---|---|
| **Built for Android** | A dedicated mobile frontend with game-library management, touch controls, controller support and Android-focused runtime integration. |
| **FEX guest execution** | Translates the emulator's x86-64 workload for modern ARM64 Android hardware. |
| **Vulkan graphics** | Supports Android Vulkan rendering with selectable system or compatible Turnip driver paths. |
| **Evidence-backed compatibility** | Reports are tied to an emulator release, commit, device, GPU, driver, screenshots, logs and measured performance. |

Bachata S4 is based on the excellent [shadPS4](https://github.com/shadps4-emu/shadPS4) project and adapts its desktop-oriented emulator core for ARM64 Android devices through a managed runtime and Android integration layer.

---

## See it running

<div align="center">
  <img src="compatibility-site/assets/screenshots/cusa07023/20260731-181457-01.png" alt="Sonic Mania running in Bachata S4" width="48%">
  <img src="compatibility-site/assets/screenshots/cusa07023/20260731-181457-02.png" alt="Sonic Mania gameplay in Bachata S4" width="48%">
</div>

### Published compatibility highlight

| Game | Status | Tested build | Device | Backend | Result |
|---|---|---|---|---|---|
| **Sonic Mania 01.03** | **In-game** | `v0.1.6` | OnePlus 13 · Snapdragon 8 Elite | FEX + Mesa Turnip | **59.83 FPS average** during the recorded test |

The result above is a **specific test report**, not a promise that every device or every part of the game will behave identically. Open the [live compatibility database](https://jica98.github.io/Bachata-S4/#games) for screenshots, driver details, logs and the newest results.

---

## Download

### Google Play — recommended

The Google Play build is the easiest supported way to install Bachata S4. Installing it from Play also directly supports continued emulator development, device testing and game-compatibility work.

<div align="center">
<a href="https://play.google.com/store/apps/details?id=com.bachatas4.android">
  <img src="https://play.google.com/intl/en_us/badges/static/images/badges/en_badge_web_generic.png" alt="Get Bachata S4 on Google Play" height="90">
</a>
</div>

### GitHub Releases — open-source builds

Release APKs, checksums and source-linked build artifacts are available from [GitHub Releases](https://github.com/JICA98/Bachata-S4/releases).

[![Download latest release](https://img.shields.io/badge/DOWNLOAD-LATEST%20GITHUB%20RELEASE-181717?style=for-the-badge&logo=github)](https://github.com/JICA98/Bachata-S4/releases/latest)

> [!NOTE]
> The download badge at the top counts **GitHub release assets only**. Google Play install totals are not included in that number.

---

## Compatibility

Compatibility is tracked as structured JSON in [`compatibility-site/data/games.json`](compatibility-site/data/games.json) and published automatically to the [Bachata S4 compatibility website](https://jica98.github.io/Bachata-S4/#games).

Each test can record:

- Emulator release and exact commit
- Game version and serial
- Phone, SoC, GPU, RAM and Android version
- Guest backend and graphics driver
- Resolution scale, FPS and frame pacing
- Screenshots, compressed logs and tester notes

<!-- compatibility-status-table -->
| Status | Reports |
|---|---:|
| `playable` | 0 |
| `ingame` | 1 |
| `menus` | 0 |
| `boots` | 0 |
| `nothing` | 0 |
<!-- compatibility-status-table -->

### Status meanings

| Status | Meaning |
|---|---|
| **Playable** | Suitable for normal play with no major blocker found in the tested session. |
| **In-game** | Reaches controllable gameplay, but completion or full stability is not verified. |
| **Menus** | Reaches menus or title screens but not controllable gameplay. |
| **Boots** | Starts and produces meaningful output before an early blocker. |
| **Nothing** | Does not reach useful output in the tested configuration. |

Compatibility is **release-specific and device-specific**. Always compare the report's emulator version, phone and selected driver with your own setup.

---

## How it works

```mermaid
flowchart LR
    A["PS4 homebrew / legally dumped content"] --> B["Bachata S4 Android frontend"]
    B --> C["shadPS4 emulator core"]
    C --> D["FEX x86-64 → ARM64 guest execution"]
    C --> E["Managed Debian runtime"]
    C --> F["Vulkan rendering path"]
    F --> G["Android system driver"]
    F --> H["Compatible Turnip driver"]
    B --> I["Touch controls / physical controller"]
    B --> J["Android display and audio integration"]
```

The current compatibility pipeline records the active guest backend and graphics driver for every submitted result, so performance claims remain attached to the exact configuration that produced them.

---

## Features

- PS4 emulation frontend designed for **ARM64 Android**
- **FEX-based** guest execution for x86-64 workloads
- shadPS4 emulator core with Android runtime integration
- Vulkan rendering through supported device or Turnip drivers
- Game library with cover artwork and metadata
- Touch controls and physical-controller mapping
- Per-game configuration and driver selection
- Session logs and diagnostics for compatibility testing
- Release-linked public compatibility database
- Screenshot- and log-backed test reports
- Google Play and source-built distribution paths
- Open-source code under GPL-2.0-or-later

---

## Requirements

| Item | Requirement |
|---|---|
| **Operating system** | Android 12 or newer |
| **CPU architecture** | `arm64-v8a` |
| **Graphics** | Vulkan-capable GPU and driver |
| **Recommended hardware** | Recent high-end Snapdragon/Adreno device |
| **Storage** | Enough space for the app, runtime and user-provided content |
| **Content** | User-owned homebrew or legally dumped software only |

> [!TIP]
> Before buying or troubleshooting, check the [compatibility website](https://jica98.github.io/Bachata-S4/#games). A result from the same SoC and driver family is much more useful than a result from an unrelated phone.

---

## Quick start

1. Install Bachata S4 from [Google Play](https://play.google.com/store/apps/details?id=com.bachatas4.android).
2. Open the app and complete the initial runtime setup.
3. Select the appropriate graphics driver for your device.
4. Import your own legal homebrew or game dump.
5. Launch the title and allow initial shader or pipeline compilation to finish.
6. Compare your result with the [compatibility database](https://jica98.github.io/Bachata-S4/#games).
7. When reporting a problem, include the emulator version, device, driver and session log.

Bachata S4 does **not** provide games, firmware, licenses, decryption keys or copyrighted console files.

---

## App screenshots

<div align="center">
  <img src="fastlane/metadata/android/en-US/images/phoneScreenshots/1.jpg" alt="Bachata S4 app screenshot 1" width="23%">
  <img src="fastlane/metadata/android/en-US/images/phoneScreenshots/2.jpg" alt="Bachata S4 app screenshot 2" width="23%">
  <img src="fastlane/metadata/android/en-US/images/phoneScreenshots/3.jpg" alt="Bachata S4 app screenshot 3" width="23%">
  <img src="fastlane/metadata/android/en-US/images/phoneScreenshots/4.jpg" alt="Bachata S4 app screenshot 4" width="23%">
</div>

<details>
<summary><strong>Show more screenshots</strong></summary>
<br>
<div align="center">
  <img src="fastlane/metadata/android/en-US/images/phoneScreenshots/5.jpg" alt="Bachata S4 app screenshot 5" width="23%">
  <img src="fastlane/metadata/android/en-US/images/phoneScreenshots/6.jpg" alt="Bachata S4 app screenshot 6" width="23%">
  <img src="fastlane/metadata/android/en-US/images/phoneScreenshots/7.jpg" alt="Bachata S4 app screenshot 7" width="23%">
  <img src="fastlane/metadata/android/en-US/images/phoneScreenshots/8.jpg" alt="Bachata S4 app screenshot 8" width="23%">
</div>
</details>

---

## Project status

Bachata S4 is not a finished, universal PS4 emulator. Some titles may:

- Fail before rendering
- Reach only menus or gameplay
- Have missing graphics, audio or input
- Run slowly on current mobile hardware
- Behave differently across Android versions and GPU drivers
- Regress or improve between releases

Public testing is part of development. Every high-quality compatibility report helps identify whether a blocker belongs to CPU translation, kernel/HLE behavior, graphics, audio, input or Android integration.

---

## Build from source

The Android frontend lives in [`android/BachataS4`](android/BachataS4). Runtime inputs and revisions are pinned under [`runtime/locks`](runtime/locks).

Start with the maintained build guide:

- **[Android runtime and APK build instructions](documents/android-building.md)**
- [Contributing guide](CONTRIBUTING.md)
- [Runtime third-party notices](NOTICE.android-runtime.md)

The runtime and Android build paths are evolving quickly. Use the commands and toolchain versions in the build guide rather than copying instructions from older releases or third-party posts.

---

## Help improve compatibility

### Test a game

Use the repository skill at [`.agents/skills/bachata-compatibility/SKILL.md`](.agents/skills/bachata-compatibility/SKILL.md) to launch a title, capture evidence and create a structured compatibility entry.

A useful report should include:

1. Official Bachata S4 release tag
2. Exact phone model, SoC, GPU and Android version
3. Selected graphics driver and version
4. Game serial and update version
5. Clear status and reproduction notes
6. At least one screenshot when visual output is available
7. Sanitized application and emulator logs
8. FPS measurements only when the sampling method is recorded

### Report a bug

Search [existing issues](https://github.com/JICA98/Bachata-S4/issues) before opening a new one. Reports with exact reproduction steps, logs and device information are far more actionable than “game does not work.”

### Contribute code

Pull requests are welcome for Android integration, runtime packaging, compatibility fixes, diagnostics, documentation and test automation. Core emulator changes may also be relevant to [upstream shadPS4](https://github.com/shadps4-emu/shadPS4).

---

## FAQ

<details>
<summary><strong>Does Bachata S4 include PS4 games?</strong></summary>
<br>
No. Bachata S4 includes no games or copyrighted console content. You must provide content you legally own or open-source homebrew.
</details>

<details>
<summary><strong>Will every PS4 game run?</strong></summary>
<br>
No. The project is experimental and compatibility is currently limited. Check the live database before assuming a title works.
</details>

<details>
<summary><strong>Why can the same game behave differently on two phones?</strong></summary>
<br>
CPU generation, GPU, Android version, available memory, graphics driver and thermal limits can all change the result. This is why every compatibility entry records the full test environment.
</details>

<details>
<summary><strong>Which devices are recommended?</strong></summary>
<br>
Modern flagship Snapdragon devices with Adreno GPUs currently provide the most practical testing environment. This is a recommendation, not a guarantee of compatibility.
</details>

<details>
<summary><strong>Where should I download Bachata S4?</strong></summary>
<br>
Google Play is the recommended supported installation. GitHub Releases provide source-linked project builds and checksums.
</details>

---

## Legal

Bachata S4 is an independent open-source project and is not affiliated with, endorsed by or sponsored by Sony Interactive Entertainment.

“PlayStation” and related marks are trademarks of their respective owners. Bachata S4 does not circumvent the requirement to obtain software legally and does not distribute games, firmware, keys or copyrighted system components.

Use only software and content you have the legal right to run.

---

## Credits

- [shadPS4](https://github.com/shadps4-emu/shadPS4) developers and contributors — emulator core
- [FEX-Emu](https://github.com/FEX-Emu/FEX) developers and contributors — x86-64 guest execution
- Runtime, graphics and Android open-source projects listed in [NOTICE.android-runtime.md](NOTICE.android-runtime.md)
- Everyone submitting reproducible compatibility reports, logs and fixes

---

## License

Bachata S4 is licensed under **[GPL-2.0-or-later](LICENSE)**. Third-party runtime components retain their respective licenses.

---

<div align="center">

### Help push PS4 emulation on Android forward.

[![Get Bachata S4 on Google Play](https://img.shields.io/badge/GET%20BACHATA%20S4-GOOGLE%20PLAY-3DDC84?style=for-the-badge&logo=googleplay&logoColor=white)](https://play.google.com/store/apps/details?id=com.bachatas4.android)

**Install it. Test responsibly. Share evidence. Improve compatibility.**

</div>

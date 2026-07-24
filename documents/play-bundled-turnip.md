# Play Store bundled Turnip packages

## Why

Google Play policy disallows downloading executable native code (`.so` / driver
ZIPs) after install. The `playstore` product flavor therefore ships Turnip
packages inside the app and disables remote driver catalogue, download, and
ZIP import.

Tracked lines from
[bachata-s4-drivers](https://github.com/JICA98/bachata-s4-drivers):

| Line | Asset | Upstream package |
|------|-------|------------------|
| `mojo-26.1` (default) | `drivers/turnip-26.1.0-EMULATOR.zip` | `Turnip-mojo-26.1-v2-08e7443-EMULATOR.zip` |
| `mojo-25.0` | `drivers/turnip-mojo-25.0-EMULATOR.zip` | `Turnip-mojo-25.0-v2-2a6fe3d-EMULATOR.zip` |
| `gen8` | `drivers/turnip-gen8-EMULATOR.zip` | `Turnip-gen8-v2-7fdde2f-EMULATOR.zip` |

The `fdroid` flavor keeps GitHub Releases download + local ZIP import.

## Flavor

| Flavor | Driver sources |
|--------|----------------|
| `playstore` | Three bundled Turnip lines (selectable in Settings → Drivers) |
| `fdroid` | System + download from `JICA98/bachata-s4-drivers` + import ZIP |

Gradle: `android/BachataS4/app/build.gradle.kts` (`productFlavors.playstore` / `fdroid`).

## Package location

All under `android/BachataS4/app/src/playstore/assets/`:

| Item | Path |
|------|------|
| ZIP assets | `drivers/turnip-*.zip` (see table above) |
| SHA-256 sidecars | matching `*.zip.sha256` |
| Licence notices | `licenses/mesa-turnip-*.NOTICE.txt` |
| Spec constants | `BundledTurnipSpec` / `BundledTurnipPackage` in `core:runtime` |

Format matches `TurnipPackageInstaller` (flat ZIP with `meta.json` +
`libvulkan_freedreno.so`).

## Behaviour

### Play (`PlaystoreDriverManagerBackend`)

1. Setup **skips** the Turnip selection screen (`SHOW_DRIVER_SELECTION=false`).
   Continue auto-selects the default bundled package (`mojo-26.1`).
2. On first use, extracts **all three** ZIPs into app-private
   `files/vulkan-drivers/installed/` via `BundledTurnipInstaller` (per-package
   markers `.bundled-turnip-<versionMarker>`).
3. Verify SHA-256 before install; atomic staging through existing installer.
4. Settings **Drivers** tab lists the three bundled packages for selection
   (no remote catalogue / import / delete).
5. Launch uses the profile `driverId` when it matches an installed bundled id;
   otherwise falls back to default `mojo-26.1`.

### Non-Play (`FdroidDriverManagerBackend`)

Unchanged: `TurnipReleaseClient` + `TurnipDownloadManager` + import ZIP.

## Updating a bundled driver

1. Obtain a new `*-EMULATOR.zip` from bachata-s4-drivers (same package format).
2. Replace the matching file under `app/src/playstore/assets/drivers/`.
3. `sha256sum` the ZIP → update `.sha256` sidecar and the matching entry in
   `BundledTurnipSpec`.
4. Update `versionMarker`, `assetName`, `releaseTag`, display label, and the
   NOTICE file (commit / branch).
5. Run unit tests + Play packaging inspection (below).

## Intentionally disabled on Play

- GitHub release catalogue HTTP
- Driver archive HTTP download
- User ZIP import
- Deleting bundled drivers

## Tests & verification

```bash
cd android/BachataS4
./gradlew :core:runtime:test :feature:drivers:test :app:testPlaystoreDebugUnitTest
./gradlew :app:assemblePlaystoreRelease :app:assembleFdroidRelease

# Play APK must contain all three bundled ZIPs; fdroid must not
node runtime/tests/verify-playstore-bundled-turnip.mjs \
  android/BachataS4/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk
! node runtime/tests/verify-fdroid-no-bundled-turnip-asset.mjs \
  android/BachataS4/app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk

# Managed runtime still must not embed Turnip (separate from Play driver asset)
node runtime/tests/verify-no-bundled-turnip.mjs runtime/build/rootfs
```

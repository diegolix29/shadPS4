# Android Runtime Build

## Requirements

- Linux or WSL2 x86-64, JDK 17, Node.js 20+
- Android SDK platform/build-tools 37
- Android NDK `30.0.14904198`, CMake `3.22.1`, Ninja
- Debian/Ubuntu runtime prerequisites from `runtime/scripts/install-debian-runtime-deps.sh`

All runtime inputs and upstream revisions are pinned under `runtime/locks`. Runtime packaging never downloads artifacts; fetch inputs separately, verify their lock hashes, then build from the repository root:

```bash
git submodule update --init --recursive --jobs 8
runtime/scripts/build-runtime-debian.sh
node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
node runtime/tests/verify-no-bundled-turnip.mjs runtime/build/rootfs
```

CI uses `--locks-only` because packaged binaries are intentionally excluded from Git. Release/local packaging must run full verification against generated assets as shown above.

Build APK from scratch:

```bash
cd android/BachataS4
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
export ANDROID_HOME=$HOME/Android/Sdk
./gradlew clean test lintDebug assemblePlaystoreDebug
cd ../..
node runtime/tests/verify-apk-runtime.mjs android/BachataS4/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk
```

The APK verifier requires both `assets/runtime/manifest.json` and `assets/runtime/runtime.zip`, then inspects the nested runtime ZIP. It fails if any Turnip driver, archive, Freedreno ICD, `vulkan.ad07xx.so`, or `libvulkan_freedreno.so` is bundled. Vulkan loader libraries such as `libvulkan.so.1` remain part of the managed runtime.

Turnip drivers are always installed after app installation. The only trusted remote feed is GitHub Releases from `JICA98/bachata-s4-drivers`; users may also import a local emulator-driver ZIP. The APK never packages a Turnip driver.

## Podman Container Build

To build inside the `bachata-debian-builder` Podman container using the host Android SDK:

### 1. Start Container with Host Mounts

```bash
mkdir -p "$HOME/.cache/bachata-ccache" "$HOME/.cache/bachata-gradle" "$HOME/.cache/bachata-android-user"

podman start bachata-debian-builder || podman run --detach \
  --name bachata-debian-builder \
  --hostname bachata-builder \
  --network=pasta \
  -v "$PWD:/workspace:rw" \
  -v "$HOME/Android/Sdk:/opt/android-sdk:ro" \
  -v "$HOME/.cache/bachata-ccache:/root/.cache/ccache:rw" \
  -v "$HOME/.cache/bachata-gradle:/root/.gradle:rw" \
  -v "$HOME/.cache/bachata-android-user:/root/.android:rw" \
  --env ANDROID_HOME=/opt/android-sdk \
  --env ANDROID_USER_HOME=/root/.android \
  --env PATH=/opt/android-sdk/platform-tools:/opt/android-sdk/cmdline-tools/latest/bin:/opt/android-sdk/emulator:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  --workdir /workspace \
  docker.io/library/debian:trixie sleep infinity
```

### 2. Container Dependencies & Host SDK Components

Install compiler tools and JDK in the container, and accept/install required Android SDK components on the host SDK (`ndk;30.0.14904198`, `platforms;android-37.0`, `build-tools;37.0.0`, `cmake;3.22.1`):

```bash
podman exec --workdir /workspace bachata-debian-builder ./runtime/scripts/install-debian-runtime-deps.sh
podman exec --workdir /workspace bachata-debian-builder apt-get install -y openjdk-21-jdk

# Ensure local.properties points to container SDK path
echo "sdk.dir=/opt/android-sdk" > android/BachataS4/local.properties

# Install missing SDK packages onto host SDK if needed (via temporary container rw mount)
podman run --rm -v "$HOME/Android/Sdk:/opt/android-sdk:rw" docker.io/library/debian:trixie bash -c "
  yes | /opt/android-sdk/cmdline-tools/latest/bin/sdkmanager --sdk_root=/opt/android-sdk --licenses
  /opt/android-sdk/cmdline-tools/latest/bin/sdkmanager --sdk_root=/opt/android-sdk 'ndk;30.0.14904198' 'platforms;android-37.0' 'build-tools;37.0.0' 'cmake;3.22.1'
"
```

### 3. Build Runtime & APK via Podman

```bash
# Build Vortek client and managed runtime
podman exec --workdir /workspace bachata-debian-builder bash -c "
  git submodule update --init --recursive --jobs 8
  bash runtime/scripts/build-vortek-client.sh
  runtime/scripts/build-runtime-debian.sh
  node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
  node runtime/tests/verify-no-bundled-turnip.mjs runtime/build/rootfs
"

# Build Play Store debug APK
podman exec --workdir /workspace/android/BachataS4 bachata-debian-builder bash -c "
  export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
  export ANDROID_HOME=/opt/android-sdk
  ./gradlew clean test lintDebug assemblePlaystoreDebug
"

# Verify Play Store APK
podman exec --workdir /workspace bachata-debian-builder bash -c "
  node runtime/tests/verify-playstore-bundled-turnip.mjs android/BachataS4/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk
"
```

## Device Gates

1. Install/launch on arm64-v8a API 31+; scaffold remains responsive.
2. Verify locked Box64/runtime probes execute from app-private storage.
3. Verify embedded X surface survives resize/recreation.
4. Verify audio bridge and Turnip Vulkan probe.
5. Launch shadPS4 without content; require `HELLO`, `Starting`, `CONTENT_INVALID`, `Stopped`.
6. Import user-owned legal homebrew and verify typed validation or title/serial. Paths are argument-list entries, never shell text.
7. Verify first frame/audio/controller, stop/relaunch, one surface recreation, sanitized diagnostics.
8. Record five cold and five warm sessions per qualified SoC using `runtime/qualification/qualification-schema.json`.

Only user-owned legal homebrew or legally dumped content may be imported. Firmware, keys, games, and copyrighted system files are never bundled.

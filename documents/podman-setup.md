# Podman Rootless Environment Setup

This guide details setting up a rootless Podman build environment on Linux/PikaOS for building **Bachata-S4** (runtime assets, native binaries, and Android APKs) using Debian Trixie while keeping the host system clean.

---

## 1. Install Host Prerequisites

Install Podman and rootless helper packages on Debian/Ubuntu/PikaOS host:

```bash
sudo apt-get update
sudo apt-get install -y \
    podman \
    buildah \
    skopeo \
    uidmap \
    passt \
    slirp4netns \
    fuse-overlayfs \
    dbus-user-session \
    nftables
```

Confirm required binaries exist:

```bash
command -v podman
command -v newuidmap
command -v newgidmap
command -v pasta
command -v slirp4netns
command -v fuse-overlayfs
```

> **Note**: `fuse-overlayfs` provides efficient rootless storage when native unprivileged OverlayFS is unavailable. Without it, Podman may fall back to the slow and disk-intensive `vfs` driver.

---

## 2. Configure Subordinate UIDs and GIDs

Check your user's current subuid and subgid mappings:

```bash
grep "^${USER}:" /etc/subuid || true
grep "^${USER}:" /etc/subgid || true
```

If both return an entry (e.g. `jica:100000:65536`), proceed to the next step.

If either entry is missing, add subuid/subgid ranges for your user:

```bash
grep -q "^${USER}:" /etc/subuid || \
    sudo usermod --add-subuids 100000-165535 "$USER"

grep -q "^${USER}:" /etc/subgid || \
    sudo usermod --add-subgids 100000-165535 "$USER"
```

Verify:

```bash
grep "^${USER}:" /etc/subuid
grep "^${USER}:" /etc/subgid
```

> **Important**: If subuids or subgids were added, **log out of your desktop session and log back in** before continuing without `sudo`.

---

## 3. Configure Rootless Podman Storage

Reset any existing rootless Podman storage (affects only unprivileged user storage):

```bash
podman system reset --force
```

Inspect Podman storage configuration:

```bash
podman info --format 'Rootless: {{.Host.Security.Rootless}}
Storage driver: {{.Store.GraphDriverName}}'
```

If the storage driver reports `vfs` instead of `overlay`, configure explicit overlay storage with `fuse-overlayfs`:

```bash
mkdir -p "$HOME/.config/containers"

cat > "$HOME/.config/containers/storage.conf" <<EOF
[storage]
driver = "overlay"
graphroot = "$HOME/.local/share/containers/storage"

[storage.options.overlay]
mount_program = "/usr/bin/fuse-overlayfs"
EOF

podman system reset --force
podman info --format 'Rootless: {{.Host.Security.Rootless}}
Storage driver: {{.Store.GraphDriverName}}'
```

Expected result:

```text
Rootless: true
Storage driver: overlay
```

---

## 4. Test Rootless Networking

Select network driver (`pasta` preferred, `slirp4netns` fallback):

```bash
if command -v pasta >/dev/null 2>&1; then
    PODMAN_NETWORK="pasta"
else
    PODMAN_NETWORK="slirp4netns"
fi

echo "Using rootless network: $PODMAN_NETWORK"
```

Test container pull and outbound networking:

```bash
podman run --rm \
    --network="$PODMAN_NETWORK" \
    docker.io/library/debian:trixie-slim \
    sh -c 'apt-get update -qq && echo "Rootless Podman networking works"'
```

Expected output:
`Rootless Podman networking works`

---

## 5. Create Persistent Bachata-S4 Container

Navigate to the repository root:

```bash
cd /path/to/Bachata-S4
REPO_ROOT="$(git rev-parse --show-toplevel)"
```

Create persistent cache directories on the host:

```bash
mkdir -p "$HOME/.cache/bachata-ccache" \
         "$HOME/.cache/bachata-gradle" \
         "$HOME/.cache/bachata-android-user"
```

> **Warning on `--ulimit`**: Do **not** pass `--ulimit nofile=1048576:1048576` when running rootlessly. Unprivileged user sessions are restricted by host hard limits (`ulimit -Hn`). Omitting `--ulimit` allows Podman to automatically apply limits supported by your rootless session.

Create the persistent container:

```bash
if command -v pasta >/dev/null 2>&1; then PODMAN_NETWORK="pasta"; else PODMAN_NETWORK="slirp4netns"; fi

podman run \
    --detach \
    --name bachata-debian-builder \
    --hostname bachata-builder \
    --network="$PODMAN_NETWORK" \
    --volume "$REPO_ROOT:/workspace:rw" \
    --volume "$HOME/Android/Sdk:/opt/android-sdk:ro" \
    --volume "$HOME/.cache/bachata-ccache:/root/.cache/ccache:rw" \
    --volume "$HOME/.cache/bachata-gradle:/root/.gradle:rw" \
    --volume "$HOME/.cache/bachata-android-user:/root/.android:rw" \
    --env ANDROID_HOME=/opt/android-sdk \
    --env ANDROID_USER_HOME=/root/.android \
    --env PATH=/opt/android-sdk/platform-tools:/opt/android-sdk/cmdline-tools/latest/bin:/opt/android-sdk/emulator:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    --workdir /workspace \
    --pull=missing \
    docker.io/library/debian:trixie \
    sleep infinity
```

Confirm container status:

```bash
podman ps
```

---

## 6. Install Container Dependencies & Tools

Install Debian runtime build dependencies and OpenJDK 21 inside the container:

```bash
podman exec \
    --env BACHATA_INSIDE_DEBIAN_CONTAINER=1 \
    --workdir /workspace \
    bachata-debian-builder \
    bash runtime/scripts/install-debian-runtime-deps.sh

podman exec bachata-debian-builder apt-get install -y openjdk-21-jdk ccache
podman exec bachata-debian-builder ccache --max-size=20G
```

Ensure `local.properties` in `android/BachataS4/` references `/opt/android-sdk`:

```bash
echo "sdk.dir=/opt/android-sdk" > android/BachataS4/local.properties
```

---

## 7. Install Required Host Android SDK Packages

If host Android SDK components are missing (`ndk;30.0.14904198`, `platforms;android-37.0`, `build-tools;37.0.0`, `cmake;3.22.1`), accept licenses and install them on the host SDK via a temporary writeable mount:

```bash
podman run --rm -v "$HOME/Android/Sdk:/opt/android-sdk:rw" docker.io/library/debian:trixie bash -c "
  yes | /opt/android-sdk/cmdline-tools/latest/bin/sdkmanager --sdk_root=/opt/android-sdk --licenses
  /opt/android-sdk/cmdline-tools/latest/bin/sdkmanager --sdk_root=/opt/android-sdk 'ndk;30.0.14904198' 'platforms;android-37.0' 'build-tools;37.0.0' 'cmake;3.22.1'
"
```

---

## 8. Build Runtime Assets & Android APK

### Build Managed Runtime:

```bash
podman exec --workdir /workspace bachata-debian-builder bash -c "
  git submodule update --init --recursive --jobs 8
  bash runtime/scripts/build-vortek-client.sh
  runtime/scripts/build-runtime-debian.sh
  node runtime/tests/verify-runtime.mjs runtime/locks/components.lock.json
  node runtime/tests/verify-no-bundled-turnip.mjs runtime/build/rootfs
"
```

### Build Play Store Debug APK:

```bash
podman exec --workdir /workspace/android/BachataS4 bachata-debian-builder bash -c "
  export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
  export ANDROID_HOME=/opt/android-sdk
  ./gradlew clean test lintDebug assemblePlaystoreDebug
"
```

### Verify Play Store APK:

```bash
podman exec --workdir /workspace bachata-debian-builder bash -c "
  node runtime/tests/verify-playstore-bundled-turnip.mjs android/BachataS4/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk
"
```

---

## 9. Daily Management Commands

- **Start Container**: `podman start bachata-debian-builder`
- **Enter Interactive Shell**:
  ```bash
  podman exec -it --workdir /workspace bachata-debian-builder bash
  ```
- **Stop Container**: `podman stop bachata-debian-builder`
- **View Container Logs**: `podman logs bachata-debian-builder`
- **Check Status**: `podman ps --all`
- **Delete and Recreate Container**: `podman rm -f bachata-debian-builder` *(Repository and ccache persist on host mounts)*

---

## 10. Validation Script

Run this command on your host to verify setup:

```bash
printf '%s\n' \
    "Host user: $(id -un)" \
    "Podman: $(podman --version)" \
    "Rootless: $(podman info --format '{{.Host.Security.Rootless}}')" \
    "Storage: $(podman info --format '{{.Store.GraphDriverName}}')" \
    "Container: $(podman inspect --format '{{.State.Status}}' bachata-debian-builder)" \
    "Container CPUs: $(podman exec bachata-debian-builder nproc)"
```

Expected key values:
- `Rootless: true`
- `Storage: overlay`
- `Container: running`

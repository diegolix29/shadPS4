#!/usr/bin/env bash
set -Eeuo pipefail

export DEBIAN_FRONTEND=noninteractive

readonly SCRIPT_NAME="${0##*/}"
readonly CONTAINER_IMAGE="${BACHATA_DEBIAN_IMAGE:-debian:trixie}"
readonly CONTAINER_NAME="${BACHATA_DEBIAN_CONTAINER:-bachata-debian-builder}"
readonly CONTAINER_MARKER="${BACHATA_INSIDE_DEBIAN_CONTAINER:-0}"
readonly OLD_ARM64_SOURCE="/etc/apt/sources.list.d/bachata-arm64.sources"

log() {
    printf '\n[%s] %s\n' "$SCRIPT_NAME" "$*"
}

die() {
    printf '\n[%s] ERROR: %s\n' "$SCRIPT_NAME" "$*" >&2
    exit 1
}

on_error() {
    local exit_code=$?
    local line_number="${1:-unknown}"

    printf '\n[%s] Failed at line %s with exit code %s\n' \
        "$SCRIPT_NAME" "$line_number" "$exit_code" >&2

    exit "$exit_code"
}

trap 'on_error "$LINENO"' ERR

find_repo_root() {
    local script_directory

    script_directory="$(
        cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &&
            pwd -P
    )"

    git -C "$script_directory" rev-parse --show-toplevel 2>/dev/null ||
        die "Unable to determine the Bachata-S4 repository root."
}

detect_container_engine() {
    if command -v podman >/dev/null 2>&1; then
        printf '%s\n' "podman"
    elif command -v docker >/dev/null 2>&1; then
        printf '%s\n' "docker"
    else
        return 1
    fi
}

is_pikaos_host() {
    [[ -r /etc/os-release ]] || return 1

    (
        # shellcheck disable=SC1091
        source /etc/os-release
        [[ "${ID:-}" == "pika" ]]
    )
}

run_privileged() {
    if [[ "$(id -u)" -eq 0 ]]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        die "sudo is required for host cleanup."
    fi
}

cleanup_old_host_multiarch_source() {
    log "Removing the incompatible Debian ARM64 source from PikaOS"

    run_privileged rm -f "$OLD_ARM64_SOURCE"

    # The failed APT transaction normally installs nothing. Remove the foreign
    # architecture only when no ARM64 packages are currently installed.
    if dpkg --print-foreign-architectures 2>/dev/null |
        grep -qx 'arm64'; then

        if ! dpkg-query -W \
            -f='${binary:Package}\n' 2>/dev/null |
            grep -q ':arm64$'; then

            run_privileged dpkg --remove-architecture arm64 || true
        else
            log "ARM64 packages already exist on the host; keeping the architecture enabled"
        fi
    fi

    run_privileged apt-get update
}

start_debian_container() {
    local repo_root
    local engine
    local script_inside_container

    repo_root="$(find_repo_root)"
    engine="$(detect_container_engine)" || {
        cat >&2 <<'EOF'

No supported container engine was found.

Install Podman on PikaOS with:

    sudo apt-get update
    sudo apt-get install -y podman

Then run this script again without sudo.
EOF
        exit 1
    }

    script_inside_container="/workspace/runtime/scripts/${SCRIPT_NAME}"

    cleanup_old_host_multiarch_source

    log "Using container engine: $engine"
    log "Pulling $CONTAINER_IMAGE"
    "$engine" pull "$CONTAINER_IMAGE"

    if "$engine" container exists "$CONTAINER_NAME" >/dev/null 2>&1; then
        log "Removing previous $CONTAINER_NAME container"
        "$engine" rm -f "$CONTAINER_NAME"
    elif "$engine" inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
        log "Removing previous $CONTAINER_NAME container"
        "$engine" rm -f "$CONTAINER_NAME"
    fi

    log "Creating persistent Debian build container"

    if [[ "$engine" == "podman" ]]; then
        "$engine" run \
            --detach \
            --name "$CONTAINER_NAME" \
            --hostname bachata-builder \
            --volume "${repo_root}:/workspace:rw,Z" \
            --workdir /workspace \
            "$CONTAINER_IMAGE" \
            sleep infinity
    else
        "$engine" run \
            --detach \
            --name "$CONTAINER_NAME" \
            --hostname bachata-builder \
            --mount "type=bind,source=${repo_root},target=/workspace" \
            --workdir /workspace \
            "$CONTAINER_IMAGE" \
            sleep infinity
    fi

    log "Installing dependencies inside Debian Trixie"

    "$engine" exec \
        --env BACHATA_INSIDE_DEBIAN_CONTAINER=1 \
        --workdir /workspace \
        "$CONTAINER_NAME" \
        bash "$script_inside_container"

    cat <<EOF

[$SCRIPT_NAME] Debian builder is ready.

Enter the persistent build container with:

    $engine exec -it --workdir /workspace $CONTAINER_NAME bash

Run a command without entering a shell with:

    $engine exec --workdir /workspace $CONTAINER_NAME <command>

Remove the builder when it is no longer needed with:

    $engine rm -f $CONTAINER_NAME
EOF
}

install_dependencies_inside_debian() {
    [[ "$(id -u)" -eq 0 ]] ||
        die "The dependency installation must run as root inside the container."

    [[ -r /etc/os-release ]] ||
        die "/etc/os-release is missing."

    # shellcheck disable=SC1091
    source /etc/os-release

    [[ "${ID:-}" == "debian" ]] ||
        die "Container is not Debian: ID=${ID:-unknown}"

    [[ "$(dpkg --print-architecture)" == "amd64" ]] ||
        die "The Debian builder must use an AMD64 base image."

    log "Running inside ${PRETTY_NAME:-Debian}"
    log "Enabling ARM64 multiarch"

    dpkg --add-architecture arm64

    log "Refreshing Debian package indexes"

    apt-get \
        -o Acquire::Retries=3 \
        update

    check_matching_candidate() {
        local package="$1"
        local amd64_version
        local arm64_version

        amd64_version="$(
            apt-cache policy "${package}:amd64" |
                awk '/^[[:space:]]*Candidate:/ {
                    print $2
                    exit
                }'
        )"

        arm64_version="$(
            apt-cache policy "${package}:arm64" |
                awk '/^[[:space:]]*Candidate:/ {
                    print $2
                    exit
                }'
        )"

        if [[ -z "$amd64_version" ||
              "$amd64_version" == "(none)" ]]; then
            printf 'Missing AMD64 candidate: %s\n' "$package" >&2
            return 1
        fi

        if [[ -z "$arm64_version" ||
              "$arm64_version" == "(none)" ]]; then
            printf 'Missing ARM64 candidate: %s\n' "$package" >&2
            return 1
        fi

        if [[ "$amd64_version" != "$arm64_version" ]]; then
            printf '%s has unsynchronized multiarch versions:\n' \
                "$package" >&2
            printf '  AMD64: %s\n' "$amd64_version" >&2
            printf '  ARM64: %s\n' "$arm64_version" >&2
            return 1
        fi

        printf '  %-24s %s\n' "$package" "$amd64_version"
    }

    log "Checking synchronized Debian multiarch packages"

    candidate_failure=0

    for package in \
        libc6 \
        libgcc-s1 \
        libstdc++6 \
        libudev1 \
        libuuid1 \
        libx11-6 \
        libx11-xcb1 \
        libxext6 \
        libxi6 \
        libxrender1 \
        libxcb1 \
        libxkbcommon0 \
        libdbus-1-3 \
        libsystemd0 \
        zlib1g \
        libdrm2; do

        if ! check_matching_candidate "$package"; then
            candidate_failure=1
        fi
    done

    if ((candidate_failure != 0)); then
        die "Debian package indexes are temporarily unsynchronized."
    fi

    arch_tools=(
        ca-certificates
        git
        nodejs
        cmake
        ninja-build
        clang
        llvm
        gcc
        g++
        gcc-aarch64-linux-gnu
        g++-aarch64-linux-gnu
        binutils
        binutils-aarch64-linux-gnu
        pkg-config
        file
        patchelf
        python3
        python3-pyelftools
        dpkg-dev
        xz-utils
        zstd
        zip
        unzip
        curl
    )

    arch_dev=(
        libc6-dev:amd64
        libsdl2-dev:amd64
        libvulkan-dev:amd64
        libx11-dev:arm64
        libxext-dev:arm64
    )

    arch_amd64=(
        libc6:amd64
        libgcc-s1:amd64
        libstdc++6:amd64
        libsdl2-2.0-0:amd64
        libvulkan1:amd64
        libudev1:amd64
        libuuid1:amd64
        libx11-6:amd64
        libx11-xcb1:amd64
        libxcursor1:amd64
        libxext6:amd64
        libxfixes3:amd64
        libxi6:amd64
        libxrandr2:amd64
        libxrender1:amd64
        libxss1:amd64
        libxcb1:amd64
        libxcb-dri3-0:amd64
        libxcb-present0:amd64
        libxcb-randr0:amd64
        libxcb-render0:amd64
        libxcb-shm0:amd64
        libxcb-sync1:amd64
        libxau6:amd64
        libxdmcp6:amd64
        libxkbcommon0:amd64
        libdbus-1-3:amd64
        libsystemd0:amd64
        zlib1g:amd64
        libdrm2:amd64
    )

    arch_arm64=(
        libc6:arm64
        libgcc-s1:arm64
        libstdc++6:arm64
        libvulkan1:arm64
        libudev1:arm64
        libudev-dev:arm64
        libuuid1:arm64
        uuid-dev:arm64
        libx11-6:arm64
        libx11-xcb1:arm64
        libxcursor1:arm64
        libxext6:arm64
        libxfixes3:arm64
        libxi6:arm64
        libxrandr2:arm64
        libxrender1:arm64
        libxss1:arm64
        libxcb1:arm64
        libxcb-dri3-0:arm64
        libxcb-present0:arm64
        libxcb-randr0:arm64
        libxcb-render0:arm64
        libxcb-shm0:arm64
        libxcb-sync1:arm64
        libxau6:arm64
        libxdmcp6:arm64
        libxkbcommon0:arm64
        libdbus-1-3:arm64
        libsystemd0:arm64
        zlib1g:arm64
        libdrm2:arm64
    )

    arch_independent=(
        xkb-data
    )

    all_packages=(
        "${arch_tools[@]}"
        "${arch_dev[@]}"
        "${arch_amd64[@]}"
        "${arch_arm64[@]}"
        "${arch_independent[@]}"
    )

    log "Installing ${#all_packages[@]} dependency entries"

    apt-get install \
        --yes \
        --no-install-recommends \
        --no-remove \
        -o Acquire::Retries=3 \
        "${all_packages[@]}"

    log "Verifying installed packages"

    local report_file
    local missing_packages=0

    report_file="$(mktemp)"

    # Capture the concrete path now instead of referencing a local variable later.
    trap 'rm -f -- '"$(printf '%q' "$report_file")"'' EXIT

    for entry in "${all_packages[@]}"; do
        if ! dpkg-query \
            -W \
            -f='${binary:Package} ${Architecture} ${Version} ${source:Package} ${source:Version}\n' \
            "$entry" \
            >>"$report_file" 2>/dev/null; then

            printf 'MISSING: %s\n' "$entry" >&2
            missing_packages=1
        fi
    done

    sort -u "$report_file"

    if ((missing_packages != 0)); then
        die "One or more required packages were not installed."
    fi

    rm -f -- "$report_file"
    trap - EXIT

    log "Debian runtime dependencies installed successfully"
}

if [[ "$CONTAINER_MARKER" == "1" ]]; then
    install_dependencies_inside_debian
elif is_pikaos_host; then
    start_debian_container
else
    install_dependencies_inside_debian
fi
# Android Runtime Third-Party Notice

## Winlator

- Upstream: https://github.com/brunodev85/winlator-app.git
- Revision: `e113da42beefc39c69c8944b27c19c3703bfa856`
- License: LGPL-2.1
- Copied paths: `app/src/main/java/com/winlator/{xserver,alsaserver,core,math,renderer,sysvshm,xconnector}` and `app/src/main/cpp/winlator`
- Local destination: `android/BachataS4/core/runtime/src/main/{java/com/winlator,cpp/winlator}`
- Modifications: source selection only; copied files remain byte-identical to the pinned revision. Bachata S4 integration lives outside `com.winlator`.

`runtime/locks/winlator-vendor.sha256` records every copied upstream path, local path, and SHA-256. Wine UI, installers, assets, and bundled binaries are excluded.

## Vortek

- Client upstream: https://github.com/brunodev85/vortek.git
- Client revision: `ab7329c4b445a4abd9b9af91b8148e1ca41464fa`
- Client license: LGPL-2.1
- Client source destination: `runtime/sources/vortek-client`
- Client is built from source into the managed runtime (`host/lib/libvulkan_vortek.so`); no prebuilt Winlator Vortek asset is redistributed.
- Server upstream: https://github.com/brunodev85/winlator-app.git
- Server revision: `e113da42beefc39c69c8944b27c19c3703bfa856` (same pin as Winlator above)
- Server source location: `runtime/sources/winlator-app/app/src/main/cpp/vortekrenderer`
- Server is also LGPL-2.1 and is built from source (Android native library integration is a later task).
- Protocol headers `request_codes.h` and `vortek_serializer.h` are verified byte-identical between the pinned client and server via `runtime/scripts/vendor-vortek.sh`.
- Shared Bachata handshake definitions live in `runtime/vortek-protocol/bachata_vortek_protocol.h`.
- Android server (Task 4): `libbachata_vortek_server.so` under `android/BachataS4/core/runtime/src/main/cpp/vortek/`, LGPL-2.1-derived ring/ashmem helpers from the pinned Winlator tree, host Vulkan via `dlopen("libvulkan.so")`.

## Runtime Components

- shadPS4 backend: GPL-2.0-or-later; corresponding source is this repository, including Bachata runtime changes.
- Box64: MIT, pinned revision recorded in `runtime/locks/components.lock.json`; local compatibility patches are under `runtime/patches`.
- GNU glibc: LGPL-2.1-or-later. Unmodified locked packages are listed in `runtime/locks/runtime-inputs.lock.json`; package-time Android seccomp compatibility edits are reproducible in `runtime/scripts/package-runtime.mjs`.
- Mesa/Turnip and Vulkan loader: MIT-family licenses; revisions and package hashes are recorded in runtime locks.
- SDL2, X11 libraries, libudev, libuuid, libstdc++, libgcc, zlib, libdrm, and CA certificates: redistributed under their respective upstream licenses with exact package hashes in runtime locks.

For GPL/LGPL components, corresponding source and build scripts are provided in this repository. A written source offer is available with distributed binaries for at least three years where required by the applicable license.

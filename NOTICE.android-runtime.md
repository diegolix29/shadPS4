# Android Runtime Third-Party Notice

## Winlator

- Upstream: https://github.com/JICA98/winlator-app.git (Bachata S4 runtime fork of brunodev85/winlator-app)
- Revision: `72ec347c9ced676e206fbc3762b9d567852cb3e3`
- License: LGPL-2.1
- Copied paths: `app/src/main/java/com/winlator/{xserver,alsaserver,core,math,renderer,sysvshm,xconnector}` and `app/src/main/cpp/winlator`
- Local destination: `android/BachataS4/core/runtime/src/main/{java/com/winlator,cpp/winlator}`
- Modifications: source selection plus Bachata S4 runtime fixes (abstract X11 sockets, keymap query, GPU image unlock, SYNC_FD wait handling) committed to the fork above; `runtime/locks/winlator-vendor.sha256` records every copied path and SHA-256.

## Vortek

- Client upstream: https://github.com/JICA98/vortek.git (Bachata S4 runtime fork of brunodev85/vortek)
- Client revision: `9325b6060fc1c690234e102fcbbb1e0283b8892e`
- Client license: LGPL-2.1
- Client source destination: `runtime/sources/vortek-client`
- Client is built from source into the managed runtime (`host/lib/libvulkan_vortek.so`); no prebuilt Winlator Vortek asset is redistributed.
- Server upstream: https://github.com/JICA98/winlator-app.git
- Server revision: `72ec347c9ced676e206fbc3762b9d567852cb3e3` (same pin as Winlator above)
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

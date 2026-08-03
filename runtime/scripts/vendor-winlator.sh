#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
revision=72ec347c9ced676e206fbc3762b9d567852cb3e3
source_root="$project_root/runtime/sources/winlator-app"
java_source="$source_root/app/src/main/java/com/winlator"
cpp_source="$source_root/app/src/main/cpp/winlator"
runtime_root="$project_root/android/BachataS4/core/runtime/src/main"
manifest="$project_root/runtime/locks/winlator-vendor.sha256"
override_root="$project_root/runtime/vendor-overrides"
license_output="$project_root/LICENSES/Winlator-LGPL-2.1.txt"

"$project_root/runtime/scripts/checkout-component.sh" \
  winlator-app https://github.com/JICA98/winlator-app.git "$revision"

java_packages=(xserver alsaserver sysvshm xconnector)
support_files=(
  core/ArrayUtils.java core/Bitmask.java core/Callback.java core/KeyValueSet.java core/StringUtils.java
  math/Mathf.java
  renderer/FullscreenTransformation.java renderer/GPUImage.java renderer/Texture.java
)
rm -rf "$runtime_root/java/com/winlator"
for package in "${java_packages[@]}"; do
  test -d "$java_source/$package"
done
test -d "$cpp_source"
rm -rf "$runtime_root/cpp/winlator"
mkdir -p "$runtime_root/java/com/winlator" "$runtime_root/cpp/winlator" "$(dirname "$manifest")"

temporary=$(mktemp)
trap 'rm -f "$temporary"' EXIT

copy_tree() {
  local upstream_root=$1 destination_root=$2
  while IFS= read -r -d '' source; do
    local relative=${source#"$upstream_root/"}
    local destination="$destination_root/$relative"
    mkdir -p "$(dirname "$destination")"
    cp -p "$source" "$destination"
    printf '%s|%s\n' \
      "${source#"$source_root/"}" \
      "${destination#"$project_root/"}" >> "$temporary"
  done < <(find "$upstream_root" -type f \( \
    -name '*.java' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' \
    -o -name '*.h' -o -name '*.hpp' -o -name 'CMakeLists.txt' \
  \) -print0 | sort -z)
}

copy_file() {
  local source=$1 destination=$2
  mkdir -p "$(dirname "$destination")"
  cp -p "$source" "$destination"
  printf '%s|%s\n' "${source#"$source_root/"}" "${destination#"$project_root/"}" >> "$temporary"
}

for package in "${java_packages[@]}"; do
  copy_tree "$java_source/$package" "$runtime_root/java/com/winlator/$package"
done
for support_file in "${support_files[@]}"; do
  copy_file "$java_source/$support_file" "$runtime_root/java/com/winlator/$support_file"
done
copy_tree "$cpp_source" "$runtime_root/cpp/winlator"

rm -f "$runtime_root/java/com/winlator/xserver/DesktopHelper.java"
for override in \
  com/winlator/xserver/XServer.java \
  com/winlator/xserver/InputDeviceManager.java \
  com/winlator/xserver/DrawableManager.java \
  com/winlator/xserver/extensions/PresentExtension.java \
  com/winlator/xserver/extensions/DRI3Extension.java \
  com/winlator/xserver/extensions/XComposite.java \
  com/winlator/xconnector/UnixSocketConfig.java \
  com/winlator/alsaserver/ALSAClient.java; do
  cp "$override_root/$override" "$runtime_root/java/$override"
done
for override in winlator/include/time_utils.h winlator/include/string_utils.h; do
  cp "$override_root/native/$override" "$runtime_root/cpp/$override"
done
mkdir -p "$(dirname "$license_output")"
cp "$source_root/LICENSE" "$license_output"

{
  printf '# Winlator source manifest\n'
  printf '# url=https://github.com/JICA98/winlator-app.git\n'
  printf '# revision=%s\n' "$revision"
  printf '# columns=upstream_sha256 local_sha256 upstream_path -> local_path\n'
  while IFS='|' read -r upstream destination; do
    test -f "$project_root/$destination" || continue
    printf '%s %s  %s -> %s\n' \
      "$(sha256sum "$source_root/$upstream" | cut -d' ' -f1)" \
      "$(sha256sum "$project_root/$destination" | cut -d' ' -f1)" \
      "$upstream" "$destination"
  done < <(LC_ALL=C sort -u "$temporary")
} > "$manifest"

if find "$runtime_root/java/com/winlator" "$runtime_root/cpp/winlator" -type f \
  \( -name '*.so' -o -name '*.a' -o -name '*.jar' -o -name '*.apk' -o -name '*.exe' \) | grep -q .; then
  echo "Vendored binary detected" >&2
  exit 1
fi

printf 'vendored_revision=%s files=%s\n' "$revision" "$(grep -vc '^#' "$manifest")"

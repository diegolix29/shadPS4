#!/usr/bin/env bash
# Vendor the pinned Vortek client and verify protocol match against the locked Winlator server tree.
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
component_lock="$project_root/runtime/locks/components.lock.json"
license_output="$project_root/LICENSES/Vortek-LGPL-2.1.txt"
protocol_manifest="$project_root/runtime/locks/vortek-protocol.sha256"

read_component_field() {
  local name=$1
  local field=$2
  node -e '
    const lock = require(process.argv[1]);
    const name = process.argv[2];
    const field = process.argv[3];
    const component = lock.components.find((entry) => entry.name === name);
    if (!component) process.exit(1);
    const value = component[field];
    if (value === undefined || value === null || value === "") process.exit(2);
    process.stdout.write(String(value));
  ' "$component_lock" "$name" "$field"
}

client_revision=$(read_component_field vortek-client revision)
client_url=$(read_component_field vortek-client url)
client_dest_rel=$(read_component_field vortek-client sourceDestination)
server_revision=$(read_component_field vortek-server revision)
server_dest_rel=$(read_component_field vortek-server sourceDestination)
winlator_revision=$(read_component_field winlator-app revision)

if [[ "$server_revision" != "$winlator_revision" ]]; then
  echo "vortek-server revision must match winlator-app ($server_revision vs $winlator_revision)" >&2
  exit 1
fi
if [[ ! "$client_revision" =~ ^[0-9a-f]{40}$ ]]; then
  echo "vortek-client revision must be a full 40-character lowercase hex commit" >&2
  exit 1
fi
if [[ ! "$server_revision" =~ ^[0-9a-f]{40}$ ]]; then
  echo "vortek-server revision must be a full 40-character lowercase hex commit" >&2
  exit 1
fi

client_source="$project_root/$client_dest_rel"
server_source="$project_root/$server_dest_rel"

"$project_root/runtime/scripts/checkout-component.sh" \
  vortek-client \
  "$client_url" \
  "$client_revision"

# checkout-component always writes to runtime/sources/<name>
if [[ "$client_dest_rel" != "runtime/sources/vortek-client" ]]; then
  echo "unexpected vortek-client sourceDestination: $client_dest_rel" >&2
  exit 1
fi

if [[ ! -d "$server_source" ]]; then
  echo "missing Vortek server source directory: $server_source" >&2
  echo "run runtime/scripts/checkout-component.sh winlator-app ... or vendor-winlator.sh first" >&2
  exit 1
fi

if [[ ! -d "$project_root/runtime/sources/winlator-app/.git" ]]; then
  echo "missing winlator-app git checkout" >&2
  exit 1
fi

actual_server_head=$(git -C "$project_root/runtime/sources/winlator-app" rev-parse HEAD)
if [[ "$actual_server_head" != "$server_revision" ]]; then
  echo "Winlator app HEAD $actual_server_head does not match pinned vortek-server $server_revision" >&2
  exit 1
fi

for required in \
  "$client_source/include/request_codes.h" \
  "$client_source/include/vortek_serializer.h" \
  "$client_source/src/main.c" \
  "$client_source/LICENSE" \
  "$server_source/include/request_codes.h" \
  "$server_source/include/vortek_serializer.h"
do
  if [[ ! -f "$required" ]]; then
    echo "missing required Vortek file: $required" >&2
    exit 1
  fi
done

client_request_hash=$(sha256sum "$client_source/include/request_codes.h" | cut -d' ' -f1)
server_request_hash=$(sha256sum "$server_source/include/request_codes.h" | cut -d' ' -f1)
client_serializer_hash=$(sha256sum "$client_source/include/vortek_serializer.h" | cut -d' ' -f1)
server_serializer_hash=$(sha256sum "$server_source/include/vortek_serializer.h" | cut -d' ' -f1)

if [[ "$client_request_hash" != "$server_request_hash" ]]; then
  cat >&2 <<'EOF'
Vortek protocol mismatch:
client request_codes.h does not match pinned Winlator server.
EOF
  exit 1
fi
if [[ "$client_serializer_hash" != "$server_serializer_hash" ]]; then
  cat >&2 <<'EOF'
Vortek protocol mismatch:
client vortek_serializer.h does not match pinned Winlator server.
EOF
  exit 1
fi

if find "$client_source" -type f \( -name '*.so' -o -name '*.a' -o -name '*.tzst' -o -name '*.apk' \) | grep -q .; then
  echo "Vendored binary detected in vortek-client source tree" >&2
  exit 1
fi

mkdir -p "$(dirname "$license_output")" "$(dirname "$protocol_manifest")"
cp "$client_source/LICENSE" "$license_output"

{
  printf '# Vortek protocol compatibility manifest\n'
  printf '# client_url=%s\n' "$client_url"
  printf '# client_revision=%s\n' "$client_revision"
  printf '# server_url=https://github.com/JICA98/winlator-app.git\n'
  printf '# server_revision=%s\n' "$server_revision"
  printf '# protocol_match=true\n'
  printf '%s  client include/request_codes.h\n' "$client_request_hash"
  printf '%s  server include/request_codes.h\n' "$server_request_hash"
  printf '%s  client include/vortek_serializer.h\n' "$client_serializer_hash"
  printf '%s  server include/vortek_serializer.h\n' "$server_serializer_hash"
} > "$protocol_manifest"

printf '[Bachata.Vortek.Build] client_commit=%s\n' "$client_revision"
printf '[Bachata.Vortek.Build] server_commit=%s\n' "$server_revision"
printf '[Bachata.Vortek.Build] protocol_match=true\n'
printf 'vendored_vortek_client=%s server_dir=%s\n' "$client_revision" "$server_dest_rel"

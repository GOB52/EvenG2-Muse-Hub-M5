#!/usr/bin/env bash
# Host tests for hub_proto and hub_turns. Needs hub/managed_components (run a hub build once).
set -euo pipefail

hub_dir=$(cd "$(dirname "$0")/../.." && pwd)
cjson_dir=$hub_dir/managed_components/espressif__cjson/cJSON
[ -f "$cjson_dir/cJSON.c" ] || { echo "cJSON not found: run hub/tools/build.sh build once" >&2; exit 1; }

out_dir=$(mktemp -d)
san=(-fsanitize=address,undefined -fno-omit-frame-pointer)
cc -std=c11 -g -c "${san[@]}" -I "$cjson_dir" "$cjson_dir/cJSON.c" -o "$out_dir/cJSON.o"
cc -std=c11 -g -Wall -Wextra -Werror "${san[@]}" \
    -I "$hub_dir/components/hub/include" -I "$cjson_dir" \
    "$hub_dir/test/host/test_hub.c" \
    "$hub_dir/components/hub/hub_proto.c" \
    "$out_dir/cJSON.o" -lpthread -o "$out_dir/test_hub"
"$out_dir/test_hub"

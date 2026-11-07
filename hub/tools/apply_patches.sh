#!/usr/bin/env bash
# Applies hub/patches/*.patch to the SDK submodule's working tree. Idempotent:
# a patch that is already in place is left alone.
set -euo pipefail

hub_dir=$(cd "$(dirname "$0")/.." && pwd)
sdk_dir=$hub_dir/third_party/muse-gadget-sdk

for patch in "$hub_dir"/patches/*.patch; do
    [ -e "$patch" ] || continue
    name=$(basename "$patch")
    if git -C "$sdk_dir" apply --reverse --check "$patch" 2>/dev/null; then
        echo "patch in place: $name"
    else
        git -C "$sdk_dir" apply "$patch"
        echo "patch applied: $name"
    fi
done

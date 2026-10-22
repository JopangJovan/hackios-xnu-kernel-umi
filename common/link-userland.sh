#!/bin/bash
# Copy an existing sibling checkout into the combined build workspace.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/lib.sh"
source_tree=$(realpath "${1:-../hackios-xnu-userland-umi}")
[ -f "$source_tree/userland/build.sh" ] || die "not a userland checkout: $source_tree"
# Only tracked source is copied, never downloads or build products.
mapfile -d '' files < <(git -C "$source_tree" ls-files -z)
for item in "${files[@]}"; do
    case "$item" in README.md|LICENSE|.gitignore) continue ;; esac
    [ ! -e "$item" ] || cmp -s "$source_tree/$item" "$item" || die "existing different file: $item; left untouched"
done
for item in "${files[@]}"; do
    case "$item" in README.md|LICENSE|.gitignore) continue ;; esac
    mkdir -p "$(dirname "$item")"
    [ -e "$item" ] || cp -a "$source_tree/$item" "$item"
done
echo "userland sources copied from $source_tree; builds stay in $ROOT"

#!/bin/bash
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
output_dir=${1:?usage: build.sh OUTPUT_DIRECTORY [I4TOOLS_FRAMEWORK_DIRECTORY]}
framework_dir=${2:-/Applications/i4Tools.app/Contents/Frameworks}
mkdir -p "$output_dir"
for name in i4-paired-info paired-syslog afc-photo-check; do
    clang -Wall -Wextra -Werror "$source_dir/$name.c" \
        -L"$framework_dir" -Wl,-rpath,"$framework_dir" -limobiledevice -lplist2 \
        -o "$output_dir/$name"
done

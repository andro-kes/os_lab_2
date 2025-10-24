#!/usr/bin/env bash
set -euo pipefail

project_root="$(dirname "$(dirname "${BASH_SOURCE[0]}")")"
src_dir="$project_root/src"
build_dir="$project_root/../build"

mkdir -p "$build_dir"

if compgen -G "$src_dir/*.c" > /dev/null; then
  for cfile in "$src_dir"/*.c; do
    fname="$(basename "$cfile" .c)"
    gcc -std=c17 -O2 -Wall -Wextra -Werror "$cfile" -o "$build_dir/$fname"
  done
  echo "Built binaries in $build_dir"
else
  echo "No C files found in $src_dir" >&2
  exit 1
fi

echo "To run a binary: $build_dir/<name>"

#!/usr/bin/env bash
set -euo pipefail

# Compile and run C programs from lab1/src
project_root="$(dirname "$(dirname "${BASH_SOURCE[0]}")")"
src_dir="$project_root/lab1/src"
build_dir="$project_root/build"

mkdir -p "$build_dir"

if compgen -G "$src_dir/*.c" > /dev/null; then
  for cfile in "$src_dir"/*.c; do
    fname="$(basename "$cfile" .c)"
    gcc -std=c17 -O2 -Wall -Wextra -Werror "$cfile" -o "$build_dir/$fname"
  done
  echo "Built binaries in $build_dir"
else
  echo "No C files found in $src_dir" >&2
  exit 1
fi

echo "To run a binary: $build_dir/<name>"



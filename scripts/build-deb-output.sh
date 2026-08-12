#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="$repo_root/output"

cd "$repo_root"

mkdir -p "$output_dir"

if [[ ! -e debian ]]; then
  ln -s packaging/debian debian
fi

dpkg-buildpackage -b -us -uc

shopt -s nullglob
artifacts=(
  "$repo_root"/../fcitx5-lotus_*.deb
  "$repo_root"/../fcitx5-lotus-dbgsym_*.ddeb
  "$repo_root"/../fcitx5-lotus_*.buildinfo
  "$repo_root"/../fcitx5-lotus_*.changes
)

if ((${#artifacts[@]} == 0)); then
  echo "No package artifacts found after build." >&2
  exit 1
fi

mv -f "${artifacts[@]}" "$output_dir"/

echo "Build artifacts:"
ls -1 "$output_dir"

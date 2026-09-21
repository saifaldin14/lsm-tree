#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
  printf '%s\n' 'CUDA bootstrap requires a native Linux x86_64 host.' >&2
  exit 1
fi
for executable in nvcc curl unzip sha256sum; do
  command -v "$executable" >/dev/null || { printf 'Required command unavailable: %s\n' "$executable" >&2; exit 1; }
done
archive="$root/.deps/nvcomp-5.3.0.16.whl"
mkdir -p "$root/.deps"
if [[ ! -f "$archive" ]]; then
  curl -fL 'https://files.pythonhosted.org/packages/b7/20/e025aa3fa2ee33ee6a4871aa5a9e10b05472913adaff649ba21f2c98291d/nvidia_libnvcomp_cu12-5.3.0.16-py3-none-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl' -o "$archive"
fi
printf '%s  %s\n' '535944be2e37ead56ad1ef80e9dd58a3126fc2ccf238ddffc9d1cb7a48f5300a' "$archive" | sha256sum -c
unzip -q -n "$archive" -d "$root/.deps/nvcomp-5.3.0.16"
printf '%s\n' 'Pinned nvCOMP package extracted. Driver and CUDA toolkit are not installed or modified by this script.'
#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
registry="$root/.deps/vcpkg"
revision=319504a5326aa870edde46438c5455fa76305a56
cache="$HOME/.cache/lsm-lab/vcpkg-${revision:0:12}"

if [[ "$cache" =~ [[:space:]] ]]; then
  printf 'Dependency build paths must not contain whitespace: %s\n' "$cache" >&2
  exit 1
fi

for executable in git cmake curl; do
  command -v "$executable" >/dev/null || { printf 'Missing prerequisite: %s\n' "$executable" >&2; exit 1; }
done

if [[ ! -d "$registry" ]]; then
  git clone --filter=blob:none --no-checkout https://github.com/microsoft/vcpkg.git "$registry"
  git -C "$registry" checkout --detach "$revision"
fi
if [[ "$(git -C "$registry" rev-parse HEAD)" != "$revision" ]]; then
  printf 'Dependency registry differs from the locked revision; refusing to overwrite it.\n' >&2
  exit 1
fi
if [[ -n "$(git -C "$registry" status --porcelain --untracked-files=no)" ]]; then
  printf 'Dependency registry has local changes; refusing to use an unverified tree.\n' >&2
  exit 1
fi
if [[ ! -x "$registry/vcpkg" ]]; then
  "$registry/bootstrap-vcpkg.sh" -disableMetrics
fi
export VCPKG_MAX_CONCURRENCY="${VCPKG_MAX_CONCURRENCY:-2}"
export VCPKG_DISABLE_METRICS=1
mkdir -p "$cache"
"$registry/vcpkg" install --x-manifest-root="$root" \
  --x-buildtrees-root="$cache/buildtrees" \
  --x-packages-root="$cache/packages" \
  --x-install-root="$cache/installed"
#!/usr/bin/env bash
# Earlier designs with tuned settings (PREREGISTRATION.md): native, full offload and the pipeline
# rebuild (with native's and with full offload's tuned 4-core settings), LZ4 then ZSTD, 3 blocks.
#
# Usage: publication/prior_tuned.sh OUT_ROOT
set -euo pipefail
out=${1:?usage: prior_tuned.sh OUT_ROOT}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export LSM_BUILD_DIR=${LSM_BUILD_DIR:-$root/build/zyra-cuda}
shm=${SHM:-/dev/shm/$USER/prior-tuned}
bench=$LSM_BUILD_DIR/lsm_system_bench
mkdir -p "$out" "$shm/scratch"

golden() {
  local directory=$1; shift
  [ -f "$directory.done" ] && return
  rm -rf "$directory"
  "$bench" --phase preload --dir "$directory" --records 50000000 --threads 8 "$@" > "$directory.json"
  touch "$directory.done"
}

python3 - "$out" <<'PY'
import json, sys
out = sys.argv[1]
headline = json.load(open("configs/system/v3-headline.json"))
tuned = {t["design"]: t["flags"] for t in headline["trials"] if t["cores"] == 4}
pipeline_env = {"LSM_GPU_CPU_DECOMPRESS": "1", "LSM_GPU_CPU_COMPRESS": "1"}
for codec in ("lz4", "zstd"):
    common = dict(headline["common"], compression=codec)
    trials = [
        {"design": "native", "mode": "native_cpu", "cores": 4, "writes": "batched", "flags": tuned["native"]},
        {"design": "full", "mode": "cuda_merge", "cores": 4, "writes": "batched", "flags": tuned["full"]},
        {"design": "pipeline_native_flags", "mode": "cuda_merge", "cores": 4, "writes": "batched",
         "flags": tuned["native"], "env": pipeline_env},
        {"design": "pipeline_full_flags", "mode": "cuda_merge", "cores": 4, "writes": "batched",
         "flags": tuned["full"], "env": pipeline_env},
    ]
    for t in trials:
        t["memory"] = "preferred"
    json.dump({"_comment": f"Earlier designs with tuned settings ({codec}); PREREGISTRATION.md.",
               "common": common, "trials": trials}, open(f"{out}/grid-{codec}.json", "w"), indent=1)
    print(codec, json.dumps(common), [(t["design"], t["flags"]["subcompactions"], t["flags"]["background-jobs"]) for t in trials])
PY

golden "$shm/golden-50m"
golden "$shm/golden-50m-zstd" --compression zstd
for codec in lz4 zstd; do
  golden_dir=$shm/golden-50m; [ "$codec" = zstd ] && golden_dir=$shm/golden-50m-zstd
  resume=(); [ -f "$out/$codec/schedule.json" ] && resume=(--resume)
  echo "== $codec $(date)"
  python3 publication/system_campaign.py "$out/$codec" --golden "$golden_dir" --grid "$out/grid-$codec.json" \
    --blocks 3 --seed 7 --scratch "$shm/scratch" --idle-seconds 0 "${resume[@]}"
  echo "== $codec exit=$? $(date)"
done
echo "== DONE $(date)"

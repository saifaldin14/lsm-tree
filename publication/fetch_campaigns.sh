#!/usr/bin/env bash
# Copies campaign results from the GPU host into publication/data/campaigns/,
# dropping per-second timelines (kept on the host) to keep the repository small.
# Usage: publication/fetch_campaigns.sh HOST [REMOTE_DIR]
set -euo pipefail
host=${1:?usage: fetch_campaigns.sh HOST [REMOTE_DIR]}
remote=${2:-/tmp/$USER/campaigns}
here=$(cd "$(dirname "$0")" && pwd)
out="$here/data/campaigns"
mkdir -p "$out"
for name in main equal-cost ycsb contention read-tradeoff shared-gpu ssd-scale prior-lz4 prior-zstd prior-none \
            prior-equal-cost; do
  if ssh "$host" "test -f $remote/$name/results.jsonl"; then
    ssh "$host" "cat $remote/$name/results.jsonl" | python3 -c '
import json, sys
for line in sys.stdin:
    record = json.loads(line)
    for result in [record.get("result")] + record.get("results", []):
        if result:
            result.pop("timeline", None)
    print(json.dumps(record))' > "$out/$name.jsonl"
    ssh "$host" "cat $remote/$name/schedule.json" > "$out/$name-schedule.json"
    echo "$name: $(wc -l < "$out/$name.jsonl") trials"
  fi
done
if ssh "$host" "test -d $remote/isolated-v2"; then
  mkdir -p "$here/data/compaction"
  ssh "$host" "cd $remote/isolated-v2 && tar -cf - *.jsonl" | tar -xf - -C "$here/data/compaction"
  echo "isolated-v2: $(ls "$here/data/compaction" | wc -l) files"
fi
if ssh "$host" "test -d $remote/prior-isolated"; then
  mkdir -p "$here/data/prior-compaction"
  ssh "$host" "cd $remote/prior-isolated && tar -cf - *.jsonl" | tar -xf - -C "$here/data/prior-compaction"
  echo "prior-isolated: $(ls "$here/data/prior-compaction" | wc -l) files"
fi
if ssh "$host" "test -d $remote/gparakv"; then
  mkdir -p "$out/gparakv"
  ssh "$host" "cd $remote/gparakv && tar -cf - *.done *.time *.spec *.err" | tar -xf - -C "$out/gparakv"
  echo "gparakv: $(ls "$out/gparakv"/*.done 2>/dev/null | wc -l) runs"
fi

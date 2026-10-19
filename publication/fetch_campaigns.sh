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
for name in main equal-cost ycsb contention read-tradeoff shared-gpu ssd-scale; do
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

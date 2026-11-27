#!/bin/bash
# Malphite NVMe re-tune: registered 8-core search for both designs, frozen selection, then X8 confirmation.
set -uo pipefail
cd ~/lsm-tree
U=$(whoami); D=/tmp/$U/retune; GOLDEN=/tmp/$U/x8/golden-50m
export LSM_BUILD_DIR=build/zyra-cuda
mkdir -p $D
exec >> $D/run.log 2>&1

echo "== build grids $(date)"
python3 - "$D" <<'PY'
import json, sys
D = sys.argv[1]
search = json.load(open("configs/system/tune-search.json"))
x8 = json.load(open("configs/system/v3-ssd.json"))
trials = []
for t in search["trials"]:
    if t["cores"] != 8:
        continue
    t = json.loads(json.dumps(t))
    t["flags"]["allow-fallocate"] = False          # as in X8
    trials.append(t)
common = dict(search["common"], **{"use-direct-io": True})  # same storage path as X8
json.dump({"_comment": "Registered tune-search.json, 8-core subset, on malphite NVMe with direct I/O as in X8.",
           "common": common, "trials": trials}, open(f"{D}/tune-grid.json", "w"), indent=1)
print("tune trials:", len(trials), "common:", json.dumps(common))
PY

echo "== tune search $(date)"
python3 publication/system_campaign.py $D/tune --golden $GOLDEN --grid $D/tune-grid.json \
  --blocks 1 --seed 3 --scratch $D/scratch --idle-seconds 0
echo "== tune exit=$? $(date)"

echo "== select and freeze $(date)"
python3 - "$D" <<'PY' || { echo "== SELECTION FAILED, not running confirmation"; exit 1; }
import json, sys, datetime
D = sys.argv[1]
rows = [json.loads(l) for l in open(f"{D}/tune/results.jsonl") if l.strip()]
table, best = [], {}
for r in rows:
    t, res = r["trial"], r.get("result") or {}
    ok = r.get("exit_code") == 0 and "keys" in (res.get("verification") or {})
    ops = res.get("throughput", {}).get("ops_per_second") if ok else None
    stall = res.get("stall_fraction") if ok else None
    eligible = ok and stall is not None and stall < 0.05
    table.append({"design": t["design"], "flags": t["flags"], "ops_per_second": ops,
                  "stall_fraction": stall, "verified": ok, "eligible": eligible})
    if eligible and (t["design"] not in best or ops > best[t["design"]]["ops_per_second"]):
        best[t["design"]] = {"ops_per_second": ops, "trial": t}
if set(best) != {"native", "full"}:
    raise SystemExit(f"no eligible setting for: {sorted({'native','full'} - set(best))}")
frozen = {"_comment": "Selection rule from PREREGISTRATION.md: per design, highest throughput with stall < 5% "
                      "and passing verification. Frozen before the confirmation run.",
          "frozen_at": datetime.datetime.now().isoformat(timespec="seconds"),
          "selected": {d: b["trial"] for d, b in best.items()},
          "search": table}
json.dump(frozen, open(f"{D}/tuned-malphite.json", "w"), indent=1)
x8 = json.load(open("configs/system/v3-ssd.json"))
json.dump({"_comment": "X8 on malphite NVMe with settings frozen in tuned-malphite.json.",
           "common": x8["common"], "trials": [best["native"]["trial"], best["full"]["trial"]]},
          open(f"{D}/confirm-grid.json", "w"), indent=1)
for d, b in best.items():
    print(f"selected {d}: {b['ops_per_second']/1e6:.3f} M/s flags={json.dumps(b['trial']['flags'])}")
PY
chmod a-w $D/tuned-malphite.json $D/confirm-grid.json

echo "== confirmation X8 $(date)"
python3 publication/system_campaign.py $D/confirm --golden $GOLDEN --grid $D/confirm-grid.json \
  --blocks 3 --seed 4 --scratch $D/scratch --idle-seconds 0
echo "== confirmation exit=$? $(date)"
echo "== DONE $(date)"

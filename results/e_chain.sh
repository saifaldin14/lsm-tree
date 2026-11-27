#!/bin/bash
# E2-E5 (PREREGISTRATION.md, "Lab machines for E2 to E5"), one after another on malphite, in tmux:
#   tmux new -d -s echain 'bash /tmp/sa29ali/lsm-e/results/e_chain.sh > /tmp/sa29ali/echain.log 2>&1'
# Order E4, E3, E2, E5. Each step resumes from its results if rerun; a failed step does not stop the rest.
set -u
REPO=/tmp/$USER/lsm-e
HOSTS=$HOME/rdma-private/rdma-hosts-e.json
PYKE=$USER@192.168.100.103
OUT=$HOME/rdma-out
cd $REPO

# pyke clears a user's /dev/shm when the user's last login ends; hold one open for the whole chain.
ssh -o BatchMode=yes $PYKE "exec sleep 30d" &
HOLD=$!
trap 'kill $HOLD 2>/dev/null' EXIT
sleep 5

step() { echo "=== $1 start $(date -Is)"; shift; "$@"; echo "=== exit $? $(date -Is)"; }

step E4 python3 results/rdma/rdma_campaign.py --hosts $HOSTS --experiment r10 --out $OUT/e4 \
  --instances 4 --engines 4 --duration-seconds 240 --kill-at 60 --outage 60
step E3 bash results/e3_run.sh
if ssh -o BatchMode=yes $PYKE "test -x /tmp/$USER/e2/bin/kvrocks"; then
  step E2 ssh -o BatchMode=yes $PYKE "cd /tmp/$USER/lsm-e && python3 results/e2/e2_campaign.py /tmp/$USER/e2/out"
else
  echo "=== E2 skipped: kvrocks is not on pyke"
fi
step E5 python3 results/rdma/rdma_campaign.py --hosts $HOSTS --experiment r11 --out $OUT/e5 \
  --instances 8 --engines 4
echo "=== chain done $(date -Is)"

#!/usr/bin/env bash
# Cost guard for the cloud experiments. Only resource groups tagged purpose=lsm-cloud are touched.
#
#   infra/azure/guard.sh sweep          delete every lsm-cloud group whose "expires" tag has passed
#   infra/azure/guard.sh watch [MIN]    sweep every MIN minutes (10) until no lsm-cloud group is left
#   infra/azure/guard.sh list           lsm-cloud groups with their expiry
#   infra/azure/guard.sh budget AMOUNT  monthly subscription budget with e-mail alerts at 50/80/100%
set -euo pipefail

groups() {
  az group list --tag purpose=lsm-cloud --query "[].[name, tags.expires]" -o tsv
}

sweep() {
  now=$(date -u +%Y-%m-%dT%H:%MZ)
  groups | while read -r name expires; do
    if [[ -z "$expires" || "$expires" < "$now" ]]; then
      echo "$now: deleting expired $name (expires $expires)"
      az group delete -n "$name" --yes --no-wait
    fi
  done
}

case "${1:-}" in
  sweep) sweep ;;
  list) groups ;;
  watch)
    minutes=${2:-10}
    while [[ -n "$(groups)" ]]; do
      sweep
      sleep $((minutes * 60))
    done
    echo "no lsm-cloud resource groups left"
    ;;
  budget)
    amount=${2:?amount in the billing currency}
    subscription=$(az account show --query id -o tsv)
    email=$(az account show --query user.name -o tsv)
    start=$(date -u +%Y-%m-01)
    body=$(python3 - "$amount" "$start" "$email" <<'PY'
import json, sys
amount, start, email = float(sys.argv[1]), sys.argv[2], sys.argv[3]
year = int(start[:4])
notify = {f"at{p}": {"enabled": True, "operator": "GreaterThanOrEqualTo", "threshold": p,
                     "contactEmails": [email], "thresholdType": "Actual"} for p in (50, 80, 100)}
print(json.dumps({"properties": {"category": "Cost", "amount": amount, "timeGrain": "Monthly",
                                 "timePeriod": {"startDate": f"{start}T00:00:00Z",
                                                "endDate": f"{year + 1}{start[4:]}T00:00:00Z"},
                                 "notifications": notify}}))
PY
)
    az rest --method put --url "https://management.azure.com/subscriptions/$subscription/providers/Microsoft.Consumption/budgets/lsm-cloud?api-version=2023-05-01" \
      --body "$body" --query "{name:name, amount:properties.amount}" -o tsv
    ;;
  *) sed -n '2,8p' "$0"; exit 2 ;;
esac

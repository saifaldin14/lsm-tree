#!/usr/bin/env bash
# Plain VMs for C4 (what Kubernetes costs): the same image and VM sizes as the AKS pools, one
# Docker container per database slot or service, pinned to the same number of vCPUs as the pods.
#
#   infra/azure/vms.sh up IMAGE [DB_SLOTS]   create the VMs and containers; writes vms-hosts.json
#   infra/azure/vms.sh down                  delete the resource group
#
# Settings (environment): LSM_AZ_VM_RG (lsm-cloud-vms), LSM_AZ_LOCATION (southcentralus),
# LSM_AZ_HOURS (6), LSM_DB_CPUS (4), LSM_SVC_CPUS (14), LSM_DB_MEMORY (16g), LSM_SVC_MEMORY (32g),
# LSM_WORK_SIZE (8g), and the sizes below. SSH is opened to this machine's public address only.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rg=${LSM_AZ_VM_RG:-lsm-cloud-vms}
location=${LSM_AZ_LOCATION:-southcentralus}
hours=${LSM_AZ_HOURS:-6}
service_size=${LSM_AZ_SERVICE_SIZE:-Standard_D16ads_v6}
tenant_sizes=${LSM_AZ_TENANT_SIZES:-Standard_D16ds_v6,Standard_D16s_v6}
db_cpus=${LSM_DB_CPUS:-4}
svc_cpus=${LSM_SVC_CPUS:-14}
key=${LSM_AZ_SSH_KEY:-$HOME/.ssh/lsm-cloud}
out=${LSM_AZ_HOSTS:-$here/vms-hosts.json}

vm() { # name size
  az vm create -g "$rg" -n "$1" --size "$2" --image Canonical:ubuntu-24_04-lts:server:latest \
    --vnet-name lsm --subnet vms --nsg lsm --public-ip-sku Standard --accelerated-networking true \
    --admin-username azureuser --ssh-key-values "$key.pub" --custom-data "$here/vm-cloud-init.yaml" \
    --tags purpose=lsm-cloud --query "{name:name, public:publicIpAddress, private:privateIpAddress}" -o json
}

up() {
  image=${1:?image}
  slots=${2:-6}
  [[ -f "$key" ]] || ssh-keygen -t ed25519 -N "" -f "$key" -C lsm-cloud >/dev/null
  expires=$(date -u -v+"${hours}"H +%Y-%m-%dT%H:%MZ 2>/dev/null || date -u -d "+${hours} hours" +%Y-%m-%dT%H:%MZ)
  az group create -n "$rg" -l "$location" --tags purpose=lsm-cloud expires="$expires" -o none
  me=$(curl -fsS https://api.ipify.org)
  az network nsg create -g "$rg" -n lsm -o none
  az network nsg rule create -g "$rg" --nsg-name lsm -n ssh --priority 100 --source-address-prefixes "$me/32" \
    --destination-port-ranges 22 --access Allow --protocol Tcp -o none
  az network vnet create -g "$rg" -n lsm --address-prefix 10.42.0.0/16 --subnet-name vms \
    --subnet-prefix 10.42.0.0/24 --network-security-group lsm -o none
  svc=$(vm svc "$service_size")
  tenants=()
  index=0
  for size in ${tenant_sizes//,/ }; do
    index=$((index + 1))
    tenants+=("$(vm "tenant$index" "$size")")
  done
  python3 - "$out" "$image" "$slots" "$db_cpus" "$svc_cpus" "$key" "$svc" "${tenants[@]}" <<'PY'
import json, sys
out, image, slots, db_cpus, svc_cpus, key, svc, *tenants = sys.argv[1:]
svc, tenants = json.loads(svc), [json.loads(t) for t in tenants]
slots, db_cpus = int(slots), int(db_cpus)
targets = {"svc": {"host": svc["public"], "private": svc["private"], "node": svc["name"],
                   "cpuset": f"0-{int(svc_cpus) - 1}"}}
for index in range(slots):
    machine = tenants[index % len(tenants)]
    first = (index // len(tenants)) * db_cpus
    targets[f"db-{index}"] = {"host": machine["public"], "private": machine["private"], "node": machine["name"],
                              "cpuset": f"{first}-{first + db_cpus - 1}"}
json.dump({"backend": "vm", "image": image,
           "ssh": ["ssh", "-i", key, "-o", "StrictHostKeyChecking=accept-new", "-o", "ServerAliveInterval=30",
                   "-l", "azureuser"],
           "targets": targets}, open(out, "w"), indent=1)
print(f"wrote {out}")
PY
  # Wait for Docker (cloud-init), then start one container per target, as the pods.
  python3 - "$out" <<'PY'
import json, os, subprocess, sys, time
config = json.load(open(sys.argv[1]))
env = os.environ
for name, t in config["targets"].items():
    service = name == "svc"
    memory = env.get("LSM_SVC_MEMORY", "32g") if service else env.get("LSM_DB_MEMORY", "16g")
    work = "" if service else f"--tmpfs /work:size={env.get('LSM_WORK_SIZE', '8g')} -v /var/lib/lsm/{name}:/golden"
    first, last = map(int, t["cpuset"].split("-"))
    # A new ssh session each attempt, so the docker group added by cloud-init applies.
    command = (f"docker info >/dev/null 2>&1 || exit 1; sudo mkdir -p /var/lib/lsm/{name}; "
               f"docker rm -f {name} >/dev/null 2>&1; docker pull -q {config['image']} >/dev/null && "
               f"docker run -d --name {name} --network host --cpuset-cpus {t['cpuset']} --cpus {last - first + 1} "
               f"--memory {memory} --memory-swap {memory} {work} {config['image']}")
    for attempt in range(60):
        result = subprocess.run(config["ssh"] + [t["host"], command], capture_output=True, text=True)
        if result.returncode == 0:
            break
        time.sleep(10)
    else:
        sys.exit(f"{name}: {result.stderr[-500:]}")
    print(f"{name} on {t['node']} cpus {t['cpuset']}")
PY
}

down() {
  if az group exists -n "$rg" | grep -q true; then
    az group delete -n "$rg" --yes --no-wait
    echo "deleting resource group $rg"
  else
    echo "no resource group $rg"
  fi
}

case "${1:-}" in
  up) shift; up "$@" ;;
  down) down ;;
  *) sed -n '2,11p' "$0"; exit 2 ;;
esac

#!/usr/bin/env bash
# AKS cluster for the cloud experiments (docs/cloud.md, Azure track). Everything lives in one
# resource group, tagged with an expiry time; `down` deletes the group (nothing else is touched).
#
#   infra/azure/aks.sh up      create the group, the cluster and its node pools; fetch credentials
#   infra/azure/aks.sh down    delete the resource group (and with it every resource it holds)
#   infra/azure/aks.sh status  node pools and nodes
#
# Settings (environment): LSM_AZ_RG (lsm-cloud-aks), LSM_AZ_LOCATION (southcentralus),
# LSM_AZ_HOURS (6: expiry tag read by guard.sh), and the VM sizes below.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rg=${LSM_AZ_RG:-lsm-cloud-aks}
location=${LSM_AZ_LOCATION:-southcentralus}
hours=${LSM_AZ_HOURS:-6}
cluster=${LSM_AZ_CLUSTER:-lsm}
system_size=${LSM_AZ_SYSTEM_SIZE:-Standard_B2s}
service_size=${LSM_AZ_SERVICE_SIZE:-Standard_D16ads_v5}
tenant_sizes=${LSM_AZ_TENANT_SIZES:-Standard_D16ds_v5,Standard_D16s_v5}

up() {
  expires=$(date -u -v+"${hours}"H +%Y-%m-%dT%H:%MZ 2>/dev/null || date -u -d "+${hours} hours" +%Y-%m-%dT%H:%MZ)
  az group create -n "$rg" -l "$location" --tags purpose=lsm-cloud expires="$expires" -o none
  echo "resource group $rg ($location), expires $expires"
  az aks create -g "$rg" -n "$cluster" -l "$location" --tier free \
    --nodepool-name system --node-count 1 --node-vm-size "$system_size" \
    --network-plugin azure --network-plugin-mode overlay \
    --enable-managed-identity --no-ssh-key --tags purpose=lsm-cloud -o none
  # Whole vCPUs for Guaranteed pods (database slots and the service).
  az aks nodepool add -g "$rg" --cluster-name "$cluster" -n service --node-count 1 --node-vm-size "$service_size" \
    --kubelet-config "$here/kubelet.json" --labels lsm/role=service --node-taints lsm/role=service:NoSchedule \
    --mode User -o none
  index=0
  for size in ${tenant_sizes//,/ }; do
    index=$((index + 1))
    az aks nodepool add -g "$rg" --cluster-name "$cluster" -n "tenant$index" --node-count 1 --node-vm-size "$size" \
      --kubelet-config "$here/kubelet.json" --labels lsm/role=tenant --mode User -o none
  done
  az aks get-credentials -g "$rg" -n "$cluster" --overwrite-existing -o none
  kubectl get nodes -L lsm/role,node.kubernetes.io/instance-type
}

down() {
  if az group exists -n "$rg" | grep -q true; then
    az group delete -n "$rg" --yes --no-wait
    echo "deleting resource group $rg"
  else
    echo "no resource group $rg"
  fi
}

status() {
  az aks nodepool list -g "$rg" --cluster-name "$cluster" \
    --query "[].{pool:name,size:vmSize,count:count,state:provisioningState}" -o table
  kubectl get nodes -L lsm/role,node.kubernetes.io/instance-type
}

case "${1:-}" in
  up) up ;;
  down) down ;;
  status) status ;;
  *) sed -n '2,12p' "$0"; exit 2 ;;
esac

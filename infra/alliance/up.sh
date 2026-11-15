#!/usr/bin/env bash
# k3s cluster on the Alliance's Arbutus cloud for the GPU track (README.md). Needs the OpenStack
# CLI authenticated for the project (source the project's RC file; see README.md).
#
#   infra/alliance/up.sh up       create keypair, security group, server, GPU node and tenant nodes
#   infra/alliance/up.sh kubeconfig   fetch the kubeconfig to ~/.kube/lsm-alliance
#   infra/alliance/up.sh down     delete every instance, the floating IP and the security group made here
#
# Settings (environment): LSM_OS_PREFIX (lsm), LSM_OS_IMAGE (an Ubuntu 22.04 or 24.04 image name),
# LSM_OS_GPU_FLAVOR (g1-12gb-c3-35gb-125), LSM_OS_SERVER_FLAVOR (p2-3gb), LSM_OS_TENANT_FLAVOR
# (c16-60gb), LSM_OS_TENANTS (2), LSM_OS_NETWORK (the project's private network),
# LSM_OS_EXTERNAL (Public-Network), LSM_OS_KEY (~/.ssh/lsm-alliance; RSA, as the docs require).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
prefix=${LSM_OS_PREFIX:-lsm}
image=${LSM_OS_IMAGE:-Ubuntu-22.04-Jammy-x64}
gpu_flavor=${LSM_OS_GPU_FLAVOR:-g1-12gb-c3-35gb-125}
server_flavor=${LSM_OS_SERVER_FLAVOR:-p2-3gb}
tenant_flavor=${LSM_OS_TENANT_FLAVOR:-c16-60gb}
tenants=${LSM_OS_TENANTS:-2}
external=${LSM_OS_EXTERNAL:-Public-Network}
key=${LSM_OS_KEY:-$HOME/.ssh/lsm-alliance}
state="$here/.state"

network() {
  if [[ -n "${LSM_OS_NETWORK:-}" ]]; then echo "$LSM_OS_NETWORK"; return; fi
  openstack network list -f value -c Name | grep -v -i -E "public|external" | head -1
}

render() { # template output
  sed -e "s|__TOKEN__|$(cat "$state/token")|g" -e "s|__SERVER_IP__|$(cat "$state/server_ip" 2>/dev/null || true)|g" \
      -e "s|__PUBLIC_IP__|$(cat "$state/public_ip" 2>/dev/null || true)|g" "$1" > "$2"
}

server_create() { # name flavor user-data
  openstack server create --image "$image" --flavor "$2" --network "$(network)" --key-name "$prefix" \
    --security-group default --security-group "$prefix" --user-data "$3" --wait -f value -c id "$1" >/dev/null
  openstack server show "$1" -f json -c addresses | python3 -c '
import json, sys, re
addresses = json.load(sys.stdin)["addresses"]
text = json.dumps(addresses)
print(re.findall(r"192\.168\.\d+\.\d+|10\.\d+\.\d+\.\d+", text)[0])'
}

up() {
  mkdir -p "$state"
  [[ -f "$key" ]] || ssh-keygen -t rsa -b 4096 -N "" -f "$key" -C lsm-alliance >/dev/null
  [[ -f "$state/token" ]] || openssl rand -hex 24 > "$state/token"
  openstack keypair show "$prefix" >/dev/null 2>&1 || openstack keypair create --public-key "$key.pub" "$prefix" >/dev/null
  if ! openstack security group show "$prefix" >/dev/null 2>&1; then
    me=$(curl -fsS https://api.ipify.org)
    openstack security group create "$prefix" >/dev/null
    for port in 22 6443; do
      openstack security group rule create --protocol tcp --dst-port "$port" --remote-ip "$me/32" "$prefix" >/dev/null
    done
  fi
  # The public address must be known for the API certificate: allocate it first.
  if [[ ! -f "$state/public_ip" ]]; then
    openstack floating ip create "$external" -f value -c floating_ip_address > "$state/public_ip"
  fi
  render "$here/cloud-init-server.yaml" "$state/server.yaml"
  server_create "$prefix-server" "$server_flavor" "$state/server.yaml" > "$state/server_ip"
  openstack server add floating ip "$prefix-server" "$(cat "$state/public_ip")"
  render "$here/cloud-init-gpu.yaml" "$state/gpu.yaml"
  render "$here/cloud-init-tenant.yaml" "$state/tenant.yaml"
  server_create "$prefix-gpu" "$gpu_flavor" "$state/gpu.yaml" > "$state/gpu_ip"
  for index in $(seq 1 "$tenants"); do
    server_create "$prefix-tenant$index" "$tenant_flavor" "$state/tenant.yaml" > "$state/tenant${index}_ip"
  done
  echo "server $(cat "$state/public_ip") (private $(cat "$state/server_ip")); GPU node $(cat "$state/gpu_ip")"
  echo "The GPU node reboots once and then installs the driver and k3s (log: /var/log/lsm-gpu-node.log)."
  echo "Next: infra/alliance/up.sh kubeconfig; kubectl get nodes"
}

kubeconfig() {
  mkdir -p "$HOME/.kube"
  ssh -i "$key" -o StrictHostKeyChecking=accept-new "ubuntu@$(cat "$state/public_ip")" \
    'until [ -f /etc/rancher/k3s/k3s.yaml ]; do sleep 5; done; cat /etc/rancher/k3s/k3s.yaml' \
    | sed "s|127.0.0.1|$(cat "$state/public_ip")|" > "$HOME/.kube/lsm-alliance"
  echo "export KUBECONFIG=$HOME/.kube/lsm-alliance"
}

down() {
  for name in $(openstack server list -f value -c Name | grep "^$prefix-" || true); do
    openstack server delete --wait "$name" && echo "deleted $name"
  done
  if [[ -f "$state/public_ip" ]]; then
    openstack floating ip delete "$(cat "$state/public_ip")" && rm -f "$state/public_ip"
  fi
  openstack security group delete "$prefix" 2>/dev/null || true
  rm -f "$state"/*_ip
}

case "${1:-}" in
  up) up ;;
  kubeconfig) kubeconfig ;;
  down) down ;;
  *) sed -n '2,13p' "$0"; exit 2 ;;
esac

# GPU track on the Alliance cloud (Arbutus vGPU, k3s)

The Azure subscription cannot create GPU VMs, so the GPU version of the cloud experiments runs
on the Digital Research Alliance of Canada's Arbutus cloud. It uses the same image (`cuda-<commit>`),
manifests ([infra/k8s](../k8s)) and driver ([publication/cloud/cloud_campaign.py](../../publication/cloud/cloud_campaign.py))
as the Azure track ([docs/cloud.md](../../docs/cloud.md)).

## What Arbutus offers (checked 2026-11-15, docs.alliancecan.ca)

- **GPU:** vGPU slices of an H100 only.
  - Rapid Access (RAS) gets one flavor, `g1-12gb-c3-35gb-125`: a 12 GB MIG-backed H100 slice, 3 vCPUs, 35 GB RAM.
  - `g1-24gb-c6-70gb-250` needs a RAC award.
  - MIG cannot be repartitioned inside the guest, and MPS is not documented. Treat both as unavailable until tested.
  - Source: [Cloud RAS allocations](https://docs.alliancecan.ca/wiki/Cloud_RAS_Allocations), [Using cloud vGPUs](https://docs.alliancecan.ca/wiki/Using_cloud_vGPUs).
- **RAS compute limits:** 80 vCPUs, 300 GB RAM, 20 instances, 2 floating IPs, instances for up to 1 month.
- **Driver:** GRID 580.105.08 (CUDA 13.0) installed in the guest and licensed with a token. `cloud-init-gpu.yaml` follows the documented procedure.
- **Network:** a private project network with a router. Instances in the `default` security group reach each other on every port (k3s needs 6443/tcp, 8472/udp, 10250/tcp).

## 1. Allocation (the PI requests it)

1. The PI (supervisor) needs a CCDB account, then submits the cloud RAS form linked from [Cloud RAS allocations](https://docs.alliancecan.ca/wiki/Cloud_RAS_Allocations).
2. Request a **compute** allocation with 1 vGPU (`g1-12gb-c3-35gb-125`), about 40 vCPUs and 160 GB RAM, and 1 floating IP. Turnaround is about two business days.
3. The student is then added to the project.

Suggested justification:

> We evaluate a shared GPU compaction service for LSM-tree databases (RocksDB), deployed with
> Kubernetes. The experiment needs a GPU node and several CPU nodes on the same private network,
> with root access to install a container runtime and k3s. A batch HPC job cannot provide this:
> long-running services, a Kubernetes control plane, and node-level network isolation. Instances
> run only during scheduled experiment sessions (a few hours each) and are deleted afterwards.

## 2. Command-line access

New Arbutus authenticates through the browser (websso):

1. Download the project's RC file from Horizon (Project → API Access → OpenStack RC File).
2. Edit it as the [migration guide](https://docs.alliancecan.ca/wiki/Arbutus_Migration_Guide) says:
   - add `OS_AUTH_TYPE=v3websso`, `OS_IDENTITY_PROVIDER=atmosphere`, `OS_PROTOCOL=openid` and `OS_PROJECT_DOMAIN_NAME=default`;
   - remove `OS_USER_DOMAIN_NAME` and the password prompt.
3. Install the client and load the RC file:

```bash
python3 -m pip install python-openstackclient keystoneauth-websso
source <project>-openrc.sh
openstack flavor list; openstack image list; openstack network list
```

Check the exact flavor, image and network names, and set `LSM_OS_*` in `up.sh` if they differ
from the defaults.

## 3. Cluster

```bash
export LSM_OS_IMAGE=<Ubuntu 22.04 image> LSM_OS_TENANT_FLAVOR=<16-vCPU compute flavor> LSM_OS_TENANTS=2
infra/alliance/up.sh up          # server (floating IP), GPU node, tenant nodes
infra/alliance/up.sh kubeconfig  # then: export KUBECONFIG=~/.kube/lsm-alliance
kubectl get nodes -L lsm/role    # the GPU node joins after its reboot and driver install
sed -e s/__CONFIG__/exclusive/ -e s/__REPLICAS__/1/ infra/k8s/gpu/nvidia-device-plugin.yaml | kubectl apply -f -
kubectl get node -l lsm/role=service -o jsonpath='{.items[0].status.capacity.nvidia\.com/gpu}'   # expect 1
```

Check the GPU node first. Over ssh through the server (`ssh -J ubuntu@<public IP> ubuntu@<GPU node>`),
`nvidia-smi -q | grep "License Status"` should report Licensed. If the device plugin reports no
GPU on the MIG-backed vGPU, set `migStrategy: single` in the ConfigMap and re-apply.

The platform file for the driver uses the CUDA image and the GPU in the service pod:

```json
{"backend": "k8s", "namespace": "lsm", "image": "ghcr.io/saifaldin14/lsm-tree:cuda-<commit>",
 "backend_name": "cuda_merge", "gpu": true,
 "server_env": "LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024 LSM_MERGE_READ_THREADS=4",
 "render": {"DB_SLOTS": 6, "DB_CPUS": 4, "DB_MEMORY": "16Gi", "WORK_SIZE": "8Gi",
            "SVC_CPUS": 2, "SVC_MEMORY": "24Gi", "GPU_LIMIT": ", nvidia.com/gpu: 1"}}
```

`infra/alliance/up.sh down` deletes every instance, the floating IP and the security group it
made.

## 4. Experiments (registered once the GPU is confirmed)

Planned, to be registered in PREREGISTRATION.md with predictions before any run:

- **G0:** engines on the 12 GB slice (M0 rule).
- **G1:** Kubernetes GPU sharing against the service at N = 1, 2, 4. The separate-process arms get the GPU through time-slicing (device plugin `time-slicing`); the service arm uses `exclusive`.
- **G2:** capacity curve over cloud TCP.
- **G3:** isolation.

The 12 GB slice is far smaller than the lab GPUs (48 and 96 GB). The engines, and therefore
capacity, are set by G0 rather than reused.

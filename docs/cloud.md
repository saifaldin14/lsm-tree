# Cloud experiments: the compaction service on Kubernetes

Registered in [PREREGISTRATION.md](../publication/PREREGISTRATION.md), "Cloud: the compaction
service on Kubernetes". Two tracks:

- **Azure (CPU service):** AKS and plain VMs. The subscription cannot create GPU VMs, so the
  service runs `cpu_merge`, bounded and ordered by the job scheduler (`--engines`, `--scheduler`).
- **Alliance cloud (GPU service):** k3s on Arbutus with an NVIDIA vGPU
  ([infra/alliance/README.md](../infra/alliance/README.md)); registered once its GPU is known.

Both use the same image, manifests and driver.

## Pieces

| Piece | Path |
|---|---|
| Image (`cpu-<commit>`, `cuda-<commit>`) | [Dockerfile](../Dockerfile), built by [.github/workflows/image.yml](../.github/workflows/image.yml) into `ghcr.io/saifaldin14/lsm-tree` |
| AKS cluster | [infra/azure/aks.sh](../infra/azure/aks.sh) (`up`, `status`, `down`), [kubelet.json](../infra/azure/kubelet.json) (static CPU manager) |
| Plain VMs (C4) | [infra/azure/vms.sh](../infra/azure/vms.sh) (`up IMAGE`, `down`), writes `vms-hosts.json` for the driver |
| Cost guard | [infra/azure/guard.sh](../infra/azure/guard.sh): `budget`, `list`, `sweep`, `watch`; every group is tagged `purpose=lsm-cloud` and `expires` |
| Prices | [infra/azure/prices.py](../infra/azure/prices.py) writes [prices-azure.json](../publication/prices-azure.json) |
| Manifests | [infra/k8s](../infra/k8s): database slots (`tenants.yaml`), service pod (`service-pod.yaml`), service Deployment and Service for C3 (`compaction-service.yaml`) |
| Driver | [publication/cloud/cloud_campaign.py](../publication/cloud/cloud_campaign.py): `setup`, `preload`, `c1`–`c4`, over `kubectl exec`, ssh + `docker exec`, or locally |

Each database slot is a Guaranteed pod (whole vCPUs) with the preloaded database in `/golden`
and a memory volume `/work` that each trial copies it into. The driver starts
`lsm_compaction_server` in the service pod for every trial, as the lab driver does over ssh. The
same commands run on plain VMs in Docker containers pinned to the same vCPUs. In C3 the service
is a Deployment behind a Service. Scaling in sends SIGTERM: the server refuses new merges with
Busy and finishes the running ones (`--drain-seconds`). Clients retry refused or failed merges
on a new connection (`LSM_REMOTE_RETRIES`) and close connections after
`LSM_REMOTE_JOBS_PER_CONNECTION` jobs, so jobs reach replicas added later.

## Running the Azure track

```bash
az login                                    # once
infra/azure/guard.sh budget 60              # monthly budget alert
python3 infra/azure/prices.py               # archive prices
IMAGE=ghcr.io/saifaldin14/lsm-tree:cpu-<commit>
LSM_AZ_HOURS=6 infra/azure/aks.sh up        # about 10 minutes
infra/azure/guard.sh watch &                # deletes the group once its expiry passes
cat > /tmp/aks.json <<EOF
{"backend": "k8s", "namespace": "lsm", "image": "$IMAGE",
 "render": {"DB_SLOTS": 6, "DB_CPUS": 4, "DB_MEMORY": "16Gi", "WORK_SIZE": "8Gi",
            "SVC_CPUS": 14, "SVC_MEMORY": "32Gi"}}
EOF
D=publication/data/cloud/azure
python3 publication/cloud/cloud_campaign.py --platform /tmp/aks.json --experiment setup --out $D
python3 publication/cloud/cloud_campaign.py --platform /tmp/aks.json --experiment preload --out $D
for K in 4 7 14; do                          # C0
  python3 publication/cloud/cloud_campaign.py --platform /tmp/aks.json --experiment c1 --out $D \
    --counts 6 --native-counts "" --engines $K --repetitions 1 --results-name c0-k$K.jsonl
done
python3 publication/cloud/cloud_campaign.py --platform /tmp/aks.json --experiment c1 --out $D --engines K
python3 publication/cloud/cloud_campaign.py --platform /tmp/aks.json --experiment c2 --out $D \
  --engines 2 --policies fair,fifo --light-rate RATE
python3 publication/cloud/cloud_campaign.py --platform /tmp/aks.json --experiment c3 --out $D --counts 6 \
  --render '{"IMAGE": "'$IMAGE'", "GPU_LIMIT": "", "BACKEND": "cpu_merge", "ENGINES": K2, "POLICY": "fair",
             "DRAIN_SECONDS": 60, "GRACE_SECONDS": 90, "SVC_CPUS": 7, "SVC_MEMORY": "24Gi"}'
infra/azure/aks.sh down
infra/azure/vms.sh up $IMAGE 6              # C4 on plain VMs
python3 publication/cloud/cloud_campaign.py --platform infra/azure/vms-hosts.json --experiment preload --out $D
python3 publication/cloud/cloud_campaign.py --platform infra/azure/vms-hosts.json --experiment c4 --out $D \
  --counts 4 --native-counts 4 --engines K
infra/azure/vms.sh down
```

`K` is C0's choice, `K2` half of it (rounded up), and `RATE` follows the C2 rule. Run the driver
offline first with `{"backend": "local", "root": "/tmp/lsm-cloud", "bin": "build/dev"}`: same
commands, on this machine.

The user's other resource groups are never touched. `aks.sh down` and `vms.sh down` delete only
their own groups, and `guard.sh` acts only on groups tagged `purpose=lsm-cloud`.

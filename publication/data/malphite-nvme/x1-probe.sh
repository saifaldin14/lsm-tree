#!/bin/bash
cd ~/lsm-tree
U=$(whoami)
B=build/zyra-cuda/lsm_compaction_bench
export LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024 \
       LSM_MERGE_READ_THREADS=4 LSM_GPU_MERGE_STREAMS=2
CPUS=0,1,2,3,4,5,6,7
exec > /tmp/$U/mal/probe.log 2>&1
for where in nvme tmpfs; do
  if [ $where = nvme ]; then S=/tmp/$U/mal/scratch-nvme; else S=/dev/shm/$U/mal/scratch-tmpfs; fi
  rm -rf "$S"
  echo "== $where start $(date)"
  taskset -c $CPUS $B --dir "$S" --modes native_cpu,cuda_merge --l0-files 8 --block-bytes 4096 \
    --repetitions 4 --allow-fallocate false --subcompactions 1 \
    --records 4000000 --value-bytes 128 --compression lz4 --delete-fraction 0 \
    > /tmp/$U/mal/x1-v128-lz4-$where.jsonl
  echo "== $where exit=$? $(date)"
  rm -rf "$S"
done

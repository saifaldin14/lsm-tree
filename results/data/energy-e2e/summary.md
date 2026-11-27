# E6: energy per operation to the end of leftover compaction

Microjoules per operation, medians over blocks. Ratio = ours / RocksDB. (i) same machine, idle GPU counted for RocksDB; (ii) RocksDB without a GPU (primary).

## headline

| cell | blocks | ours µJ | RocksDB µJ (i) | RocksDB µJ (ii) | ratio (i) | ratio (ii) | ratio (ii) range |
|---|---|---|---|---|---|---|---|
| background_jobs=16, cores=16, rocksdb_cores=16, writes=batched | 5 | 200.06 | 194.08 | 172.07 | 1.029 | 1.162 | 1.155–1.176 |
| background_jobs=4, cores=4, rocksdb_cores=4, writes=batched | 5 | 459.35 | 505.09 | 445.52 | 0.908 | 1.029 | 1.026–1.055 |
| background_jobs=8, cores=8, rocksdb_cores=8, writes=batched | 5 | 306.35 | 288.50 | 255.56 | 1.059 | 1.202 | 1.197–1.206 |

## main

| cell | blocks | ours µJ | RocksDB µJ (i) | RocksDB µJ (ii) | ratio (i) | ratio (ii) | ratio (ii) range |
|---|---|---|---|---|---|---|---|
| background_jobs=2, cores=4, rocksdb_cores=4, writes=batched | 5 | 672.34 | 1651.34 | 1442.65 | 0.402 | 0.462 | 0.432–0.472 |
| background_jobs=2, cores=4, rocksdb_cores=4, writes=single | 5 | 1317.99 | 1842.87 | 1617.12 | 0.719 | 0.820 | 0.780–0.863 |
| background_jobs=4, cores=4, rocksdb_cores=4, writes=batched | 5 | 562.51 | 655.99 | 571.99 | 0.850 | 0.975 | 0.881–1.045 |
| background_jobs=4, cores=4, rocksdb_cores=4, writes=single | 5 | 1300.58 | 1294.21 | 1128.93 | 1.007 | 1.155 | 1.132–1.161 |

## equal-cost

| cell | blocks | ours µJ | RocksDB µJ (i) | RocksDB µJ (ii) | ratio (i) | ratio (ii) | ratio (ii) range |
|---|---|---|---|---|---|---|---|
| background_jobs=4, cores=8, rocksdb_cores=16, writes=batched | 5 | 377.45 | 253.54 | 224.55 | 1.504 | 1.696 | 1.629–1.767 |
| background_jobs=4, cores=8, rocksdb_cores=32, writes=batched | 5 | 377.45 | 251.04 | 221.41 | 1.521 | 1.710 | 1.671–1.748 |
| background_jobs=4, cores=8, rocksdb_cores=8, writes=batched | 5 | 377.45 | 346.12 | 305.30 | 1.090 | 1.236 | 1.213–1.263 |
| background_jobs=8, cores=8, rocksdb_cores=16, writes=batched | 5 | 373.35 | 254.82 | 225.93 | 1.463 | 1.652 | 1.634–1.668 |
| background_jobs=8, cores=8, rocksdb_cores=32, writes=batched | 5 | 373.35 | 246.15 | 218.74 | 1.512 | 1.704 | 1.688–1.733 |
| background_jobs=8, cores=8, rocksdb_cores=8, writes=batched | 5 | 373.35 | 359.16 | 315.16 | 1.043 | 1.188 | 1.173–1.201 |

## ycsb

| cell | blocks | ours µJ | RocksDB µJ (i) | RocksDB µJ (ii) | ratio (i) | ratio (ii) | ratio (ii) range |
|---|---|---|---|---|---|---|---|
| background_jobs=2, cores=4, distribution=uniform, rocksdb_cores=4, workload=A | 3 | 1091.47 | 1182.36 | 1035.92 | 0.923 | 1.054 | 0.999–1.065 |
| background_jobs=2, cores=4, distribution=uniform, rocksdb_cores=4, workload=B | 3 | 890.25 | 831.41 | 729.24 | 1.071 | 1.221 | 1.185–1.227 |
| background_jobs=2, cores=4, distribution=uniform, rocksdb_cores=4, workload=F | 3 | 1750.71 | 2163.89 | 1895.34 | 0.809 | 0.925 | 0.916–0.927 |
| background_jobs=2, cores=4, distribution=uniform, rocksdb_cores=4, workload=W | 3 | 1270.11 | 1894.19 | 1650.24 | 0.677 | 0.773 | 0.770–0.778 |
| background_jobs=2, cores=4, distribution=zipfian, rocksdb_cores=4, workload=A | 3 | 758.59 | 715.20 | 628.35 | 1.058 | 1.205 | 1.187–1.211 |
| background_jobs=2, cores=4, distribution=zipfian, rocksdb_cores=4, workload=B | 3 | 484.40 | 452.02 | 397.83 | 1.065 | 1.210 | 1.199–1.240 |
| background_jobs=2, cores=4, distribution=zipfian, rocksdb_cores=4, workload=F | 3 | 1262.00 | 1268.78 | 1109.80 | 1.014 | 1.153 | 1.122–1.187 |
| background_jobs=2, cores=4, distribution=zipfian, rocksdb_cores=4, workload=W | 3 | 1087.09 | 1048.33 | 911.66 | 1.030 | 1.184 | 1.137–1.196 |

## contention

| cell | blocks | ours µJ | RocksDB µJ (i) | RocksDB µJ (ii) | ratio (i) | ratio (ii) | ratio (ii) range |
|---|---|---|---|---|---|---|---|
| background_jobs=2, burn_percent=0, cores=2, rocksdb_cores=2, writes=batched | 3 | 931.90 | 1788.30 | 1558.25 | 0.521 | 0.597 | 0.592–0.609 |
| background_jobs=4, burn_percent=0, cores=16, rocksdb_cores=16, writes=batched | 3 | 378.13 | 588.13 | 517.27 | 0.648 | 0.737 | 0.713–0.763 |
| background_jobs=4, burn_percent=0, cores=8, rocksdb_cores=8, writes=batched | 3 | 367.89 | 582.76 | 509.80 | 0.635 | 0.723 | 0.695–0.726 |
| background_jobs=4, burn_percent=40, cores=4, rocksdb_cores=4, writes=batched | 3 | 776.24 | 1150.11 | 1005.74 | 0.677 | 0.770 | 0.764–0.819 |
| background_jobs=4, burn_percent=80, cores=4, rocksdb_cores=4, writes=batched | 3 | 756.41 | 1297.18 | 1136.83 | 0.583 | 0.665 | 0.642–0.720 |

## prior-lz4

| cell | blocks | ours µJ | RocksDB µJ (i) | RocksDB µJ (ii) | ratio (i) | ratio (ii) | ratio (ii) range |
|---|---|---|---|---|---|---|---|
| background_jobs=2, cores=4, distribution=uniform, memory=preferred, rocksdb_cores=4, workload=A | 5 | 1097.19 | 1139.22 | 1000.12 | 0.963 | 1.101 | 1.008–1.111 |
| background_jobs=2, cores=4, memory=preferred, rocksdb_cores=4, writes=batched | 5 | 654.07 | 1573.79 | 1374.59 | 0.417 | 0.475 | 0.470–0.482 |
| background_jobs=2, cores=4, memory=preferred, rocksdb_cores=4, writes=single | 5 | 1314.33 | 1734.44 | 1513.41 | 0.758 | 0.868 | 0.854–0.886 |
| background_jobs=4, cores=4, memory=preferred, rocksdb_cores=4, writes=batched | 5 | 555.21 | 640.24 | 560.22 | 0.870 | 0.997 | 0.946–1.024 |
| background_jobs=4, cores=4, memory=preferred, rocksdb_cores=4, writes=single | 5 | 1305.43 | 1274.85 | 1113.05 | 1.025 | 1.174 | 1.123–1.197 |

## prior-zstd

| cell | blocks | ours µJ | RocksDB µJ (i) | RocksDB µJ (ii) | ratio (i) | ratio (ii) | ratio (ii) range |
|---|---|---|---|---|---|---|---|
| background_jobs=2, cores=4, memory=preferred, rocksdb_cores=4, writes=batched | 3 | 776.00 | 2423.53 | 2116.45 | 0.320 | 0.368 | 0.343–0.373 |
| background_jobs=2, cores=4, memory=preferred, rocksdb_cores=4, writes=single | 3 | 1379.57 | 2835.41 | 2474.26 | 0.487 | 0.558 | 0.542–0.586 |
| background_jobs=4, cores=4, memory=preferred, rocksdb_cores=4, writes=batched | 3 | 736.60 | 1064.56 | 928.36 | 0.691 | 0.792 | 0.761–0.794 |
| background_jobs=4, cores=4, memory=preferred, rocksdb_cores=4, writes=single | 3 | 1407.35 | 1715.47 | 1496.76 | 0.820 | 0.936 | 0.902–0.940 |

## prior-none

| cell | blocks | ours µJ | RocksDB µJ (i) | RocksDB µJ (ii) | ratio (i) | ratio (ii) | ratio (ii) range |
|---|---|---|---|---|---|---|---|
| background_jobs=2, cores=4, memory=preferred, rocksdb_cores=4, writes=batched | 3 | 937.96 | 1214.94 | 1059.38 | 0.772 | 0.885 | 0.866–0.896 |
| background_jobs=2, cores=4, memory=preferred, rocksdb_cores=4, writes=single | 3 | 1486.50 | 1396.31 | 1225.24 | 1.065 | 1.213 | 1.195–1.234 |
| background_jobs=4, cores=4, memory=preferred, rocksdb_cores=4, writes=batched | 3 | 620.33 | 527.64 | 463.79 | 1.162 | 1.322 | 1.290–1.404 |
| background_jobs=4, cores=4, memory=preferred, rocksdb_cores=4, writes=single | 3 | 1405.93 | 1194.48 | 1041.26 | 1.193 | 1.370 | 1.350–1.380 |

## prior-equal-cost

| cell | blocks | ours µJ | RocksDB µJ (i) | RocksDB µJ (ii) | ratio (i) | ratio (ii) | ratio (ii) range |
|---|---|---|---|---|---|---|---|
| background_jobs=4, cores=8, memory=preferred, rocksdb_cores=32, writes=batched | 5 | 370.19 | 236.37 | 208.80 | 1.561 | 1.768 | 1.712–1.801 |
| background_jobs=4, cores=8, memory=preferred, rocksdb_cores=8, writes=batched | 5 | 370.19 | 338.31 | 299.48 | 1.093 | 1.239 | 1.219–1.295 |
| background_jobs=8, cores=8, memory=preferred, rocksdb_cores=32, writes=batched | 5 | 372.80 | 232.88 | 206.77 | 1.599 | 1.801 | 1.742–1.819 |
| background_jobs=8, cores=8, memory=preferred, rocksdb_cores=8, writes=batched | 5 | 372.80 | 349.90 | 307.01 | 1.064 | 1.213 | 1.201–1.271 |

## Hypotheses

- Y-E6a (ii): ours <= RocksDB at 4, 8, 16 cores: not supported
- Y-E6b (i): ours < RocksDB at 4, 8, 16 cores: not supported

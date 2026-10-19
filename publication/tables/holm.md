| Metric | Campaign | Primary | Wilcoxon p | Holm-adjusted p |
|---|---|:-:|---:|---:|
| Median compaction-job duration | stress | ✓ | <0.001 | 0.007 |
| Mean compaction-job duration | stress |  | <0.001 | 0.007 |
| Compaction wall time per GiB | stress |  | <0.001 | 0.007 |
| Compaction-thread CPU per GiB | stress | ✓ | <0.001 | 0.007 |
| Slowest compaction job | stress |  | 0.042 | 0.297 |
| Whole-process CPU (lsm_db lifetime) | stress |  | <0.001 | 0.010 |
| Whole-process CPU (measurement window) | stress | ✓ | 0.007 | 0.062 |
| Write p50 | stress |  | 0.339 | 1.000 |
| Write p99 | stress | ✓ | 0.266 | 1.000 |
| Median compaction-job duration | mixed | ✓ | <0.001 | <0.001 |
| Mean compaction-job duration | mixed |  | <0.001 | <0.001 |
| Compaction wall time per GiB | mixed |  | <0.001 | <0.001 |
| Compaction-thread CPU per GiB | mixed | ✓ | <0.001 | <0.001 |
| Slowest compaction job | mixed |  | 0.126 | 0.753 |
| Whole-process CPU (lsm_db lifetime) | mixed |  | <0.001 | <0.001 |
| Whole-process CPU (measurement window) | mixed | ✓ | <0.001 | 0.002 |
| Write p50 | mixed |  | 0.348 | 1.000 |
| Write p99 | mixed | ✓ | 0.027 | 0.213 |
| Read p50 | mixed |  | 0.454 | 1.000 |
| Read p99 | mixed | ✓ | 0.249 | 1.000 |

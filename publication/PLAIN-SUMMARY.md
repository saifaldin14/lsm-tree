# Does a GPU help RocksDB's background merging?

## What we tested

RocksDB is a database that keeps its data in files. As new data arrives, it
periodically merges smaller files into larger ones. During each merge it
compresses the data and calculates checksums, which are later used to detect
corruption. This merging runs in the background on the main processor (CPU).

We built a version of RocksDB that sends the compression and checksum work to
a graphics card (GPU) instead, and compared it with the standard version.

## How we tested it

All tests ran on a single server with four CPUs, one NVIDIA RTX A6000 GPU, and
an SSD. A program sent read and write requests to the database at a fixed rate,
and we ran every test twice with exactly the same requests: once with the
standard version and once with the GPU version. Most tests were repeated three
to six times in random order and lasted 10 seconds each. In total we ran 392
tests, and after every one we checked that all stored data could be read back
correctly.

For each test we recorded:

- how long each merge took
- how much CPU time the database used
- how much electricity the CPUs, memory, and GPU used
- how long read and write requests took, both typically and for the slowest
  1 in 100
- the highest request rate the database could keep up with

## Scenarios

We tested five mixes of reads and writes: writes only, 90% writes, 75% writes,
50% writes, and 10% writes. Each mix was run at three request rates, ranging
from 3,000 to 20,000 requests per second. We also increased the load on the
read-heavy mix up to 50,000 requests per second to find the point where the
database could no longer keep up.

We then tried several variations:

- letting the program sleep while it waits for the GPU, instead of checking
  repeatedly
- running the database on the CPU physically closest to the GPU
- giving the standard version a second merge worker, to see whether more CPU
  effort could match the GPU
- having the GPU also decompress the files being read, not only compress the
  files being written
- a larger database of 2 million records (about 2 GB), tested for one minute
  instead of 10 seconds

## Results

The GPU made merges faster. A typical merge took about 52 ms instead of 66 ms,
which is 16–20% faster. This held, on average, for every mix and request rate
we tried, and also for the larger database (17.5% faster for the same amount
of data).

That speed did not reduce costs. The GPU version used 2–3% more CPU time, not
less, because preparing data for the GPU and waiting for it to finish take CPU
time of their own. Electricity use was 17–27% higher, because CPU power stayed
the same while the GPU added its own power draw. Request response times did not
change in any meaningful way, and both versions reached their limit at the same
point: both kept up at 20,000 requests per second and both fell behind at
25,000.

The variations showed the following:

- Letting the program sleep while waiting for the GPU cut the GPU version's CPU
  time by 2–4%.
- Running the database on the CPU closest to the GPU cut CPU time by 8–16% for
  both versions. This saved more than the GPU ever did.
- With both changes together, the GPU version used about the same CPU time as
  the standard version.
- A second merge worker did not speed up the standard version, so the GPU stayed
  ahead on merge time.
- Having the GPU also decompress incoming files made merges 35–45% slower than
  the standard version.
- On the larger database, response times were long for both versions, and the
  GPU did not improve them.

No data errors occurred in any of the 392 tests.

## Write-heavy workloads

The three mixes where most requests are writes (100%, 90%, and 75% writes)
produce the most merging, so they are where the GPU had the most effect.

| Writes | Requests per second | Time per merge | Share of time spent merging | Data merged per second |
|---:|---:|---|---|---|
| 100% | 3,000 | 65 → 51 ms | 21% → 17% | 137 → 166 MB/s |
| 100% | 4,000 | 65 → 51 ms | 28% → 22% | 137 → 168 MB/s |
| 100% | 5,000 | 66 → 52 ms | 36% → 28% | 135 → 168 MB/s |
| 90% | 3,334 | 65 → 52 ms | 20% → 16% | 137 → 165 MB/s |
| 90% | 4,445 | 66 → 53 ms | 29% → 23% | 134 → 165 MB/s |
| 90% | 5,556 | 67 → 56 ms | 35% → 30% | 134 → 157 MB/s |
| 75% | 4,000 | 65 → 67 ms | 21% → 21% | 137 → 140 MB/s |
| 75% | 5,334 | 66 → 52 ms | 28% → 23% | 136 → 163 MB/s |
| 75% | 6,667 | 66 → 56 ms | 35% → 30% | 135 → 157 MB/s |

*Each cell shows the standard version first, then the GPU version.*

In eight of these nine cases, merges were 15–22% faster with the GPU and
16–25% more data was merged per second. At the busiest rates, the share of
time the database spent merging dropped from about 35% to about 29%. With
writes only at 5,000 requests per second, and with both the sleep setting and
the closer CPU in use, the GPU version used 2.7% less CPU time than the
standard version in all six repeats. This was the only case where the GPU
saved CPU time.

Users did not see a benefit. A typical write took 1.3–1.9 ms with either
version at most rates. With writes only at 5,000 requests per second, it took
3.3 ms with the standard version and 4.4 ms with the GPU. The slowest 1 in 100
writes showed no consistent difference. Electricity use was 17–27% higher with
the GPU, and with default settings CPU time was 1–7% higher.

## Conclusion

Moving compression and checksums to the GPU makes RocksDB's background merging
faster, but it does not lower CPU use or electricity, and it does not make
requests faster for users.

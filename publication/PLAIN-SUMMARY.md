# Does a GPU help RocksDB's background merging?

## What we tested

RocksDB is a database that keeps its data in files. As new data arrives, it
regularly merges smaller files into larger ones in the background. This is
called compaction. A merge reads the files, sorts their contents, throws away
old copies of data, and writes new compressed files with checksums. All of
this normally runs on the main processor (CPU).

We moved this work to a graphics card (GPU) in three steps and compared each
step with standard RocksDB:

1. **Compression only:** the GPU compresses and checksums the new files.
2. **Merging:** the GPU also reads, sorts and cleans up the old files.
3. **Merging and building:** the GPU also lays out the new files. The CPU
   only writes them to disk and keeps the database's bookkeeping.

## How we tested it

All tests ran on one server with an NVIDIA RTX A6000 GPU, with the data held
in memory so that disk speed did not matter. After every test we checked
that every piece of stored data was exactly the same as standard RocksDB
would have produced.

We ran two kinds of test:

- **One merge at a time:** we timed a single large merge and measured how
  much CPU time it used. We tried small (32-byte), medium (128-byte) and large
  (1 KB) data items, with and without compression.
- **The whole database:** four programs wrote 128-byte items into a
  50-million-item database as fast as they could, for two and a half minutes,
  on 4 CPU cores. They wrote either one item at a time or 100 items at a time.
  When writing one at a time, the database also kept its safety log, a
  file where every new item is noted first so it can be recovered after a
  crash. When writing 100 at a time, the log was turned off, as is common
  when loading data in bulk. That makes writing cheaper, so the background
  merging becomes most of the work.
  We counted the time until the database had finished all the merging the
  writes caused, not just the writing time.

## Results

**One merge at a time.** With all three steps on the GPU, a merge was
**2.3 to 6 times faster** and used **3 to 7 times less CPU time**. The
smaller the data items, the bigger the gain. Compression alone on the GPU
helped very little (up to 1.2 times faster).

**The whole database, 100 items at a time.** Standard RocksDB spent most of
its CPU on merging, could not keep up, and made writers wait.

| | Standard RocksDB | GPU (all three steps) |
|---|---|---|
| Writes per second | 349,000 | **602,000** (1.7 times more) |
| Slowest 1 in 100 writes | 7.1 ms | **1.1 ms** |
| Merging still left to do after the test | 68 seconds | **3 seconds** |

**The whole database, one item at a time.** The GPU version handled **1.9
times more writes per second** (232,000 vs 121,000).

**A measuring pitfall.** Standard RocksDB can look fast in short tests
because it puts off merging until later. During the test itself the gap
looked much smaller. Once the merging it had put off
was counted, it was about half as fast. The GPU version keeps up as it goes.

## What is not settled yet

- The whole-database tests were run once each; they need repeating.
- Whether the GPU version saves electricity is not yet known. The GPU draws
  extra power, and we have not yet measured through to the end of the merging
  standard RocksDB puts off.
- The tests used memory instead of a disk, synthetic data and write-only
  workloads.

## Conclusion

Moving only the compression to the GPU is not worth it. Moving the whole
merge is: merges become several times faster and cheaper in CPU, and when
merging is what holds the database back, the database handles 1.7 to 1.9
times more writes with much shorter waits.

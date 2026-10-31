# Engineering report

Measured on 2026-10-28. This is an educational, single-instrument engine with explicit correctness and persistence boundaries. The measurements below are observations of this implementation, not exchange-grade latency or durability claims.

## Verification

All **12 doctest cases and 4,555,789 assertions** passed in Debug, Release, AddressSanitizer + UndefinedBehaviorSanitizer, and ThreadSanitizer builds. The same suite includes 12,000 seeded reference-engine commands, replay of all four randomized histories, 2,400 concurrent commands with a snapshot observer, and exhaustive byte truncation of a small two-record file. Both in-memory and file-backed CLI replay demonstrations passed. Passing sanitizers and finite randomized tests provide evidence, not a proof of correctness.

A final review added two boundary protections: signed-negative/floating-point construction checks for domain values, and locale-independent canonical event formatting/parsing. All four build configurations were verified after these changes.

## Reproduction and environment

- Apple M4 Pro, 14 logical CPUs, 24 GiB memory; ARM64.
- macOS 15.4.1 (24E263), Darwin 24.4.0.
- Apple Clang 17.0.0 (`clang-1700.0.13.5`), CMake 4.4.4, default platform C++ standard library.
- Release core flags: `-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Applications use `-O3 -DNDEBUG -std=c++20`. No LTO, custom allocator, CPU affinity, or frequency pinning.
- Command: `./build/release/lob_bench 5000 5 > docs/benchmark-results.csv`.
- Raw data: [benchmark-results.csv](benchmark-results.csv). Recompute grouped summaries using `python3 scripts/summarize_bench.py docs/benchmark-results.csv` from the repository root.
- Five measured repeats per scenario/mode after one full unreported warm-up per scenario/mode. Seed 424242; fresh engine and file for every run. Measured mode order rotates each repeat. The final measurements were run after build/test processes finished; unrelated OS/background activity was not controlled.
- 3 scenarios × 8 configurations × 5 repeats = 120 measured rows, plus 24 warm-up runs.

## Workloads

Each repeat contains 5,000 cycles. IDs are unique per cycle; the seeded generator varies a base price from 100 through 106 ticks. Commands and timing arrays are prepared outside the timed interval.

| Scenario | Commands per cycle | Operations per repeat |
|---|---|---:|
| resting | Bid at base−10 and ask at base+10, quantity 10 each; cancel both | 20,000 |
| crossing | Sell 10 at base, then buy 10 at base | 10,000 |
| partial_cancel | Bid 10 at base−10; ask 10 at base+10; buy 4 then buy 8 at the ask; sell 2 at that price; cancel bid and original ask ID | 35,000 |

The mixed cycle produces partial maker fills, an incoming remainder, a subsequent fill, a successful cancellation, and an unsuccessful cancellation in serial execution. Under contention, producers may match each other's orders, changing fill/cancellation counts and final depth. The CSV retains those observed counts. Each producer owns a contiguous partition of complete cycles and preserves its own command order; the global interleaving is intentionally unspecified. These are shallow-book workloads, not a deep-book stress test.

## Measurement boundaries

`core_no_log` calls the unlocked core with history retention disabled. It **still generates and applies event batches**, allocates results, and tracks reserved IDs. This is the matching-core baseline, not an event-free or allocation-free loop. `core_log` adds retained in-memory history.

`mutex_no_log` measures the same command stream through the mutex facade with 1, 2, or 4 producers. `mutex_log` uses 4 producers and retained history. Thread construction is excluded; barrier release, scheduling, joins, latency sampling, and loop bookkeeping are included in total elapsed time. Each latency includes time waiting for the mutex. Threads are created fresh for each run.

`file_buffered` adds canonical record encoding, checksumming, and file append to `core_log`; it includes a final stream flush in total elapsed time. Individual latency samples exclude that final flush. `file_flush` additionally flushes the stream after every command, inside its latency sample. Both use temporary files under the OS temporary directory. Files are validated/replayed after timing and removed. Neither mode invokes fsync or bypasses the OS cache; these numbers must not be called durable-disk performance.

Throughput is total commands divided by wall elapsed seconds. All commands, including unsuccessful cancellations, count as operations. Latency uses two `steady_clock` readings around command dispatch, result/counter handling, and any per-command file work. Result creation/destruction is included. Timer overhead is not subtracted. The sample-array write and outer loop overhead are included in wall time but outside the per-command latency interval. Sorting, snapshot validation, history/file replay, file removal, warm-up, and workload generation are excluded. Per-run p50/p95/p99 use nearest-rank percentiles over all command samples.

This is a **closed-loop** workload: producers submit their next command only when the preceding command returns. It does not measure open-loop queueing delay, coordinated-omission-corrected latency, client/network latency, or service capacity at a specified arrival rate. Repeated runs on a laptop do not establish statistical confidence or production tail bounds.

## Actual results

The table reports the median of five per-run throughputs and the median of five per-run percentiles; it does not pool latency distributions. Min–max throughput exposes run variation. Values are rounded for display; the CSV preserves the recorded precision.

| Scenario | Mode | Threads | Runs | Median ops/s | Min–max ops/s | p50 ns | p95 ns | p99 ns |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| crossing | core_log | 1 | 5 | 767,889 | 658,257–982,198 | 958 | 2,000 | 2,500 |
| crossing | core_no_log | 1 | 5 | 887,938 | 838,443–1,038,166 | 792 | 1,875 | 2,208 |
| crossing | file_buffered | 1 | 5 | 201,180 | 174,000–217,728 | 4,333 | 7,917 | 28,416 |
| crossing | file_flush | 1 | 5 | 84,675 | 74,183–98,648 | 10,625 | 19,083 | 35,708 |
| crossing | mutex_log | 4 | 5 | 267,485 | 239,853–324,970 | 3,958 | 56,959 | 245,667 |
| crossing | mutex_no_log | 1 | 5 | 785,886 | 741,079–1,152,738 | 875 | 1,917 | 2,250 |
| crossing | mutex_no_log | 2 | 5 | 612,044 | 431,062–799,390 | 1,125 | 4,750 | 55,875 |
| crossing | mutex_no_log | 4 | 5 | 362,628 | 262,576–396,338 | 2,584 | 42,917 | 187,958 |
| partial_cancel | core_log | 1 | 5 | 1,053,112 | 950,790–1,119,586 | 708 | 1,792 | 2,333 |
| partial_cancel | core_no_log | 1 | 5 | 932,557 | 893,224–995,845 | 833 | 2,125 | 2,334 |
| partial_cancel | file_buffered | 1 | 5 | 213,955 | 180,084–268,115 | 3,541 | 7,833 | 24,375 |
| partial_cancel | file_flush | 1 | 5 | 79,727 | 78,861–91,775 | 11,750 | 19,791 | 39,167 |
| partial_cancel | mutex_log | 4 | 5 | 354,621 | 328,165–375,676 | 3,125 | 51,208 | 126,541 |
| partial_cancel | mutex_no_log | 1 | 5 | 997,276 | 942,945–1,191,865 | 792 | 2,083 | 2,250 |
| partial_cancel | mutex_no_log | 2 | 5 | 548,428 | 538,377–827,421 | 1,000 | 5,667 | 62,333 |
| partial_cancel | mutex_no_log | 4 | 5 | 328,864 | 305,344–383,889 | 3,750 | 55,958 | 124,791 |
| resting | core_log | 1 | 5 | 1,721,442 | 1,360,633–1,807,842 | 417 | 875 | 1,000 |
| resting | core_no_log | 1 | 5 | 2,151,222 | 1,720,769–2,536,448 | 375 | 791 | 875 |
| resting | file_buffered | 1 | 5 | 321,226 | 283,098–337,855 | 2,417 | 4,625 | 21,000 |
| resting | file_flush | 1 | 5 | 108,214 | 93,070–124,586 | 7,375 | 14,542 | 31,041 |
| resting | mutex_log | 4 | 5 | 440,757 | 380,008–714,232 | 2,292 | 40,333 | 115,500 |
| resting | mutex_no_log | 1 | 5 | 1,601,970 | 1,515,314–2,380,150 | 500 | 875 | 1,000 |
| resting | mutex_no_log | 2 | 5 | 1,394,404 | 982,894–1,688,660 | 417 | 2,375 | 22,125 |
| resting | mutex_no_log | 4 | 5 | 610,431 | 401,450–676,678 | 2,084 | 29,667 | 97,167 |

## Interpretation and next experiments

On `partial_cancel`, the no-retained-log core measured a median 932,557 commands/s and 2,334 ns p99. Four producers through the no-retained-log mutex facade measured 328,864 commands/s and 124,791 ns p99. Increasing producer count did not create parallel matching capacity; synchronization/scheduling and changed cross-producer interactions dominate this comparison. These figures measure this complete harness, not a separately isolated mutex cost.

For that same scenario, buffered persistence measured 213,955 commands/s and per-command stream flushing 79,727 commands/s. This supports investigating formatting, buffering, and flush policy before optimizing a few instructions in matching. Neither rate measures crash-safe persistence.

Some in-memory logging configurations measured faster than corresponding no-log configurations (notably the mixed workload). Run ranges overlap and short-run allocator, cache, scheduler, thermal, and workload-interleaving effects are uncontrolled. The data do **not** establish that logging improves performance or isolate its causal cost. Longer runs, randomized paired comparisons, hardware counters, and controlled scheduling would make attribution stronger. The fastest observed run must not be substituted for the median as a representative claim.

Prioritized optimization opportunities, not implemented performance claims:

1. **Profile allocations and locality.** Lists, ordered indices, temporary events/results, and copied history all allocate. Measure allocation counts and CPU samples first; compare a pool/arena, reserved hash index, and reusable batch buffers while retaining oracle/replay tests. Reserved-ID memory growth also needs a product-level retention policy.
2. **Test a single-consumer queue against the mutex baseline.** It may reduce producer contention and allow batching, but adds enqueue and scheduling latency. Include end-to-end enqueue-to-result time and overload behavior. The current design is mutex-protected, not lock-free.
3. **Separate persistence service time from durability policy.** Binary encoding/batched writes may reduce formatting overhead. A genuine durable WAL needs fsync/group-commit semantics, acknowledgement ordering, and subprocess kill/fault-injection tests before making any crash-safety claim.
4. **Broaden measurements before tuning the book.** Add deep books, long-lived orders, bursty cancellation, realistic tick distributions, long runs, and an open-loop arrival generator. Measure snapshots and recovery separately; current benchmarks do not time those operations. Use a dedicated host or document affinity/frequency controls where practical.

The implementation intentionally retains the simpler data structures until profiling identifies a bottleneck. Every proposed optimization should preserve deterministic results against the independent reference engine and replay before its speed is reported.

# Event-sourced limit order book · C++20

A single-instrument, integer-tick matching engine built to study correctness, deterministic replay, and the cost of synchronization. Supports good-till-cancelled limit orders, price-time priority, partial fills, submission, and cancellation. It has no networking, exchange connectivity, real trading, or frontend.

## Build and run

Requires a C++20 compiler, CMake 3.20+, and platform threads. Tests use a pinned, vendored doctest 2.4.11 header; no downloads are needed. Presets require CMake 3.21+ and Make; use the explicit commands below with CMake 3.20 or another generator.

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
./build/release/lob_demo
./build/release/lob_demo /tmp/new-demo.events  # refuses an existing file
./build/release/lob_bench 5000 5 > benchmark-results.csv
```

Or run `cmake --preset debug`, `cmake --build --preset debug`, and `ctest --preset debug`. Other presets: `release`, `asan`, `tsan`. Sanitizers must use separate build directories and a supported Clang/GCC runtime:

```sh
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DLOB_ASAN_UBSAN=ON
cmake --build build/asan -j
ctest --test-dir build/asan --output-on-failure
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DLOB_TSAN=ON
cmake --build build/tsan -j
ctest --test-dir build/tsan --output-on-failure
```

Debug, Release, ASan/UBSan, and TSan were actually run on the recorded machine; see [the engineering report](docs/engineering-report.md). These commands do not guarantee sanitizer availability on every OS/toolchain.

## Architecture

```text
include/lob/domain.hpp       Strong values, incoming intent, resting state, results
include/lob/engine.hpp       Book, command planning, event model, replay API
include/lob/persistence.hpp  Versioned file records and prefix recovery
include/lob/concurrent.hpp   Mutex facade, copied snapshots/history
src/engine.cpp              Plan commands, apply events, validate replay
src/persistence.cpp         Canonical encoding, checksum, checked file I/O
apps/demo.cpp               Fills, snapshots, cancellation, replay
apps/benchmark.cpp          Seeded workloads, latency and throughput CSV
tests/reference.hpp        Independent flat-vector oracle
tests/tests.cpp            doctest examples, randomized and concurrent checks
docs/                      Results, measurement method, design rationale
```

The core has no locks. A command first plans an immutable batch of events against the current book; it then records that batch and applies it. Replay validates batches and invokes the event application path directly. It does not resubmit commands or generate a second event history. The mutex facade encloses the complete command, sequence assignment, event recording, and event application.

### 1. Domain guarantees

`Price`, `Quantity`, and `OrderId` are different `Positive<Tag>` types with explicit integral constructors. Mixing them, implicitly converting integers, or constructing from floating point fails at compile time. Zero and signed negative inputs throw `invalid_argument` at runtime. Prices are unsigned 64-bit tick counts; there is no floating-point price representation or tick conversion. IDs must also be positive.

`Side` is a scoped enum, checked at construction and submission because C++ permits explicit casts to invalid enum values. `Remaining` deliberately permits zero, unlike an incoming `Quantity`; subtraction rejects overfills. Sequence increments check overflow before changing state. Matching uses comparison, minimum, and checked subtraction, avoiding additions and price × quantity multiplication. There is no unchecked notional calculation. FNV checksum arithmetic intentionally wraps modulo 2^64.

### 2. Order lifecycle

`IncomingOrder` expresses intent: ID, side, limit, quantity. `RestingOrder` tracks original quantity, remaining quantity, and acceptance sequence. Snapshots return value copies. The active index contains only live orders; a separate set permanently reserves every accepted ID. Reuse is rejected after full fills or cancellation as well as while resting. Rejected duplicate payloads never enter the book; the result's remaining quantity is zero.

Cancellation removes the full unfilled remainder. Unknown, fully filled, and already cancelled IDs all return `UnknownOrder`; these are expected outcomes, not exceptions. Rejected submissions and unsuccessful cancellations still advance sequence and are logged. Invalid domain values throw before submission and do not advance sequence.

### 3. Book and cancellation

Each side is `std::map<Price, std::list<RestingOrder>>`. Bids use the map's reverse order, asks its forward order. Lists act as FIFO queues: append on acceptance and match at the front. An ordered ID index stores side, price, and a stable list iterator. It makes cancellation deterministic in worst-case complexity without a queue scan. Lists cost allocations and pointer chasing; a `deque` has better locality but lacks these stable arbitrary-removal semantics. The core is deliberately non-copyable/non-movable to prevent copying an index whose iterators refer to another book.

Let L be live price levels, N live orders, U all accepted IDs, F individual fills, and H logged events:

| Operation | Complexity |
|---|---|
| Best bid/ask | O(1), access map endpoints |
| Non-crossing submission | O(log U + log L + log N) |
| Matching F orders | O(log U + log L + log N + F(log N + log L)); conservative bound including maker removal |
| Cancellation | O(log N + log L), list erase O(1) |
| Book/metadata snapshot | O(N + U), detached value copies |
| Facade history snapshot | O(H), copies under lock |
| Storage | O(N + L + U + H); omit H when history retention is disabled |

Appending history is amortized, with occasional vector reallocations. Returned fills cost O(F). No aggregate level quantities are maintained, so book snapshots enumerate individual orders.

### 4. Matching rules

Buys consume the lowest asks whose price is at or below the buy limit. Sells consume the highest bids whose price is at or above the sell limit. Within a level, older orders execute first. Each fill uses the maker/resting price. Exhausted orders and empty levels are removed; an incoming remainder rests at its own limit with its acceptance sequence.

An `Accepted` event temporarily inserts the taker into the book before its planned `Traded` events reduce both sides. This intermediate state is private; snapshots and subsequent commands see only a complete batch. The facade never releases the mutex during a batch. A valid completed command cannot leave a crossed book or zero-quantity resting order.

Example: asks of 10 at 101 and 5 at 102 meet a buy of 12 at 102. Fills are 10 at 101 and 2 at 102, leaving 3 at 102. The CLI then rests a bid at 99, cancels the remaining ask, and verifies replay equality.

### 5. Correctness checks

The suite checks exact/partial/multilevel fills in both directions, price and FIFO priority, non-crossing/empty books, middle-of-queue cancellation, duplicate/completed IDs, detached snapshots, integer boundaries, and invalid values. Four fixed seeds drive 3,000 commands each against an independent flat-vector reference implementation. Every step compares results and complete snapshots and checks:

- accepted quantity = twice traded quantity + cancelled remainder + live remainder;
- submission quantity = fills + unfilled remainder for accepted orders;
- all resting quantities are positive and no greater than original quantities;
- best bid is strictly below best ask when both exist.

The reference engine scans all live orders to find each best maker; it shares value/result types but no production indexing, event generation, or matching code.

### 6. Event sourcing and file contract

Version 1 events are `Accepted` (full incoming intent), `Traded` (maker/taker IDs, price, quantity), `Cancelled` (ID), and `Rejected` (ID and duplicate/unknown reason). A batch carries a version and monotonically increasing command sequence. Within-batch event position orders fills; sequence is per command, not per fill. Rejected payload details beyond ID/reason are not retained. Original quantities, remainders, FIFO order, reserved IDs, and the next command sequence can all be reconstructed.

`replay()` requires an empty engine. It validates the whole stream in a scratch engine, checking contiguous sequences, versions, rejection legitimacy, exact prices/quantities, FIFO, and all expected fills. Only then does it apply the original batches to the destination. Invalid input leaves the destination empty. Retained history is copied once; it is not emitted again. Replay uses two passes and is intentionally more expensive than trusted event application.

File format: one canonical ASCII record per command, ending in a newline:

```text
LOB <version> <sequence> <event-count> <event tokens...> # <decimal FNV-1a checksum>\n
A <id> <B|S> <limit ticks> <original quantity>
T <maker id> <taker id> <execution ticks> <quantity>
C <id>
R <id> <D|U>
```

The latter four lines describe token shapes within a record, not separate file records. Checksums cover bytes before ` # `. Strict parsing rejects overflow, missing fields, invalid values/tags, noncanonical/trailing data, unknown versions, checksum failures, sequence gaps, and records above 16 MiB. A missing final newline is truncation, even if the preceding bytes would parse. A completely missing final record cannot be detected without an external committed-length marker. Checksums detect accidental corruption; they are not cryptographic authentication.

`read_events(path)` fails on malformed input. `read_events(path, true)` returns only the structurally valid prefix, an error, and its byte boundary. It stops at the first bad record and never skips forward. Call `replay()` to validate the recovered prefix semantically. Recovery never edits the original file; preserve it and write a new file from the recovered batches. Structurally valid but semantically invalid histories are rejected by replay, not automatically repaired.

`EventFile` refuses an existing path in `CreateNew` mode. `Append` validates and replays the existing file before accepting the next contiguous sequence. One process/writer must own the path: creation is not an atomic exclusive-create operation and there is no cross-process file locking. `append()` writes through `ofstream`; `flush()` checks stream flush errors. Neither operation calls `fsync`, and **there is no crash-safe or power-loss durability guarantee**. A command accepted in memory can be lost before persistence. The file is an explicit export/append sink, not a transactional write-ahead log tied to command acknowledgement. A failed writer is poisoned until reopened after recovery; disk failure does not roll back the engine.

### 7. Concurrency

`ConcurrentEngine` uses `std::mutex` and RAII `lock_guard` for all public operations. It returns copied snapshots and histories; no core reference or iterator escapes. Sequence assignment and in-memory event recording share the matching critical section. The file sink is separate and is not thread-safe; do not append from concurrently returned results out of sequence. Export a facade history snapshot from one owner instead.

Lock acquisition determines concurrent processing order, which can differ across runs. Replaying the recorded sequence is deterministic. There is no lock-free claim, fairness guarantee, or parallel matching inside one book. Copying a large snapshot/history holds the same mutex and may delay commands.

The concurrency test runs four producers issuing 1,200 submissions and 1,200 cancellations, with a concurrent snapshot reader. It sorts observed results by sequence, verifies contiguous unique sequence numbers and every result against the independent serial reference, then compares replay and live snapshots.

### 8. Performance and limitations

See [actual CSV](docs/benchmark-results.csv) and [engineering report](docs/engineering-report.md) for hardware, flags, timing boundaries, repeated results, and optimization opportunities. No latency claim should be transferred to a different machine or workload without remeasurement. This is a closed-loop microbenchmark, not an exchange load simulator.

History and reserved IDs grow without bound. The design has no checkpointing, production WAL, session ID reuse, amend/replace, market/IOC/FOK orders, auctions, self-trade prevention, risk limits, fees, multiple instruments, or external clock semantics. Memory allocation failures are fatal resource failures: the commit/application path is not strongly exception-safe under `bad_alloc`; discard the engine and recover from a validated external history rather than continuing. Semantic validation errors and sequence overflow are detected before mutation. No production readiness is claimed.

## Design notes and extensions

See [the design notes](docs/design-notes.md) for the reasoning behind the main decisions.

Possible extensions: add snapshot/checkpoint recovery with reserved-ID semantics; build and fault-test an fsync/group-commit WAL before acknowledging durability; implement amend/replace with explicit time-priority rules; profile and compare an arena-backed FIFO; add an open-loop load generator with queue-delay accounting; implement a bounded single-consumer command queue and compare it against this mutex baseline. Each extension should state its semantics and add independent tests before claiming a performance improvement.

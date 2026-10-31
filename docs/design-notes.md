# Design notes

## Design decisions

**Why integer ticks?** Order-book comparisons and eligibility are exact. The product's tick size belongs outside this engine; mapping decimal prices to ticks needs its own explicit rounding policy.

**Why different positive and remaining types?** Zero is invalid incoming intent but a valid completed state. Using one positive-only type for both would make completion awkward or invalid. Private mutation and immediate removal keep zero outside observable resting state.

**Why list rather than queue/deque?** Cancellation must remove an arbitrary live order while preserving FIFO for survivors. Stable list iterators provide O(1) removal after lookup. An arena/intrusive list could reduce allocations but would introduce ownership and lifetime complexity; first measure whether that matters.

**Why an ordered ID map?** It provides predictable worst-case O(log N) lookup without hash assumptions or collision behavior. The tradeoff is allocation and tree traversal. A reserved hash table is a reasonable independently measured experiment.

**Why reserve IDs forever?** It makes duplicates deterministic after cancellation and completion, including after replay. It costs O(U) memory. Session-scoped IDs or checkpoints require an explicit reuse policy; silently forgetting old IDs changes semantics.

**Why plan, then apply?** Matching decisions are calculated against a stable pre-command state, become explicit facts, then flow through one mutation path. Invalid replay is validated before touching the destination. The price is temporary event allocation and two replay passes. This implementation does not promise rollback on allocation failure.

**Why a batch per command?** It ties acceptance and every resulting trade to one recoverable record. Truncated records cannot expose only half an incoming command. It does not make the filesystem write atomic or durable: newline framing and checksums detect incomplete/corrupt data, not power-loss persistence.

**Why record rejection?** The processing sequence covers every valid command, including duplicates and unsuccessful cancels. Replay therefore reproduces metadata and future sequence numbers, not merely visible resting orders.

**Why a mutex first?** A single instrument already has a total matching order. A mutex keeps that ordering and history atomic with a small, reviewable implementation. Lock-free transport would not remove the requirement to serialize matching; it is a different scheduling design to benchmark later.

**Why a reference implementation?** Handpicked tests demonstrate examples. An independently structured oracle exercises many combinations and can reveal errors in ordering, partial state changes, or cancellation that examples miss. Seeded reproducibility does not prove correctness, and shared domain types remain a possible common-mode bug; direct boundary tests complement the oracle.

**Why distinguish timing modes?** Without it, a reported speed might mostly reflect disk buffering or mutex scheduling. The no-retained-log mode still generates/applies events because event sourcing is the core design. It is a core processing baseline, not an allocation-free pure matching loop.

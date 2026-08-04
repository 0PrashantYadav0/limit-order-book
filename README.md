# Limit Order Book + Matching Engine

A low-latency, price-time-priority **limit order book** and **matching engine**
in modern C++20 with **zero external dependencies**. Supports limit and market
orders, cancel, modify, self-trade prevention, and strict input validation, with
a correctness + property-based test suite and an honest latency/throughput
benchmark.

> Measured on Apple Silicon (Release, `-O3`): **~6.97M limit orders/sec**, with
> an **engine-only p50 latency of ~68 ns** (clock overhead measured and
> subtracted). See [Benchmarks](#benchmarks).

## Features

- **Price-time priority (FIFO):** best price first; ties broken by arrival order.
- **Order types:** limit and market (market = immediate-or-cancel, never rests).
- **Operations:** `add_limit`, `add_market`, `cancel`, `modify`.
- **Price improvement:** aggressor executes at the resting (maker) price.
- **Multi-level sweeps:** a marketable order walks as many levels as needed.
- **Genuine O(1) cancel/modify** via an order-id -> {level, node} index.
- **Robust API:** rejects duplicate ids, non-positive/out-of-range prices, and
  zero quantity *before* mutating state; returns explicit `SubmitResult`/`Reject`.
- **Self-trade prevention (STP):** optional `CancelResting` policy per account.
- **Integer tick prices** (no floating-point error) with overflow-safe bounds.
- **Sanitizer-clean** (ASan + UBSan), warning-clean under
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`.

## API

```cpp
lob::OrderBook book;                       // or OrderBook(StpPolicy::CancelResting)

lob::SubmitResult r = book.add_limit(id, lob::Side::Buy, price, qty, account);
if (!r.accepted()) { /* r.status: DuplicateId | BadPrice | ZeroQty */ }
for (const lob::Trade& t : r.trades) { /* taker_id, maker_id, price, quantity */ }

book.add_market(id, lob::Side::Sell, qty, account);   // IOC
book.cancel(id);                                       // O(1), bool
lob::ModifyResult m = book.modify(id, new_price, new_qty); // m.found, m.trades
book.best_bid(); book.best_ask(); book.spread();      // std::optional<Price>
```

## Design & Complexity

A single `std::map<Price, std::list<Order>>` holds every level; asks match from
`begin()` (lowest), bids from the last element (highest). Each level is a
`std::list<Order>` in FIFO order (O(1) erase, stable iterators). An
`OrderId -> {level iterator, node iterator}` index makes cancel/modify O(1) with
**no map lookup on the cancel path**.

| Operation | Complexity |
|-----------|-----------|
| `add_limit` / `add_market` | `O(L + log P)` (L levels crossed, P distinct prices) |
| `cancel` | **O(1)** amortized (index lookup + list/`map::erase` by iterator) |
| `best_bid` / `best_ask` | `O(log P)` |

Executions occur at the maker price, so the aggressor keeps price improvement.

## Thread safety

**Not thread-safe.** Designed for a single writer (single-producer /
single-consumer ingest). Guard externally or shard one book per thread.

## Build

Requires a C++20 compiler and CMake 3.16+.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Options: `-DLOB_STRICT=ON` (warnings as errors),
`-DLOB_SANITIZE=ON` (AddressSanitizer + UBSan).

## Run

```bash
./build/lob_demo          # narrated demo of every feature (incl. STP + rejects)
./build/lob_tests         # unit + property tests (exit 0 on success)
./build/lob_bench         # benchmark (optional arg: ops per phase)
ctest --test-dir build    # run tests via CTest
```

## Benchmarks

`./build/lob_bench 2000000` (Release `-O3`, Apple Silicon). Latencies are
measured per-op and the **`steady_clock::now()` pair overhead (~15 ns) is
measured and subtracted** to report engine-only time; batch throughput times the
whole loop.

| Phase | Throughput | p50 | p99 | p99.9 |
|-------|-----------:|----:|----:|------:|
| `add_limit` (match + rest, ~78% crossed) | **6.97M ops/s** | **68 ns** | 319 ns | 1.03 us |
| `cancel` (random, ~2M-order book) | **2.55M ops/s** | 360 ns | 610 ns | 1.57 us |

The `index == physical` invariant is asserted at the end of the run.

## Correctness

`lob_tests` runs **83 assertions** across 18 cases: fills, FIFO priority, price
improvement, multi-level sweeps, market IOC, cancel/modify semantics, duplicate-id
rejection (regression for a real corruption bug), price/quantity validation,
self-trade prevention, and a **randomized property test** (5,000 random ops)
asserting three invariants after every operation:

1. `order_count() == physical_order_count()` (index never desyncs from the book),
2. the resting book is never crossed (`best_bid < best_ask`),
3. every resting order is cancelable and the book fully empties.

CI (GitHub Actions) builds gcc + clang x Debug/Release with
`-Werror -Wpedantic -Wconversion -Wshadow`, runs `ctest`, and runs a dedicated
ASan + UBSan job.

## Possible extensions

- Intrusive lists + a slab/object pool to remove per-order allocation.
- Array-indexed price levels for O(1) best-price on dense tick grids.
- Iceberg / stop orders; FIX or binary ingest; lock-free SPSC input queue.
- `rdtsc`/`mach_absolute_time` latency harness with CPU pinning.

## License

MIT (see `LICENSE`).

# struct_view Route A vs Route B benchmark result

- Date: 2026-08-05
- Hardware: Apple M5 (arm64), Darwin 25.5.0 (xnu-12377.121.10~1/RELEASE_ARM64_T8142)
- Compiler: Apple clang 21.0.0 (clang-2100.1.1.1.101), Target arm64-apple-darwin25.5.0
- Build: Release (-O2)
- N = 2,000,000 renders of `alarm_line` (Event/PersonInfo/Box, 2-level struct-block nesting: person->name-age, rect->x,y,w,h; 1 device getter, 1 uint64 field, 4 literal connectors)

## Results

Median of 3 runs (best/median of multiple runs; first run excluded as warm-up of the OS scheduler).

| Route | Time (ms) | us/render |
|---|---|---|
| B (ValueProvider*) | 634.995 | 0.317498 |
| A (four-kind Step) | 617.755 | 0.308877 |
| Speedup A/B | 1.02x | |

All three raw runs:

| Run | B (ms) | A (ms) | us/render B | us/render A | Speedup A/B |
|---|---|---|---|---|---|
| 1 | 630.064 | 617.755 | 0.315032 | 0.308877 | 1.01993x |
| 2 | 637.111 | 614.014 | 0.318555 | 0.307007 | 1.03762x |
| 3 | 634.995 | 623.791 | 0.317498 | 0.311896 | 1.01796x |
| median | 634.995 | 617.755 | 0.317498 | 0.308877 | 1.01993x |

## Analysis

Route A is faster, but only by ~2% median (range 1.8%-3.8% across runs) — roughly 0.009 us/render. The theoretical wins (no virtual call for connectors: `ConnectorBinding` holds the literal by value, no `ConnectorProvider` heap, no virtual `get`; and inlined sub-recipe navigation: `SubRecipeBinding` holds nav + `shared_ptr` directly, no `StructBlockProvider` virtual) are real but small in absolute terms because both routes spend the overwhelming majority of their per-render time in identical shared work: `snprintf` formatting of 7 numeric fields plus `std::string` heap allocation/deallocation for the ~39-char output (exceeds libc++ SSO of 22 bytes). That shared formatting/allocation cost dwarfs the one-virtual-call-per-segment overhead Route B pays (the recipe has only ~8 segments, so ~8 virtual calls saved per render — a handful of nanoseconds against ~300 ns of formatting+allocation). The margin reflects exactly this: a small, consistent win that tracks the segment count rather than the formatting cost.

## Decision

Per spec §3.2: if the difference is negligible in the target scenario, choose B (simpler); if the hot path is a bottleneck, choose A.

Chosen route: **B** — rationale: The measured margin is ~2% median (max 3.8% across three runs), well under the 10% threshold the spec sets for "negligible." Security video structured-data extraction does run at high call frequency (thousands of renders/sec across a frame pipeline), but at ~0.32 us/render the per-render delta is ~0.009 us — at 10,000 renders/sec that is ~90 us/sec saved, which is invisible against the milliseconds of I/O, JSON parsing, and network round-trips in any real video pipeline. Route A's hot path is NOT a bottleneck; the bottleneck is the formatting/allocation code both routes share. Meanwhile Route A carries real code-complexity cost: a separate `StepA` variant type, `BuilderA` compile pass, and `runRecipeA` runner — three additional surfaces to maintain, test, and keep in parity with Route B (the existing `test_route_a` parity gate). For a ~2% win that does not address the actual bottleneck, that complexity is not justified. Simplicity wins; the spec's "negligible -> B" branch applies.

## Action

Keep Route B as the default (`render`); Route A (`renderA`) remains available behind the API for parity testing and as a reference implementation of the four-kind representation, but is not the primary render path. No production code changes are required — both routes are already implemented and parity-verified; this benchmark confirms the spec §3.2 deferral is resolved in favor of Route B on the grounds of negligible measured difference and lower maintenance burden. The `bench_routes` target is kept (built but not run by ctest) so the measurement can be re-run if the recipe shape changes materially (e.g. many more connectors, deeper struct-block recursion) — at which point the decision should be revisited.

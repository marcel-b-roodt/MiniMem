# MiniMem — Copilot Instructions

## Project Overview

**MiniMem** is an open-source research and implementation project for transparent, lossless memory compression at the OS kernel and GPU driver level.

The design pillars are:
- **Transparent** — applications and userspace code require no changes. Compression happens in the kernel or driver, not in the application.
- **Lossless** — compressed data must decompress to exactly the original bytes. No approximations, no quality loss.
- **Fast** — decompression must be fast enough to sit in the memory access path without unacceptable latency. Target: decompression overhead below the cost of a page fault or a swap-in from disk.
- **Memory-efficient** — the whole point is reducing memory overhead. Compression metadata and algorithm state must not consume more memory than they save.
- **Measurable** — every component is benchmarked for throughput, latency, compression ratio, and memory overhead before being accepted.

---

## Architecture Summary

| Layer | Description |
|---|---|
| `src/lib/` | Compression algorithm library — LZ4, LZSSE8, WKdm, BDI, Zstd wrappers. Pure C with optional SIMD paths. |
| `src/kernel/` | Linux kernel module for transparent in-memory page compression. |
| `src/vram/` | VRAM compression layer — driver integration for GPU memory compression. |
| `src/hw/` | Hardware acceleration backends — CXL, Intel QAT, FPGA offload. |
| `src/specialized/` | Domain-specific compressors — AI workload weights, page-table-aware, delta-streaming. |
| `tests/` | Criterion-based unit + benchmark tests. Mirror `src/` structure. |
| `reports/` | Benchmark output and performance regression reports. |
| `docs/` | Public design docs (git-tracked). |
| `docs/research/` | Numbered research documents referenced by design docs. |

### Language policy
- **C (C11)** — kernel modules, algorithm library, performance-critical paths. C is the kernel's language; no exceptions.
- **Rust** — optional for userspace tooling, benchmark harness, or VRAM layer (if targeting Mesa/driver work). Only if the target subsystem natively uses Rust.
- **Assembly (x86-64, AArch64)** — SIMD-optimized decompression hot paths only, behind runtime feature detection.

---

## Documentation

Public design docs live in `docs/` and are **git-tracked**.
Research documents live in `docs/research/` and are **git-tracked** — these are the project's institutional memory.

```
docs/                             ← git-tracked (public)
  README.md                      ← navigation index
  goal.md                        ← north star — what MiniMem is and isn't
  roadmap.md                     ← ordered stage-by-stage feature roadmap
  feature-registry.md            ← source of truth for every feature's status
  architecture.md                ← codebase architecture for contributors
  candidates.md                  ← compression algorithm & approach candidates (RAM)
  vram-compression.md            ← VRAM-specific compression deep dive
  hardware-acceleration.md       ← hardware acceleration paths: CXL, QAT, FPGA, SIMD
  specialized-compression.md     ← second-layer investigation: novel specialized approaches
  research/                      ← git-tracked research drop zone
    README.md                    ← index of all research documents
    001-wkdm-memory-compression.md
    002-bdi-cache-line-compression.md
    ...
```

### Docs workflow — follow this on every feature change:
1. **Before implementing:** check whether a doc exists for the feature area. Read it — the doc describes *intended* behaviour. Implement to match the doc's intent where possible.
2. **If no doc exists:** write one as part of the work. Document the intent, the data model, and the current behaviour. Be honest about stubs.
3. **After implementing:** update the relevant doc to reflect what was actually built. **Update `docs/feature-registry.md` too.**
4. **When researching:** add a numbered research doc in `docs/research/` and reference it from design docs.

Docs should be **concise and accurate**, not exhaustive. A table or diagram is worth more than paragraphs.

---

## Feature Registry Workflow

The registry lives at `docs/feature-registry.md`. It is the **single source of truth** for what MiniMem has and what it plans to have.

Status legend: ✅ Complete · 🔧 In Progress · 📋 Planned · ❌ Removed / Deferred

### Rules:
1. **Consult the registry before each task set.** Understand the project's current shape before adding or changing anything.
2. **Update the registry after each task set.** New features or significant changes must be reflected immediately.
3. **Completed TODOs → move to the registry** with status ✅. Do not leave completed entries in `TODO.md` indefinitely.
4. **The registry drives scoping decisions** — if something is not in the registry, it does not exist.

---

## NearTermTodos Workflow

`NearTermTodos.txt` is a prioritised queue of improvements for **recent additions** — things to resolve before moving on.

### Rules:
1. **Read `NearTermTodos.txt` in full when requested.** Treat each item as an actionable task.
2. **Iterate in order.** An item is done when code and tests are complete and verified.
3. **Remove completed items.** Do not leave completed items in the file.
4. **Leave out-of-scope items** with a brief note — do not silently skip.
5. **The file is a short-term queue, not an archive.** Keep it small. Leave it empty when exhausted.

---

## HighLevelTodos Workflow

`HighLevelTodos.txt` is a scratchpad for raw design intent and vague feature desires.

### Rules:
1. **Check at the start of every task set.** Identify items that can be turned into `TODO.md` entries or registry rows.
2. **Source each item cleanly** — translate vague notes into specific, scoped TODO entries. Add to `TODO.md` and/or registry as 📋 Planned.
3. **Remove sourced items** from the file. Leave it empty when all items are sourced.
4. **Leave unsourceable items** until they can be clarified.

---

## Research Document Workflow

Research docs in `docs/research/` are the project's institutional memory. They capture findings from papers, experiments, and investigations so we don't re-research the same ground.

### Naming convention
`NNNN-short-topic.md` — four-digit zero-padded number, hyphen, lowercase topic. Numbers are sequential.

### Structure (every research doc must have these sections)
```markdown
# NNNN — Topic

## Summary
One paragraph: what this is and why we looked at it.

## Key Findings
Bullet points or table. The data we actually care about.

## Relevance to MiniMem
How does this apply to our specific targets (RAM, VRAM, HW accel, specialized)?

## Open Questions
What we still don't know. What needs benchmarking or experimentation.

## References
Links, papers, source code URLs.
```

### Rules
1. **Reference from design docs.** Candidates, roadmap, and architecture docs should link to research docs, not duplicate their content.
2. **Add before investigating.** When starting a research session, create the research doc first. Fill it in as you go.
3. **Update when revisited.** If new information comes to light, update the existing doc — don't create a new one.

---

## Copilot Session Workflow

### 1 — Calibrate step granularity to task size
Always state a step plan at the start of a reply. Then apply this rule:

| Situation | Action |
|---|---|
| All steps are small and low-risk (≤ 3 files, straightforward changes) | Complete all steps in one reply |
| A step requires explicit approval (new module, architecture decision) | Stop and yield after stating the proposal |
| A step touches 4+ files with significant new content | Do one step, yield, continue next reply |
| Any step is uncertain or experimental | Do that step alone, yield for review |
| Cumulative response is getting long | Finish the current step cleanly, yield |

### 2 — Benchmark before accepting
Every compression algorithm or kernel component must be benchmarked before being marked ✅ in the registry. Benchmarks live in `tests/` and produce output in `reports/`. If a component has no benchmark, it is not done.

### 3 — Split work into release-trackable task slices
Every non-trivial request must be split into small, independently verifiable slices.

### 4 — Commit messages must be changelog-friendly
Required format:
- `feat(scope): <user-facing change>`
- `fix(scope): <bug fix>`
- `refactor(scope): <internal cleanup>`
- `test(scope): <coverage or benchmark addition>`
- `docs(scope): <documentation update>`
- `chore(scope): <non-user-facing maintenance>`
- `bench(scope): <benchmark addition or update>`

---

## Code Quality Guidelines

### Priority order: **Correctness → Performance → Simplicity**

### When working on any feature:
1. **Write the benchmark alongside the code.** Every algorithm function must have a Criterion benchmark measuring throughput (MB/s), latency (μs), and compression ratio. No exceptions.
2. **Avoid allocations in hot paths.** Decompression runs in the page fault path or GPU command stream. No malloc, no dynamic dispatch. Pre-allocate buffers.
3. **Kernel code follows Linux coding style.** `indent -linux -nbad -bap -nbc -bbo -hnl -br -brs -cd0 -nce -cli0 -d0 -di0 -nfc1 -i8 -ip0 -l80 -lp -npcs -nprs -psl -saf -sai -saw -ci4 -nut`. No exceptions.
4. **SIMD paths use runtime feature detection.** Compile multiple code paths; dispatch at runtime via CPUID/EL0 feature registers. Never assume SIMD availability.
5. **Test on real memory pages.** Synthetic benchmarks (Silesia, enwik) are useful for comparison but not sufficient. Always benchmark on actual memory page dumps and AI model weight tensors.

### Kernel module rules
- Must compile against the latest stable kernel and the latest LTS.
- Must not depend on out-of-tree kernel patches.
- Must be loadable as a module (no kernel rebuild required).
- Must provide `/sys/kernel/minimem/` stats (pages compressed, bytes saved, decompression count, latency histogram).
- Must have a clean `rmmod` path — no resource leaks on module unload.

---

## Testing Strategy

### Framework
- **Unit tests:** Criterion (C testing framework). Install via package manager or build from source.
- **Kernel module tests:** kselftest framework for in-kernel tests; user-space test driver for module API tests.
- **Benchmarks:** Criterion benchmarks + custom harness for page-level and tensor-level workloads.

### What to test

| Area | Test type | Notes |
|---|---|---|
| Compression algorithms | Unit + benchmark | Round-trip correctness, throughput, ratio on page/tensor data |
| Kernel module | kselftest + user-space driver | Load/unload, page compress/decompress, stats accuracy, concurrency |
| VRAM layer | User-space simulation + GPU harness | Correctness on mock VRAM buffers, throughput on real GPU |
| Hardware acceleration | Hardware-in-the-loop | QAT/CXL on appropriate hardware; CI can skip if HW unavailable |
| Specialized compressors | Unit + benchmark on domain data | AI weights, page tables, streaming deltas |

### Test file location
Mirror the source path under `tests/`:
- `tests/lib/test_lz4.c`
- `tests/lib/test_wkdm.c`
- `tests/kernel/test_compress_page.c`
- `tests/vram/test_vram_compress.c`

### Benchmark reports
Benchmarks write results to `reports/` in a parseable format (CSV or JSON). Track regressions across commits.

---

## Candidate Assessment Format

When evaluating a compression algorithm or approach, use this structure in the candidates docs:

```markdown
### [Algorithm/Approach Name]

**Category:** (general-purpose / memory-page-specific / cache-line / GPU / hardware)
**Decompression speed:** (MB/s on reference hardware)
**Compression ratio:** (typical on memory page data)
**Decompression latency:** (μs per 4KB page)
**Implementation complexity:** (low / medium / high)

**Pros:**
- ...

**Cons:**
- ...

**Verdict:** (adopt / investigate further / reject — with reason)

**Implementation notes:**
Specific considerations for MiniMem integration.

**Open questions:**
What we still need to find out.
```

---

## Common Patterns

### Compression round-trip test
```c
// Every algorithm must pass this pattern:
void test_roundtrip(void) {
    uint8_t src[4096] = { /* page data */ };
    uint8_t compressed[8192];
    uint8_t decompressed[4096];
    size_t compressed_size = compress(src, 4096, compressed, 8192);
    cr_assert_gt(compressed_size, 0);
    cr_assert_le(compressed_size, 4096); // must actually compress
    size_t decompressed_size = decompress(compressed, compressed_size, decompressed, 4096);
    cr_assert_eq(decompressed_size, 4096);
    cr_assert_eq(memcmp(src, decompressed, 4096), 0); // lossless
}
```

### Kernel module stats
```c
// Every module must expose these via sysfs:
static struct minimem_stats {
    atomic64_t pages_compressed;
    atomic64_t bytes_saved;        // original - compressed
    atomic64_t decompress_count;
    atomic64_t decompress_ns_total;
};
```
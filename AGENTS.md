## The development plan is append-only

`docs/game-engine-development-plan.md` is ~6400 lines and **chronological**.
Sections are added as work happens and are not retro-edited. Read its "How to
read this document" preamble before using it.

The failure mode this causes, repeatedly: **a completed phase's section reads
as current state when it is a period record.** Figures inside it ("7
failures", "W4-W5 remain", "declared and stubbed") were true when written and
are routinely false now.

Rules:
- **Never quote a status, count, or "remaining work" claim from a completed
  phase section.** Verify it against the code first. Multiple stale claims
  were found and corrected in Phase 6C W9 alone.
- Each phase appears **twice**: a short scope stub in the roadmap, and — once
  worked — a long grounded section near the end. The stub is intent; the
  later section is what happened. Grep gives you both; check which you have.
- The file is **two documents concatenated** (the phase roadmap, then "First
  tiny vertical slice"). Headings like `Goal` and `Work packages` belong to
  the second.
- For current state, prefer: **Test baseline** (in the plan, authoritative
  and supersedes every earlier figure) · `docs/scripting.md`,
  `docs/scene-management.md`, `docs/game-loop.md` for what the engine does.
- **`docs/glossary.md` before naming anything or touching a resource index.**
  It records only terms that have already caused a defect, plus the four
  context boundaries (Asset / Authoring / Scene / GPU) that resource indices
  are translated across — four defects in July 2026 were all missed
  translations at one of them. It is corrected in place, not append-only.
- **`Phase N` refers to a roadmap phase and nothing else.** Off-roadmap work
  gets a name, never a number.
- When you find a stale claim in a completed section, **add a dated
  supersession note; do not rewrite it.** Those sections are the audit trail
  for why decisions were made. Correct headers and forward-looking specs
  freely; leave the record intact.

## Writing a phase spec

A roadmap stub is not a spec. Before implementing a phase, produce a grounded
implementation plan — the pattern used for 6B, 6C and 7:

1. **Ground it against the code**, not the roadmap and not memory. Half of
   Phase 7 already existed under other names; only reading the tree found
   that.
2. **Every finding carries a `file:line`** so the next reader verifies rather
   than trusts. State the commit you grounded against — the tree moves.
3. Record **decisions that must be answered before code**, with a
   recommendation but marked unsettled. Name collisions and semantic
   conflicts with existing code belong here.
4. Recover **commitments earlier phases deferred to this one**. They are
   recorded where the deferral happened, so a roadmap-only spec misses them
   (`grep -n "Phase N" docs/game-engine-development-plan.md`).
5. Order workstreams so nothing depends on unbuilt UI and the riskiest
   unification lands after its foundations are proven.
6. **Get the spec reviewed before implementing.** Amend it, then write it in.

When the phase completes, append a **verification report**: what was built,
what was measured (both Release and Debug), and defects found along the way.

## Silent failure is this codebase's characteristic bug

Phase 6 shipped three: a policy that left a cache stale for a whole session,
a field-declaration form that parsed and did nothing, and a fixture generator
whose writes were never checked. When something "doesn't work", suspect a
swallowed return value or an unvalidated write before suspecting logic.
Prefer loud failure; route errors through the existing diagnostic types
rather than returning empty/false.

## Build and test

- `msbuild RT2App.sln -p:Configuration=Release -p:Platform=x64`. Targets:
  `RT2App`, `RT2Tests`, `RT2SliceRunner`.
- **Release is green (554/554) and must stay green** — a Release failure is a
  real regression, not baseline noise. Debug has 8 known failures in OBJ
  fixture generation; see "Test baseline" in the plan.
- **Run `RT2Tests.exe` from the repository root.** It resolves some fixtures
  by relative path; run elsewhere it fails extra cases *and* writes stray
  fixture files into the tree.
- `RT2Tests` and `RT2SliceRunner` are **CPU-only by design** — no Vulkan,
  ImGui or Walnut. Keep new engine logic linkable into both; that constraint
  is why the scripting core is testable at all.
- `run_script_test.ps1` is the scripting regression gate.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

When the user types `/graphify`, use the installed graphify skill or instructions before doing anything else.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- Dirty graphify-out/ files are expected after hooks or incremental updates; dirty graph files are not a reason to skip graphify. Only skip graphify if the task is about stale or incorrect graph output, or the user explicitly says not to use it.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
- `graph.json`, `manifest.json` and `.graphify_labels.json` are gitignored — they are generated, and graph.json alone is ~42MB rewritten on every refresh. **On a fresh clone they do not exist, so `graphify query` will fail until you run `graphify update .` once.** `GRAPH_REPORT.md` is tracked and readable without regenerating.
- Codex sandbox note: the installed `graphify.exe` is a uv trampoline whose environment lives under the user's AppData. Inside the restricted sandbox it may fail immediately with `uv trampoline failed to canonicalize script path`; this is a sandbox-access issue, not a broken graph. Rerun Graphify commands with sandbox escalation. The user has approved the `graphify update` command prefix. Allow up to 180 seconds for `graphify update .` on this repository.

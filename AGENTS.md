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

# Studio AI model evaluation

Evaluation date: 2026-08-09. Provider: SiliconFlow China endpoint. The secret was read from the
gitignored local `ai.env`; it was not printed, logged or persisted in a request fixture.

All candidates received the same real 640×640 Studio preview (506 KiB), current `FractalSpec`,
capabilities and Chinese task: describe visible structure/colour, then propose a deep-ocean colour
change through the sole `propose_studio_patch` tool without changing geometry.

| Model | Result |
|---|---|
| `Qwen/Qwen3.6-35B-A3B` | Winner. With `enable_thinking=false`: 1.64 s, accurate visible description, normal text content, and a valid structured tool call containing only `colorMap=viridis`. |
| `Qwen/Qwen3-VL-32B-Instruct` | 3.23 s but returned prose and a textual `<tool_call>` in `reasoning_content`; API `tool_calls` was empty. |
| `Qwen/Qwen3-VL-30B-A3B-Instruct` | Approximately 38 s in thinking mode with no structured tool call; rejects `enable_thinking=false` with provider code 20015. |

An initial Qwen3.6 call without `enable_thinking=false` spent 648 completion tokens in reasoning,
returned no visible text, and happened to produce a tool call. This would look like an empty answer
in the UI and is therefore a release-blocking protocol failure. Production fixes one model only:
`Qwen/Qwen3.6-35B-A3B`, with thinking disabled and server-side validation of every proposed patch.

This comparison is evidence for the current provider/model versions, not a permanent benchmark.
Run `platform-backend/scripts/ai-provider-contract.py` explicitly before release. Model quality is
never replaced by a mock response.

## MiMo-V2.5 follow-up

`mimo-v2.5` was tested separately through Xiaomi's pay-as-you-go API with the same real preview,
project prompt and forced tool schema. It can consume the image and, with a 1500-token budget,
eventually returns a valid tool call. It was not selected:

- the image/tool call took about 48 seconds and used 1059 completion tokens, 916 of them hidden
  reasoning tokens; its patch repeated unchanged fields instead of returning only a delta;
- with a smaller 700-token budget it ended at `finish_reason=length` before making the tool call;
- on a project-specific compatibility question it incorrectly described `min_abs` as an escape
  parameter, invented a nonexistent nested request shape, and failed to finish within 1000 tokens.

Qwen3.6 answered the same compatibility question substantially better and correctly calculated the
preview colour correction (`60 × 4096 / 60000 ≈ 4.10`). Its wording exposed smaller metric/colouring
ambiguities, which were fixed by making those field relationships explicit in the system prompt.
Quality and contract reliability therefore favour Qwen3.6 even when latency is ignored.

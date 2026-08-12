# Studio AI model evaluation

Evaluation date: 2026-08-09 to 2026-08-10. Provider: SiliconFlow China endpoint. The secret was read from the
gitignored local `ai.env`; it was not printed, logged or persisted in a request fixture.

All candidates received the same real 640×640 Studio preview (506 KiB), current `FractalSpec`,
capabilities and Chinese task: describe visible structure/colour, then propose a deep-ocean colour
change through the sole `propose_studio_patch` tool without changing geometry.

| Model | Result |
|---|---|
| `Qwen/Qwen3.6-35B-A3B` | Winner overall. With `enable_thinking=false`, text streaming and named forced tool calls are stable. It passed the validated 3/4/3 exploration shapes and the real initial/revision listing-copy contracts. Its quadrant descriptions can still over-emphasize a small pale highlight or mis-rank a corner, so browser review of the actual candidate previews remains a release gate. |
| `Qwen/Qwen3-VL-32B-Instruct` | Better raw vision was not consistent. SiliconFlow rejects a named function in `tool_choice` for this endpoint; with the supported `auto` choice it returned an unchanged location candidate, which the Platform correctly rejected. |
| `Qwen/Qwen3-VL-30B-A3B-Instruct` | Usually described the dark centre and warm diagonal structures more accurately. However, in a two-round 3/4/3 run it failed 2 of 6 checks: one composition exceeded the declared rotation bound and one request timed out. It therefore did not meet the stable tool-contract requirement. |

An initial Qwen3.6 call without `enable_thinking=false` spent 648 completion tokens in reasoning,
returned no visible text, and happened to produce a tool call. This would look like an empty answer
in the UI and is therefore a release-blocking protocol failure. Production fixes one model only:
`Qwen/Qwen3.6-35B-A3B`, with thinking disabled and server-side validation of every proposed patch.

The later comparison used the same actual 640×640 Compute preview for a separate visual-observation
phase followed by the strict Platform candidate tool. The Qwen3-VL Instruct compatibility path omits
the unsupported `enable_thinking` extension and uses `tool_choice=auto`; this makes comparison
possible, but it does not weaken server validation or enable runtime model switching.

This comparison is evidence for the current provider/model versions, not a permanent benchmark.
Run `platform-backend/scripts/ai-exploration-contract.py` and
`platform-backend/scripts/ai-listing-copy-contract.py` with actual rendered work before release.
The production `app.ai.provider` runtime adapter also passed its explicit real-image contract on
2026-08-12: visible text streaming and usage, one validated forced image/tool call, deliberate
closure after the first visible delta, and sanitized connection-failure mapping all passed.
The small generated image in `ai-provider-contract.py` is only a wire-protocol smoke test and is not
accepted as model-quality evidence. Model quality is never replaced by a mock response.

## MiMo-V2.5 follow-up

`mimo-v2.5` was tested separately through Xiaomi's pay-as-you-go API with the same real preview,
project prompt and forced tool schema. It can consume the image and, with a 1500-token budget,
eventually returns a valid tool call. It was not selected:

- a final low-cost compatibility rerun passed real text streaming (44 chunks, 403 total tokens) but
  returned only hidden reasoning for the in-memory image plus forced tool call, so the visible/tool
  contract failed outright at the normal smoke-test budget;
- the image/tool call took about 48 seconds and used 1059 completion tokens, 916 of them hidden
  reasoning tokens; its patch repeated unchanged fields instead of returning only a delta;
- with a smaller 700-token budget it ended at `finish_reason=length` before making the tool call;
- on a project-specific compatibility question it incorrectly described `min_abs` as an escape
  parameter, invented a nonexistent nested request shape, and failed to finish within 1000 tokens.

Qwen3.6 answered the same compatibility question substantially better and correctly followed the
then-provided 4096-iteration premise. A later source audit found the current preview cap is 512, not
4096. The Platform now derives that value from `PREVIEW_MAX_ITERATIONS` and automatically applies the
exact escape-gradient correction (`cycles × (previewIterations+2)/(masterIterations+2)`) so the model
must not repeat a stale hard-coded correction. This regression must be rerun whenever the mapping changes.
Quality and contract reliability therefore favour Qwen3.6 even when latency is ignored.

## Small-model and on-device cost follow-up

The listing-copy flow is the best candidate for a cheaper model because its output is text-only,
strictly validated and never auto-published. It is not safe to route it by parameter count alone:
the user-visible copy must still pass the same real-image initial/revision contract and a human
quality review against Qwen3.6.

The proposed [Gemma 4 E2B Q4 GGUF](https://huggingface.co/google/gemma-4-E2B-it-qat-q4_0-gguf/blob/main/gemma-4-E2B_q4_0-it.gguf)
is 3.35 GB, not roughly 2 GB. Its model card describes E2B as an on-device model and provides a
mobile-optimized format, but sending the GGUF to every browser is not an acceptable default: first
load, cache eviction, memory pressure and WebGPU support would make the publish button unreliable.
If evaluated later, browser inference must be an optional capability with progress, cancellation,
cache controls and an immediate server fallback. A server-side llama.cpp deployment is the simpler
first experiment.

Official Qwen3.5 post-trained weights exist at
[0.8B](https://huggingface.co/Qwen/Qwen3.5-0.8B),
[2B](https://huggingface.co/Qwen/Qwen3.5-2B) and
[4B](https://huggingface.co/Qwen/Qwen3.5-4B). On 2026-08-12, the current SiliconFlow account exposed
Qwen3.5 4B and 9B but not 0.8B, 2B or Gemma 4. Real listing-copy checks produced these results:

| Model | Real listing-copy result |
|---|---|
| `Qwen/Qwen3.5-4B` | Failed twice with no complete response before the adapter read timeout, including a single-request run. It is not a usable cost route on the current provider. |
| `Qwen/Qwen3.5-9B` | Passed one initial request in 13.74 s / 2857 tokens and an initial-plus-revision run in 33.41 s / 6091 tokens. The copy was repetitive and more generic than Qwen3.6, so it is not selected for users. |
| `Qwen/Qwen3.6-35B-A3B` | Passed initial-plus-revision in 32.26 s / 6148 tokens and remained the stronger factual/editorial baseline, although a human should still shorten overly clinical descriptions. |

The next cost experiment should run 0.8B, 2B, 4B and Gemma E2B on a separate server/CPU test target,
not on a production Compute GPU. Use the same hidden set of real artworks and score: Chinese title,
description and tags; revision-instruction compliance; invented visual/mathematical facts; schema
pass rate; time to first token; complete latency; peak memory; and cost. The smallest model that
meets the Qwen3.6 quality floor may handle listing copy, with automatic server-side Qwen3.6 fallback.
Image analysis, Studio patch tools and low-confidence outputs stay on Qwen3.6. This is a future
optimization and does not add runtime multi-model routing to the current release.

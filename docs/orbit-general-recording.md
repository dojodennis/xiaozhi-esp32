# OS-07 — general Service recording candidate

5 October 2026 · Codex · branch `codex/orbit-general-recording`, base `005c549`.
The shared task/acceptance plan remains Dojo `docs/orbit-service.md`.

Service mode advertises `dojo_general_v2`; Chef negotiation is unchanged. A ready
or recovery hello with explicit protocol 2 and shift scope has no table. Unknown
scope, missing version and mixed table/shift messages fail closed. The normal
Service face says Dojo · Service. Yellow starts/stops the existing general audio
journal after waiter approval. Guest/table identity is reviewed afterward on the
desk; it never becomes a pre-recording picker or a last-table default.

Recovery remains upload-only and cannot begin another recording. Existing audio
IDs, timestamps, retry acknowledgement, discard and flash ownership are preserved.
This source requires the coordinated v2 backend/gateway and Dojo app candidate.

Validation: all 367 host tests pass; focused actual parser/application tests cover
no-table ready, start/stop, malformed hellos and recovery refusal. The canonical
stopwatch build passes from a clean build directory with the original dependency
lock. A stale incremental object after restoring pinned dependencies failed the
first retry; rebuilding cleanly resolved it without changing dependencies.

Unsigned `build/xiaozhi.bin`: 4,063,232 bytes; application slot 4,128,768 bytes;
65,536 bytes headroom. SHA-256
`269edf4f61e3b1a5ea579b35b354664219a8ed862852d9a849d49ef2c3e0798e`.
Dependency lock SHA-256
`f9f8cb891983801320a3e0c1b38ce8832f9b21ee6a92e5d21ff580e574782652`.

Independent source review is recorded in the shared OS-07 receipt. This is an
unsigned build, not a flashed or accepted device. Signing, fresh Ring 2 identity,
protected backup, exact application-region write authorization, normal boot and
physical recording/recovery acceptance remain separate. No merged firmware image
belongs in that installation procedure.

Change log — 5 October · OS-07-A/B · Codex: added explicit table-free Service v2
negotiation and shift copy without changing the audio journal or Chef mode. Full
host verification and pinned clean build pass. Physical Ring 2 remains unchanged.

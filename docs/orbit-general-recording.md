# OS-07 — general Service recording candidate

Current source snapshot — 5 October 2026 · OS-07-B · Codex.
Branch `codex/orbit-transcript-routing`, base
`df1e8375840a54dac1d370ca2c67b3439d9520eb`. The approved shared plan remains
Dojo `docs/orbit-service.md`; its source, review, publication, installation and
physical acceptance gates remain separate. The OS-07-A record below is retained
as history and is superseded for the current firmware candidate.

The Service face uses the existing Orbit crest and distinguishes preparation,
recording, processing, transcript receipt, confirmed note saving and session loss.
Negotiated `dojo_transcript_v1` adds bounded, correlated transcript pages and
explicit per-note General or immutable Guest-alias routing. A new recording and
every note reset to Unassigned. The waiter reads the excerpt, chooses a destination,
reads the full paged destination label and confirms separately. A saved label
requires the matching committed receipt. Stale revisions, wrong recordings,
bindings, sessions and late receipts cannot change that choice or claim a save.
Long or mixed Guest notes stay available for splitting on the desk. Existing
audio persistence, recording control, Chef, Ring 1 and incapable-gateway behavior
remain covered by the existing host suite.

Validation on the exact frozen source: 370 host tests pass in 118.732 seconds.
The production parser consumes the three byte-identical gateway projector fixtures
for transcript, General save and Guest save. Parser/model tests run with address
and undefined-behavior sanitizers. Actual application extraction checks explicit
confirmation, reset, replay/stale responses, session loss and hidden-display read
gates. Wide text, newline and long destination reconstruction plus pinned LVGL
font extents check that every reachable character can be paged within the body.
Root performed independent source review and cleared the final numeric-only
diagnostic delta after the destination and typography findings were fixed.

The canonical offline build succeeds with the existing ESP-IDF 6.0 toolchain:
`python3 scripts/build.py m5stack/stopwatch --name provisions-kitchen-helper-stopwatch`.
Unsigned `build/xiaozhi.bin`: 4,013,680 bytes; application slot: 4,128,768 bytes;
headroom: 115,088 bytes (3%). SHA-256:
`02d8365df2f5e34fa114140063b7a001d01a211f4e39c109c555c803c2b210d4`.
Current unchanged dependency lock SHA-256:
`a6ce82ff9d77c9cf9ea04cbb1940fe92ccf894f129bec36f45f9f0c754ae0d49`.

Build preflight used only the tracked canonical stopwatch selection/defaults and
the existing pinned dependencies. No credentials, provisioning state, device
identifiers or live configuration were supplied or copied into this source or
recovery package. Generated configuration had no nonempty credential/identity
candidate fields. The package excludes `sdkconfig`, environment files, NVS,
bootloader, partition images, merged images and device backups; it contains only
the unsigned application, source delta bundle, bounded fixtures and check receipts.
The build command and host tests made no device, serial, provisioning or live
application-service calls. Local process metadata access was required by the
ESP-IDF dependency manager during the authorized offline compilation.

Debug timings contain numbers only. `receipt_to_paint_us` measures the local
callback-to-display label update submission/return, excluding LVGL/LCD flush and
physical visibility. `since_last_send_us` is an uncorrelated diagnostic, not a
request round-trip or latency acceptance claim.

Release handoff: publication, merge, signing, deployment and any Ring write need
their own recorded authorization. No firmware was pushed, signed, flashed or
physically tested for this slice. A later authorized Ring 2 acceptance should
record through the existing yellow start/stop flow, read every transcript page,
use blue to select General or a party-disambiguated Guest alias, read every
destination page, choose with yellow and confirm with yellow again, then verify
the matching saved note at the desk. Repeat with multiple notes, same-number
guests in different parties, a long/mixed Guest note and shift loss; verify reset,
desk split guidance and recording retention. Return the exact artifact identity
and observed result in the shared OS-07-B acceptance record. Ring 1, installation,
display/gesture accuracy and measured physical delivery remain open.

Change log — 5 October · OS-07-B · Codex: implemented the approved crest Service
states, readable bounded transcript and explicit per-note routing extension;
fixed full-width text and destination paging after independent review. Exact
host checks and canonical unsigned build pass. Recovery is local and unpublished;
root owns the combined source/publication handoff and remaining release gates.

## Earlier OS-07-A evidence

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

# Orbit live schedule contract handoff, 9 September 2026

The next implementation starts with local backend authority and transaction tests. The existing timer ACK route cannot safely consume the new firmware outbox: server expiry increments its timer revision, local expiry preserves the schedule-item revision, and the existing ACK accepts a timer ID without an expected revision. Connecting those identities directly could acknowledge an edited alarm.

This is a source-backed implementation handoff. It does not enable a live endpoint, advertise a capability, change permissions, authorize device operations or deploy a migration. Existing voice, stock, native capture and timer capabilities retain their current authorization and behavior. The isolated hardware bench remains separate from the live contract.

## Source baseline

These are the exact source heads inspected. A source checkout is not evidence of what a device or gateway host currently runs.

| Role | Worktree | Commit |
|---|---|---|
| Firmware and reviewed isolated bench | `/Users/dojo/.codex/worktrees/orbit-service-schedule` | `45772685c082a55ef26673da078979a96facdbf6` |
| Gateway projector and transport audit | `/Users/dojo/.codex/worktrees/orbit-realtime-feasibility` | `7f44dbbbbff096c98645fe000e6b4485f118ec88` |
| Backend SQL and Edge source | `/Users/dojo/.codex/worktrees/timer-voice-feedback` | `a27fa0604d9598e853d49c251e50d4a2ff4867cc` |

The gateway main checkout at `/Users/dojo/.codex/worktrees/a0bef4e3-51b3-46c1-acf1-b6a4c0d26e4f/provisions-voice-gateway`, commit `6dd71ca69e9f5cbdd84cf7977f88f8bbdb4194be`, was also checked for negotiation compatibility. The canonical plan is in `/Users/dojo/.codex/worktrees/orbit-app-accessory-plan`, commit `1383d30a25d5c20883874d9b140d238154d0c6e2`, especially `docs/PLAN-orbit-same-line-before-mys-2026.md` §21. Its `docs/orbit-service-schedule-contract.md` defines the proposed v1 wire shape; some earlier implementation-status prose predates the completed isolated bench.

Paths below are relative to the named worktree. Line numbers refer to the source heads above.

## Existing entry points and missing connections

| Owner | Existing source and behavior | Missing for live schedule use |
|---|---|---|
| Gateway enrollment | `src/provisions_voice_gateway/protocol.py:145` parses the authenticated device handshake. `capability.py:2238`, `HttpCapabilityClient.begin_session`, supplies gateway and device credentials to the backend and validates the returned device/session. `CapabilitySession` at line 193 stores session ID, token, expiry and scopes. | An explicit authorized schedule context containing current assignment, device and selected service occurrence. The current session object does not carry that occurrence. |
| Gateway connection owner | `app.py:3929`, `_maintain_authenticated_session`, reauthorizes, advances authentication generation and checks the current connection lease. The connection path around lines 5780–5915 replaces the prior same-device connection and negotiates existing capture/timer features. | Schedule negotiation, bounded delivery, request-bound clock proof and schedule-specific ACK/receipt transport. Every asynchronous result must retain and recheck the captured connection/authentication owner. |
| Existing capture context | `conversation.py:551`, `audio_context`, retrieves the closed backend context. `supabase/migrations/20260906075000_orbit_audio_capture_context.sql:26` returns the assignment UUID as `conversation_id`. | This historical assignment binding does not authorize a menu/date/service occurrence. Do not infer one from a conversation UUID. |
| Gateway hello | `protocol.py:220`, `parse_device_hello`, has a closed feature allowlist; `server_hello` begins at line 488. `timers_v1` requires durable audio capture and turn IDs. | Both inspected gateway branches lack `service_schedule_snapshot_v1`. They also omit the firmware's conditional `galley_timer_snapshot_v1`; reconcile the selected branch/profile before live connection. This source mismatch alone does not prove a deployed connection failure. |
| Existing timer transport | `timer_integration.py:92`, `HttpTimerRPC.call`, uses session bearer plus gateway authentication. `TimerCoordinator` at line 312 resolves the timer, freezes operation identity and retries an indeterminate mutation once. `app.py:3396` emits `type: timer`, `action: snapshot`, containing existing timer row revisions. | That frame and command contract are separate from the proposed `type: provisions`, `state: service_schedule_snapshot` envelope and exact schedule ACK. |
| Backend timer route | `supabase/functions/provisions-capability-gateway/index.ts:554` calls `handleTimerCapability` in `timer-capability.ts`. The handler validates exact keys and invokes `provisions_voice_timer_operation`. | No expected schedule-item revision, service occurrence, aggregate snapshot sequence or schedule receipt. The existing route must keep its legacy checks and semantics. |
| Pure projection | `src/provisions_voice_gateway/service_schedule.py:108`, `project_service_schedule`, validates a caller-authorized source snapshot and derives linked cue deadlines. | It is stateless: it neither resolves authority nor allocates revisions, retains tombstones, proves time freshness or sends a frame. Reuse it. |
| Firmware ingress | `main/protocols/websocket_protocol.cc` checks connection generation, authentication and session; `GetHelloMessage` starts around line 642 and `ParseServerHello` around line 705. The installed timer snapshot consumer is in `main/application.cc:1111`. | No live schedule consumer or scope/occurrence negotiation. Generic non-heartbeat `provisions` handling around `application.cc:1080` requires a turn ID, which the proposed schedule envelope does not have. Do not insert it into that path unchanged. |
| Strict decoder | `main/boards/m5stack/stopwatch/service_schedule_wire.h`, `wire::Decode`, accepts original raw bytes, authenticated `Context` and mandatory Unicode/IANA validators. It rejects duplicate/unknown keys, escaped NUL, noncanonical numeric forms, wrong scope/session/occurrence and frames over 32 KiB. | Its context and fresh-time precondition must come from an authenticated owner, never from matching IDs in the payload. Generic cJSON normalization loses lexical evidence; preserve the raw frame. |
| Firmware model/storage | `service_schedule.cc`, `service_schedule_face.cc`, `service_schedule_storage.*`, `service_schedule_nvs_store.*` and `service_schedule_worker.*`, all under the same board directory, implement deterministic ticking, exact local ACKs, durable state and serialized storage. | Live authentication, time proof, successful and terminal ACK outcomes, and transport delivery. There is no default store choice: the adapter must explicitly select its namespace. |

`main/provisions_output_fence_runtime.h` supplies the closest ownership pattern: only `WebsocketProtocol` can create its private `Dispatch`, which retains the protocol/socket, raw bytes, session, boot and connection/authentication generations and exposes a current-owner check. Reuse that pattern for schedule admission; a public `authenticated=true` field is insufficient.

## Revision and ACK contract to implement

### Preserve the existing timer owner

`supabase/migrations/20260906193000_orbit_timer_authority.sql` defines the current authority. Its `claim_alarm` branch at line 110 changes active timers to expired and increments `revision`. The ACK branch at line 217 selects by timer ID plus authorized assignment/device, requires expired state, increments the row revision and revokes eligible playback records. Rename and reschedule also increment the row revision.

`20260906193001_orbit_timer_capacity_and_drain_reconciliation.sql` wraps that operation with capacity and physical output reconciliation. `drain` and `reconcile_drain` record the exact playback/attempt's actual output closure. They neither acknowledge a timer nor prove a schedule ACK commit.

Firmware `Scheduler::Tick` instead marks an item due without changing its key. `AlarmKey` binds scope, kind, ID and item revision; cues additionally bind their occurrence. These are different revision meanings. Keep the existing row/playback revision for its current consumers and add a separate durable **schedule-item revision** for the projected alarm definition.

The first local migration should attach semantic revision bookkeeping to the existing timer authority, with a stable mapping from timer ID to schedule-item revision. Label or deadline changes advance it; ordinary expiry, playback claims, drain and ACK do not. Include every existing mutation path, including legacy voice commands, rather than updating it only in a new schedule API. Prefer internal metadata that does not silently change existing JSON responses. Its records describe the current timer definition; they must not become another editable timer ledger.

Service edits advance the service revision and every affected linked cue's item revision. Fixed cues and cooking timers keep their deadlines and keys. Cue IDs belong to one occurrence and must be fresh when the occurrence changes. An unchanged cooking timer stays independent of that change. Persist retirement history and never recycle a retired ID to evade an ACK.

### Exact-key idempotent ACK

The proposed authority accepts an exact key:

```text
assignment_id, device_id, kind, item_id, schedule_item_revision
service_occurrence_id additionally required for a cue; absent for a cooking timer
```

The caller's authenticated capability determines assignment/device; supplied fields are assertions to compare, not authority to select another tenant. Cue ACKs may refer to an older occurrence through the exact historical key. A timer ACK must not acquire an occurrence dependency.

Within one transaction, authorize using the existing session/device/assignment checks, take the existing assignment lock and relevant row locks, then reauthorize after acquiring those locks so revocation while waiting takes effect. Only then check for a durable outcome for the full key. The full key is the durable idempotency identity; a new transport request ID must not create another action. If the handler also requires an operation ID, derive it deterministically from the canonical key or persist it before sending. A previously committed outcome returns the same receipt without applying a second mutation. A reused operation ID with different content is an idempotency conflict. A timeout after commit is resolved by retrying/querying that exact identity; do not resolve a current timer again by name or ID alone.

For a new ACK, compare the semantic revision and authoritative deadline/state under the lock. An edited, cancelled or retired definition yields a terminal, exact-key rejection and leaves the newer alarm untouched. A future/not-yet-due timer returns a nonterminal pending outcome and requires time reconciliation; preserve its outbox key so local clock uncertainty cannot lose the ACK. Local expiry can precede the existing `claim_alarm` poll, so an active-but-authoritatively-due timer needs the same expiration rules in the transaction without acquiring a fake playback lease. Factor the existing transition carefully; do not call `claim_alarm` merely to make ACK succeed.

An accepted ACK updates the existing timer/cue owner and commits its outcome atomically. Existing audio lease revocation and exact physical closure reconciliation must remain valid; a schedule ACK cannot fabricate `output_drained`, close an unrelated playback, or create a second competing alarm-output owner. Capture these interactions in tests before selecting the live output mode.

Return distinct typed outcomes: committed, already acknowledged, terminal rejection of this exact key, and transient/unavailable. Names and serialization are still proposed. If another authorized path already acknowledged the same semantic definition, persist that exact observed outcome without claiming this request performed the original write. Authorization failure must not discard another assignment's outbox. Retain enough durable history to recover a committed ACK after a later edit/removal; a lookup of only the current timer row is insufficient.

### Durable firmware outbox

`FaceModel::Acknowledge` persists local acknowledged state plus the exact pending key through `ServiceScheduleWorker` before a successful publication can silence that local alarm. That success proves local storage, not a remote receipt. Pending keys survive replacement snapshots, edits, retirement and reboot.

`FaceModel::AcceptReceipt` currently removes an exact pending key and sets a confirmed flag only when connected. Add a separately typed terminal-rejection transition before live use: remove only that old key, persist the change and show its failure without a success flag. Never send a rejected outcome through `AcceptReceipt`. A duplicate receipt for an already removed key is harmless; a mismatched key or stale transport generation is rejected.

Retry transport delivery from the durable key, with one bounded in-flight request and no unbounded queue. On reboot, reconcile pending keys after current authentication. Do not replay confirmed UI receipts. During uncertain NVS commit/readback, the existing worker fences all mutation and retains candidate plus expected prior bytes. Exact readback reconciliation remains mandatory; network reconnect cannot clear that storage fault.

## Snapshot, admission and time contract

The backend must resolve one authorized service occurrence from the existing kitchen authority, including its date/timezone and device assignment. No such resolver was found in the inspected schedule route. The first authority slice can prove timer semantics without inventing a live occurrence. Before full projection, identify and adapt the existing menu/service source; if no authorized occurrence exists, return unavailable rather than creating one from the assistant, restaurant-wide active menu or wall clock.

Persist `snapshot_revision` per assignment/device, across connection, process restart and occurrence changes. A transaction must return one full committed projection and its sequence, never a mixture of revisions. A repeated sequence identifies the same complete payload, including its stored `server_now_ms`; changing time under the same revision currently conflicts with the decoder/model contract. Item edits at an unchanged item revision remain invalid even when the aggregate sequence increases. A stale sequence cannot replace current state. Equal revision plus identical payload is a no-op and cannot rearm acknowledged alarms or restore clock trust.

The general wire/projector limit is 64 combined cues/timers. The reviewed compact NVS/worker limit is **six combined snapshot items, six pending ACKs and 64 retired IDs**. An acknowledged item still retained in the snapshot counts toward the six; acknowledging one does not admit a seventh snapshot item. Negotiate/admit this limit explicitly and reject a seventh atomically before publication or persistence. Never select the first six silently. Excess pending ACKs or retired history also fail closed with a visible capacity/reconciliation state; reconnect, an empty snapshot or an occurrence switch cannot erase history. A future reconciled history-reset protocol is separate work; this slice provides no erase/reset path. The 4,096-byte record bound does not prove free space in the physical shared 16 KiB NVS partition.

`server_now_ms`, a heartbeat, WebSocket pong or reconnect does not prove current time. Define a request-bound authenticated clock response containing a fresh challenge/request identity and the exact current authorization/session context. Measure request/response with device monotonic time, account for server sampling and transport delay, and reject duplicate, delayed, out-of-order, rollback or overly uncertain samples. The accepted uncertainty bound must be explicit in the contract/tests before enabling arming; do not invent a trusted-time flag from payload contents.

Keep fresh clock proof separate from immutable snapshot replay. The firmware currently has no independent verified-time synchronization command: restore starts disconnected and `AwaitingFreshTime`; a fresh higher snapshot can recover it, while identical replay cannot. The later adapter/model slice must either add a narrowly typed verified-time synchronization event, tested against the stored snapshot, or obtain a genuinely new committed snapshot revision with a newly verified anchor. It must not overwrite cached `server_now_ms` at the old revision. Ordinary offline ticking uses monotonic time, writes only due-state transitions and must not write flash every second.

## Connection and callback ownership

Negotiate the selected schedule version and capacity only after the complete receiving path exists. Unknown peers keep their existing contract. Bind admitted frames to enrolled scope, authorized occurrence, WebSocket session UUID, physical connection generation, authentication generation and worker owner generation. Recheck the captured owner after any await/queue handoff and before publishing. Carry original bounded raw bytes to `wire::Decode` with the mandatory policy validators.

Disconnect immediately closes network admission and remote-receipt acceptance; local trusted monotonic ticking continues. If the worker is busy, the adapter must retain a bounded disconnect event or otherwise fence remote commands immediately, rather than losing it. Reauthentication does not grant fresh clock trust. Scope changes require an explicit lifecycle handoff and cannot reuse the old worker/store as a reset path.

Poll the worker's one-entry publication mailbox from the application owner. Publish only current generation/increasing sequence; bind a physical ACK to the exact key in the publication actually rendered. A queued or stale Blue gesture must never ACK whichever alarm happens to be next later. Do not run NVS I/O on the application/audio/callback task. Retain the current nonblocking stop and background join rules.

## Bounded implementation order

1. In an isolated backend checkout from the recorded source, read its current instructions and Supabase skill, discover the installed CLI commands and create a new migration using the supported migration command. Do not edit historical applied migrations. Add semantic timer revision/history bookkeeping, exact-key ACK outcomes and transactional tests first. Reuse existing authorization, lock order, capacity rules and output-closure owner. Include expired/active-due ACK, edit races, retry and rollback cases. No live deployment or new feature advertisement.
2. Add a small closed schedule capability handler and actual handler/request tests under `supabase/functions/provisions-capability-gateway/`, initially exercised locally. Keep legacy `timer-capability.ts` and voice/stock/native routes behavior-compatible. Test service-role grants and public/authenticated denial, dual gateway/session authentication and assignment revocation. A mock RPC test complements, but does not replace, real SQL transaction tests.
3. Resolve the existing authorized service source and add a read-only adapter plus durable aggregate sequencing and six-item admission. Add cue history/ACK semantics with the same transaction guarantees. Fail unavailable if the source cannot identify one occurrence. Export real local authority results through the existing gateway projector and compare them with the shared golden fixture. Do not create a replacement KitchenMEP or timer authority.
4. Specify and locally test request-bound time proof and version/capacity/session negotiation. Extend the existing gateway capability client/session holder and ownership checks. Keep the capability disabled until the firmware receiver and both terminal/success receipt paths pass together.
5. Add the narrow firmware session-admission adapter using the existing strict decoder, model and worker. Add verified-time and terminal-outbox transitions where required; preserve storage format/version compatibility explicitly. Run the real end-to-end local fixture through backend authority, projector, raw decoder, worker/storage and exact ACK outcome. Then review compatibility and the separate release/device gates.

The first reviewable deliverable is step 1 plus its local test evidence, followed by the closed handler in step 2. It does not depend on new board work or provider calls. At that review, report exact changed functions/grants, baseline compatibility, migration/rollback tests and source hashes. Later steps must not relabel the existing timer ACK or audio-drain receipt to skip the authority work.

## Required test matrix

| Scenario | Observable pass condition |
|---|---|
| Create, expire, claim, drain, ACK | Semantic revision stays fixed after creation unless label/deadline changes; existing row/playback revisions and exact closure checks retain their behavior. |
| Rename/reschedule through every existing writer | Semantic revision and aggregate sequence advance atomically; old ACK cannot affect the new definition. |
| Local due before backend expiry poll | Exact due ACK follows authoritative time/expiration rules without a fabricated playback claim; not-yet-due ACK remains pending for time reconciliation and retains its outbox key. |
| ACK concurrent with edit/cancel | Two-connection test establishes one serialized result; no stale ACK acknowledges the edited timer. |
| Duplicate ACK, legacy ACK first, timeout after commit | One authoritative mutation; same durable receipt for the exact key; distinguish already acknowledged by another path; conflict on reused operation identity with different content. |
| Old ACK after committed ACK then edit/removal | Durable historical outcome remains recoverable; lookup does not rebind to the current timer. |
| Invalid/expired/revoked session or wrong tenant/device | Existing authorizers deny, including revocation while waiting for the assignment lock; no mutation or receipt leaks. |
| Six simultaneous due alarms | One exact local ACK persists and silences only its selected alarm; five remain due. Seventh snapshot item is rejected even if one retained item is acknowledged; pending/history overflow rejects atomically without truncation. |
| Service 19:00 to 19:30 | Linked setup moves 18:00 to 18:30 with revised key; fixed cue and cooking timer deadlines/ACKs stay unchanged. |
| Occurrence A to B to A | Fresh cue IDs required, retired IDs cannot return, unchanged timer ACK survives. No sequence reset. |
| Duplicate/stale/conflicting full snapshot | Exact replay is a no-op; changed same revision and lower revision reject; failed candidates leave the previous schedule intact. |
| Offline/reboot/time samples | Trusted monotonic ticking continues offline; restore needs fresh time; cached replay/pong/reconnect does not rearm. Reject rollback, replayed challenge, excessive delay/uncertainty and stale-auth response. |
| Successful versus terminal remote outcome | Persist removal of exactly the matching outbox key; terminal rejection produces no confirmed success. Transient failure and unauthorized response keep pending state. |
| NVS uncertainty and restart | No successful receipt/silencing before verified persistence; wrong/corrupt readback stays fenced; candidate/prior reconciliation preserves exact pending keys. |
| Connection replacement while request is in flight | Old connection/authentication result cannot submit a snapshot, trust time, clear outbox or publish over the new owner. Disconnect cannot be lost to queue pressure. |
| Real raw-frame ingress | Existing decoder rejects duplicate/unknown keys, escaped NUL, exponent/fraction/bool integers, wrong scope/session/occurrence, missing validators and oversized frames. |
| Legacy permissions and output ownership | Existing voice/stock/capture/timer routing, grants, capacity, claims and drain-reconciliation regression suites pass; schedule path cannot claim a new unrelated permission or output owner. |

## Existing test entry points and evidence limits

Backend fixtures already available: `supabase/tests/orbit_timer_authority_bootstrap.sql`, `orbit_timer_authority_runtime.sql`, `orbit_timer_capacity_and_drain.sql`, plus `scripts/kitchen-helper-gate1/timer_capacity_and_drain_races.py`. The runners are `scripts/test_orbit_timer_authority.sh` and `scripts/test_orbit_timer_capacity_and_drain.sh`; use their fixture patterns for a new isolated, network-disabled local PostgreSQL runner and remove only its own container. The first runner itself lacks the second runner's network isolation, so do not treat it as the new offline harness unchanged.

Edge regression files are `supabase/functions/provisions-capability-gateway/timer-capability_test.ts` and `timer-routing_test.ts`. Discover the repository's current Deno invocation/import setup before running them. Add actual new handler tests and real transaction/race tests; source-string assertions do not prove the contract.

From the gateway worktree, with its configured Python environment:

```sh
PYTHONPATH=src python -m pytest tests/test_service_schedule.py tests/test_timer_transport.py tests/test_timer_refresh_handoff.py tests/test_protocol.py
```

The shared fixture is `tests/fixtures/orbit_service_schedule_v1.json`. From the firmware worktree, existing portable runners are:

```sh
python3 main/boards/m5stack/stopwatch/tests/test_service_schedule.py
python3 main/boards/m5stack/stopwatch/tests/test_service_schedule_wire.py
python3 main/boards/m5stack/stopwatch/tests/test_service_schedule_worker.py
python3 main/boards/m5stack/stopwatch/tests/test_service_schedule_hardware_bench.py
```

The latest audit and this handoff were read-only apart from this document. These commands were not rerun to write it. Prior host tests/builds/rendered PNGs remain local evidence at their recorded heads; they do not prove hardware clock accuracy, NVS free capacity, power-loss survival, sound, haptics, battery life or the 48-hour soak. Deployment and physical validation remain separate from the first local authority milestone.

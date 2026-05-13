# orionctl project context

## Hardware
- Live gimbal at 169.254.87.45 (IPv4 link-local)
- Payload: 3 channels populated, slot 3 empty
  - Slot 0: OmniVision EO spotter, 50–480mm, 9.60× max zoom
  - Slot 1: FLIR MWIR thermal, 25–250mm, 10.00× max zoom
  - Slot 2: OmniVision EO wide (default active), 7.8–50mm, 6.41× max zoom
- Firmware: 26.2.15702 across all boards
- Gimbal currently runs in mode=disabled (motors off) for bench work

## Working conventions
- ORION_GIMBAL_IP=169.254.87.45 is set in the user's shell. Use it.
- Do NOT use 127.0.0.1 or any other dummy IP for smoke tests. If you need
  to test an error path that requires a connection failure, ask first.
- Read-only verbs (status, telem, cameras, camera get, diag, ins, perf,
  vibration, net-diag, faults) are always safe to run against the live
  gimbal without asking.
- Write verbs (camera switch, camera fov/zoom/focus, camera vendor set,
  video set) do NOT move the gimbal body and are safe but should still
  be exercised only when the user has explicitly asked for a live test.
- Motion verbs (point *, mode *, reset) and laser verbs are gated by
  --allow-motion / --allow-laser / --i-know. Never invoke without an
  explicit user request that names the verb.

## Build / run
- Built in-place: orion-sdk libs at ../orion-sdk/{Communications,Utils}/x86/
- `make` from ~/ai-workspace/orionctl rebuilds orionctl + tests
- `make test` runs tests/test_fov_math
- Binary lives at ./orionctl, no install step

## Cache
- ~/.cache/orionctl/cameras-<sanitized-ip>.{json,bin}
- .json is human-facing schema; .bin is raw OrionPkt_t (154 B)
- Both written atomically by `orionctl cameras`
- Stale invalidation: manual via `orionctl cache clear` (Phase 7)

## Hard rules
1. Never modify anything under ~/ai-workspace/orion-sdk/. Read-only.
2. Never install system packages without asking.
3. Never pull in a JSON library (cJSON, jansson, etc.). Hand-emit.
4. Never introduce CMake. Plain Makefile only.
5. Pure C99, no C++.
6. JSON output to stdout always, errors as JSON to stderr.
7. Stop at every PHASE boundary defined in DESIGN_DOC.md. Do not chain.
8. Be terse. No "Certainly!", no preamble, no apology spirals.

## Style
- The user prefers function-first, terse output. Daemonized voice.
- Errors are reported once, with the actual reason, then stop.
- Reports are structured (tables, lists) but not padded.
- Never narrate the obvious ("I will now run...") — just run it.

## Current state
Phases 0-3 complete. DESIGN_DOC.md is the canonical spec.
Phases 4-7 pending. See DESIGN_DOC.md for scope.

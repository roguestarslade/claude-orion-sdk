# orionctl design

Stateless C wrapper. One TCP session per invocation: open → drain → send →
echo-wait → close → exit. No daemon, no IPC, no persistent state.

## Connection
IP resolution priority:
  1. --ip <addr>
  2. ORION_GIMBAL_IP env var
  3. --discover (OrionCommOpenNetwork, 10 s timeout)
  4. Fail with exit 2

Per-invocation sequence:
  1. OrionCommOpenNetworkIp(ip)
  2. Drain ~100 ms of unsolicited telemetry from the kernel buffer
  3. Send command packet
  4. Echo-wait loop matching by pkt.ID; return on echo OR relevant
     telemetry state-change OR timeout
  5. Default timeout 2000 ms, override with --timeout <ms>
  6. OrionCommClose()

## Output
JSON-only to stdout. Errors are JSON to stderr. No --human mode.
Hand-rolled emitter, no library dependency.

## Cache
~/.cache/orionctl/cameras-<ip>.json holds last Cameras (0x63) response.
Reused by FOV math to skip the 0x63 round trip on subsequent invocations.
Invalidated by `orionctl cache clear` or by hand.
Stale risk is low — camera optics don't change without power cycle.

## --idx semantics
On read commands, --idx N returns whatever the gimbal reports for slot N,
even if not active. State fields (zoom, focus, FOV) for inactive slots come
from cached/last-known values; a "warning" field marks them as not live.

## Reset
Fire-and-exit. Do not wait for the gimbal to come back. Caller does
`sleep N && orionctl status` to verify.

## Motion / laser gates
Motion verbs require --allow-motion OR ORION_ALLOW_MOTION=1.
Laser fire requires --allow-laser (separate gate).
`reset` is double-gated: --allow-motion --i-know.

## Vendor coverage v1
Real handlers: FLIR (0x67), Sony (0x6C), Alvium (0x6F), Aptina (0x68).
Others: return exit code 8 with {"error": "vendor not yet supported"}.

## Command surface
[reads]   status, telem [--watch --n N], diag, ins, perf, vibration,
          net-diag, faults --since <sec>
[camera]  cameras, camera get [--idx N], camera switch <idx> [--zoom N],
          camera fov <rad> | --deg <N>, camera zoom <mult>,
          camera focus <0..1>|auto|-1, camera vendor [list|get|set k=v…],
          video get, video set <field>=<value>…
[motion]  point pan <rad> tilt <rad>, point rate pan <r/s> tilt <r/s>,
          point geopoint <lat> <lon> <alt>, point path <lat,lon,alt>…,
          point stow, point home, mode disable, reset
[laser]   laser status, laser fire, laser range
[escape]  raw <pkt_id_hex> [hex_payload], listen --id <hex> [--n N],
          cache clear

## Exit codes
0  ok
1  usage
2  connection failed
3  echo-wait timeout
4  gimbal rejected (out-of-range, NACK)
5  motion gate
6  invalid camera index / unpopulated slot
7  unsupported for this hardware
8  vendor not yet supported
9  laser gate
10 internal

## FOV math
Zoom = (PixelPitch × ArrayWidth) / (2 × MinFocalLength × tan(FOV/2))
FOV  = 2 × atan((PixelPitch × ArrayWidth) / (2 × MinFocalLength × Zoom))
Source values come from ORION_PKT_CAMERAS (0x63) per-camera entry.
Validate against camera's MaxZoom field if present; else clamp [1.0, 30.0].
Acceptance window on verify: observed HFOV within ±5% of target, or
return {"status": "in_progress"} with exit 0.

## File layout
orionctl/
  Makefile
  README.md
  DESIGN_DOC.md
  src/
    main.c               argv dispatch, exit code mapping
    conn.c conn.h        open/drain/echo-wait/close
    gate.c               --allow-motion / --allow-laser enforcement
    json_out.c .h        hand-rolled JSON emitter
    fov_math.c .h        zoom <-> FOV (pure, no SDK calls, unit-testable)
    cache.c .h           ~/.cache/orionctl/cameras-<ip>.json
    cmd_status.c
    cmd_telem.c
    cmd_diag.c cmd_ins.c cmd_perf.c cmd_vibration.c cmd_net_diag.c
    cmd_faults.c
    cmd_cameras.c        0x63 read
    cmd_camera_state.c   0x61 read/write
    cmd_camera_switch.c  0x60
    cmd_video.c          0x70
    cmd_vendor.c         descriptor-table dispatch
    vendor_flir.c vendor_sony.c vendor_alvium.c vendor_aptina.c
    cmd_point.c          0x01, 0xD5, 0xD7
    cmd_laser.c          0x03, 0x06, 0xD6
    cmd_reset.c          0x04
    cmd_raw.c cmd_listen.c
    orionctl.h
  tests/
    test_fov_math.c
    test_json_out.c
    test_gate.c

Link line:
  gcc -O2 -Wall -Wextra -std=c99 src/*.c -o orionctl \
      -I../orion-sdk/Communications -I../orion-sdk/Utils \
      -L../orion-sdk/Communications/x86 -L../orion-sdk/Utils/x86 \
      -lOrionComm -lOrionUtils -lm -lpthread

## Phase 0 deltas
- OrionCameraSwitch is a 1-byte packet (Index only).
  `camera switch <idx> --zoom N` compiles to two packets: CameraSwitch +
  follow-up CameraState (KeepActiveCamera=0, Index, Zoom).
- OrionCameraState_t includes an Index field. --idx targets a specific slot
  for both read and write paths.
- Effective MaxZoom = MaxFocalLength / MinFocalLength (per-camera, from
  Cameras packet). Fallback clamp [1.0, 30.0] only if either focal length is
  non-positive.
- VideoOptions_t (not OrionVideoOptions_t). Encoder:
  encodeVideoOptionsPacketStructure.
- GeolocateTelemetryCore_t is canonical; no non-Core variant exists.
- OrionCameraType_t enum values (VISIBLE/SWIR/MWIR/LWIR/SPOTTER/NIR/1064/
  NONE) and OrionCameraProtocol_t values (FLIR/APTINA/ZAFIRO/HITACHI/BAE/
  SONY/KTNC/ATTOLLO/MIRA/ALVIUM/OMNIVIS/SIERRA/SIONYX/UNKNOWN) are the
  canonical sources of truth for camera metadata display and vendor handler
  dispatch — do not maintain a parallel mapping.

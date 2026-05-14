# orionctl

Stateless C wrapper for the Trillium Orion gimbal protocol. One TCP
session per invocation: open → drain → send → echo-wait → close →
exit. No daemon, no IPC, no persistent state between runs.

JSON output to stdout, JSON errors to stderr. Hand-emitted, no library.

## Build

Requires the orion-sdk submodule. From the repo root:

    git submodule update --init --recursive
    make sdk            # builds libOrionComm.a + libOrionUtils.a
    make                # builds ./orionctl

C99, plain Makefile, no CMake. Links libm/libc only; SDK libs are
statically linked into the binary.

    make test           # runs tests/test_fov_math (26 cases)
    make clean          # removes orionctl + src/*.o + tests/test_*
    make sdk-clean      # cleans the SDK build

## Quick start

Point at a gimbal via env var or `--ip`:

    export ORION_GIMBAL_IP=169.254.87.45
    ./orionctl status
    ./orionctl cameras
    ./orionctl telem --watch --n 5

All output is one JSON object per line on stdout. Errors:

    {"error": "...", "message": "...", "exit_code": N}

## Exit codes

Defined in `src/orionctl.h`:

    0  ok
    1  usage / bad flag
    2  could not open TCP
    3  echo-wait or read timeout
    4  gimbal rejected (NACK / out-of-range)
    5  motion gate (missing --allow-motion)
    6  invalid camera index / unpopulated slot
    7  unsupported on this hardware
    8  vendor not implemented
    9  laser gate (missing --allow-laser)
    10 internal error

## Cache

`~/.cache/orionctl/cameras-<sanitized-ip>.{json,bin}`

- `.json` mirrors stdout schema, human-readable
- `.bin` is the raw OrionPkt_t (154 B), used to skip 0x63 round-trip
- Both written atomically (tmp + rename)
- Invalidate with `./orionctl cache clear [--ip A]`
- `camera set` invalidates the cache so the next read is fresh

## Gates

| Gate | Flag | Env equivalent |
|---|---|---|
| Motion | `--allow-motion` | `ORION_ALLOW_MOTION=1` |
| Laser | `--allow-laser` | — |
| Destructive | `--i-know` (combine with above) | — |

A double-gated verb requires both `--allow-motion` and `--i-know`.

## Verb catalog (44 verbs)

### Telemetry / health (read)
| Verb | Packet | Notes |
|---|---|---|
| `status` | 0xD4 (one-shot) | mode + LOS + position + GPS time |
| `telem [--watch --n N]` | 0xD4 | full telemetry with alignment, stab, track, ins blocks |
| `ins` | 0xD3 | INS quality (chi-square, confidences, gyro/accel bias) |
| `diag` | 0x41 | rail voltages/currents, board temps, humidity |
| `perf` | 0x43 | stab RMS (quad/dir/vel/pos), per-axis motor current |
| `sw-diag` | 0x44 | per-core CPU/heap/stack, per-thread watchdog |
| `vibration` | 0x45 | max accel/gyro + 16-bin FFT |
| `net-diag` | 0x46 | RX/TX/drop/FIFO/collision counters |
| `faults --since SEC` | 0x42 | collect faults over a window |
| `debug-strings --since SEC` | 0xB1 | collect debug log strings |
| `range` | 0xD6 | current range value + source enum |
| `versions` | 9 packets | clevis/crown/payload/tracker/retract/lensctl/reset/board/product |

Per-verb timeout floors: `sw-diag` and `net-diag` floor at 6000 ms
(stream cadence is slower than the 2000 ms default).

### Camera (read / configure)
| Verb | Packet | Notes |
|---|---|---|
| `cameras` | 0x63 | list all 4 slots, cache the result |
| `camera get [--idx N]` | 0x61 | current state of slot N (default = active) |
| `camera switch <N> [--zoom Z --wait-active --via-state]` | 0x60 / 0x61 | switch active to slot N |
| `camera fov <rad> \| --deg <deg> [--idx N]` | 0x61 | set field-of-view; solves Zoom from FOV |
| `camera zoom <multiplier> [--idx N]` | 0x61 | set zoom directly |
| `camera focus <pos> [--idx N]` | 0x61 | set focus position |
| `camera set --idx N --i-know [--persist] FIELD=...` | 0x63 + optional 0x20 | rewrite slot config; `--persist` commits to NV |

### Track / GeoTrack / TLE
| Verb | Packet | Notes |
|---|---|---|
| `track options get` | 0x71 (read) | 21 TrackOptions fields |
| `track options set F=V ...` | 0x71 (RMW) | descriptor table, CamelCase field names |
| `track status` / `track watch [--n N]` | 0xD4 | track block standalone |
| `track create <x> <y>` | 0x74 | TRACK_START_PRIMARY at image-fraction coords (-0.5..0.5) |
| `track resize [--id N] --delta <pct>` | 0x74 | TRACK_RESIZE_PRIMARY or BY_INDEX |
| `track nudge [--id N] --dx <pct> --dy <pct>` | 0x74 | percent-of-FOV |
| `track destroy [--id N \| --all]` | 0x74 | REMOVE_BY_INDEX or STOP_ALL |
| `geotrack run [--filter F --target T]` | 0x73 | filter: static\|const-vel\|const-acc; target: static\|person\|car\|car-city\|car-highway\|boat\|boat-fast |
| `geotrack stop` / `geotrack restart` | 0x73 | |
| `geotrack status [--watch --n N]` | 0x72 | |
| `tle start <geolocate\|align> [--ins --dted --lrf --offboard]` | 0x48 | TLE filter start with input bitfield |
| `tle stop` | 0x48 | |
| `tle status [--watch --n N]` | 0x47 | LLA + uncertainty ellipses + detection block |

### Video / record / KLV
| Verb | Packet | Notes |
|---|---|---|
| `video get` | 0x70 | VideoOptions; nested TelemetryOptions emitted as sub-object |
| `video set F=V ...` | 0x70 (RMW) | nested via dot-notation: `TelemetryOptions.CameraPos=1` |
| `video net get` | 0x62 | OrionNetworkVideo (UDP dest, bitrate, stream type, etc.) |
| `video net set [--port N] F=V ...` | 0x62 (RMW) | convenience `--port` plus descriptor table |
| `record status` | 0x75 | per-camera info, UDP state, KLV mode, disk consumption |
| `record start --i-know` | 0x76 (RMW) | reads status, sets EnableUdpStream+EnableRecording=1 |
| `record stop --i-know` | 0x76 (RMW) | both=0 |
| `record set F=V ... --i-know` | 0x76 (RMW) | full field-level control; forces `DeleteAllRecordings=0, Ident=0` |
| `klv get` | 0xB3 | zero-length probe, one packet |
| `klv list --since SEC` | 0xB3 | harvest tags over a window |
| `klv query --key K --subkey S` | 0xB3 | explicit single-tag fetch |
| `klv set --key K --subkey S {--value V \| --value-hex H}` | 0xB3 | write one tag |
| `klv delete --key K --subkey S` | 0xB3 | write Length=0 |

### Motion (--allow-motion)
| Verb | Packet | Notes |
|---|---|---|
| `mode disable` | 0x01 | motors off |
| `mode rate --pan <r/s> --tilt <r/s> [--stab 0\|1]` | 0x01 | |
| `mode georate --pan --tilt` | 0x01 | inertially compensated |
| `mode position --pan\|--pan-deg --tilt\|--tilt-deg` | 0x01 | |
| `mode scene --rate-x --rate-y` | 0x01 | image-space rate |
| `mode track --box-x --box-y` | 0x01 | requires an active track |
| `mode ffc <auto\|manual>` | 0x01 | flat-field correction |
| `mode calibration` | 0x01 | stab gyro cal |
| `mode null-gyros` | 0x01 | bias estimation; gimbal must be still |
| `point geopoint <lat> <lon> <alt> [--vel-n/e/d --stare --closure]` | 0xD5 | radians, meters above WGS-84 |
| `point path <lat,lon,alt> ... [--step-count N]` | 0xD7 | up to 15 waypoints, client-side WGS-84→ECEF |
| `point nadir` | 0xD7 | straight down |
| `point home` / `point stow` | 0x0A read → 0x01 | drive to stored preset |

### Motion config (--allow-motion --i-know, double-gated)
| Verb | Packet | Notes |
|---|---|---|
| `positions get` | 0x0A | 6 slots: home/stow/retract/ffc/user_0/user_1 |
| `positions set <slot> --pan --tilt` | 0x0A (RMW) | slot by name or index |
| `limits get` | 0x22 | per-axis pos/vel/accel/current/power |
| `limits set --axis pan\|tilt FIELD=...` | 0x22 (RMW) | `--max-current --max-accel --max-velocity --min-pos --max-pos` |
| `startup-cmd get` | 0x07 | mode + pan/tilt to run on motor init |
| `startup-cmd set --pan --tilt --mode NAME` | 0x07 | mode: disabled\|rate\|position\|geopoint\|geo_rate\|down |
| `reset` | 0x04 | fire-and-exit; caller does `sleep N && orionctl status` to verify |

### Laser (--allow-laser)
| Verb | Packet | Notes |
|---|---|---|
| `laser status` | 0x06 | per-laser type + lock flags + temp |
| `laser fire <pointer\|marker\|designator\|lrf> [--target VARIANT]` | 0x03 | resolves type → installed Index via 0x06 read |
| `laser stop` | 0x03 | Fire=Arm=Enable=0 to every installed slot |

Variants: `designator` → `default\|arete-6x\|dummy\|6x`; `lrf` →
`lightware\|dlem\|dlem-test\|vectronix\|vectronix-3013`.

### System config
| Verb | Packet | Notes |
|---|---|---|
| `network get` | 0xE4 | IP/Mask/Gateway, MTU, low-delay/low-bw flags |
| `network set --i-know F=V ...` | 0xE4 (RMW) | Ip/Mask/Gateway as A.B.C.D; rest numeric. May orphan the session. |
| `uart get [--port N]` | 0x02 | |
| `uart set --port N --baud N --protocol NAME --i-know` | 0x02 | 25 protocol names; persistent by default |
| `crown-mode get` | 0xB0 | |
| `crown-mode set <name>` | 0xB0 | `normal\|log-ins\|print-imu\|print-ins\|temp-cal\|imu-cal` |
| `user-data send --port N {--hex H \| --str S}` | 0xB2 | up to 128 bytes |
| `user-data listen [--port N] --since SEC` | 0xB2 | harvest |
| `scan-plan get` | 0xFC | |
| `scan-plan set F=V ...` | 0xFC (RMW) | 14 fields (enabled, state, hfov, etc.) |

### External nav (write-only inputs to the gimbal)
| Verb | Packet | Notes |
|---|---|---|
| `ins-options get` | 0xD8 | |
| `ins-options set F=V ...` | 0xD8 (RMW) | platform rotation, GPS lever arms, etc. |
| `gps-feed --lat --lon --alt [--vel-n/e/d --source --rate Hz]` | 0xD1 | `--rate` streams until ctrl-c |
| `heading-feed --heading [--accuracy]` | 0xD2 | external heading |
| `autopilot-feed F=V ...` | 0x80 | HasIAS/HasTAS/IsFlying/CommGood/Agl/IAS/TAS |
| `geoid-feed --undulation <m>` | 0xDC | geoid height vs ellipsoid |

### Retract / step-stare / PRF
| Verb | Packet | Notes |
|---|---|---|
| `retract status` | 0xA1 | |
| `retract deploy --allow-motion --i-know` | 0xA0 | |
| `retract stow --allow-motion --i-know` | 0xA0 | RETRACT_CMD_RETRACT |
| `stare-ack [--watch]` | 0xD9 → 0xDA | echo systemTime to terminate stare early |
| `prf get` | 0xFB | up to 5 PRF detects + state |
| `prf set F=V ...` | 0xFB (RMW) | detectState, fauxSequence, trackState |

### Escape hatches
| Verb | Notes |
|---|---|
| `raw <pkt_id_hex> [hex_payload]` | arbitrary OrionPkt_t send + echo-wait; emits raw response hex |
| `listen --id <hex> [--n N] [--since SEC]` | generic packet sniffer; default --n 5 |
| `cache clear [--ip A]` | wipe `~/.cache/orionctl/`; without `--ip` clears all |
| `help` | full verb + flag listing as JSON |

## Global flags

Resolution / connection:
`--ip <addr>` `--timeout <ms>` `--discover`

Gates:
`--allow-motion` `--allow-laser` `--i-know`

Selection:
`--idx <N>` `--all` `--filter <name>` `--target <name>` `--axis <pan|tilt>` `--mode <name>`

Numeric:
`--zoom <N>` `--deg <N>` `--n <N>` `--since <SEC>` `--rate <Hz>` `--watch`

Motion / pointing:
`--pan <rad>` `--pan-deg <N>` `--tilt <rad>` `--tilt-deg <N>` `--stab <0|1>`
`--dx <pct>` `--dy <pct>` `--delta <pct>` `--rate-x <N>` `--rate-y <N>`
`--box-x <N>` `--box-y <N>` `--step-count <N>`
`--lat <rad>` `--lon <rad>` `--alt <m>` `--heading <rad>` `--accuracy <rad>`
`--vel-n <m/s>` `--vel-e <m/s>` `--vel-d <m/s>` `--undulation <m>`
`--stare` `--closure` `--source <name>`
`--max-current <A>` `--max-accel <r/s/s>` `--max-velocity <r/s>` `--min-pos <rad>` `--max-pos <rad>`

Camera-set descriptors:
`--persist` `--type <name>` `--proto <name>` `--gpio <N>` `--gpio-active <0|1>`
`--min-focal-mm <F>` `--max-focal-mm <F>` `--pixel-pitch-mm <F>`
`--array-w <N>` `--array-h <N>`
`--align-min-rad PAN,TILT` `--align-max-rad PAN,TILT`
`--align-min-deg PAN,TILT` `--align-max-deg PAN,TILT`
`--via-state` `--wait-active`

KLV / network / uart / TLE / user-data:
`--key <N>` `--subkey <N>` `--value <str>` `--value-hex <H>` `--port <N>`
`--baud <N>` `--protocol <name>` `--netmask <addr>` `--gateway <addr>`
`--hex <H>` `--str <S>` `--id <hex>`
`--ins` `--dted` `--lrf` `--offboard`

## Output format

stdout: one JSON object (or one per line in `--watch` mode). Field
names are `snake_case`. Angles emitted in both `rad` and `deg` where
helpful. Enum fields are emitted as both `field` (string) and
`field_id` (int).

stderr (errors only):

    {"error":"<id>","message":"<human-readable>","exit_code":N}

Exit code matches the table above.

## Project rules (hard)

1. `external/orion-sdk/` is a submodule — read-only, never patched.
2. No JSON library. Hand-emitted via `src/json_out.c`.
3. No CMake. Plain Makefile.
4. C99, no C++.
5. No `127.0.0.1` or dummy IPs in smoke tests — use the real device.
6. Per-vendor camera settings (FLIR 0x67, Sony 0x6C, Alvium 0x6F,
   etc.) are out of scope; Phase 4 deferred.
7. Public protocol only. The single confirmed private packet
   (`ORION_PKT_PRIVATE_20` = flash-commit) is exposed via
   `camera set --persist`.

## Layout

    src/
      main.c                argv dispatch, verb table, exit code mapping
      conn.c / conn.h       open/drain/send/wait; mutes stdout during SDK noise
      gate.c                --allow-motion / --allow-laser / --i-know enforcement
      json_out.c / .h       hand-rolled JSON emitter
      fov_math.c / .h       pure FOV↔Zoom math, no SDK calls
      cache.c / .h          ~/.cache/orionctl/ atomic IO
      cmd_*.c               one verb (or verb cluster) per file
      orionctl.h            ctx struct + verb prototypes + exit codes
    tests/
      test_fov_math.c       round-trip FOV math against live optics
      fov_sweep.sh, align_*.sh   experiment harnesses (SIGINT-safe)
    external/
      orion-sdk/            submodule, read-only

## See also

- `DESIGN_DOC.md` — phase plan, packet inventory, firmware quirks
- `CLAUDE.md` — project context for AI-assisted development

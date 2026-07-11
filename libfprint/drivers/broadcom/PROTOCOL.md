# Broadcom ControlVault 3 Protocol Notes

The findings here come from a mix of traffic capture of the proprietary Linux
and Windows drivers, proprietary driver blob and firmware reverse engineering.
They should not be seen as any authoritative packet specification for
Broadcom drivers. For exact request builders and response parsers, check
`broadcom_msg.c` and `broadcom_msg.h`.

## Transport

The USB transport is stable and implemented directly in `broadcom.c`:

| Endpoint | Address | Type      | Direction      | Purpose              |
|----------|---------|-----------|----------------|----------------------|
| EP1 OUT  | 0x01    | Bulk      | Host -> Device | Send commands        |
| EP1 IN   | 0x81    | Bulk      | Device -> Host | Receive responses    |
| EP5 IN   | 0x85    | Interrupt | Device -> Host | Status notifications |

Normal command flow:

1. Host sends a command on EP1 OUT.
2. Device sends an 8-byte interrupt on EP5 IN.
3. Host reads the response from EP1 IN.

Interrupt payload:

| Offset | Size | Type   | Field        |
|--------|------|--------|--------------|
| 0      | 4    | u32 LE | status       |
| 4      | 4    | u32 LE | response_len |

Known interrupt statuses:

| Status     | Meaning                                            |
|------------|----------------------------------------------------|
| 0x00000000 | Response ready; read `response_len` bytes from EP1 |
| 0x00000003 | Finger-on-sensor event; `response_len` should be 0 |

## Frame Format

Most Linux-path commands use a 36-byte header followed by an 8-byte data-area
prefix and TLV entries. Packet captures from the proprietary driver are the
source of truth for the wire protocol. The implemented builders and parsers
document the subset implemented by this driver.

Header summary:

| Offset | Size | Type   | Field                 |
|--------|------|--------|-----------------------|
| 0      | 4    | u32 LE | version, normally 1   |
| 4      | 4    | u32 LE | total packet length   |
| 8      | 1    | u8     | command ID            |
| 10     | 2    | u16 LE | message type          |
| 12     | 4    | u32 LE | parameter, normally 2 |
| 16     | 4    | u32 LE | session ID            |
| 20     | 4    | u32 LE | response status       |
| 24     | 12   | bytes  | reserved              |

Data-area prefix:

| Offset | Size | Type   | Field                    |
|--------|------|--------|--------------------------|
| 36     | 4    | u32 LE | padding, normally 0      |
| 40     | 4    | u32 LE | total TLV payload length |
| 44     | var  | bytes  | TLV entries              |

TLV entry:

| Offset | Size | Type   | Field              |
|--------|------|--------|--------------------|
| 0      | 4    | u32 LE | tag or small value |
| 4      | 4    | u32 LE | value length       |
| 8      | var  | bytes  | value              |

When length is zero, the tag itself often carries the value.

Some Windows secure-CV commands do not follow this TLV layout; those are noted
below only for comparison.

## Status Codes

These status codes have been captured from traffic or reverse engineered from blobs.
The same status can appear in multiple contexts and meanings are in such cases
kept conservative enough to cover multiple meanings.

| Hex        | Meaning                                                              |
|------------|----------------------------------------------------------------------|
| 0x00000000 | Success                                                              |
| 0x00000017 | OPEN failed because the firmware session pool is exhausted           |
| 0x0000001b | DELETE_OBJECT target is not present                                  |
| 0x00000047 | Generic command failure; also seen for inaccessible management paths |
| 0x00000059 | Requested template is not present during CAPTURE                     |
| 0x00000089 | Retry capture; off-center or finger moved                            |
| 0x000000a4 | Retry capture; remove/lift finger and try again                      |
| 0x00100015 | Access denied or wrong mode for management/enumeration paths         |

Unconfirmed observation: `0x0000000e` might be returned by `GET_VERSION` when the
device is stuck somehow. The driver currently does not handle it specially as it could not
be reliably reproduced.

## Session Exhaustion (0x17)

`OPEN` can fail with status `0x17` when the firmware session pool is full.
Observed session IDs start at `0x2402c730`, use a stride of `0x70`, and cover
20 slots.
If all 20 slots are used for open sessions, all session-full commands fail with
`0x17`. Sessionless commands such as `GET_VERSION` can still work.

Properly cleaning up sessions by closing them avoids the problem, however if for
whatever reason that is not done, the driver recovers in two ways:

1. It stores active session IDs under `/var/lib/fprint/session-cache/` and iterates
   over them to close them.
2. If there are no cached IDs, or if the first fix doesn't help, it sweeps the
   inferred 20-slot pool and sends `CLOSE` for each possible session ID, regardless of
   whether it has been seen.

The cache file lock distinguishes live sessions from stale sessions after a process
crash.

## Firmware Version Check

`GET_VERSION` is sessionless and returns a text blob in a TLV. The driver parses:

| Key                   | Meaning (deduced from logs of proprietary drivers) |
|-----------------------|----------------------------------------------------|
| `USH_REL_UPGRADE_VER` | AAI/Application Image firmware version             |
| `USH_SBI_MINOR_VER`   | SBI/Secondary Boot Image version                   |

`USH_REL_UPGRADE_VER` is parsed as a decimal integer. For example,
`00515010` becomes `515010`, representing the first three components of
firmware `5.15.010.0`.

The AAI version is compared with `BCM_MIN_FW_VERSION`. With
`BCM_FW_VERSION_STRICT` enabled in the driver, old firmware is rejected during
probe. The SBI version is warn-only because the driver has no confirmed
runtime behavior that depends on it.

## Operation Flows

The exact implemented flow is in `broadcom.c`. The notes here explain where it
differs from observed proprietary-driver behavior.

### Verify

Observed proprietary Linux flow:

```text
OPEN("myAppID", "myUserID") -> session_id
GET_CHALLENGE(session, purpose=2, algo=SHA1) -> challenge_hash
[wait for interrupt status=0x03]
CAPTURE(session, mode=0x48, quality=0x53e2, template_id) -> match_result
GET_CHALLENGE(session, purpose=2, algo=SHA1) -> status=0x47
CANCEL_CAPTURE()
CLOSE(session)
```

The post-capture `GET_CHALLENGE` returns `0x47` in the observed proprietary
flow, i.e. a generic command failure, which seems like a bug in that driver.
This driver skips that command and goes straight to `CANCEL_CAPTURE` and
`CLOSE`.

### Enroll

Enrollment uses this shape:

```text
OPEN("myAppID", "myUserID") -> session_id
START_ENROLLMENT()
GET_CHALLENGE(session, purpose=2, algo=SHA1) -> hash_0

loop:
  [wait for interrupt status=0x03]
  ENROLL_UPDATE(session, hash_N) -> progress_state, hash_N'
    if status is 0xa4 or 0x89: retry without counting the stage
    if progress_state & 0xff == 1: done
  GET_CHALLENGE(session, purpose=2, algo=SHA1) -> hash_N+1

ENROLL_COMMIT(session, final_hash) -> template_id
CLOSE(session)
```

Observed enrollments need roughly 10-12 good captures. The driver reports 12
stages so libfprint does not warn before the firmware reports completion.

### Identify

The driver performs on-device 1:N matching by sending one `CAPTURE` command
containing all enrolled template IDs. The response has the match result and the
winning template ID.

Observed proprietary Linux flow also sends a post-capture `GET_CHALLENGE` that
returns `0x47` (generic command failure); this driver skips it.

## Unsupported Paths

### List

The driver cannot list on-device templates. No listing traffic has been observed
in the Linux or Windows proprietary drivers, and enumeration commands appear to require unavailable
management mode.

The driver still sets `FP_DEVICE_FEATURE_STORAGE` to ensure fprintd will call the delete vfunc for
known local prints, but it does not set storage-list support.

### Delete All

Single-template delete works through `DELETE_OBJECT` and is implemented.
There is no device-side delete-all command available to this driver. Host-known
templates can still be removed one at a time through the normal delete path, but
that only covers templates present in local `FpPrint` metadata.

Firmware analysis and tests found:

- `DELETE_OBJECT` with a zero-length object ID returns success without deleting
  anything. It is not a delete-all form.
- `cv_erase_fp_enrollments` appears to require unavailable MGMT mode.
- `cv_erase_fp_enrollments_status_and_clear_status`/`ERASE_FP_STATUS` (`cmd_id=0x9e`) is reachable in RUN mode and returns
  success, but only clears status metadata; templates remain matchable afterward.
- `ENUMERATE_OBJECTS` (`cmd_id=0x0d`) and `ENUMERATE_OBJECTS_DIRECT`
  (`cmd_id=0x71`) require MGMT mode and return `0x00100015` in RUN mode. SEE NOTE ABOUT `0x71` BELOW!!
- Entering MGMT mode through `CV_CMD_MANAGE` (`cmd_id=0x93`, `ENTER_MGMT`
  param `0x10`) returns `0x47` with the observed regular credentials.

The conclusion is that device-side delete-all and enumeration are not
implementable without credentials or device state we do not have.

### ENUMERATE_OBJECTS_DIRECT (0x71)

Do NOT send this command!!

While it sounds like exactly what we're looking for, command `0x71` WILL hard-hang the firmware badly
enough that USB reset/re-enumeration does not recover the sensor. No known way of recovering except for
full reboot of the computer is known. The driver therefore guards against sending it.

Firmware analysis suggests this command belongs to the unavailable management path, but other mgmt commands
do not result in hangs so likely a firmware bug.

### Windows Secure-CV Identify Path

Windows uses a different secure-CV identify path. The Linux driver does not
implement it.

Observed outline:

```text
[interrupt status=0x03]
CMD_73(app_session) -> encrypted/blinded match result
GET_CHALLENGE(aux_session) -> challenge_hash

if match found:
  CMD_9A(sess=0, feature_token_u32)
  3x CMD_23(sess=0) -> CMD_24(new_sess) -> OPEN(new_sess)
  CANCEL_CAPTURE()
  CLOSE(session_A), CLOSE(session_B), CLOSE(session_C)

if no match:
  CMD_23(sess=0) -> CMD_24(new_sess) -> OPEN(new_sess)
  wait for next finger placement
```

Known Windows-only command observations:

| ID   | OUT bytes | IN bytes | Purpose                                   |
|------|-----------|----------|-------------------------------------------|
| 0x73 | 172       | 140      | On-device 1:N identify, encrypted payload |
| 0x9a | 56        | 44       | Match registration, only sent on success  |
| 0x23 | 68        | 196      | Create post-match credential session      |
| 0x24 | 128       | 44       | Bind biometric to credential session      |

These commands use non-standard secure-CV payloads. Template references cannot
be derived from the observed encrypted payload without crypto material.
No idea why the proprietary Linux driver doesn't use the same flow, nor why the
firmware doesn't mandate it.

## Fixed Credentials

The proprietary Linux path opens sessions with fixed credentials:

| Field           | Value      |
|-----------------|------------|
| App ID          | `myAppID`  |
| User ID         | `myUserID` |
| Credential type | `0x44`     |

For now, we keep those hard coded credentials. Possibly, credentials might be
a way into management mode, which would possibly enable on-device enumeration and
delete-all.

## Template IDs

Template IDs are returned by enrollment commit and stored in `FpPrint` metadata.
The driver later passes them to CAPTURE for verify/identify and to DELETE_OBJECT
for delete.

The exact firmware-side object-ID structure is not fully understood. The
single-template DELETE_OBJECT format used here works in practice, but firmware
analysis shows that the internal `id_data` parser is more structured than a bare
u32.

## Hash Chain

Enrollment uses a challenge chain:

1. `GET_CHALLENGE` returns hash `H_n`.
2. `ENROLL_UPDATE` sends `H_n` and returns `H_n'`.
3. The next `GET_CHALLENGE` returns a new hash.
4. `ENROLL_COMMIT` uses the final hash returned by `ENROLL_UPDATE`.

The hash is 20 bytes and is requested with algorithm value `0x23`, identified as
SHA-1 in the observed protocol.

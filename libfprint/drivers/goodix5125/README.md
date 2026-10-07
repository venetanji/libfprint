# Goodix 27c6:5125 protocol notes

The `goodix5125` driver supports the USB `27c6:5125` ChicagoHS sensor. It is
a host-matching device: the sensor sends encrypted 80 x 64 raw images and the
host performs preprocessing, enrollment, matching, and template updates.

Only one OEM-integrated unit has been tested. The sensor-specific calibration
and TLS key make testing it as a stand-alone peripheral impractical; broader
hardware compatibility must not be inferred from that one unit.

## USB framing

The driver claims USB interface 1 and uses bulk endpoint `0x01` for output and
`0x81` for input. Output transfers are padded with zeroes to a 64-byte packet
boundary. A message has two layers:

- The four-byte outer header is `[flag, payload_length_le16,
  header_checksum]`. The checksum is the low byte of the sum of the first
  three bytes. Known flags are `0xa0` for commands, `0xb0` for TLS handshake
  records, and `0xb2` for TLS image data.
- The inner command is `[command, body_length_le16, payload, trailer]`, where
  the body length includes the trailer. Normally the trailer makes the sum of
  the complete inner message equal `0xaa`. NOP requests and some MCU replies,
  including navigation, use the fixed trailer `0x88` instead.

Each normal command first receives an ACK command (`0xb0`) whose two-byte
payload contains the original command and status flags. Bit 0 indicates
success. Bit 1 announces queued data, but the tested ChicagoHS firmware does
not set it reliably, so the command contract determines whether a reply is
read. Replies repeat the original command identifier.

The 27c6:5125 MCU regularly drops the ACK when the reply follows immediately,
and a reply of an earlier command (a late FDT-down) can still be queued. The
driver therefore reads until the command's own reply (or, for commands
without a reply, its ACK) and skips stale messages; a command without a reply
whose ACK does not arrive within one second is taken as executed
(`goodix5125_proto_classify_rx`).

The command identifiers and payload construction implemented by the driver
are listed in `goodix5125-proto.h`. Firmware update and storage erase commands
are deliberately not exposed.

## Session and capture sequence

Initialization is volatile. The driver drains stale input, enables and resets
the MCU, verifies chip ID `0x2504`, reads and validates the 64-byte OTP, then
checks the host pairing against the MCU's PSK hash. OTP fields select DAC and
timing values which are applied to a 224-byte configuration with its own
little-endian word checksum.

The host then acts as a TLS 1.2 PSK server while the sensor is the TLS client.
TLS records are carried inside the Goodix outer framing through OpenSSL memory
BIOs. Finger images contain 7,680 packed bytes: every six bytes encode four
12-bit pixels. The 64 x 80 stream is transposed to the 80 x 64 row-major input
expected by ChicagoHS.

The first FDT measurement uses conservative bootstrap thresholds. Two
confirmed no-finger measurements surround a live `0x20` background capture;
the resulting FDT thresholds and ImageBase replace the bootstrap values. This
is the same cold-session calibration boundary used by the device's native
protocol and removes any dependency on an OEM `goodix.dat` cache.

FDT replies carry two status bytes, the touch mask of the six channels in
byte 2 (`0x3f` finger, `0x00` none) and six little-endian channel values.
Finger detection: FDT mode `0x0d` with the latest values (arm), an FDT-down
`0x0c` probe that normally gets no reply within 2 s, the MCU state query, then
FDT-down `0x0c`. The lift is detected by polling FDT mode `0x0d` with the
armed no-finger values until the touch mask is 0. This sequence is reliable on
27c6:5125 hardware, where FDT-up was not.

All long waits are asynchronous and use the libfprint action cancellable.

Matching uses openchicago (`chicago/`), a free C implementation of the
ChicagoHS algorithm (profile 12). Enrollment follows the ChicagoHS enrolment
protocol: 12 counted touches, coverage >= 65 and quality
>= 25, "enrolled already" hints (`FP_DEVICE_RETRY_CENTER_FINGER`) when a new
touch overlaps the template too much, and a second frame of the same touch
(`GOODIX5125_ENROLL_PAIR=0` disables it) from which the better one is used.
Verify and identify preprocess the frame once and compare it with each
template; on a match the template study step may update (learn) the
template. libfprint has
no signal for an updated print and fprintd reloads prints from disk for every
match and never stores them again, so the driver keeps the learned template
itself (`learned/` in the state directory, `GOODIX5125_LEARNED_DIR`): one
file per print, named by the SHA-256 of the print's `fpi-data` as the caller
stores it, holding the last learned `fpi-data` with a format version, the
sensor id and a checksum; written atomically with mode 0600. A later match of
the same print compares with the learned template; a missing, corrupt or
foreign entry only logs a warning and the print's own template is used. The
driver never sees a print being deleted, so entries unused for 180 days are
removed and at most 50 are kept (least recently used removed first). Only
if the entry cannot be written does the learned template replace the
`fpi-data` of the matched `FpPrint` in memory. The sensor itself stores no
enrolled templates.

## Power-button frames

The sensor is the power button. When it is pressed with a finger, the MCU
captures up to three frames by itself, before the host runs: the MCU-state
reply (`0xae`) then has bit 0 of byte 1 set. The frames are kept for about
180 s and are dropped when the USB link is suspended, so the device must not
be runtime-suspended before they are read. They are read after a TLS
handshake and before any `0x20` capture, which overwrites the first one: each
`0xd2 00 00` returns the next frame as a TLS record of the normal image size
(command `0xd2`, length, marker `0xaa`, header, packed image, CRC, `0x88`),
and the MCU clears the flag after the last. The 4-byte CRC after the image is
CRC-32/MPEG-2 of the packed image, stored as two big-endian 16-bit halves,
the lower half first.

The hwdb of libfprint and that of systemd
(`60-autosuspend-fingerprint-reader.hwdb`) enable autosuspend for 27c6:5125,
and fprintd opens the device only on the first request, long after it went
to sleep. Using the frames therefore also needs a udev rule that sorts after
`60-autosuspend.rules`, e.g. `/etc/udev/rules.d/71-goodix5125-pov.rules`:

```
ACTION!="remove", SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="27c6", ATTR{idProduct}=="5125", ATTR{power/control}="on"
```

With `GOODIX5125_POWER_BUTTON_UNLOCK=1`, the driver reads the frames at the
start of the sensor initialization and the first verify or identify tries
them before waiting for a touch: a match completes the action, anything else
is not reported and the action waits for a touch as usual. The frames are
used once, within 10 s of reading them, and are cleared on enrolment and
close. This is off by default because the driver cannot tell what an action
is for: for about 180 s after a press that did not suspend the USB link, the
first verify or identify is answered without a touch, whether it comes from a
lock screen, sudo, polkit or fprintd's duplicate check before an enrolment.

## Host pairing and local state

The MCU stores a write-only 102-byte container for its TLS PSK and exposes
`SHA-256(container)` at register `0xbb020003`. The driver reproduces the
container with AES-256-GCM and HMAC-SHA256, writes it through command `0xe0`,
and verifies the reported hash before using the key.

The driver never writes the PSK on its own:

- the host PSK (32 raw bytes or 64 hex digits, mode `0600`, owned by the
  service user) is read from `psk` in the state directory;
- a key whose wrapped hash matches the sensor is used without writing;
- a missing file, a blank sensor or a different key is an error. Only
  `GOODIX5125_PROVISION_PSK=random` authorizes a write: a missing file then
  gets a new random key (saved before the write), an existing file's key is
  written. This can invalidate another operating system's pairing.

The state directory is `/var/lib/fprint/goodix5125` (fprintd's state
directory) when the process can use it, otherwise
`$XDG_STATE_HOME/libfprint/goodix5125` (`~/.local/state/...`), e.g. for the
examples run as a user; `GOODIX5125_STATE_DIR` overrides it, and
`GOODIX5125_PSK_FILE`, `GOODIX5125_STATE_FILE`, `GOODIX5125_LEARNED_DIR`
single paths. fprintd's unit (`ProtectSystem=strict`, `StateDirectory=fprint`)
leaves only `/var/lib/fprint` writable, which covers all of them.

The live ImageBase is captured on every cold activation. The complete
adaptive openchicago state (calibration, ImageBase, the learned gain and the
context history of the preprocessor, the cross-frame feature state) is kept
in `openchicago.state`, bound to the first 16 OTP bytes. It is read at
`open()`, rebased onto the fresh ImageBase at activation (the adaptive
variables are kept) and
saved after every action and at `close()`. State for another sensor or a
corrupt file is rejected rather than overwritten. The state and the
templates are only checksummed, so every counter and index read from them is
range-checked on load. Without a state file, a
calibration in the `[sensor id][payload]` layout (`chicago-calibration.dat`,
`GOODIX5125_CHICAGO_CALIBRATION_FILE`)
is imported once if present; otherwise the first-run calibration is derived
from the ImageBase.

PSKs are cleansed from temporary memory after initialization. Device OTPs,
calibration, PSKs, captures, and biometric templates of real users must never
be committed. The unit tests use synthetic fixtures; the umockdev recording
was made with a finger that is not used for login.

## Device emulation

With `FP_DEVICE_EMULATION=1` (umockdev tests and their recording) the driver
keeps its state in a temporary directory that is removed on close, ignores the
`GOODIX5125_*` environment, uses the all-zero PSK and never writes a key. The
TLS server takes its random values (ServerHello random, session ID, AES-GCM
nonces) from a fixed pattern and sends no session ticket. What the host sends
then only depends on what the sensor sent, so a recorded session replays byte
for byte. Recording a test needs a sensor paired with the all-zero PSK.

umockdev replays a read that timed out on the device (a discarded URB) as an
empty completion. The driver therefore treats an IN transfer without data as
a timeout in every mode (`goodix5125_usb_in_check_empty`).

## Provenance

The transport implementation is free software written from hardware traces
and interoperability behavior, with the GoodixTLS libfprint driver and
`goodix-fp-dump` used as protocol references. No proprietary driver, firmware,
or binary blob is included or loaded.

The I/O-free ChicagoHS backend under `chicago/` (openchicago, derived from
the libfprint MR !648 port; every modified file carries a modification note)
and the white-box pairing primitive adapted as `goodix5125-pairing.c` come
from
[`berkekbgz/libfprint-goodix-spi`](https://github.com/berkekbgz/libfprint-goodix-spi),
revision `010a665f54089a1632b1ab7be588b316ace934e2`, Copyright (C) 2026 Berke
Kabagöz, under `LGPL-2.1-or-later`. They were integrated into libfprint with
source formatting and build adaptations; their copyright and SPDX notices are
retained.

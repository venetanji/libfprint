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

Finger detection uses the MCU's FDT-down and FDT-up commands. All long waits
are asynchronous and use the libfprint action cancellable. Enrollment collects
12 accepted samples. Verify and identify compare native Chicago subtemplates;
a successful match may update the in-memory template through `templateStudy`.
The sensor itself stores no enrolled templates.

## Host pairing and local state

The MCU stores a write-only 102-byte container for its TLS PSK and exposes
`SHA-256(container)` at register `0xbb020003`. The driver reproduces the
container with AES-256-GCM and HMAC-SHA256, writes it through command `0xe0`,
and verifies the reported hash before using the key.

Normal operation is automatic:

- the host PSK is kept in `/var/lib/fprint/goodix5125/psk` with mode `0600`;
- a blank sensor gets a random key, persisted atomically before the write;
- an existing matching key is used without writing the sensor;
- a sensor paired by another OS is not overwritten silently. Setting
  `GOODIX5125_PROVISION_PSK=random` explicitly authorizes one replacement;
  this can invalidate another operating system's pairing.

`GOODIX5125_PSK_FILE` can override the state path for testing or migration.
The live ImageBase is captured on every cold activation. Derived Chicago state
is stored at `/var/lib/fprint/goodix5125/chicago-calibration.dat`, or the path
in `GOODIX5125_CHICAGO_CALIBRATION_FILE`, and is bound to the first 16 OTP
bytes. State for another sensor is rejected rather than overwritten.

PSKs are cleansed from temporary memory after initialization. Device OTPs,
calibration, PSKs, captures, and biometric templates must never be committed;
all tests use synthetic fixtures.

## Provenance

The transport implementation is free software written from hardware traces
and interoperability behavior, with the GoodixTLS libfprint driver and
`goodix-fp-dump` used as protocol references. No proprietary driver, firmware,
or binary blob is included or loaded.

The I/O-free ChicagoHS backend under `chicago/`, the CRC helper under
`common/`, and the white-box pairing primitive adapted as
`goodix5125-pairing.c` come from
[`berkekbgz/libfprint-goodix-spi`](https://github.com/berkekbgz/libfprint-goodix-spi),
revision `010a665f54089a1632b1ab7be588b316ace934e2`, Copyright (C) 2026 Berke
Kabagöz, under `LGPL-2.1-or-later`. They were integrated into libfprint with
source formatting and build adaptations; their copyright and SPDX notices are
retained.

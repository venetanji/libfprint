# Experimental Goodix 27c6:521d driver

This feature branch implements the libfprint `FpDevice` interface for GF5298 / firmware `GFUSB_GM168SEC_APP_10034`: device discovery, enrollment, verification, identification, cancellation and native-template persistence. The free OpenChicago implementation is selected explicitly as sensor profile 17. It does not invoke a Windows DLL or require a Windows calibration cache.

The earlier working GTK application remains a separate reference. It used vendor preprocessing and matching. Its successful hardware results are not validation of this free driver.

## Build

Use `packaging/arch/README.md` for the Omarchy/Arch package. A selected-driver developer build is:

```sh
meson setup build -Ddrivers=goodix521d -Dintrospection=false -Dgtk-examples=false -Ddoc=false
meson compile -C build
meson test -C build --print-errorlogs
```

Keep the password authentication path available. Do not enable fingerprint login until the local checks below pass. This repository does not alter PAM or start services.

## Existing private provisioning

The driver reads two already-provisioned files from `/var/lib/libfprint/goodix521d`:

- `factory.bin`: the reader's previously saved, CRC-valid 64-byte factory record
- `psk.bin`: the existing 32-byte TLS PSK, root-owned with no group/world permissions

These files are not distributed or created by the package. The PSK digest must match the connected reader before the TLS handshake. There is no fallback key and no key-write operation. Keep the factory record paired with that reader's key; factory CRC establishes integrity, not a hardware identity attestation.

For installation-free developer tests, `LIBFPRINT_GOODIX521D_DATA_DIR` may name an absolute directory containing those files. Normal fprintd execution uses the fixed default; do not assume a terminal environment variable propagates into the service.

All file access is read-only. The original Windows volume and any saved calibration cache are unused. Configuration is generated in memory from the validated record and public unpersonalized profile, then uploaded to volatile sensor state. No firmware, factory record or persistent pairing data is written.

## Capture and matching contract

Start with the reader clear. Opening obtains a fresh paired background: three FDT snapshots, two differently flagged background images, the original shifted-word FDT gates and strict cropped image MAD below 200. The free preprocessor additionally rejects an invalid reference range. A finger-covered reference is not an acceptable workaround.

A validated FDT or image stability rejection permits at most one reacquisition within that same initialized open. The second attempt uses the first FDT snapshot transformed as `uint16((raw >> 1) * 257)` for its seed; partial reference images are discarded. This is a bounded Linux recovery policy based on the Windows seed-update operation, not a Windows retry-count claim. Both attempts share the original 45-second open deadline and unchanged acceptance gates. Transport, malformed-data, integrity, or cancellation failures do not trigger this retry. The seed is never saved or carried into another open. A journal `paired-reference-retry` line identifies this recovery attempt.

The driver restores the earlier per-contact hardware transaction boundary:
reset, volatile configuration and authenticated TLS are established for each
capture, then transport is torn down before algorithm processing. The validated
OpenChicago reference/session remains in memory across ordinary contacts as in
that earlier lifecycle. This rolls back the reported regression from retaining
hardware context; it does not prove which undocumented sensor state caused it.

Cold public opens still obtain a new paired reference. This rollback does not
solve repeated cold-open baseline failures, and those failures do not establish
that a finger was present or placed too early. No matching threshold, template
format, image request recipe, or frame count is changed.

Each contact uses the established ten-frame settled capture recipe and the
final integrity-checked native 80×64 frame. After the final validated frame, an acknowledged volatile write restores sensor register `0x022c` to `0x020a` before either removal-arm phase. This restores the Windows image-success-to-FDT transition while leaving the ten-frame burst unchanged; a failed restore aborts the contact. A current FDT-down event supplies the translated base for a separately armed, checksummed FDT-up event. ACK alone cannot satisfy finger removal. Contact images never pass through the old min/max/resize/invert preview path.

Before enrollment, fprintd performs a duplicate-identification capture even when
no prints are stored. An empty gallery still uses the ordinary capture and
quality checks and returns no match; then enrollment begins. Its initial
`enroll-stage-passed` notification is the duplicate check, not an accepted
OpenChicago enrollment sample. No old templates need deleting for this path.

Enrollment uses OpenChicago's natural completion policy: 12 counted stages, its existing quality/coverage/tip policy and maximum-sample behavior. The driver cannot export a partial enrollment. Every accepted or retried contact must finish removal before a new contact is acquired. The session has a 64-contact and 15-minute bound; open and verification are separately bounded. Cancelling discards late algorithm results.

Templates contain a strict versioned envelope with backend and preprocessing versions, profile 17, sensor identity digest and exact configuration digest. Incompatible or malformed templates fail import. Adaptive session memory is not serialized into fingerprints. Verification does not perform template study and does not try another backend after rejection.

## Visible touch, hold and removal guidance

The stock `fprintd-enroll` CLI prints enrollment results but does not display
fprintd's finger-status properties. Use the read-only monitor from this checkout
in a separate terminal for the next local test. It needs Python and PyGObject
(`python-gobject` on Arch); it does not claim the reader or start enrollment.

Complete any separate administrator authentication and approved service restart
first, then keep the reader completely clear while the device opens. If testing
a newly installed build after a failed open in an older process, restart fprintd
through the normal local administrator workflow before this test.

Use two ordinary user terminals after that separate administrator step.
Run exactly one verification client, without `sudo`; do not start another sudo
or authentication operation during the test. The monitor observes this device's
properties for **all clients**, not just your verification process. If guidance
appears before your intended verification starts, or the CLI reports AlreadyInUse,
stop and identify the other client before attributing a timeout to this test.
Record the intended CLI's start time and keep its output with the corresponding
journal interval; monitor guidance alone does not establish who owns capture.
Terminal 1, from this checkout:

```sh
python3 examples/goodix521d-finger-monitor.py --device /net/reactivated/Fprint/Device/0
```

Wait for `Monitor connected. KEEP READER EMPTY.` Keep your finger completely off the reader before starting terminal 2 and until the monitor says PLACE. Connection does not mean hardware initialization has finished: background initialization happens when the client claims/opens the device, which can be later than the service restart. With an existing enrollment, run:

```sh
fprintd-verify "$USER"
```

Use `fprintd-enroll "$USER"` only when a new enrollment is actually needed; do not overwrite a working enrollment to test this change.

Follow PLACE, HOLD and REMOVE in terminal 1. HOLD lasts through the ten-frame
capture; REMOVE is published only after the reader acknowledges its removal
arm commands. The removal event owner starts before phase 0 because APP10034 can emit IRQ 0x0200 in that interval; success still requires both ordered phase ACKs and an exact checksummed current-owner 0x34 event. Pre-arm or wrong-generation events cannot complete the contact. A stage is not accepted until the separate removal event is validated.
For enrollment, the initial contact is fprintd's duplicate check, before counted stages.
The monitor exits after its bounded observation window or Ctrl+C; stopping it
does not cancel the separate client command.
Prompts are best-effort: very brief property transitions can be missed. The
monitor never determines acceptance or changes capture state.

If REMOVE is displayed, fully lift the finger. If removal still times out,
stop rather than repeating the test and preserve the precise error plus the
last displayed guidance. Removal timeout errors include phases sent, ACK mask, event-seen and owner-current scalars; no FDT sample values are logged. A `goodix-up-cancel` line snapshots those fields before cancellation invalidates the read owner. It does not identify who requested cancellation or claim that the 30-second removal deadline expired. This distinguishes a missing prompt from an unproven
hardware removal path. A paired-reference stability failure now includes the
failing snapshot pair, maximum shifted-word difference and factory threshold;
it does not permit a finger-covered reference or relax that threshold.

Failed-open cleanup waits up to two seconds for cancelled USB reads before
closing the handle. If drainage or transport teardown fails, the handle is
retained and the original failure includes cleanup context. Normal close also
waits for owned RX completion before allowing the core to close USB. On
incomplete cleanup, the internal close-abort path reports an error while keeping
the device logically open and retaining its handle; acquisition is blocked until
a later successful close. It does not fabricate a closed state with a transfer
still using the handle. Other drivers retain their existing close semantics.
fprintd clears its claim bookkeeping after a failed Release; its next Claim can
therefore report ALREADY_OPEN for this retained handle. A service restart may be
required in that exceptional state. Direct libfprint callers can retry close.
A new hardware transaction must not race a cancelled read from its predecessor.

## Responsiveness timing

Ordinary journal messages prefixed `goodix-timing` report monotonic elapsed
microseconds. Correlate them by process, `action_id`, and `contact`; action IDs
are local to a device object and are not persistent identifiers. Driver records
separate bootstrap, TLS, reference/capture, cleanup when performed, worker queue, algorithm,
callback delivery, and overall action duration. Acquisition records break the
contact into DOWN wait, ten-frame burst, register restore, and UP wait.

These intervals overlap: capture includes the acquisition subphases, and the
overall duration includes all stages. Do not sum all records together. Algorithm
elapsed time is wall time, not CPU utilization, and includes preprocessing and
matching as one existing API call. A reference retry stays inside the same
reference interval. Error or cancellation records retain the completed/active
phase context; no successful matching result is inferred from timing alone.

Timing messages contain phase/status labels, counters, and durations only. They
do not export images, feature vectors, templates, keys, or factory values.
Logging itself has a small runtime cost. Timing instrumentation does not change capture counts, timeouts, matching
thresholds, or stored enrollment. `goodix-processing` records the existing
preprocessing status code and an allowlisted rejection reason for completed
verification/identification computation. It does not report a match decision.
For example, `0x000029aa` with `bad-input` means the raw input range gate
rejected the frame before enhancement/features/matching; it does not identify
the physical reason for that rejection. Cancelled or failed computations do not
publish this result diagnostic.

## Required local qualification

Before authentication use, test with libfprint examples and then the existing fprintd tools:

1. Confirm discovery identifies `27c6:521d` as `goodix521d`, with no second process owning it.
2. Enroll through natural completion with genuinely separate placements.
3. Verify fresh genuine placements and multiple different-finger controls.
4. Close/reopen and restart the test process; verify the saved template still works.
5. Cancel while waiting for a finger, while waiting for removal and during processing. Check clean reopen afterward.
6. Test unplug/replug and deliberate malformed/incompatible-template rejection.

Do not interpret compilation, a synthetic USB fixture, partial-gallery diagnostics or a few successful comparisons as an authentication error-rate measurement. Physical free-driver and Arch package results must be recorded separately from offline tests.

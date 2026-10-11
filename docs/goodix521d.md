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

A validated FDT or image stability rejection permits at most one reacquisition within that same initialized open. The second attempt uses the first FDT snapshot transformed as `uint16((raw >> 1) * 257)` for its seed; partial reference images are discarded. This is a bounded Linux recovery policy based on the Windows seed-update operation, not a Windows retry-count claim. Both attempts share the original action deadline (45 seconds during open) and unchanged acceptance gates. Transport, malformed-data, integrity, or cancellation failures do not trigger this retry. The seed is never saved or carried into another open. A journal `paired-reference-retry` line identifies this recovery attempt.

Normal contacts now follow the pinned GF5298 detector sequence within one
healthy initialized hardware context. Accepted reference F1 initializes two
separate in-memory seeds. The full 40-byte DOWN request uses factory-derived
FDT DACs, current image DACs, settings, and the current detector seed. Both ordered arm
ACKs and a separate checksummed current DOWN event are required. One two-phase
manual FDT transaction then compares the current DOWN snapshot with its new
snapshot, using the separate reference/manual seed. These are vendor-derived
state transitions; snapshot similarity is not a finger-absence determination.

When the pre-burst snapshots are equal within the factory threshold, Windows
requests reference refresh. This implementation conservatively waits for a
qualified current-contact UP first, without collecting contact images or
writing an unnecessary image restore. Verification/identification reports a
retry and rebuilds the reference for the next action. Enrollment permits one
bounded refresh in the existing action: its accepted stages are preserved by
preprocessor rebase, and the original deadline remains in force. The refresh
reference still passes every paired-image, FDT and preprocessing range check.
A second enrollment refresh or a failed refresh terminates the action. This
lift-before-refresh ordering is an explicit safety adaptation of Windows.

Each normal contact captures one native 80×64 image, matching the pinned
Windows caller and its single image submission. After integrity validation,
exposure feedback updates the four DAC words for future requests without
changing the captured pixels. An acknowledged volatile write then restores
register `0x022c` to `0x020a` before the removal arm. A failed image or restore
stops the contact; there is no hidden settling burst or quality-based recapture. The validated UP snapshot updates only
the next DOWN seed, using `uint16((raw >> 1) * 257)`; it never replaces the
reference/manual seed or a biometric template. UP arming still uses the distinct
touch-adjusted translation of the current DOWN event. Contact images never pass
through the old min/max/resize/invert preview path.

Reference and detector state are bound to the healthy read-loop, TLS and
configuration generations. Successful contacts retain that context. Reset,
configuration loss, transport/protocol failure, cancellation or a classified
reverse-detector event invalidates it; no state is carried blindly into a fresh
bootstrap. Recovery drains old receive ownership before reinitializing, keeps
finger-needed clear while a fresh reference is requested, and never extends the
action deadline. The reference seed is committed only after preprocessing
initialization/rebase and main-thread ownership checks succeed.

Cold public opens still request a clear reader and obtain a fresh paired
reference. Consistency/range checks do not establish automatic finger absence,
and unstable reference readings do not prove a finger was placed too early.
After an unqualified transport failure, the next initialization likewise requests
a clear reader; resetting the sensor is not reported as a validated removal.
Matching thresholds, preprocessing profile and template format are unchanged.
The image count and exposure feedback are the functional changes; physical
speed and reliability require qualification on this build.

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

Follow PLACE, HOLD and REMOVE in terminal 1. HOLD lasts through image acquisition and restoration; REMOVE is published only after the reader acknowledges its removal
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
contact into DOWN wait, pre-burst FDT check, image acquisition/feedback,
register restore, and UP wait. The historical `contact-burst` label now measures
the single-image acquisition/feedback interval, for log comparison.

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
publish this result diagnostic. On `bad-input` only, `goodix-input-range` reports
three scalar counts in the existing 4,560-pixel interior: `low` is at most100,
`in-range` is101–3799, and `high` is at least3800. This distinguishes raw-range
failure classes without exporting pixels or changing the preprocessing gate.

## Required local qualification

Before authentication use, test with libfprint examples and then the existing fprintd tools:

1. Confirm discovery identifies `27c6:521d` as `goodix521d`, with no second process owning it.
2. Enroll through natural completion with genuinely separate placements.
3. Verify fresh genuine placements and multiple different-finger controls.
4. Close/reopen and restart the test process; verify the saved template still works.
5. Cancel while waiting for a finger, while waiting for removal and during processing. Check clean reopen afterward.
6. Test unplug/replug and deliberate malformed/incompatible-template rejection.

Do not interpret compilation, a synthetic USB fixture, partial-gallery diagnostics or a few successful comparisons as an authentication error-rate measurement. Physical free-driver and Arch package results must be recorded separately from offline tests.

## Exposure feedback

The free controller follows the pinned GF5298 mode29/mode1 arithmetic: factory
integration and immutable initial DAC, selected native-ADC mean, float32 ratio
and step calculations, directional hysteresis, and minimum-of-four DAC
distribution. Its literal151 call argument is logging metadata, not the starting
DAC or a brightness target. The accepted FIRST paired reference is deep-copied
for feedback only after preprocessing initialization/rebase succeeds.

Controller history and live request words persist across healthy contacts.
Both the next DOWN request and next image request use those same four words.
Even an ineligible measurement can redistribute the words when their minimum
differs from the controller target. Feedback never rescues the current image
retroactively: its unchanged preprocessing range/quality gates may return a
retry. Transport, restore, removal or cancellation failure invalidates the
controller and raw-reference copy together. A fresh hardware epoch starts with
factory DACs and cleared history; this explicit Linux lifetime policy differs
from the vendor global initializer, which preserves some history.

Public synthetic fixtures cover75 stateful vendor-oracle updates, four DAC
distributions, ineligible measurement redistribution, invalid inputs and state
ownership. No DLL, personal factory record or captured fingerprint is included.
Older ten-frame captures used different DOWN firmware setup and cannot establish
the quality of the first image under this corrected sequence.

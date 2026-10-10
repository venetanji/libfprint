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

Each contact uses a new authenticated hardware context, the established ten-frame settled capture recipe and the final integrity-checked native 80×64 frame. A current FDT-down event supplies the translated base for a separately armed, checksummed FDT-up event. ACK alone cannot satisfy finger removal. Contact images never pass through the old min/max/resize/invert preview path.

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
Terminal 1, from this checkout:

```sh
python3 examples/goodix521d-finger-monitor.py --device /net/reactivated/Fprint/Device/0
```

Wait for the monitor's ready message, then in terminal 2:

```sh
fprintd-enroll "$USER"
```

Follow PLACE, HOLD and REMOVE in terminal 1. HOLD lasts through the ten-frame
capture; REMOVE is published only after the reader acknowledges its removal
arm. A stage is not accepted until the separate removal event is validated.
The initial contact is fprintd's duplicate check, before counted enrollment.
The monitor exits after its bounded observation window or Ctrl+C; stopping it
does not cancel the separate enrollment command.
Prompts are best-effort: very brief property transitions can be missed. The
monitor never determines acceptance or changes capture state.

If REMOVE is displayed, fully lift the finger. If removal still times out,
stop rather than repeating enrollment and preserve the precise error plus the
last displayed guidance. This distinguishes a missing prompt from an unproven
hardware removal path. A paired-reference stability failure now includes the
failing snapshot pair, maximum shifted-word difference and factory threshold;
it does not permit a finger-covered reference or relax that threshold.

Failed-open cleanup waits up to two seconds for cancelled USB reads before
closing the handle. If drainage or transport teardown fails, the handle is
retained and the original failure includes cleanup context. Ordinary close's
teardown-failure behavior remains an unqualified edge of this experimental
branch; this repair does not establish all close/unplug failure paths as safe.

## Required local qualification

Before authentication use, test with libfprint examples and then the existing fprintd tools:

1. Confirm discovery identifies `27c6:521d` as `goodix521d`, with no second process owning it.
2. Enroll through natural completion with genuinely separate placements.
3. Verify fresh genuine placements and multiple different-finger controls.
4. Close/reopen and restart the test process; verify the saved template still works.
5. Cancel while waiting for a finger, while waiting for removal and during processing. Check clean reopen afterward.
6. Test unplug/replug and deliberate malformed/incompatible-template rejection.

Do not interpret compilation, a synthetic USB fixture, partial-gallery diagnostics or a few successful comparisons as an authentication error-rate measurement. Physical free-driver and Arch package results must be recorded separately from offline tests.

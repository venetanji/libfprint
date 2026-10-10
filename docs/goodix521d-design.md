# Goodix 27c6:521d free-software driver

## Goal and boundaries

Provide an experimental libfprint driver that Omarchy can build from Git using an Arch PKGBUILD. Start from official libfprint master 6f9479c3d55f847c1b3769f28ceb99227f9858cf. Preserve upstream and imported LGPL notices. Import only relevant OpenChicago algorithm sources from MR669 revision a9286cce45a276886a04db5170dd984e84c4d2e4, with attribution. Do not import the 5125 transport, pairing writes or power-button behavior.

The working v3 GTK candidate remains a separate rollback/reference artifact. Its public source archive is identified by SHA256 0ea715bbe428f029907c2354b25ac7d22a8b65077164897687276f6aaa2217ad. No vendor DLL, emulator runtime, private factory/calibration bytes, key, biometric image or personal template belongs in this repository.

## Architecture

Use an FpDevice driver named goodix521d. Reuse the v3 authenticated USB/TLS transport and exact paired-reference acquisition, adapting their completion boundary to FpDevice callbacks instead of pretending the old FpImageDevice/NBIS preview path is the working matcher.

Keep four interfaces separate:

1. Transport: bounded commands, ACK/checksum association, configuration-loss failure, authenticated TLS before images, bounded cancellation/cleanup and exact image integrity.
2. Acquisition: a fresh reader-clear reference with the traced three FDT snapshots/two image sequence, followed by independent finger contacts. No cached image substitution or automatic retry after a fatal failure.
3. Processing/matching: explicitly profile-aware GF5298 mode17 free preprocessing and a qualified free matcher. Mode24 is preserved for its original users. Matcher selection depends on fixed held-out evaluation; no automatic backend fallback after rejection.
4. Persistence: a strict FpPrint raw-data envelope carrying schema, backend/profile/preprocessing versions, compatibility identity and validated native template bytes. Never persist a guest-memory snapshot or borrowed pointer. Incompatible templates require re-enrollment, not silent conversion.

The libfprint callbacks own enrollment progress, verify/identify reporting, cancellation and exactly-once completion. The GTK worker and its per-click prompts are not installed as an authentication service. Any backend work that could block runs outside the main-loop callback with explicit lifetime and cancellation ownership.

## Private provisioning

Reuse locally provisioned factory/configuration/key inputs through documented read-only files. No credential creation, pairing write, firmware update, automatic key fallback or calibration-storage write is part of this change. Installed-service paths and owner/permission checks must be explicit; GUI environment variables are not an authentication configuration mechanism. Generic profile/integrity checks replace personal hashes in public code. No private data is embedded in the package.

## Validation and release boundary

Test parser/transport/SSM failures, both reference gates, image integrity, cancellation, unplug, repeated open/close, enrollment completion, genuine/wrong-finger decisions, and fresh-process template serialization round trips. Reject malformed, wrong-profile and wrong-backend templates. Retain baseline mode24 tests. Build selected-driver and all-driver configurations, run native sanitizers where supported, and provide an Arch package build without installing it here.

A draft PR is an experimental integration checkpoint. It must disclose any unresolved mode17, matching, persistence or hardware qualification gap; compiling alone does not establish authentication readiness. No merge, release tag, system installation, PAM change or deployment is authorized by this work.

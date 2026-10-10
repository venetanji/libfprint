# Offline verification, 2026-10-10

These checks do not exercise a physical fingerprint reader.

- Official-master selected-driver and all-driver Meson builds succeed with GCC, GLib 2.84.4, GUsb 0.4.9 and OpenSSL 3.5.7.
- All 11 Goodix test targets pass in both builds. They cover private input validation, decoded UP requests, the twelve-state paired reference, strict session completion, native/FpPrint persistence, compiled registration, acquisition/removal, FpContext JSON-device discovery, 23 lifecycle cases, 15 profile cases and transport/TLS regressions.
- The selected-driver complete suite passes 13 targets and skips 35 configuration-dependent targets. GObject introspection and Cairo-dependent replay tests are not available in this build environment.
- The all-driver complete suite passes the available core, generated-hwdb and Goodix checks. Two pre-existing virtual-device/context targets cannot create their Unix sockets in this execution environment, including a retried run with the requested execution permission. The other 34 configured skips remain explicit. The branch's GitHub workflow runs the complete suite on a normal Ubuntu runner.
- The FpContext discovery test executes real driver selection and object construction over a synthetic JSON GUsb device. Generic probe completion is mocked to avoid host USB-wakeup sysfs access. It is discovery coverage, not a USB capture replay.
- The transport suite exercises actual OpenSSL peers, including wrong key, unsupported cipher, tampering, timeout and bounded cleanup. Source-executed parser tests cover ACK/checksum/config-loss association, command cancellation, synchronous-send reentrancy and owned FDT-up events.
- Nine native Goodix targets also pass AddressSanitizer and UndefinedBehaviorSanitizer, run directly with `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`. The transport runner passes both sanitizers separately; 15 profile tests additionally pass UBSan. LeakSanitizer cannot operate under this executor’s ptrace environment.
- Independent review closed the algorithm/profile, template-envelope, provisioning, cancellation, deadline and publication boundaries. Mutation tests show that removing either the monotonic deadline check or cancellation guard causes lifecycle failures.

## Reproduce

```sh
meson setup build -Ddrivers=all -Ddoc=false -Dintrospection=false -Dgtk-examples=false
meson compile -C build
meson test -C build --print-errorlogs
meson test -C build --suite goodix521d --print-errorlogs
python3 packaging/arch/test_recipe.py
```

The Arch recipe enables the usual introspection and documentation dependencies and does not suppress failing tests. Its shell syntax and temporary-Git recipe regression pass here. Actual `makepkg` execution and installation/SONAME metadata still require Arch.

The fixed offline matcher diagnostics use a gallery that reached 10 of 12 natural stages; strict production export rejected it. Those partial-gallery scores are useful algorithm comparisons only. No threshold was fitted to held-out probes. Physical free-driver enrollment, genuine/wrong-finger checks, removal, unplug/reopen and fprintd restart remain required local qualification.

## Arch LTO acquisition-test regression

The first Arch build completed compilation but reported two test failures. The
Goodix acquisition fixture reached the real cancellable accessor on a synthetic
inactive device. This was reproduced with `CFLAGS='-O2 -fno-plt -flto=auto'` and
`LDFLAGS='-flto=auto'`: whole-program optimization bypassed the fixture's linker
`--wrap` boundary. The fixture now compiles the exact acquisition source with
explicit external-boundary substitutions, as the lifecycle test already does.
Its production acquisition code and behavioral assertions are unchanged. All 11
Goodix targets pass with those flags and in the ordinary build. CI now runs both
configurations rather than relying on the unoptimized linker behavior.

The separate `egis_etu905` cancellation-replay failure is being compared against
unchanged upstream code; it must not be hidden with `--nocheck`. The original
GitHub run passed 18 targets and skipped 33 fixture-dependent targets, including
that replay, so its green result did not establish full Arch replay coverage.

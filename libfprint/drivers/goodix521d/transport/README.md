# Goodix 521d authenticated transport

This isolated transport is adapted from the working v3 Goodix TLS capture source
(`goodix-linux-gtk-pipeline-20261010`, preserved source archive SHA-256
`0ea715bbe428f029907c2354b25ac7d22a8b65077164897687276f6aaa2217ad`). Original LGPL
notices are retained. The host-side image boundary uses the shared
`../goodix-profile.[ch]`; no proprietary executable or per-reader data is included.

## Owner and lifecycle

`FpiDeviceGoodixTls` derives from `FpDevice`. All USB and owner-facing operations
run on the device's main context. The TLS worker does not enter libfprint.

1. Claim the USB interface with `goodix_dev_init`, then start the owned read loop.
2. Load the existing reader key with `goodix_tls_load_psk_file`, or its fixed-path
   `/etc/libfprint/goodix-521d.psk` wrapper. Loading requires an absolute path,
   a regular root-owned mode-0600 inode, and exactly 32 bytes. The final path
   component cannot be a symlink. A failure clears the output; no fallback key
   or pairing operation exists.
3. After the caller's reader/key association checks, supply the key explicitly
   with `goodix_tls_set_psk`. Start TLS with `goodix_tls(dev, callback, user_data)`.
   The callback owns its error and runs exactly once. Success requires verified
   TLS Finished and the reader's validated acknowledgement. The transport does
   not call image-device activation APIs.
4. To cancel, call `goodix_cancel` on the same main context. Pending command,
   TLS and FDT-event owners are completed inline. Do not start a replacement
   action or tear down a parent action from those callbacks until cancellation
   returns; defer that teardown in the caller. Old USB completions retain their
   device/token and cannot enter a replacement read generation.
5. Deinitialize after callbacks unwind. If TLS cleanup times out, the transport
   retains its resources and USB ownership. Keep the device alive and retry
   cleanup; do not create a replacement transport or report it closed.

Only TLS 1.2 PSK-AES128-CBC-SHA256 (cipher `0x00ae`) is accepted; tickets are
suppressed. Authentication has a five-second deadline; TLS I/O and cleanup have
two-second deadlines. USB commands retain bounded transfer/response deadlines.
Timer creation refreshes the monotonic deadline locally, including on upstream
versions whose timeout factory uses a cached main-context timestamp.

## Finger-removal event

Register `goodix_wait_fdt_up(dev, timeout_ms, callback, user_data, destroy)`
immediately before the ACK-only phase-1 `0x34` command. The request must be 40
bytes with phase byte 26 equal to 1. The ordinary command ACK callback is separate
from the event callback; both may report an error. Give the event callback an
owned, generation-checked context rather than a raw parent state-machine pointer.
The destructor runs after callback dispatch or cancellation, including a rejected
registration. Deadlines must be between 1 and 30,000 milliseconds.

Only an additive-checksummed `0x34` event with exactly 20 checksum-stripped bytes
and little-endian IRQ `0x0200` can release the contact. An event received before
its ACK is retained, and ACK/event coalescing is handled in either order. A down
IRQ `0x0002`, manual-mode event, malformed body, checksum error, configuration loss,
cancellation or timeout cannot synthesize removal. Completion discards the old
batch before invoking callbacks, preventing trailing bytes from entering a
reentrant successor operation.

No wire transaction identifier is known. Ownership uses serialized arm phases,
command and read-loop generations, and caller-controlled drain/reset boundaries
between contacts. This does not claim that an arbitrary delayed identical packet
can be distinguished from a current packet without such a boundary.

## Verification

From the repository root:

```sh
python3 tests/goodix521d-transport.py
```

The runner needs Python 3, a C compiler, OpenSSL development files and GLib/GIO
(via `pkg-config`). For a workspace-only header/library prefix, set
`GOODIX_TEST_GLIB_CFLAGS` and `GOODIX_TEST_GLIB_LIBS`. `CC`, `CFLAGS` and `LDFLAGS`
are supported. Temporary compilation output is removed on exit.

The tests execute production receiver, command, completion and lifecycle code.
Receiver tests use a small libfprint surface shim; ownership/cancellation tests use
real GLib objects; TLS cases compile the production TLS module and perform real
OpenSSL handshakes with synthetic peers and keys. No tests open a reader, read a
real key, install system files, or send pairing/key-write commands. A real device
capture/enrollment run remains a separate hardware qualification step.

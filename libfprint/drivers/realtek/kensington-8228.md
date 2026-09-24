# Kensington VeriMark Desktop 2.0 (047d:8228)

This reader uses the Realtek MOC command framing and bulk endpoints 01 OUT and
82 IN. Its vendor control request `c0/07` (value `000d`, eight-byte response)
selects the storage layout: bit 0 of the first response byte selects the
host-bound layout used by the tested reader.

| Operation | Host-bound layout / reply | Evidence |
| --- | --- | --- |
| `45/0e` storage read | 52 bytes per slot: common 35-byte prefix, host index at byte 35, UUID at bytes 36–51 | Record length observed; offsets from offline protocol analysis |
| `85/21` enrollment commit | 49 bytes: 32-byte ID field, host index, 16-byte canonical UUID | Commit succeeded; field layout from offline protocol analysis |
| `45/22` identification | 42-byte data reply; no subsequent status packet | Matches and no-matches observed; extra status read stalled |
| `05/05` start capture | Purpose in parameter 0 (`02` identify, `04` enroll) | LED lit during finger wait and went out after a matching scan |

An empty-reader enrollment follows `c0/07` device info, `05/13` OS
selection, `45/0d` slot count, `45/0e` storage read, `05/20` begin slot,
then repeated `05/05` start / `45/06` finger poll / `45/08` sample
acceptance. After eight reported samples and one finishing scan, `45/10`
checks duplicates and `85/21` commits the ID and host context. Verification
reads storage, starts capture with purpose `02`, polls and accepts a scan,
and reads one `45/22` response. The success path does not send the standard
Realtek `05/11` template-update request.

An empty storage area uses host index zero. Additional enrollments reuse the
index of the local host's existing records; foreign or inconsistent records
must abort enrollment before capture. The UUID is read from
`/sys/class/dmi/id/product_uuid` only when required for a host-bound operation,
and is not stored in the libfprint print reference. Legacy records with
subfactor `f5` contain a 28-byte opaque ID rather than a text ID.

Capture polling is paced at 200 ms with a 30-second finger-detection limit.
The ninth finishing scan can be retried up to five times on quality errors.
Successful verification completes without issuing `05/11` (template update).
These differences are scoped to USB ID 047d:8228; other Realtek IDs retain
their existing command flow.

Hardware evidence: enrollment, storage listing, verification match and
no-match, and the capture-purpose LED behavior have been observed with this
reader. The host-index reuse path with multiple enrolled fingers has not yet
been exercised on hardware. The offline tests use synthetic UUIDs and records;
they do not model biometric matching or USB timing.

`tests/realtek-kensington/make-replay.py` constructs a pcapng from fictional
records and USB responses. Its umockdev replay covers init, empty and occupied
storage listing, the verification capture-purpose byte, a waiting poll,
matching without a trailing status read or `05/11`, and malformed/short
storage responses. The replay has no enrollment or destructive USB commands;
the separate C test checks the host context format and foreign records using
synthetic UUIDs.

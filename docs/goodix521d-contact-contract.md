# GF5298 contact protocol

This bounded profile targets the already qualified sensor12/GF5298 firmware.
It was checked against the pinned WBDI image identified in the existing research
by SHA256 `a6357a78c14fe1b82e4bad495fdf39d9fbe4af93193a14ee23a250e3f544553d`.
Only derived protocol behavior is implemented here; that binary and all private
sensor records are excluded. Addresses below are relative virtual addresses.

## Three distinct inputs

- Reference/manual FDT seed corresponds to device field `+4ef0`.
- DOWN-arm seed corresponds to `+f8`.
- Current validated DOWN event supplies the separate UP-arm translation.

A successfully accepted reference initializes the first two from FIRST raw FDT:
`uint16((raw >> 1) * 257)` (69d14/58c9c,69d19–69d60). These values remain pending
until preprocessing initialization/rebase succeeds and the owning main-context
action commits them. After a valid UP event and both arm ACKs, only the DOWN seed
is updated through the same multiply/add transform (765e4,58a01–58a11,
69274–6927f,69454–69462). This is different from DOWN-to-UP touch-adjusted OR
packing. No seed survives an unqualified reset or failed hardware context.

Windows has additional reverse-event transitions (68a63–68a8d). This bounded
implementation invalidates that context and requests a fresh reference rather
than guessing those transitions. A reverse IRQ is not a finger removal.
This classification applies only to exact20-byte command32/34 events with raw
IRQ0080/0082. The Windows manual36 parser overrides its event class to manual;
those IRQ bits alone in a manual snapshot do not invalidate the context.

## Contact sequence

1. Opcode32: two 40-byte DOWN phases, each ACK-only; independent exact20-byte
   IRQ0002 event. Both ordered ACKs and the current event are required.
2. Opcode36: one manual transaction, phase0 ACK-only and phase1 ACK+data.
   Primary1 uses0x0d, four factory FDT DAC words and the reference/manual seed
   (68400–6842d,59d90,5a4f4).
3. Compare current DOWN raw words against manual raw words with57030. If all
   eight shifted-word absolute deltas are at most the factory timing threshold,
   Windows selects its reference-refresh branch (684b7–684c6). Otherwise capture
   proceeds. Neither result is a machine-verifiable finger-absence predicate.
4. Capture policy retains ten images and selects the final validated image.
   Each image wrapper writes022c capture mode, receives authenticated image20,
   then restores022c=020a with acknowledged opcode80 payload `00 2c 02 0a 02`
   before the next image/UP (5a0e0,5a2bc/5a2f7,68606–68655).
5. Opcode34: both ordered UP ACKs and a current20-byte IRQ0200 event. Only then
   publish removal, update the DOWN seed, and hand a contact to preprocessing.

DOWN builder5ae3c–5af1a uses a zeroed40-byte payload: byte0=9c for the qualified
initial globals;1=1;2..9=factory FDT DAC LE16;10..25=DOWN seed;26=phase;27=0;
28=OTP23 highbit?5:10;29=3;30..37=factory image DAC LE16;38=3;39=0.
The fixed runtime-global defaults are part of this bounded profile, not a claim
that every Windows runtime configuration uses them.

## Explicit Linux safety policies

Windows directly invokes reference refresh on the similar-snapshot branch,
without a prior UP wait. Here refresh requires a qualified lift first, using the
current DOWN-derived UP base with no image/restore prerequisite (687aa–687c7).
The old reference is not replaced until a qualified UP event and full candidate
acceptance. A new touch after UP is still possible. All reference gates
and main-thread ownership/deadline checks still apply to the new candidate.

The ten-frame selection, one in-action enrollment refresh limit, existing
bounded paired-reference reacquisition and conservative reverse-event
invalidation are Linux policies. They are not recovered Windows retry counts or
universal absence detectors. Existing template format and matching policy stay
unchanged. USB/operation errors terminate and invalidate the current context;
the next fresh initialization requests a clear reader.

## Verification boundary

Synthetic public tests cover exact payload bytes, high DAC bits, seed arithmetic
(including carries and16-bit truncation), comparison boundary/polarity, every
image restore, event/ACK orderings, stale/coalesced replies, cancellation and
bounded deadlines. No raw captured images or personalized configuration bytes
are needed by those fixtures. These tests establish source contracts, not
physical reliability or authentication error rates.

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
4. Capture exactly one image:6864e–68653 compares the count against literal1;
   wrapper5a0e0 makes one5a990 call, which submits image20 once at5ab67.
   The wrapper writes022c capture mode, receives the authenticated image,
   updates future image DACs via59bc8/58888, then restores022c=020a with
   acknowledged opcode80 payload `00 2c 02 0a 02` before UP (5a2bc/5a2f7).
   Current image pixels remain unchanged; no quality-selection loop is used.
5. Opcode34: after both ordered arm ACKs, hand the owned image to
   preprocessing without waiting for removal. Keep a separate release guard.
   Only a current20-byte IRQ0200 event may update the DOWN seed and permit
   another capture. The guard remains independent of public result completion.

DOWN builder5ae3c–5af1a uses a zeroed40-byte payload: byte0=9c for the qualified
initial globals;1=1;2..9=factory FDT DAC LE16;10..25=DOWN seed;26=phase;27=0;
28=OTP23 highbit?5:10;29=3;30..37=current live image DAC LE16;38=3;39=0.
The fixed runtime-global defaults are part of this bounded profile, not a claim
that every Windows runtime configuration uses them.

## Explicit Linux safety policies

Windows directly invokes reference refresh on the similar-snapshot branch,
without a prior UP wait. Here refresh requires a qualified lift first, using the
current DOWN-derived UP base with no image/restore prerequisite (687aa–687c7).
The old reference is not replaced until a qualified UP event and full candidate
acceptance. A new touch after UP is still possible. All reference gates
and main-thread ownership/deadline checks still apply to the new candidate.

The one in-action enrollment refresh limit, fresh-epoch exposure-history reset, existing
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

## Exposure startup and feedback

Factory decoder550d8 initializes live image DACs at context90, then550dd–550e7
snapshots word0 into immutable contexta0. Getter55430 and caller66dac–66db4
initialize the controller through42a84 with mode29,80×64,integration,initialDAC,1.
Paired-reference descriptors697da–697e2 disable post-image feedback; they use
FDT DACs. The first normal image therefore uses factory image DACs.

Post-image5a295–5a2b1 invokes59bc8 before restore. Inner57b0c uses the persistent
controller DAC, not literal151;58888 distributes the result around the minimum
of all four live words. This distribution still runs for an ineligible exposure
measurement. Updated words apply to future images and DOWN setup, not the raw
image just returned. Public synthetic oracle tests preserve exact float32 and
uint16 history semantics.

APP10034 also performs internal image/setup work before emitting DOWN:
5c20→61ba consumes the request's image DACs and byte38. The qualified byte38=3
differs from older Linux39-byte requests ending in0. This is a real firmware
setup difference; it is not labeled a recovered settling count or a DAC offset.
Consequently old saturated first-frame bursts do not prove current first-frame
behavior. Physical first-capture quality remains a hardware validation item.

## Early sample publication

Windows67850→67b15→6da00→6dae2→6a7a4 completes the pending sample request
through the WDF table at6a97a;6dae7 clears the request. This precedes68692
setting contact flag+cc and687c7 arming UP. It proves sample publication order,
not the eventual WBF matcher completion time. A later DOWN handler682ce checks
+cc and rearms UP rather than capturing. New request6c08d likewise selects UP
when+cc remains set. Valid UP clears+cc at69435 before next DOWN.

Linux follows this separation with its own generation-owned release guard,
but waits for both arm ACKs before image publication. Within one public-open session, no reference-refresh
candidate or new contact bypasses validated removal. A result delivered before
release does not assert absence or make the next contact eligible.

## Retained accepted reference after restoration

The pinned power-loss handler5f770 marks recovery and5f7bf calls5f304. That
path invokes sensor table+38 (55020) to regenerate factory configuration/DACs
and table+40 to upload, without clearing accepted reference-ready+c0 or
ImageBase+128. Request0 startup tests+c0 at66ef8 and uses retained-reference
initialization5eba0 at66f4d; it does not necessarily call696cc again. Thus an
uninterrupted MCU/transport generation is not a demonstrated prerequisite for
retaining the accepted raw baseline. Disk cache parsing alone still does not
set readiness.

Linux retains only an accepted raw reference/F1 seed after clean close on the
same device object, with exact factory/config/profile binding. Full reset, key
verification, configuration restoration and TLS authentication still precede
cache staging and fresh processing initialization. Old detector/contact/event
ownership and adaptive/exposure history do not survive. The normal-close
cancellation of an owed UP guard is expected cleanup; a new public session
requires a fresh authenticated capture, rather than treating cached data as
a match or asserting physical absence. Runtime faults and failed restoration
remove cache eligibility.

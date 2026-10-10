# Chicago preprocessing and matching for Goodix 521d

The Chicago sources were imported from libfprint merge request !669, commit
`a9286cce45a276886a04db5170dd984e84c4d2e4`, directory
`libfprint/drivers/goodix5125/chicago/`. Original copyright and
LGPL-2.1-or-later notices are retained in each source. The additional GF5298
arithmetic and profile separation use the same license. No proprietary binary,
calibration cache, biometric image or extracted template is included.

## Explicit profiles

The legacy constructors retain ChicagoHS profile 24 behavior. The 521d driver
must use `GOODIX_CHICAGO_PROFILE_GF5298` (17) through the additive profile-aware
constructors. Input is an 80 by 64 row-major uint16 image, containing decoded
12-bit samples. It is already transposed from the sensor's native packed order;
this module does not decode transport packets or infer their orientation.

A measured empty-reader reference initializes a session. The free first-run
path generates calibration from that reference. An explicitly supplied valid
calibration payload is supported separately. Neither path substitutes a finger
image for a background, invents an exposure, or silently changes profiles.

Profile 17 includes its own reference/capture range gates, edge preparation,
reference-offset convention, mask generation and filling, coated signal
normalization, local gain, ratio/history arithmetic, repeated-frame gate,
Q14 final gain, flat-range rejection and alternate-image selector. Shared
feature, enhancement and resolution-context stages remain derived from MR669;
full equivalence to the Windows pipeline is not asserted.

Adaptive preprocessor and session states are profile-tagged. Cross-profile
restoration is rejected. Production enrollment uses `oc_enroll_finish_complete`;
a partial gallery must never become a completed stored print. The older partial
finish interface exists for diagnostic compatibility only.

## Verification

Run `sh tests/goodix521d-chicago-profile.sh` from a checkout with GLib/GIO
development files, pkg-config, a C compiler and Python 3. Its fixtures are purely
synthetic. The adapter exposes selected internal arithmetic for tests and must
not be linked into the production library.

The development validation also compared synthetic intermediate stages against
an offline, pinned vendor oracle: seven raw/filled mask cases, five local-gain
cases and twenty normalized frames matched exactly. These private oracle runs
are supporting evidence, not a build dependency.

A fixed private corpus of 79 Windows API captures, split into 18 enrollment,
20 same-finger and 41 other-finger captures, was replayed without fitting
thresholds. Final profile24 outputs and scalar metadata remained identical for
all 79 captures. Both profile17 replay variants repeated identically.
The cache-free profile17 path returned usable preprocessing for all 79 captures.
Its partial-gallery matcher diagnostic matched 19/20 same-finger probes and 0/41
other-finger probes. The supplied-calibration variant matched 20/20 same-finger
probes and 0/38 usable other-finger probes; three other-finger images were
unusable. These small diagnostic counts are not production error-rate estimates.

Crucially, the 18 fixed enrollment captures reached only 10 of 12 natural stages.
An actual raw-frame public-session test confirmed strict completion refuses to
export that incomplete enrollment. Hardware acquisition, completed independent
placement enrollment and subsequent live verification remain separate
integration validation requirements.

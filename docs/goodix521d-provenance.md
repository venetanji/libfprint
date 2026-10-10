# Source provenance and review boundaries

- Base: official libfprint master `6f9479c3d55f847c1b3769f28ceb99227f9858cf`, imported into `venetanji/libfprint`.
- Free Chicago code: libfprint MR !669 at `a9286cce45a276886a04db5170dd984e84c4d2e4`, selectively copied from its Chicago module. Original LGPL notices and authors are retained. Profile 17 additions are explicit; historical profile 24 entry points and outputs remain supported.
- Authenticated transport and the GF5298 paired-reference/contact profile: the previously tested isolated v3 candidate, SHA256 `0ea715bbe428f029907c2354b25ac7d22a8b65077164897687276f6aaa2217ad`, retaining the original 2021 Goodix TLS authors' LGPL notices. It has been adapted to FpDevice with additional cancellation, event ownership and cleanup tests.
- The public unpersonalized 256-byte configuration base has SHA256 `101c07451512f8ea7220b8988a04f8d044d2b91918d082a94fba68bdcc9bdd72`. Personalization uses the provisioned reader record in memory; no unit-specific configuration or factory bytes are included.

The branch does not import the MR's 5125 USB transport, pairing writes, power-button unlock behavior or a proprietary runtime. No vendor DLL, calibration cache, fingerprint image, enrolled template, PSK or other private input belongs in the repository.

Profile 17 behavior was derived from bounded analysis and offline comparisons. The qualified acquisition sequence and independent-placement protocol are described in `goodix521d.md`. Numerical matcher observations are small-sample engineering diagnostics; they are not a population false-accept/reject claim. The old vendor-backed GTK application's physical success does not qualify this new fully free backend.

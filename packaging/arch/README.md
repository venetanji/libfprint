# Build the experimental Goodix 521d package on Arch / Omarchy

This branch adds experimental free-software support for USB **27c6:521d**. A successful build is not evidence that fingerprint authentication is reliable or secure on your reader. Keep password login available; this package does not change PAM, enable services, provision keys, or flash firmware.

## Clone and build

Use a regular user account on an up-to-date Arch/Omarchy x86_64 system with `git` and the `base-devel` package group available:

```sh
git clone --branch feat/goodix-521d-openchicago https://github.com/venetanji/libfprint.git
cd libfprint
# Record exactly which source revision you are testing.
git rev-parse HEAD
cd packaging/arch
makepkg --syncdeps
```

`--syncdeps` may ask to install missing build/test dependencies through pacman. It **does not install the resulting libfprint package**. Omit this flag if dependencies are already installed and you want makepkg to stop rather than install anything. Do not add `--install`/`-i` for a build-only test.

The package takes its source from the local clone's exact committed HEAD using a `#commit=` Git source. It does not download a mutable merge-request branch or include uncommitted changes. Commit or stash tracked changes before building. After an intentional `git pull`, run `makepkg --cleanbuild --force --syncdeps` to rebuild the newly selected commit; preserve its printed revision with your test report.

The output is `libfprint-goodix-521d-*.pkg.tar.zst` (and possibly a separate debug package). The normal Meson tests run before packaging. Do not use `--nocheck` to claim a passing build; report any failing tests and the build log.

## Existing package protection

The actual package name is deliberately **libfprint-goodix-521d**, matching the already installed package and the user's existing removal-protection hook. Same-name package upgrades/downgrades should remain upgrade transactions rather than removing this protected package. Compare the installed and built versions before any installation; this recipe does not assume or change the installed epoch.

Keep `/etc/pacman.d/hooks/00-protect-goodix-521d.hook` intact. This build does not disable it, rename it, bypass its `AbortOnFail` guard or modify pacman hooks. If a future transaction proposes removing `libfprint-goodix-521d`, stop and inspect it rather than weakening the protection.

The existing local fingerprint-setup adapter makes a temporary copy of Omarchy's wizard, checks/replaces its libfprint-git installation block and backs up enrollment data. This repository does not overwrite that adapter or the packaged Omarchy scripts. Continue to use the existing reviewed adapter instead of assuming virtual Provides makes the unmodified wizard provider-aware.

## Dependencies and compatibility

This recipe follows Omarchy’s libfprint-git dependency set and the official Arch all-driver build. It retains GObject introspection and API documentation. Runtime dependencies: `gcc-libs glib2 glibc libgudev libgusb openssl pixman`. Build/test dependencies are listed explicitly in `PKGBUILD`.

The experimental package provides `libfprint=1.94.100`, versioned `libfprint-git` at this experimental package version, and `libfprint-2.so`; makepkg derives the actual shared-library provision. It conflicts explicitly with both `libfprint` and Omarchy’s `libfprint-git` because they own the same library and headers. There is no automatic `replaces` directive or install script.

Arch fprintd 1.94.5-2 currently depends on both `libfprint` and `libfprint-2.so=2-64`. Upstream libfprint's major SONAME is still `libfprint-2.so.2`. Keeping that ABI means the existing fprintd package can use this library without a fprintd fork. This is package/ABI compatibility, not a hardware authentication guarantee.

Inspect the built package before any separate installation decision:

```sh
pacman -Qip ./libfprint-goodix-521d-*.pkg.tar.zst
bsdtar -xOf ./libfprint-goodix-521d-*.pkg.tar.zst .PKGINFO
```

If debug packages are enabled, choose the non-debug filename explicitly instead of a broad glob. Check that `Provides` includes the libfprint package and the major-2 64-bit library dependency required by your installed fprintd. An installation would replace your system library; reader testing, enabling authentication and rollback are separate steps. Do not copy `.so` files over `/usr/lib` manually.

## Omarchy patch compatibility

The reference is Omarchy’s recipe at commit `5fcf959788954043d769928080beedca8ded894b`, which pins upstream libfprint `6f9479c3d55f847c1b3769f28ceb99227f9858cf`, the same base as this branch.

- Synaptics `06cb:010b` and FocalTech `2808:a97a` are already in that base and remain present. Omarchy carries an old one-line A97A patch but conditionally skips it when the ID exists. Applying it blindly now fails because its old table context has moved. We keep the existing source entry, not a duplicate patch.
- The package check verifies that the compiled device list retains both IDs and adds `27c6:521d`.
- Omarchy applies `aurora-apple-sep.patch` only on aarch64 for Apple Touch ID via `/dev/sep-bio` interface version 4. This recipe is explicitly x86_64-only, matching the 521d target; it does not claim aarch64/Apple support. Do not broaden the architecture list without reviewing and preserving that patch. Its current patch touches core device APIs and both Meson files; it applies to the untouched base, but must be rechecked against the completed Goodix integration.
- Omarchy’s canonical package is `libfprint-git`, epoch 1, package release 3. This experimental package retains the existing actual name `libfprint-goodix-521d`; it does not impersonate Omarchy’s canonical package or copy its epoch. Its versioned `libfprint`, `libfprint-git` and SONAME provisions can satisfy package dependencies, but they do not make the installed package’s actual name `libfprint-git`. Current Omarchy’s `omarchy-pkg-missing` uses exact `pacman -Q -- "$pkg"` queries; fingerprint setup then explicitly installs `libfprint-git` with `--ask 4` (accepting replacement conflicts). Thus virtual Provides alone does **not** prevent that setup flow from replacing this experiment. Ordinary system updates use `pacman -Syu`; this is a different path from fingerprint setup. Keeping the canonical package name would satisfy the exact-name guard but would still allow future repository upgrades to replace custom code. No package hold or helper workaround is installed here. Check the specific installed Omarchy helper version and any existing workaround before invoking setup.

Pinned helper evidence (Omarchy `077ac1da939de00d061c1035e1a1d00587a119b8`):
- https://github.com/omacom/omarchy/blob/077ac1da939de00d061c1035e1a1d00587a119b8/bin/omarchy-pkg-missing
- https://github.com/omacom/omarchy/blob/077ac1da939de00d061c1035e1a1d00587a119b8/bin/omarchy-setup-security-fingerprint
- https://github.com/omacom/omarchy/blob/077ac1da939de00d061c1035e1a1d00587a119b8/bin/omarchy-update-system-pkgs

Pinned reference: https://github.com/omacom/omarchy-pkgs/tree/5fcf959788954043d769928080beedca8ded894b/pkgbuilds/libfprint-git

## Licensing and provenance

The libfprint source and derived driver retain their LGPL-2.1-or-later notices and attribution. The proprietary Goodix DLL, Windows calibration caches, enrolled fingerprints and private captures must not be bundled. The package's code provenance is the checked-out repository commit, with further upstream attribution in the driver files.

Primary packaging references, checked 2026-10-10:
- https://archlinux.org/packages/extra/x86_64/libfprint/
- https://gitlab.archlinux.org/archlinux/packaging/packages/libfprint/-/blob/main/PKGBUILD
- https://archlinux.org/packages/extra/x86_64/fprintd/
- https://man.archlinux.org/man/PKGBUILD.5.en

## Verification limits

The packaging metadata, shell syntax and recipe command construction were checked in the preparation environment. The actual `makepkg` dependency resolution, full Arch build and package SONAME metadata must be verified on Arch. A Linux build elsewhere does not substitute for that check.

# Build and sign WindowsiOS-ez

## Manual Codemagic build

1. Connect the repository in Codemagic and use the YAML workflow **WindowsiOS-ez unsigned iOS IPA**. Leave automatic triggers and artifact publishing disabled.
2. Select the development branch or commit to test. Confirm that the workflow shows Mac mini M2, Xcode 27.2 and the 20-minute limit, then start it manually. [Codemagic Xcode 27.2 image](https://docs.codemagic.io/specs-macos/xcode-27-2/)
3. Download `WindowsiOS-ez.ipa`, `WindowsiOS-ez.entitlements` and `SHA256SUMS` together. Confirm the checksums before signing.

```sh
shasum -a 256 -c SHA256SUMS
```

The pipeline syncs submodules, installs missing tools, stages installer resources, compiles device-only static libraries and generates the Xcode project. It checks Release settings and signing entitlements, builds the physical ARM64 iOS destination, checks linked archives and validates the executable and ZIP. The bundled QEMU firmware banks, ARM64 NetKVM files and licenses are checked against SHA-256 pins. The build fetches installer inputs once per digest in the existing Codemagic cache. The IPA contains firmware and the driver package. Windows installation media are obtained separately. Review both the cold and warm duration during manual validation. The workflow specifies the same machine during both builds.

## Local build on Apple Silicon macOS

Use Xcode 27.2 with the iPhoneOS SDK selected and the license accepted. Install XcodeGen, Meson, Ninja, pkgconf and Python 3. The build uses Clang, Make, `libtool`, `lipo`, `vtool`, curl and the system OpenSSL command-line utility.

```sh
HOMEBREW_NO_AUTO_UPDATE=1 brew install xcodegen meson ninja pkgconf autoconf automake libtool
git submodule sync --recursive
git submodule update --init --recursive
./scripts/build_unsigned_ipa.sh
(cd build/artifacts && shasum -a 256 -c SHA256SUMS)
```

Native dependency versions are libffi 3.5.0, GLib 2.83.0, libslirp v4.9.1 from the UTM mirror, libudfread at upstream revision `b0bc6957e7e0`, wimlib 1.14.5, FatFs R0.16, libtpms 0.10.2 and OpenSSL 3.5.9. The source archives are checksum-pinned. The libudfread revision supports the nonzero UDF physical partition number used by Microsoft 24H2 media. GLib's wraps pin PCRE2 10.44 and the `proxy-libintl` revision. These are device static archives and the app also links the platform resolver and iconv libraries. Dependency failure output includes the tail of the affected component's build log. Before distributing signed binaries, review the source and relinking obligations for the statically linked components. [Third-party notices](THIRD_PARTY_NOTICES.md)

## Downstream signing

The IPA requires signing before installation. Use a sideloader that supports your device and permitted entitlements. The default App ID is `io.github.hazerbvisor.windowsios-ez`. The separate manifest requests `get-task-allow`, increased memory and extended virtual addressing. Use a signing profile that authorizes the entitlements needed for the test, and verify the signed result. A profile or sideloader may omit entitlements it cannot grant. The application's runtime diagnostics distinguish signature values from available process headroom.

For teams signing directly with Xcode, run the dependency script and `make ios-lib` first, then stage the installer resources and generate the project.

```sh
python3 scripts/prepare_installer_assets.py
xcodegen generate --spec ios/project.yml
xcodebuild -project ios/WindowsiOS.xcodeproj -scheme WindowsiOS -configuration Release \
  -sdk iphoneos -destination 'generic/platform=iOS' \
  DEVELOPMENT_TEAM="$WINIOS_SIGNING_TEAM" \
  CODE_SIGN_IDENTITY="$WINIOS_SIGNING_IDENTITY" \
  PROVISIONING_PROFILE_SPECIFIER="$WINIOS_SIGNING_PROFILE" build
```

Keep credentials and pairing records local. If the sideloader changes the app's bundle ID, use the installed bundle ID for debugger attachment. See [TESTING.md](TESTING.md) for the prepared-disk smoke test.

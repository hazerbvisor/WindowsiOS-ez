# Build and sign WindowsiOS-ez

## Manual Codemagic build

1. Connect the repository in Codemagic and use the YAML workflow **WindowsiOS-ez unsigned iOS IPA**. Leave automatic triggers and artifact publishing disabled.
2. Select the development branch or commit to test. Confirm that the workflow shows Mac mini M2, Xcode 27.2 and the 20-minute limit, then start it manually. [Codemagic Xcode 27.2 image](https://docs.codemagic.io/specs-macos/xcode-27-2/)
3. Download `WindowsiOS-ez.ipa`, `WindowsiOS-ez.entitlements` and `SHA256SUMS` together. Confirm the checksums before signing.

```sh
shasum -a 256 -c SHA256SUMS
```

The pipeline syncs submodules, installs missing tools, compiles device-only static libraries with the existing native build rules, and generates the Xcode project. It checks the generated Release settings and signing manifest, builds the generic physical iOS destination, checks the linked archives, and verifies the ARM64 iOS executable, ZIP contents and JIT helper resource. The cache is keyed by dependency-script content, compiler, SDK, minimum OS and cross-build tool versions. The build artifacts contain no Windows guest or firmware. Review both the cold and warm duration during manual validation. The workflow specifies the same machine during both builds.

## Local build on Apple Silicon macOS

Use Xcode 27.2 with the iPhoneOS SDK selected and the license accepted. Install XcodeGen, Meson, Ninja, pkgconf and Python 3. The developer toolchain also supplies Clang, Make, `libtool`, `lipo` and `vtool`.

```sh
HOMEBREW_NO_AUTO_UPDATE=1 brew install xcodegen meson ninja pkgconf
git submodule sync --recursive
git submodule update --init --recursive
./scripts/build_unsigned_ipa.sh
(cd build/artifacts && shasum -a 256 -c SHA256SUMS)
```

Native dependency versions are libffi 3.5.0, GLib 2.83.0, libslirp v4.9.1 from the UTM mirror, and libudfread 1.2.0. The four tarballs are checksum-pinned. GLib's wraps pin PCRE2 10.44 and the `proxy-libintl` revision. These are device static archives and the app also links the platform resolver and iconv libraries. Dependency failure output includes the tail of the affected component's build log. Before distributing signed binaries, review the source and relinking obligations for the statically linked components. [Third-party notices](THIRD_PARTY_NOTICES.md)

## Downstream signing

The IPA requires signing before installation. Use a sideloader that supports your device and permitted entitlements. The default App ID is `io.github.hazerbvisor.windowsios-ez`. The separate manifest requests `get-task-allow`, increased memory and extended virtual addressing. Use a signing profile that authorizes the entitlements needed for the test, and verify the signed result. A profile or sideloader may omit entitlements it cannot grant. The application's runtime diagnostics distinguish signature values from available process headroom.

For teams signing directly with Xcode, generate the project, then pass your signing values as build arguments. Run the dependency script and `make ios-lib` first.

```sh
xcodegen generate --spec ios/project.yml
xcodebuild -project ios/WindowsiOS.xcodeproj -scheme WindowsiOS -configuration Release \
  -sdk iphoneos -destination 'generic/platform=iOS' \
  DEVELOPMENT_TEAM="$WINIOS_SIGNING_TEAM" \
  CODE_SIGN_IDENTITY="$WINIOS_SIGNING_IDENTITY" \
  PROVISIONING_PROFILE_SPECIFIER="$WINIOS_SIGNING_PROFILE" build
```

Keep credentials and pairing records local. If the sideloader changes the app's bundle ID, use the installed bundle ID for debugger attachment. See [TESTING.md](TESTING.md) for the prepared-disk smoke test.

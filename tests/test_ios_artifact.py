# SPDX-License-Identifier: GPL-2.0-or-later
import json
import plistlib
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from scripts import verify_ios_artifact as verify


class BuildArtifactTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.entitlements = self.work / "WindowsiOS.entitlements"
        self.entitlements.write_bytes(plistlib.dumps(verify.ENTITLEMENTS))

    def make_ipa(self, *, platform=2, signature=False, resource=True):
        ipa = self.work / "test.ipa"
        commands = struct.pack("<6I", 0x32, 24, platform, 17 << 16, 27 << 16, 0)
        if signature:
            commands += struct.pack("<4I", 0x1D, 16, 0, 0)
        header = struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0, 2,
                             2 if signature else 1, len(commands), 0, 0)
        info = {
            "CFBundleIdentifier": verify.BUNDLE_ID,
            "CFBundlePackageType": "APPL",
            "CFBundleExecutable": "WindowsiOS",
            "CFBundleSupportedPlatforms": ["iPhoneOS"],
            "MinimumOSVersion": "17.0",
        }
        with zipfile.ZipFile(ipa, "w") as bundle:
            bundle.writestr(verify.APP + "Info.plist", plistlib.dumps(info))
            bundle.writestr(verify.APP + "WindowsiOS", header + commands)
            if resource:
                bundle.writestr(verify.APP + "henge-jit.js", "script")
        return ipa

    def test_device_payload_passes(self):
        verify.verify_ipa(self.make_ipa())

    def test_arm64_simulator_payload_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "physical iOS"):
            verify.verify_ipa(self.make_ipa(platform=7))

    def test_signed_executable_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "signature"):
            verify.verify_ipa(self.make_ipa(signature=True))

    def test_missing_resource_is_rejected(self):
        with self.assertRaises(KeyError):
            verify.verify_ipa(self.make_ipa(resource=False))

    def test_generated_settings_and_signing_manifest(self):
        settings = {
            "ARCHS": "arm64",
            "CONFIGURATION": "Release",
            "PLATFORM_NAME": "iphoneos",
            "PRODUCT_BUNDLE_IDENTIFIER": verify.BUNDLE_ID,
            "IPHONEOS_DEPLOYMENT_TARGET": "17.0",
            "CODE_SIGNING_ALLOWED": "NO",
            "CODE_SIGNING_REQUIRED": "NO",
            "CODE_SIGN_ENTITLEMENTS": self.entitlements.name,
            "SRCROOT": str(self.work),
        }
        path = self.work / "settings.json"
        path.write_text(json.dumps([{"target": "WindowsiOS", "buildSettings": settings}]))
        verify.verify_settings(path, self.entitlements)
        verify.verify_entitlements(self.entitlements)
        self.entitlements.write_bytes(plistlib.dumps({"get-task-allow": True}))
        with self.assertRaisesRegex(ValueError, "Signing manifest"):
            verify.verify_entitlements(self.entitlements)


if __name__ == "__main__":
    unittest.main()

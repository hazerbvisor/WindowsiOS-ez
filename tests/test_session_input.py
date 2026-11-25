# SPDX-License-Identifier: GPL-2.0-or-later
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SWIFTC = os.environ.get("SWIFTC") or shutil.which("swiftc")


class SessionInputTests(unittest.TestCase):
    @unittest.skipUnless(SWIFTC, "Swift compiler is unavailable")
    def test_button_sources_and_display_geometry(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            source = work / "main.swift"
            source.write_text("""
import Foundation
var state = GuestPointerState()
state.set(1, source: "mouse", down: true)
state.set(2, source: "mouse", down: true)
state.set(1, source: "touch", down: true)
state.set(4, source: "second mouse", down: true)
precondition(state.buttons == 7)
state.release("mouse")
precondition(state.buttons == 5)
state.set(1, source: "touch", down: false)
precondition(state.buttons == 4)
state.reset()
precondition(state.buttons == 0)

let viewport = CGSize(width: 1000, height: 600)
let guest = CGSize(width: 1024, height: 768)
let fit = GuestDisplayGeometry.frame(viewport: viewport, pixels: guest, displayScale: 2, scaling: .fit)
precondition(fit == CGRect(x: 100, y: 0, width: 800, height: 600))
let fill = GuestDisplayGeometry.frame(viewport: viewport, pixels: guest, displayScale: 2, scaling: .fill)
precondition(fill == CGRect(x: 0, y: -75, width: 1000, height: 750))
let pixel = GuestDisplayGeometry.frame(viewport: viewport, pixels: guest, displayScale: 2, scaling: .pixelPerfect)
precondition(pixel.size == CGSize(width: 512, height: 384))
let rotated = GuestDisplayGeometry.frame(viewport: CGSize(width: 600, height: 1000), pixels: guest,
                                          displayScale: 2, scaling: .fit)
precondition(rotated.height == 450 && rotated.minY == 275)
let zoom = GuestDisplayGeometry.frame(viewport: viewport, pixels: guest, displayScale: 2, scaling: .zoom,
                                      zoom: 2, offset: CGSize(width: 5000, height: -5000))
precondition(zoom.minX == 0 && zoom.maxY == 600)
precondition(GuestDisplayGeometry.normalized(CGPoint(x: 500, y: 300), in: fit) == CGPoint(x: 0.5, y: 0.5))
precondition(GuestDisplayGeometry.normalized(CGPoint(x: -20, y: 670), in: fit) == CGPoint(x: 0, y: 1))
""")
            executable = work / "input"
            subprocess.run([SWIFTC, "-warnings-as-errors", ROOT / "ios/Sources/GuestPointerState.swift",
                            source, "-o", executable], check=True)
            subprocess.run([executable], check=True)

    @unittest.skipUnless(os.uname().sysname == "Linux", "requires ELF section garbage collection")
    def test_tablet_preserves_drag_origin_and_reports_release(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            (work / "TargetConditionals.h").write_text("#define TARGET_OS_IPHONE 0\n")
            main = work / "main.c"
            main.write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "xhci.h"

void hv_machine_wake(HvMachine *m) { (void)m; }

static unsigned x_at(const uint8_t *report) {
    return report[1] | (unsigned)report[2] << 8;
}

int main(void) {
    HvXhci controller = {0};
    pthread_mutex_init(&controller.mu, 0);
    hv_xhci_tablet(&controller, 1000, 2000, 1, 0);
    hv_xhci_tablet(&controller, 1500, 2500, 1, 0);
    hv_xhci_tablet(&controller, 3500, 4500, 1, 0);
    hv_xhci_tablet(&controller, 5100, 6100, 0, 0);
    hv_xhci_tablet(&controller, 8600, 9600, 0, 0);
    assert(controller.ttail == 4);
    assert(controller.tab_reports[0][0] == 1 && x_at(controller.tab_reports[0]) == 1000);
    assert(controller.tab_reports[1][0] == 1 && x_at(controller.tab_reports[1]) == 3500);
    assert(controller.tab_reports[2][0] == 0 && x_at(controller.tab_reports[2]) == 5100);
    assert(controller.tab_reports[3][0] == 0 && x_at(controller.tab_reports[3]) == 8600);
    for (unsigned i = 0; i < HV_XHCI_REPORTS; i++)
        hv_xhci_tablet(&controller, 5100, 6100, 7, 1);
    hv_xhci_tablet(&controller, 5100, 6100, 0, 0);
    assert(controller.ttail - controller.thead == HV_XHCI_REPORTS);
    assert(controller.tab_reports[(controller.ttail - 1) % HV_XHCI_REPORTS][0] == 0);
    for (unsigned i = 0; i < HV_XHCI_REPORTS; i++) hv_xhci_key(&controller, 0xe0, 1);
    hv_xhci_key(&controller, 0xe0, 0);
    assert(controller.reports[(controller.rtail - 1) % HV_XHCI_REPORTS][0] == 0);
    pthread_mutex_destroy(&controller.mu);
}
""")
            executable = work / "input"
            subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-ffunction-sections",
                            "-fdata-sections", "-Wl,--gc-sections", "-I", work, "-I", ROOT / "devices",
                            "-I", ROOT / "core", "-I", ROOT / "host", main, ROOT / "devices/xhci.c",
                            "-pthread", "-o", executable], check=True)
            subprocess.run([executable], check=True)

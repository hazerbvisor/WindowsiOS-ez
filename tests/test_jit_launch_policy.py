# SPDX-License-Identifier: GPL-2.0-or-later
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class JITLaunchPolicyTests(unittest.TestCase):
    def test_independent_jit_and_ram_bounds(self):
        source = r"""
#include <assert.h>
#include "winios_jit.h"
#include "jit_region.h"

int main(void) {
    const unsigned combinations[][2] = {{4608,384}, {4608,512}, {6144,384}, {8192,1024}, {10240,1536}};
    for (unsigned i = 0; i < sizeof combinations / sizeof combinations[0]; ++i) {
        uint64_t ram = combinations[i][0];
        size_t jit = (size_t)combinations[i][1] << 20;
        assert(winios_guest_ram_fits(ram, 12288));
        assert(!winios_guest_ram_fits(ram, ram + 767));
        assert(winios_guest_ram_fits(ram, ram + 768));
        assert(winios_jit_rx_fits(WINIOS_JIT_LOW_LIMIT - jit, jit));
        assert(!winios_jit_rx_fits(WINIOS_JIT_LOW_LIMIT - jit + 16384, jit));
        assert(!winios_jit_rx_fits(UINT64_MAX - 1024, jit));
    }
    const void *rx = (const void *)(uintptr_t)0x123400000ull;
    HvJitRegion far = {.rw_diff = (intptr_t)0x900000000ull};
    assert((uintptr_t)hv_jit_writable_address(&far, rx) == 0xa23400000ull);
    far.rw_diff = -(intptr_t)0x100000000ull;
    assert((uintptr_t)hv_jit_writable_address(&far, rx) == 0x23400000ull);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "policy"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", ROOT / "ios/Sources/Native", "-I", ROOT / "host",
                            "-x", "c", "-", "-o", path], input=source, text=True, check=True)
            subprocess.run([path], check=True)

    @unittest.skipUnless(shutil.which("node"), "Node.js is unavailable")
    def test_debugger_rejects_bad_rx_and_writes_full_registers(self):
        script = ROOT / "ios/Sources/Resources/henge-jit.js"
        test = r"""
const {readFileSync} = require("node:fs");
const {runInNewContext} = require("node:vm");
const assert = require("node:assert/strict");
const source = readFileSync(process.argv[1], "utf8");
const functions = source.slice(source.indexOf("function JIT26PrepareRegion"));
function check(reply, shouldPrepare, shouldFree, result, prepareResult = true) {
    const sent = [];
    const prepared = [];
    const globals = {
        x0: 0n, x1: 1536n << 20n, tid: "01",
        send_command: command => { sent.push(command); return command.startsWith("_M") ? reply : "OK"; },
        prepare_memory_region: (address, bytes) => { prepared.push([address, bytes]); return prepareResult; },
        log: () => {},
    };
    runInNewContext(functions + "\nJIT26PrepareRegion();", globals);
    assert.equal(prepared.length, Number(shouldPrepare));
    assert.equal(sent.some(command => command.startsWith("_m")), shouldFree);
    assert.equal(sent.at(-1), `P0=${globals.numberToLittleEndianHexString(result)};thread:01;`);
    assert.equal(globals.littleEndianHexStringToNumber("21436587a9cbed0f"), 0x0fedcba987654321n);
    assert.equal(globals.numberToLittleEndianHexString(0x0fedcba987654321n), "21436587a9cbed0f");
}
check("100000000", true, false, 0x100000000n);
check("E01", false, false, 1n);
check("7000000000", false, true, 3n);
check("100000000", true, true, 2n, false);
"""
        subprocess.run(["node", "-e", test, script], check=True)


if __name__ == "__main__":
    unittest.main()

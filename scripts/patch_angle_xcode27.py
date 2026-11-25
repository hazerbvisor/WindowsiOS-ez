#!/usr/bin/env python3
"""Adapt the pinned WebKit ANGLE build to Xcode 27's libc++."""
import sys
from pathlib import Path
p = Path(sys.argv[1]) / "Configurations/CommonBase.xcconfig"
s = p.read_text()
old = "-D_LIBCPP_ENABLE_ASSERTIONS=1"
new = "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE"
if old not in s:
    raise SystemExit("Expected pinned ANGLE assertion configuration not found")
p.write_text(s.replace(old, new))
print("ANGLE: migrated libc++ assertions to extensive hardening")

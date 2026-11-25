#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Wrap verified upstream raw dylibs as iOS frameworks, preserving ANGLE lookup."""
import pathlib, plistlib, re, shutil, subprocess, sys
app,prefix=map(pathlib.Path,sys.argv[1:])
lib=(prefix/"lib").resolve()
out=app/"Frameworks"
out.mkdir(exist_ok=True)
staged={}
def run(*args):
    return subprocess.check_output(args,text=True).strip()
def check(p):
    if run("xcrun","lipo","-archs",str(p))!="arm64":
        raise RuntimeError(f"Not thin ARM64: {p}")
    if not re.search(r"^\s*platform IOS\s*$",run("xcrun","vtool","-show-build",str(p)),re.M):
        raise RuntimeError(f"Not physical iOS: {p}")
def imports(p):
    return [l.strip().split(" (compatibility version",1)[0] for l in run("xcrun","otool","-L",str(p)).splitlines()[1:]]
def identity(p):
    return run("xcrun","otool","-D",str(p)).splitlines()[-1]
def canonical(p):
    name=p.name
    if not name.endswith(".dylib"):
        raise RuntimeError(f"Not dylib: {p}")
    name=name[:-6]
    if name.startswith("lib"): name=name[3:]
    return re.sub(r"(?:\.\d+)+$","",name)
def stage(source):
    source=source.resolve(strict=True)
    if not source.is_relative_to(lib):
        raise RuntimeError(f"Dependency outside verified sysroot: {source}")
    name=canonical(source)
    if not re.fullmatch(r"[A-Za-z0-9_.+-]+",name):
        raise RuntimeError(f"Invalid library name: {name}")
    rel=f"{name}.framework/{name}"
    if name in staged:
        if staged[name]!=source: raise RuntimeError(f"Conflicting binaries for {name}")
        return rel
    check(source)
    original=identity(source)
    target=out/rel
    target.parent.mkdir()
    shutil.copyfile(source,target)
    target.chmod(0o755)
    staged[name]=source
    if subprocess.run(["codesign","-d",str(target)],capture_output=True).returncode==0:
        subprocess.run(["codesign","--remove-signature",str(target)],check=True)
    subprocess.run(["xcrun","install_name_tool","-id","@rpath/"+rel,str(target)],check=True)
    for dep in imports(source):
        if dep==original or dep.startswith(("/System/Library/","/usr/lib/")): continue
        base=pathlib.PurePosixPath(dep).name
        child=lib/base
        if not child.is_file() and ".framework/" in dep:
            child=lib/("lib"+base+".dylib")
        if not child.is_file(): raise RuntimeError(f"Unresolved {dep} in {source}")
        child_rel=stage(child)
        subprocess.run(["xcrun","install_name_tool","-change",dep,"@rpath/"+child_rel,str(target)],check=True)
    (target.parent/"Info.plist").write_bytes(plistlib.dumps({
        "CFBundleExecutable":name,"CFBundleIdentifier":"io.github.hazerbvisor.windowsios-ez.gpu."+name.replace("_","-").replace("+","-"),
        "CFBundleName":name,"CFBundlePackageType":"FMWK","CFBundleVersion":"1","CFBundleShortVersionString":"1.0",
        "MinimumOSVersion":"17.0","CFBundleSupportedPlatforms":["iPhoneOS"]
    }))
    return rel
roots={k:stage(lib/v) for k,v in {
    "Renderer":"libvirglrenderer.dylib","DXMT":"libdxmt-native.dylib","EGL":"libEGL.dylib"
}.items()}
# libepoxy dlopens these by their framework names rather than linking them.
stage(lib/"libGLESv2.dylib")
if (lib/"libGLESv1_CM.dylib").is_file(): stage(lib/"libGLESv1_CM.dylib")
for name in staged:
    target=out/f"{name}.framework"/name
    for dep in imports(target):
        if dep.startswith(("/System/Library/","/usr/lib/")): continue
        if not dep.startswith("@rpath/") or not (out/dep[7:]).is_file():
            raise RuntimeError(f"Unresolved final dependency: {dep}")
(app/"MetalGPULab.plist").write_bytes(plistlib.dumps(roots))
print("Staged precompiled ARM64 iOS frameworks:",", ".join(sorted(staged)))

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage only the transitive iPhoneOS dylib dependencies into a lab app."""
import argparse
import pathlib
import plistlib
import re
import shutil
import subprocess

def run(*args):
    return subprocess.check_output(args,text=True).strip()

def check(path):
    if run("xcrun","lipo","-archs",str(path))!="arm64":
        raise RuntimeError(f"Not thin arm64: {path}")
    if not re.search(r"^\s*platform IOS\s*$",run("xcrun","vtool","-show-build",str(path)),re.M):
        raise RuntimeError(f"Not physical iOS: {path}")

def deps(path):
    return [l.strip().split(" (compatibility version",1)[0] for l in run("xcrun","otool","-L",str(path)).splitlines()[1:]]

def identity(path):
    return run("xcrun","otool","-D",str(path)).splitlines()[-1]

def system(name):
    return name.startswith(("/System/Library/","/usr/lib/"))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("app",type=pathlib.Path)
    p.add_argument("renderer",type=pathlib.Path)
    p.add_argument("dxmt",type=pathlib.Path)
    p.add_argument("egl",type=pathlib.Path)
    p.add_argument("search",nargs="+",type=pathlib.Path)
    a=p.parse_args()
    dest=a.app/"Frameworks"
    dest.mkdir(exist_ok=True)
    staged={}
    def stage(source, name=None):
        source=source.resolve(strict=True)
        name=name or source.name
        if name in staged:
            if staged[name]!=source: raise RuntimeError(f"Ambiguous dependency: {name}")
            return name
        check(source)
        original=identity(source)
        out=dest/name
        if out.exists(): raise RuntimeError(f"Refusing to replace {out}")
        shutil.copyfile(source,out)
        out.chmod(0o755)
        staged[name]=source
        # Remove ad-hoc build signatures before changing the install names.
        if subprocess.run(["codesign","-d",str(out)],capture_output=True).returncode==0:
            subprocess.run(["codesign","--remove-signature",str(out)],check=True)
        subprocess.run(["xcrun","install_name_tool","-id","@loader_path/"+name,str(out)],check=True)
        for dep in deps(source):
            if dep==original or system(dep): continue
            base=pathlib.PurePosixPath(dep).name
            candidates={q.resolve() for root in a.search+[source.parent] if (q:=root/base).is_file()}
            if len(candidates)!=1:
                raise RuntimeError(f"Missing or ambiguous dependency {dep}: {candidates}")
            child=stage(next(iter(candidates)),base)
            subprocess.run(["xcrun","install_name_tool","-change",dep,"@loader_path/"+child,str(out)],check=True)
        return name
    roots={"Renderer":stage(a.renderer,"libvirglrenderer.dylib"),"DXMT":stage(a.dxmt,"libdxmt-native.dylib"),"EGL":stage(a.egl,"libEGL.dylib")}
    for name in staged:
        out=dest/name
        check(out)
        for dep in deps(out):
            if system(dep): continue
            if not dep.startswith("@loader_path/") or not (dest/dep.split("/",1)[1]).is_file():
                raise RuntimeError(f"Unresolved dependency {dep} in {name}")
    (a.app/"MetalGPULab.plist").write_bytes(plistlib.dumps(roots))
    print("Staged physical iOS lab dependencies:",", ".join(sorted(staged)))

if __name__=="__main__":
    main()

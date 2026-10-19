"""4AIAD: post-generation step for the static USD BRep build.

premake writes the project's `links` into <Lib><AdditionalDependencies> for StaticLib projects,
which makes lib.exe merge third-party archives (TBB, USD) into SMLib.lib, SM_API.lib, ... and
would produce duplicate symbols when the host links everything. Static archives must carry only
their own objects; the host executable resolves all dependencies once.

Usage: python usd-brep-static-postgen.py <usd-brep>/_compiler/vs2022
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
changed = 0
for proj in sorted(root.glob("*/*.vcxproj")):
    text = proj.read_text(encoding="utf-8")
    if "<ConfigurationType>StaticLibrary</ConfigurationType>" not in text:
        continue
    new = re.sub(r"(<Lib>\s*)<AdditionalDependencies>[^<]*</AdditionalDependencies>(\s*)",
                 r"\1", text)
    new = re.sub(r"(<Lib>\s*)<AdditionalLibraryDirectories>[^<]*</AdditionalLibraryDirectories>(\s*)",
                 r"\1", new)
    if new != text:
        proj.write_text(new, encoding="utf-8")
        changed += 1
        print("static lib cleaned:", proj.parent.name)
print(f"{changed} project(s) updated")

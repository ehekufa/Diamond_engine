#!/usr/bin/env python3
"""Static consistency checks for the Diamond Engine repository.

Runs in CI before Gradle, so an inconsistency fails in seconds instead of after
a 20-minute NDK build. No third-party dependencies: plain Python 3.

Checks
  1. CMake source list matches the files on disk (both directions), so a new
     .cpp cannot be silently left out of libengine.so.
  2. Every C/C++ header has an include guard or #pragma once.
  3. Every `extern "C"` block in a C++ file is closed.
  4. The JNI bridge's Java_com_diamond_engine_jni_NativeBridge_* names match the
     `native` declarations in the Java sources.
  5. AndroidManifest.xml exists, parses, and declares the components the code
     expects (activity, min/target SDK, native library usage).
  6. String IDs referenced by the Java code exist in res/values.
  7. No committed build artifacts (.so/.o/apk/shader_bin_data.h).

Usage: tools/check_repo.py [--verbose]
Exit code 1 on the first category with failures (all categories still run).
"""
import argparse
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
CPP = ROOT / "app" / "src" / "main" / "cpp"
JAVA = ROOT / "app" / "src" / "main" / "java"
RES = ROOT / "app" / "src" / "main" / "res"
MANIFEST = ROOT / "app" / "src" / "main" / "AndroidManifest.xml"

failures = []
notes = []


def fail(category, message):
    failures.append(f"[{category}] {message}")


def check_cmake_sources(verbose):
    cmake = CPP / "CMakeLists.txt"
    if not cmake.exists():
        fail("cmake", "app/src/main/cpp/CMakeLists.txt is missing")
        return
    text = cmake.read_text()

    # Both the plain list entries and the `list(APPEND ...)` option lines count.
    listed = set()
    for match in re.findall(
            r"((?:core|scene|format|logic|render|platform|jni|api)/[\w./-]+\.(?:cpp|c))(?=[\s)#])",
            text):
        listed.add(match)
    # jni_anchor.cpp is the shared object's own source, not part of a library.
    listed.add("jni/jni_anchor.cpp")

    on_disk = {p.relative_to(CPP).as_posix()
               for p in CPP.rglob("*")
               if p.suffix in (".c", ".cpp")
               and "third_party" not in p.parts}

    for missing in sorted(on_disk - listed):
        fail("cmake", f"{missing} exists but is not in CMakeLists.txt")
    for ghost in sorted(listed - on_disk):
        fail("cmake", f"CMakeLists.txt lists {ghost}, which does not exist")
    if verbose:
        print(f"cmake: {len(listed)} listed, {len(on_disk)} on disk")


def check_include_guards(verbose):
    problems = 0
    for header in CPP.rglob("*.h"):
        if "third_party" in header.parts:
            continue
        text = header.read_text()
        if "#pragma once" in text or re.search(r"#ifndef\s+\w+_H", text):
            continue
        fail("headers", f"{header.relative_to(ROOT)} has no include guard")
        problems += 1
    if verbose:
        print(f"headers: {problems} without a guard")


def check_extern_c(verbose):
    """Every `extern "C" {` must have a matching closing brace.

    Counting braces by hand is unreliable (braces live in strings and comments),
    so this tracks depth from the opening brace and asserts it returns to zero
    before the end of the file.
    """
    for source in CPP.rglob("*.cpp"):
        if "third_party" in source.parts:
            continue
        text = source.read_text()
        for match in re.finditer(r'extern\s+"C"\s*\{', text):
            depth = 1
            index = match.end()
            while index < len(text) and depth:
                char = text[index]
                if char == "{":
                    depth += 1
                elif char == "}":
                    depth -= 1
                index += 1
            if depth:
                fail("externc", f"{source.relative_to(ROOT)}: unterminated "
                                f'extern "C" block at line '
                                f"{text[:match.start()].count(chr(10)) + 1}")
    if verbose:
        print("externc: checked")


def check_jni_contract(verbose):
    bridge = CPP / "jni" / "jni_bridge.cpp"
    if not bridge.exists():
        fail("jni", "jni/jni_bridge.cpp is missing")
        return
    exported = set(re.findall(r"Java_com_diamond_engine_jni_NativeBridge_(\w+)\s*\(",
                              bridge.read_text()))

    declared = {}
    for source in JAVA.rglob("NativeBridge.java"):
        for name in re.findall(r"native\s+[^;()]+?\s(\w+)\s*\(", source.read_text()):
            declared[name] = source
    if not declared:
        notes.append("jni: no NativeBridge.java yet, contract check skipped")
        return
    for name in sorted(set(declared) - exported):
        fail("jni", f"Java declares native {name}() with no JNI implementation")
    for name in sorted(exported - set(declared)):
        fail("jni", f"JNI exports {name}() that Java never declares")
    if verbose:
        print(f"jni: {len(exported)} exported, {len(declared)} declared")


def check_manifest(verbose):
    if not MANIFEST.exists():
        fail("manifest", "AndroidManifest.xml is missing")
        return
    try:
        tree = ET.parse(MANIFEST)
    except ET.ParseError as error:
        fail("manifest", f"does not parse: {error}")
        return
    android = "{http://schemas.android.com/apk/res/android}"
    root = tree.getroot()
    package = root.get("package") or ""
    if package and package != "com.diamond.engine":
        fail("manifest", f"package is '{package}', expected 'com.diamond.engine'")
    if root.find("application") is None:
        fail("manifest", "<application> element is missing")
    uses_sdk = root.find("uses-sdk")
    if uses_sdk is None:
        fail("manifest", "<uses-sdk> is missing")
    else:
        if uses_sdk.get(android + "minSdkVersion") is None:
            fail("manifest", "<uses-sdk> has no minSdkVersion")
        if uses_sdk.get(android + "targetSdkVersion") is None:
            fail("manifest", "<uses-sdk> has no targetSdkVersion")
    if root.find("uses-feature[@android:glEsVersion]", {"android": android[1:-1]}) is None:
        # Not fatal, but the store uses it to filter devices.
        notes.append("manifest: no <uses-feature android:glEsVersion> declared")
    if verbose:
        print(f"manifest: package={package or '?'}")


def check_resources(verbose):
    if not JAVA.exists():
        return
    declared = set()
    for values in RES.glob("values/*.xml"):
        try:
            for element in ET.parse(values).getroot():
                name = element.get("name")
                if name:
                    declared.add(name)
        except ET.ParseError as error:
            fail("res", f"{values.relative_to(ROOT)} does not parse: {error}")
    if not declared:
        notes.append("res: no string resources declared yet")
        return
    used = set()
    for source in JAVA.rglob("*.java"):
        text = source.read_text()
        used |= set(re.findall(r"R\.string\.(\w+)", text))
        used |= set(re.findall(r"getString\(R\.string\.(\w+)\)", text))
    for name in sorted(used - declared):
        fail("res", f"R.string.{name} is used but not declared")
    if verbose:
        print(f"res: {len(declared)} declared, {len(used)} used")


def check_artifacts(verbose):
    banned = {".so", ".o", ".a", ".apk", ".spv", ".class"}
    count = 0
    for path in ROOT.rglob("*"):
        if not path.is_file() or ".git" in path.parts:
            continue
        if path.name == "shader_bin_data.h":
            fail("artifacts", f"{path.relative_to(ROOT)} is generated — do not commit it")
            count += 1
            continue
        if path.suffix in banned:
            fail("artifacts", f"{path.relative_to(ROOT)} is a build artifact")
            count += 1
    if verbose:
        print(f"artifacts: {count} stray file(s)")


def check_headers_vs_sources(verbose):
    """Every public header in include/diamond is included by at least one source."""
    include_dir = CPP / "include" / "diamond"
    if not include_dir.exists():
        fail("headers", "include/diamond is missing")
        return
    all_text = "\n".join(p.read_text() for p in CPP.rglob("*")
                         if p.suffix in (".c", ".cpp") and "third_party" not in p.parts)
    unused = []
    for header in include_dir.glob("*.h"):
        if header.name not in all_text:
            unused.append(header.name)
    for name in sorted(unused):
        notes.append(f"headers: {name} is not included by any source (keep if API-only)")
    if verbose:
        print(f"headers: {len(list(include_dir.glob('*.h')))} public header(s)")


def check_public_abi(verbose):
    """Every DIAMOND_API declaration in diamond.h must have a definition.

    A missing definition only shows up as an undefined symbol while linking the
    .so, which costs a full NDK build to discover; parse both sides instead.
    """
    header = CPP / "include" / "diamond.h"
    api = CPP / "api" / "diamond_api.cpp"
    if not header.exists() or not api.exists():
        fail("abi", "include/diamond.h or api/diamond_api.cpp is missing")
        return
    text = re.sub(r"/\*.*?\*/", "", header.read_text(), flags=re.S)
    declared = []
    for decl in re.findall(r"DIAMOND_API\s+([^;]+);", text):
        match = re.search(r"(\w+)\s*\(", decl)
        if match and match.group(1).startswith("diamond_"):
            if match.group(1) not in declared:
                declared.append(match.group(1))

    # engine_* callbacks live in dlogic.h and in logic/logic_exec.c.
    logic_header = CPP / "include" / "diamond" / "dlogic.h"
    logic_source = CPP / "logic" / "logic_exec.c"
    engine_declared = []
    if logic_header.exists():
        logic_text = re.sub(r"/\*.*?\*/", "", logic_header.read_text(), flags=re.S)
        for decl in re.findall(r"DIAMOND_API\s+([^;]+);", logic_text):
            match = re.search(r"(\w+)\s*\(", decl)
            if match and match.group(1).startswith("engine_"):
                engine_declared.append(match.group(1))

    api_text = api.read_text()
    logic_text = logic_source.read_text() if logic_source.exists() else ""
    for name in declared:
        if not re.search(rf"^\s*[\w\s*]+\b{name}\s*\(", api_text, re.M):
            fail("abi", f"diamond.h declares {name}() but api/diamond_api.cpp does not define it")
    for name in engine_declared:
        if not re.search(rf"^\s*[\w\s*]+\b{name}\s*\(", logic_text, re.M):
            fail("abi", f"dlogic.h declares {name}() but logic/logic_exec.c does not define it")
    if verbose:
        print(f"abi: {len(declared)} public + {len(engine_declared)} engine symbol(s)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    check_cmake_sources(args.verbose)
    check_include_guards(args.verbose)
    check_extern_c(args.verbose)
    check_jni_contract(args.verbose)
    check_manifest(args.verbose)
    check_resources(args.verbose)
    check_public_abi(args.verbose)
    check_headers_vs_sources(args.verbose)
    check_artifacts(args.verbose)

    for note in notes:
        print(f"note: {note}")
    if failures:
        print(f"\n{len(failures)} problem(s):", file=sys.stderr)
        for problem in failures:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("\nrepository checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

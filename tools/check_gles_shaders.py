#!/usr/bin/env python3
"""Static checks for the GLSL ES shaders embedded in the C++ render backends.

No GLSL ES compiler is available on CI (glslc targets Vulkan, and there is no
GPU), so this script checks the invariants that actually break on device:

  * the first line is a #version directive;
  * braces balance and main() is present;
  * every fragment stage declares a precision qualifier;
  * each fragment stage's inputs are produced by a vertex stage in the same
    file, with identical types -- the classic cause of a program that links on
    one driver and fails on another.

Usage: tools/check_gles_shaders.py [--verbose]
Exit code 1 when a check fails.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
RENDER = ROOT / "app" / "src" / "main" / "cpp" / "render"
SOURCES = ["gl_backend.cpp", "gl2_backend.cpp", "hybrid_backend.cpp"]

# A C++ raw string holding a shader, and the variable it is assigned to.
RAW_STRING = re.compile(r'R"\((.*?)\)"', re.S)
VARIABLE = re.compile(r'(\w+)\s*=\s*R"\(')

# Qualifier declarations. ESSL 1.00 uses attribute/varying, ESSL 3.00 in/out.
DECL = re.compile(
    r'^\s*(?:layout\s*\([^)]*\)\s*)?(in|out|uniform|attribute|varying)\s+'
    r'([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;',
    re.M,
)


def parse(source):
    """Returns (version, {qualifier: [(type, name)]}) for one shader source."""
    first = source.strip().splitlines()[0].strip()
    version = first if first.startswith("#version") else ""
    qualifiers = {}
    for qual, type_name, name in DECL.findall(source):
        qualifiers.setdefault(qual, []).append((type_name, name))
    return version, qualifiers


def is_vertex(source):
    """A stage that writes gl_Position is a vertex stage."""
    return "gl_Position" in source


def check_file(path, verbose):
    problems = []
    text = path.read_text()
    shaders = RAW_STRING.findall(text)
    if not shaders:
        return [f"{path.name}: no embedded shader sources found"]

    variables = VARIABLE.findall(text)
    entries = []
    for index, source in enumerate(shaders):
        label = variables[index] if index < len(variables) else f"shader{index}"
        version, qualifiers = parse(source)
        prefix = f"{path.name}:{label}"
        if not version:
            problems.append(f"{prefix}: missing #version directive on the first line")
        if source.count("{") != source.count("}"):
            problems.append(f"{prefix}: unbalanced braces")
        if "void main" not in source:
            problems.append(f"{prefix}: no main()")
        if not is_vertex(source) and "precision" not in source:
            problems.append(f"{prefix}: fragment stage needs a precision qualifier")
        entries.append((prefix, index, source, qualifiers))
        if verbose:
            print(f"{prefix}: {version or 'NO VERSION'}, "
                  f"{len(qualifiers.get('in', [])) + len(qualifiers.get('attribute', []))} in, "
                  f"{len(qualifiers.get('out', [])) + len(qualifiers.get('varying', []))} out")

    vertex_outs = {}
    fragment_ins = {}
    for prefix, index, source, qualifiers in entries:
        outs = qualifiers.get("out", []) + qualifiers.get("varying", [])
        ins = qualifiers.get("in", []) + qualifiers.get("varying", [])
        if is_vertex(source):
            vertex_outs[(prefix, index)] = {
                name: type_name for type_name, name in outs if not name.startswith("gl_")
            }
        else:
            fragment_ins[(prefix, index)] = {name: type_name for type_name, name in ins}

    for (flabel, findex), fins in fragment_ins.items():
        # Several pairs can share a file (hybrid_backend), so pick the vertex
        # stage with the largest overlap rather than the first one.
        best_key, best_overlap = None, 0
        for vkey, vouts in vertex_outs.items():
            shared = len(set(vouts) & set(fins))
            if shared > best_overlap:
                best_key, best_overlap = vkey, shared
        if best_key is None:
            if fins:
                problems.append(f"{flabel}({findex}): no vertex stage provides {sorted(fins)}")
            continue
        vouts = vertex_outs[best_key]
        for name in sorted(set(vouts) & set(fins)):
            if vouts[name] != fins[name]:
                problems.append(
                    f"{path.name}: varying '{name}' is {vouts[name]} in {best_key[0]} "
                    f"but {fins[name]} in {flabel}"
                )
        missing = set(fins) - set(vouts)
        if missing:
            problems.append(f"{flabel} reads {sorted(missing)} which {best_key[0]} does not write")
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    failures = []
    checked = 0
    for name in SOURCES:
        path = RENDER / name
        if not path.exists():
            print(f"warning: {name} is not present, skipping", file=sys.stderr)
            continue
        checked += 1
        failures.extend(check_file(path, args.verbose))

    if failures:
        print(f"{len(failures)} shader problem(s):", file=sys.stderr)
        for problem in failures:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print(f"GLES shader checks passed ({checked} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

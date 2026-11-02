#!/usr/bin/env python3
"""Compile every project source once more, syntax only, the way the platforms that Linux PR checks don't build see it.

    python3 tests/portability_check.py <build dir> <source dir> [--opencv5 <OpenCV 5 install prefix>]

Pull requests build on Linux with the distro's OpenCV 4; Windows, macOS and OpenCV 5 are built only after merge
(release.yml).  Two classes of breakage got through that gap and were found by hand on main:

  * windows.h macros.  App.cpp and Server.cpp include windows.h first on Windows (portable-file-dialogs, cpp-httplib),
    so a variable named `far` became `bool = ...` and MSVC stopped (PR #43).  Every source here is compiled with
    cmake/WindowsMacros.h force-included, which defines the same names the same way.
  * OpenCV 5 deprecations.  `--opencv5 PREFIX` swaps the OpenCV include directory for an OpenCV 5 install (CI restores
    the one release.yml builds and caches on main) and turns deprecation warnings into errors; Joe counted 18 of them in
    the release logs before PR #43.

How: reads <build dir>/compile_commands.json (CMAKE_EXPORT_COMPILE_COMMANDS is on in CMakeLists.txt), keeps the entries
for files under <source dir> (not fetched dependencies or generated font files), and reruns each command with -c/-o
replaced by -fsyntax-only, in parallel.  About 10 s on four cores.  Exits 1 listing each failing file and its first
errors; prints PORTABILITY_CHECK_SKIPPED (which ctest reports as skipped) where there's nothing to check, such as an
MSVC or Xcode build with no compile_commands.json.
"""
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor


def skip(why):
    print(f"PORTABILITY_CHECK_SKIPPED: {why}")
    sys.exit(0)


def opencv5_include(prefix):
    """The directory holding opencv2/ in an OpenCV 5 install: include/opencv5 on Linux, include on macOS and Windows."""
    for rel in ("include/opencv5", "include/opencv4", "include"):
        d = os.path.join(prefix, rel)
        if os.path.isfile(os.path.join(d, "opencv2", "core", "version.hpp")):
            return d
    sys.exit(f"no OpenCV headers under {prefix}")


def is_opencv_dir(d):
    return os.path.isfile(os.path.join(d, "opencv2", "core", "version.hpp"))


def syntax_command(entry, shim, cv5):
    """The entry's compiler command as a syntax-only check: shim first, OpenCV 5 headers in place of the build's."""
    args = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
    out, i = [args[0], "-fsyntax-only", "-include", shim], 1
    while i < len(args):
        a = args[i]
        if a in ("-o", "-MF", "-MT", "-MQ"):          # output and dependency files: nothing is written
            i += 2
            continue
        if a in ("-c", "-MD", "-MMD"):
            i += 1
            continue
        if cv5 and a in ("-I", "-isystem") and i + 1 < len(args) and is_opencv_dir(args[i + 1]):
            out += [a, cv5]
            i += 2
            continue
        if cv5 and a.startswith(("-I", "-isystem")) and len(a) > 2:
            d = a[2:] if a.startswith("-I") else a[len("-isystem"):]
            if is_opencv_dir(d):
                out.append(a[: len(a) - len(d)] + cv5)
                i += 1
                continue
        out.append(a)
        i += 1
    if cv5:
        out.append("-Werror=deprecated-declarations")
    return out


def main():
    argv = sys.argv[1:]
    cv5 = None
    if "--opencv5" in argv:
        k = argv.index("--opencv5")
        cv5 = opencv5_include(argv[k + 1])
        del argv[k : k + 2]
    if len(argv) != 2:
        sys.exit(__doc__)
    build, src = (os.path.realpath(a) for a in argv)
    db = os.path.join(build, "compile_commands.json")
    if not os.path.isfile(db):
        skip(f"no {db} (a Visual Studio or Xcode build doesn't write one)")
    shim = os.path.join(src, "cmake", "WindowsMacros.h")

    entries, seen = [], set()
    for e in json.load(open(db)):
        f = os.path.realpath(os.path.join(e["directory"], e["file"]))
        if f in seen or not f.startswith(src + os.sep) or f.startswith(build + os.sep) or "/_deps/" in f:
            continue
        # Objective-C++ (the macOS camera lister) never meets windows.h, and the shim's `interface` macro would break
        # every @interface in the SDK headers it includes.
        if f.endswith((".mm", ".m")):
            continue
        cmd = e["arguments"][0] if "arguments" in e else shlex.split(e["command"])[0]
        if os.path.basename(cmd).lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            skip("MSVC builds see the real windows.h")
        seen.add(f)
        entries.append((f, e))
    if not entries:
        skip("no project sources in compile_commands.json")

    def check(item):
        f, e = item
        r = subprocess.run(syntax_command(e, shim, cv5), cwd=e["directory"], capture_output=True, text=True)
        return f, r.returncode, r.stderr

    failed = 0
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for f, code, err in pool.map(check, entries):
            if code:
                failed += 1
                errors = [l for l in err.splitlines() if "error" in l][:6]
                print(f"FAIL {os.path.relpath(f, src)}\n  " + "\n  ".join(errors))
    what = "windows.h macros" + (f" and OpenCV 5 headers ({cv5})" if cv5 else "")
    print(f"{len(entries) - failed} of {len(entries)} sources compile with {what}")
    if failed:
        print("A name windows.h defines as a macro (near, far, small, ERROR, IN, OUT, interface, ...) can't be used as an "
              "identifier; see cmake/WindowsMacros.h." + (" OpenCV 5 deprecations: use the replacement the "
              "warning names." if cv5 else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

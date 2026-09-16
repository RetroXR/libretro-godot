#!/usr/bin/env python3
"""Build and run libretro-godot's standalone C++ tests.

These deliberately do not need Godot, a core, or the built extension: each test
compiles the unit under test straight from src/ alongside a harness that stubs
whatever little it reaches outside itself. That keeps them quick enough to run
on every change, which matters most for LinkCoordinator, where the failure mode
is a deadlock or a timing-dependent answer rather than a wrong return value.

    python tests/run_tests.py              # build and run everything
    python tests/run_tests.py --repeat 20  # hunt for flakiness in the threaded ones
"""

import argparse
import glob
import os
import platform
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# test source -> extra sources it needs from src/
TESTS = {
    "link_coordinator_test.cpp": ["LinkCoordinator.cpp"],
    # Header-only: the encoding is the whole unit, and InputHandler.cpp is not
    # Godot-free the way LinkCoordinator.cpp deliberately is.
    "sensor_index_test.cpp": [],
    # Header-only: MicrophoneHandler.cpp needs Wrapper and Godot.
    "microphone_stream_test.cpp": [],
    # Header-only, and deliberately so: the level is measured for machines whose
    # core has no microphone open at all.
    "microphone_level_test.cpp": [],
    # A benchmark rather than a test: it prints a cost curve and only fails if
    # the bus cannot be set up. Run it on its own (--only link_bench), because
    # it is slow by design and its numbers want reading, not gating.
    "link_bench.cpp": ["LinkCoordinator.cpp"],
}

INCLUDES = [
    os.path.join(ROOT, "src"),
    os.path.join(ROOT, "external", "libretro-common", "include"),
]


def find_vcvars():
    """Newest vcvars64.bat, so the Windows SDK headers are reachable."""
    roots = [
        r"C:\Program Files\Microsoft Visual Studio",
        r"C:\Program Files (x86)\Microsoft Visual Studio",
    ]
    found = []
    for root in roots:
        found += glob.glob(os.path.join(root, "*", "*", "VC", "Auxiliary", "Build", "vcvars64.bat"))
    return sorted(found)[-1] if found else None


def build_windows(test_src, extra, out_exe, workdir):
    vcvars = find_vcvars()
    if not vcvars:
        raise SystemExit("no vcvars64.bat found; install the MSVC build tools")
    incs = " ".join('/I"%s"' % i for i in INCLUDES)
    srcs = " ".join('"%s"' % s for s in [test_src] + extra)
    # Driven through a batch file rather than cmd.exe /c. A command line that
    # begins with a quoted path gets its quotes eaten by cmd's own parsing
    # rules, and the result is a silent exit 1 with nothing on either stream.
    #
    # Object files land in the working directory instead of being pointed at
    # with /Fo, because a quoted path ending in a backslash escapes its own
    # closing quote.
    script = os.path.join(workdir, "build.bat")
    lines = [
        "@echo off",
        'call "%s" >nul 2>&1 || exit /b 1' % vcvars,
        'cl /nologo /EHsc /std:c++17 /W4 %s %s /Fe:"%s"' % (incs, srcs, out_exe),
    ]
    with open(script, "w") as handle:
        handle.write(chr(10).join(lines) + chr(10))
    return subprocess.run([script], capture_output=True, text=True, cwd=workdir)


def build_posix(test_src, extra, out_exe, workdir):
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        raise SystemExit("no C++ compiler found; set CXX")
    cmd = [cxx, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread"]
    for i in INCLUDES:
        cmd += ["-I", i]
    cmd += [test_src] + extra + ["-o", out_exe]
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeat", type=int, default=1,
                    help="run each test N times; the threaded tests can only be "
                         "trusted after a few passes")
    ap.add_argument("--only", help="run just this test source")
    ap.add_argument("--args", default="",
                    help="extra arguments passed through to the test binary, "
                         "which is how link_bench is swept (e.g. --args='--grain=2048')")
    args = ap.parse_args()

    workdir = tempfile.mkdtemp(prefix="libretro-godot-tests-")
    windows = platform.system() == "Windows"
    failures = []

    try:
        for src, extra_names in sorted(TESTS.items()):
            if args.only and args.only not in src:
                continue
            name = os.path.splitext(src)[0]
            test_src = os.path.join(HERE, src)
            extra = [os.path.join(ROOT, "src", e) for e in extra_names]
            out_exe = os.path.join(workdir, name + (".exe" if windows else ""))

            print("=== %s ===" % name, flush=True)
            build = build_windows(test_src, extra, out_exe, workdir) if windows \
                else build_posix(test_src, extra, out_exe, workdir)
            if build.returncode != 0:
                sys.stdout.write(build.stdout or "")
                sys.stdout.write(build.stderr or "")
                print("  build failed (exit %d)" % build.returncode)
                failures.append(name + " (build)")
                continue

            for run in range(args.repeat):
                result = subprocess.run([out_exe] + args.args.split(), capture_output=True, text=True)
                if result.returncode != 0:
                    print(result.stdout)
                    failures.append("%s (run %d)" % (name, run + 1))
                    break
                if run == 0:
                    print(result.stdout, end="")
            else:
                if args.repeat > 1:
                    print("  %d runs, all clean" % args.repeat)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print("\nFAILED: " + ", ".join(failures))
        return 1
    print("\nall tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

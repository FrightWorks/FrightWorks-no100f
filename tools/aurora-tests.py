#!/usr/bin/env python3
"""Build and run Aurora's own GPU tests (Windows and Linux, Python 3.8+, standard library only).

    python tools/aurora-tests.py                       # configure (first time), build, run every test
    python tools/aurora-tests.py --filter gx_readback  # the GPU test alone (its 3 CTest entries)
    python tools/aurora-tests.py --play D:\\somewhere\\Play --build-dir autotest\\aurora-build

It does what docs/testing.md "Aurora tests" lists by hand: configure wiicompiled/aurora-main (not its tests folder) with
-DAURORA_BUILD_GPU_TESTS=ON, build gx_fifo_tests and gx_readback_tests, then `ctest --output-on-failure --timeout 300`.
Configuring downloads googletest and Dawn.

On Windows the test needs a built Play folder for its runtime libraries (SDL3 and the rest). Without them Windows pops a
modal "SDL3.dll not found" dialog on the desktop instead of failing, so the libraries are checked before anything runs.
On Linux Play is optional (SDL3 and Dawn are linked statically there, as in the game build): the built test binary is
checked with `ldd` instead, and a --play folder is only put on the library path.
PATH (Windows) or LD_LIBRARY_PATH (Linux) is changed for the test run only, never for this shell.

The tests open a small window. A build folder that already has a build.ninja (a finished configure; a failed one leaves only CMakeCache.txt) is not configured again (--reconfigure).
CTest's `*_NOT_BUILT` placeholders (tests whose target was not built) are reported as not run: neither pass nor failure.
Exit code: 0 when at least one test ran and none failed, 1 otherwise (2 for a setup problem such as no usable Play).
"""
import argparse
import glob
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FILTER = ""   # every test; --filter gx_readback runs the GPU test alone
TARGETS = ["gx_fifo_tests", "gx_readback_tests"]
CTEST_TIMEOUT = "300"

# Libraries the GPU test loads at start. A name with a * is a glob (Linux sonames carry versions).
WINDOWS_LIBS = ["SDL3.dll", "webgpu_dawn.dll", "libc++.dll", "libunwind.dll"]
# Linux needs none from Play: Aurora's standalone build links SDL3 and Dawn statically there (see configure_cmd), so
# the check is `ldd` on the built test binary instead (ldd_missing).
LINUX_LIBS = []


class SetupError(Exception):
    pass


def is_windows(system=None):
    return (system or os.name) in ("nt", "Windows", "win32")


def exe_suffix(system=None):
    return ".exe" if is_windows(system) else ""


def required_libs(system=None):
    return list(WINDOWS_LIBS if is_windows(system) else LINUX_LIBS)


def toolchain_dirs(root, system=None):
    """Folders of the portable toolchain that exist, in PATH order (the build scripts' toolchain/ layout)."""
    tc = Path(root) / "toolchain"
    names = ["llvm-mingw/bin", "CMake/bin", "Ninja"] if is_windows(system) else ["CMake/bin"]
    return [tc / n for n in names if (tc / n).is_dir()]


def find_tool(name, root, system=None, which=None):
    """cmake/ctest: the toolchain's copy first (as the other build scripts do), else the one on PATH."""
    import shutil
    which = which or shutil.which
    for d in toolchain_dirs(root, system):
        p = d / (name + exe_suffix(system))
        if p.is_file():
            return str(p)
    return which(name)


def lib_present(folder, pattern):
    return bool(glob.glob(os.path.join(glob.escape(str(folder)), pattern))) if "*" in pattern else \
        (Path(folder) / pattern).is_file()


def missing_libs(folders, system=None):
    """Required libraries found in none of `folders`."""
    return [lib for lib in required_libs(system) if not any(lib_present(f, lib) for f in folders)]


def play_ok(play, system=None):
    """A Play folder is usable when it has SDL3 (the library whose absence pops the dialog) and the game's dawn."""
    return Path(play).is_dir() and not [m for m in missing_libs([play], system) if m.startswith(("SDL3", "webgpu_dawn"))]


def find_play(root, given=None, system=None):
    """The Play folder to take libraries from: the argument if given (it must be usable), else <root>/Play.

    Windows needs one. On Linux Play is optional: an existing one (or --play) is only added to LD_LIBRARY_PATH, and
    without one this returns None (no libraries are needed from it).
    """
    if given:
        p = Path(given)
        if not p.is_dir():
            raise SetupError(f"Play folder not found: {p}")
        gaps = missing_libs([p], system)
        if gaps and not play_ok(p, system):
            raise SetupError(f"Play folder {p} lacks: {', '.join(gaps)}")
        return p
    p = Path(root) / "Play"
    if play_ok(p, system):
        return p
    if not is_windows(system):
        return None
    raise SetupError(f"No usable built Play folder at {p} (needs {', '.join(required_libs(system))}). "
                     "Build the game first (tools/build-no100f.ps1 or tools/build-no100f-linux.sh), or pass --play.")


def check_libraries(play, root, system=None):
    """Libraries missing from Play and from the toolchain folders that go on the path too. Empty list = fine."""
    return missing_libs([play] + toolchain_dirs(root, system), system)


def run_env(base_env, play, root, system=None):
    """A copy of the environment with Play (and on Windows the toolchain bins) first on the library path."""
    env = dict(base_env)
    if is_windows(system):
        key = next((k for k in env if k.upper() == "PATH"), "PATH")
        dirs = [str(play)] + [str(d) for d in toolchain_dirs(root, system)]
        env[key] = os.pathsep.join(dirs + ([env[key]] if env.get(key) else []))
    elif play:
        dirs = [str(play)]
        env["LD_LIBRARY_PATH"] = os.pathsep.join(dirs + ([env["LD_LIBRARY_PATH"]] if env.get("LD_LIBRARY_PATH") else []))
    return env


def build_tool_env(base_env, root, system=None):
    """Environment for configure/build: the toolchain first (Windows: it must find llvm-mingw and Ninja)."""
    env = dict(base_env)
    dirs = [str(d) for d in toolchain_dirs(root, system)]
    if dirs:
        key = next((k for k in env if k.upper() == "PATH"), "PATH")
        env[key] = os.pathsep.join(dirs + ([env[key]] if env.get(key) else []))
    return env


def configure_cmd(cmake, root, build_dir, system=None):
    cmd = [cmake, "-S", str(Path(root) / "wiicompiled" / "aurora-main"), "-B", str(build_dir), "-G", "Ninja",
           "-DCMAKE_BUILD_TYPE=Release", "-DAURORA_BUILD_GPU_TESTS=ON"]
    if not is_windows(system):
        # The same compilers and providers as tools/build-no100f-linux.sh, so the tests use the game's SDL3 and Dawn
        # (standalone defaults differ: SDL3 would be the system one when it is new enough).
        cmd += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
                "-DAURORA_DAWN_PROVIDER=package", "-DAURORA_SDL3_PROVIDER=vendor",
                "-DCMAKE_POLICY_DEFAULT_CMP0168=NEW"]
    return cmd


def build_cmd(cmake, build_dir, jobs=None):
    cmd = [cmake, "--build", str(build_dir), "--parallel"] + ([str(jobs)] if jobs else [])
    return cmd + ["--target"] + TARGETS


def ctest_cmd(ctest, build_dir, test_filter=DEFAULT_FILTER):
    cmd = [ctest, "--test-dir", str(build_dir), "--output-on-failure", "--timeout", CTEST_TIMEOUT]
    if test_filter:
        cmd += ["-R", test_filter]
    return cmd


def ldd_missing(output):
    """Libraries `ldd` could not resolve (Linux)."""
    return [m.group(1) for m in re.finditer(r"^\s*(\S+)\s*=>\s*not found", output, re.M)]


def find_test_exe(build_dir, system=None):
    """The built gx_readback_tests (it sits in the tests/ subfolder of the build folder)."""
    found = sorted(Path(build_dir).glob("**/gx_readback_tests" + exe_suffix(system)))
    return found[0] if found else None


def binary_gaps(build_dir, env, system=None, run=subprocess.run, which=None):
    """Linux: libraries the built test binary cannot find with `env`'s library path (none if ldd is not there)."""
    import shutil
    exe = None if is_windows(system) else find_test_exe(build_dir, system)
    if not exe or not (which or shutil.which)("ldd"):
        return []
    out = run(["ldd", str(exe)], env=env, stdout=subprocess.PIPE, universal_newlines=True).stdout
    return ldd_missing(out)


_LINE = re.compile(r"^\s*\d+/\d+ Test\s+#\d+:\s+(\S+)\s+\.+\s*(\*\*\*)?\s*(.*?)\s+[\d.]+ sec\s*$")


def parse_ctest(text):
    """CTest's output -> {'passed': [...], 'failed': [(name, why)], 'not_run': [...]}.

    `*_NOT_BUILT` placeholders (CTest says "Not Run") count as not run. Any other "Not Run" (a missing executable, say)
    is a failure: that test was wanted and did not happen.
    """
    res = {"passed": [], "failed": [], "not_run": []}
    for line in text.splitlines():
        m = _LINE.match(line)
        if not m:
            continue
        name, stars, status = m.group(1), m.group(2), m.group(3).strip()
        if not stars and status.lower() == "passed":
            res["passed"].append(name)
        elif status.lower().startswith("not run") and name.endswith("_NOT_BUILT"):
            res["not_run"].append(name)
        elif status.lower().startswith("not run") and not stars:
            res["failed"].append((name, "not run"))
        elif status.lower() == "test disabled" or status.lower() == "disabled":
            res["not_run"].append(name)
        else:
            res["failed"].append((name, status or "failed"))
    return res


def verdict(results, ctest_rc):
    """(ok, reason). ctest's own exit code is nonzero for "Not Run" placeholders, so it is not trusted alone."""
    if results["failed"]:
        return False, f"{len(results['failed'])} failed"
    if not results["passed"]:
        return False, "no test ran (does the filter match anything?)"
    if ctest_rc != 0 and not results["not_run"]:
        return False, f"ctest exited {ctest_rc}"
    return True, "ok"


def summary(results, ok, reason, play, build_dir):
    lines = [f"Aurora tests: {'PASS' if ok else 'FAIL'} ({reason})",
             f"  passed {len(results['passed'])}, failed {len(results['failed'])}, not run {len(results['not_run'])}",
             f"  Play: {play or '(none; not needed here)'}", f"  build folder: {build_dir}"]
    gpu = [n for n in results["passed"] if n.startswith("gx_readback")]
    if gpu:
        lines.append(f"  GPU test passed: {', '.join(gpu)}")
    for n, why in results["failed"]:
        lines.append(f"  FAILED:  {n} ({why})")
    for n in results["not_run"]:
        lines.append(f"  not run: {n} (placeholder: its target was not built; neither a pass nor a failure)")
    return "\n".join(lines)


def run_step(cmd, env, name, run=subprocess.run, cwd=None):
    print(f"[{name}] {' '.join(cmd)}", flush=True)
    return run(cmd, env=env, cwd=cwd).returncode


def run_ctest(cmd, env, popen=subprocess.Popen):
    """Runs ctest, echoing its output as it comes; returns (returncode, full output)."""
    print(f"[test] {' '.join(cmd)}", flush=True)
    p = popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True,
              encoding="utf-8", errors="replace")
    out = []
    for line in p.stdout:
        sys.stdout.write(line)
        out.append(line)
    p.wait()
    return p.returncode, "".join(out)


def main(argv=None, base_env=None, run=subprocess.run, popen=subprocess.Popen, which=None, system=None):
    ap = argparse.ArgumentParser(description="Build and run Aurora's GPU tests.")
    ap.add_argument("--play", help="built Play folder to take SDL3 and the other libraries from (default: <checkout>/Play)")
    ap.add_argument("--build-dir", help="build folder (default: <checkout>/autotest/aurora-tests-build)")
    ap.add_argument("--filter", default=DEFAULT_FILTER, help="ctest -R regex (default: every test); gx_readback = the GPU test alone")
    ap.add_argument("--reconfigure", action="store_true", help="configure even if the build folder is already configured")
    ap.add_argument("--jobs", type=int, help="parallel build jobs")
    args = ap.parse_args(argv)
    base_env = os.environ if base_env is None else base_env

    try:
        play = find_play(ROOT, args.play, system)
        cmake = find_tool("cmake", ROOT, system, which)
        ctest = find_tool("ctest", ROOT, system, which)
        if not cmake or not ctest:
            raise SetupError("cmake/ctest not found (the toolchain/ folder has them after tools/build-no100f.*, or put them on PATH)")
        gaps = check_libraries(play, ROOT, system) if play else []
        if gaps:
            raise SetupError(f"Missing libraries (not in {play} or the toolchain folders): {', '.join(gaps)}. Nothing was run.")
    except SetupError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    build_dir = Path(args.build_dir) if args.build_dir else ROOT / "autotest" / "aurora-tests-build"
    tool_env = build_tool_env(base_env, ROOT, system)
    if args.reconfigure or not (build_dir / "build.ninja").is_file():
        if run_step(configure_cmd(cmake, ROOT, build_dir, system), tool_env, "configure", run):
            print("error: configure failed", file=sys.stderr)
            return 1
    else:
        print(f"[configure] skipped: {build_dir} is already configured")
    if run_step(build_cmd(cmake, build_dir, args.jobs), tool_env, "build", run):
        print("error: build failed", file=sys.stderr)
        return 1

    env = run_env(base_env, play, ROOT, system)
    gone = binary_gaps(build_dir, env, system, run, which)
    if gone:
        print(f"error: the test binary cannot find: {', '.join(gone)}. Nothing was run.", file=sys.stderr)
        return 2
    rc, out = run_ctest(ctest_cmd(ctest, build_dir, args.filter), env, popen)
    results = parse_ctest(out)
    ok, reason = verdict(results, rc)
    print("\n" + summary(results, ok, reason, play, build_dir))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

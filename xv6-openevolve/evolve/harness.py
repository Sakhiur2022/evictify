"""Build + run helpers shared by selftest.py and evaluator.py (host build of the real bio.c)."""
import os, re, shutil, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)                      # xv6-lru/
CC = os.environ.get("CC", "gcc")
CFLAGS = ["-Wall", "-Werror", "-O2", "-fno-strict-aliasing", "-fno-builtin"]   # same strictness as the xv6 kernel build


class BuildError(Exception):
    pass


class RunError(Exception):
    pass


def build(tmp, tag, nbuf, defines, policy=None):
    """Compile bio.c (unmodified) + host stubs.  `policy` is copied to policy_evolve.h next to bio.c."""
    d = os.path.join(tmp, tag)
    os.makedirs(d, exist_ok=True)
    shutil.copy(os.path.join(SRC, "bio.c"), os.path.join(d, "bio.c"))
    if policy:
        shutil.copy(policy, os.path.join(d, "policy_evolve.h"))
    exe = os.path.join(d, "sim")
    cmd = [CC, *CFLAGS, *defines, f"-DNBUF={nbuf}", "-I" + d, "-I" + os.path.join(SRC, "bcsim"), "-I" + SRC,
           "-o", exe, os.path.join(d, "bio.c"), os.path.join(SRC, "bcsim", "stubs.c"), os.path.join(SRC, "bcsim", "main.c")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise BuildError(r.stderr[-3000:])
    return exe


def _num(x):
    try:
        return int(x)
    except ValueError:
        return x


def run(exe, cfg, timeout=10):
    """cfg = (workload, scan, hot, interval, passes, rounds[, trace]).  Returns the CSV line as a dict."""
    try:
        r = subprocess.run([exe, *map(str, cfg)], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RunError("timeout (policy too slow or looping)")
    if r.returncode != 0:
        msg = (r.stderr.strip() or r.stdout.strip()[-400:])
        raise RunError(msg[-600:])
    hdr = row = None
    for l in r.stdout.splitlines():
        if l.startswith("CSVH,"):
            hdr = l.split(",")[1:]
        elif l.startswith("CSV,"):
            row = l.split(",")[1:]
    if not hdr or not row:
        raise RunError("no CSV line in benchmark output")
    return {k: _num(v) for k, v in zip(hdr, row)}

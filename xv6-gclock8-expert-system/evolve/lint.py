"""Static rules for an evolved policy_evolve.h.

Why: the benchmark charges a policy for the work it reports through P_GET/P_SET/G_GET/G_SET/pol_cand_ok
and for the bytes of struct pol_meta / struct pol_global.  A program that hides state or work elsewhere
(globals, static locals, direct access to the storage, libc calls) would get a better score than it deserves.
check() returns a list of human-readable problems (empty list = OK)."""
import re

START, END = "// EVOLVE-BLOCK-START", "// EVOLVE-BLOCK-END"
FORBIDDEN_IDS = (
    "pol_pm pol_pg pol_ops pol_clock pol_cur_c pol_search_insp pol_reset bcache bench_stats bench_last bench_running "
    "printf cprintf puts putchar panic exit abort malloc calloc realloc free rand srand random time clock getenv system "
    "fork exec kalloc kfree acquire release initlock sleep wakeup iderw bread bwrite brelse bget memset memcpy memmove "
    "asm __asm__ __asm extern"
).split()
FORBIDDEN_TYPES = "long double float uint64 int64 uint64_t int64_t __int128 size_t".split()
REQUIRED_HOOKS = ["pol_init", "pol_on_hit", "pol_on_load", "pol_on_release", "pol_victim"]


def split_blocks(src):
    """-> (outside_segments, block_texts)"""
    outside, blocks, pos = [], [], 0
    while True:
        a = src.find(START, pos)
        if a < 0:
            outside.append(src[pos:])
            break
        b = src.find(END, a)
        if b < 0:
            outside.append(src[pos:])
            blocks.append(None)
            break
        outside.append(src[pos:a])
        blocks.append(src[a + len(START):b])
        pos = b + len(END)
    return outside, blocks


def strip_comments_strings(t):
    t = re.sub(r"/\*.*?\*/", " ", t, flags=re.S)
    t = re.sub(r"//[^\n]*", " ", t)
    t = re.sub(r'"(\\.|[^"\\])*"', '""', t)
    t = re.sub(r"'(\\.|[^'\\])*'", "''", t)
    return t


TYPE_HEAD = re.compile(r"^(typedef\s+)?(struct|union|enum)\b[^()=]*$")


def scan_toplevel(text):
    """Report global data, static locals, function-pointer variables."""
    probs, depth, buf, head, i, n = [], 0, "", "", 0, len(text)
    body_start = 0
    while i < n:
        ch = text[i]
        if depth == 0:
            if ch == "{":
                head, depth, body_start = buf.strip(), 1, i + 1
                buf = ""
            elif ch == ";":
                st = buf.strip(); buf = ""
                if st and not st.startswith("typedef") and not re.match(r"^(struct|union|enum)\s+\w+$", st):
                    if "(" not in st or re.search(r"\(\s*\*", st):
                        probs.append(f"global/static data is not allowed outside pol_meta/pol_global: '{st[:60]}'")
            else:
                buf += ch
        else:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    body = text[body_start:i]
                    if TYPE_HEAD.match(head):
                        j = text.find(";", i)
                        trail = text[i + 1:j if j >= 0 else n].strip()
                        if trail and not head.startswith("typedef"):
                            probs.append(f"variable declared with a type definition: '{head[:40]} ... {trail[:20]}'")
                        i = j if j >= 0 else n
                    elif "(" in head and "=" not in head.split("(")[0]:
                        if re.search(r"\bstatic\b", body):
                            probs.append(f"'static' local variable inside '{head.split('(')[0].split()[-1]}' hides state from the cost model")
                    else:
                        probs.append(f"initialised global data is not allowed: '{head[:50]}'")
                        j = text.find(";", i); i = j if j >= 0 else n
        i += 1
    return probs


def check(src, frame_reference=None):
    probs = []
    outside, blocks = split_blocks(src)
    if len(blocks) != 2 or any(b is None for b in blocks):
        return ["expected exactly two EVOLVE-BLOCK-START/END pairs"]
    if frame_reference is not None:
        ref_out, _ = split_blocks(frame_reference)
        if [o.strip() for o in outside] != [o.strip() for o in ref_out]:
            probs.append("text outside the EVOLVE blocks was modified (frame must stay identical)")
    code = strip_comments_strings("\n".join(blocks))
    if re.search(r"^\s*#", code, flags=re.M):
        probs.append("preprocessor directives are not allowed (#define/#include could bypass cost counting)")
    for ident in FORBIDDEN_IDS:
        if re.search(r"\b" + re.escape(ident) + r"\b", code):
            probs.append(f"forbidden identifier '{ident}'")
    for ty in FORBIDDEN_TYPES:
        if re.search(r"\b" + re.escape(ty) + r"\b", code):
            probs.append(f"forbidden type '{ty}' (32-bit xv6 kernel: no 64-bit/FP types)")
    if not re.search(r"\bstruct\s+pol_meta\s*\{", code):
        probs.append("struct pol_meta must be defined")
    if not re.search(r"\bstruct\s+pol_global\s*\{", code):
        probs.append("struct pol_global must be defined")
    for h in REQUIRED_HOOKS:
        if not re.search(r"\b" + h + r"\s*\([^;{]*\)\s*\{", code):
            probs.append(f"hook {h}() is not defined")
    # constants that look like benchmark block numbers (the workloads live at blocks 100..999)
    for lit in re.findall(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]*\b", code):
        try:
            v = int(lit, 16) if lit.lower().startswith("0x") else int(lit, 10)
        except ValueError:
            continue
        pow2 = v > 0 and (v & (v - 1)) == 0
        mask = v > 0 and ((v + 1) & v) == 0
        if 100 <= v <= 999 and not (pow2 or mask):
            probs.append(f"constant {lit} looks like a benchmark block number; tuning to specific block ids is not allowed")
            break
    probs += scan_toplevel(code)
    return probs

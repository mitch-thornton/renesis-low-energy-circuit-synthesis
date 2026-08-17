# ---------------------------------------------------------------------------
#  release_gate.py -- The release gate: refuse to cut a bundle that repeats a recorded mistake
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  Every check in this file is a lesson this project already paid for
#  once:
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-16  (Renesis v92.3)
#  Created:     Renesis v76.4 (earliest version token in file)
# ---------------------------------------------------------------------------
"""The release gate: refuse to cut a bundle that repeats a recorded mistake.

Every check in this file is a lesson this project already paid for once:

  stray top-level artifacts   the `--only/` directory v89.3 shipped
  stale version strings       the README that sat at a v88 title over a v83
                              "start here" for six cuts; the primer frozen at
                              v79.2 for ten
  unexecuted EXPECT blocks    three validation documents shipped printing a
                              command that was never run (the -q --json trap)
  forbidden build products    the Linux ELF binaries that broke macOS builds
                              (v76.4 rule); __pycache__; *.o
  mirror drift                scripts/ vs scripts_adiabatic/, four releases of
                              SOME TESTS FAILED nobody read (TODO 58f)
  stale github image          two recorded drift episodes (v83-v86, v88.x)
  a suite that failed         58f's tail obligation: SOME TESTS FAILED must be
                              something a cut cannot ship past

TWO MODES, because the two trees a release touches are opposites:

    --mode validate   run in the BUILT work tree after `make`: everything
                      except tree hygiene (build products are expected
                      here), INCLUDING the C suite -- 58f's obligation that
                      SOME TESTS FAILED be unshippable is enforced here.
    --mode prepack    run in the PRISTINE packing tree just before tar:
                      everything except the C suite -- a pristine tree has
                      no binaries, and run_tests.sh silently skips its
                      binary stages there, which is exactly the silent-skip
                      trap RENESIS-PROCEDURES 5 warns about.  Requiring
                      tests here would manufacture a green that means
                      nothing.

The cut may proceed only when BOTH runs exit 0:

    python3 scripts_adiabatic/release_gate.py --version 89.5 --mode validate
    python3 scripts_adiabatic/release_gate.py --version 89.5 --mode prepack

`--skip-tests` (validate mode) is for fast doc iteration only; the final
validate run must not use it.

Manual COVERAGE (every options-table key documented in RENESIS-MANUAL.md) is
reported as WARN, not FAIL, until the manual rewrite lands; `--strict-manual`
flips it to FAIL.  Manual DEFAULTS are already a hard gate via prepack_check.
"""
import argparse
import json
import os
import re
import subprocess
import sys

BUNDLE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(BUNDLE)

MIRRORED = ["revsynth.py", "netlist.py", "energy.py",
            "test_cover_strategies.py", "netlist_io.py", "bench_front.py"]

FORBIDDEN_SUFFIX = (".o", ".so", ".a", ".pyc", ".pid")
FORBIDDEN_NAMES = ("__pycache__", ".DS_Store", "nohup.out")

fails, warns = [], []


def ok(msg):
    print("  PASS: %s" % msg)


def bad(msg):
    print("  FAIL: %s" % msg)
    fails.append(msg)


def warn(msg):
    print("  WARN: %s" % msg)
    warns.append(msg)


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def check_banner(v):
    """The tool's own banner must carry the bundle version.

    v89.7 lesson: scripts_adiabatic/renesis.py printed `renesis v89.3` inside
    the v89.4, v89.5 AND v89.6 bundles -- three owner-validated cuts whose
    console output named a version they were not.  The docs gate below checks
    every document for currency and never looked at the one line every single
    run prints first.  This check reads the VERSION assignment out of the
    driver source and fails the cut on any mismatch, so banner and bundle
    can never drift again."""
    print("[0] tool banners vs bundle version (both languages, both trees)")
    # v89.8: the C banner is checked too, under the owner's policy that a
    # banner names the BUNDLE it ships in and is synced at every cut.  The
    # C tool printed v84 from the port's first cut through v89.7.
    #
    # v91.1: the GITHUB IMAGE is checked too, and this is the v89.7 lesson
    # repeating one level down.  The image is a separate copy of the drivers;
    # this check only ever read the bundle root, so v91.0 shipped a public
    # repository whose Python driver and C tool both announced themselves as
    # v90.8 -- on every run, to every reader of the paper and the DOI, for the
    # first public release.  Nothing caught it because the gate was written
    # before the image existed and was never extended to it.
    PY = r'^VERSION\s*=\s*"v([^"]+)"'
    C = r'#define\s+RENESIS_VERSION\s+"v([^"]+)"'
    for path, pat, what in (
            ("scripts_adiabatic/renesis.py", PY, "VERSION assignment"),
            ("csrc/renesis_main.c", C, "RENESIS_VERSION"),
            ("github/scripts_adiabatic/renesis.py", PY, "VERSION assignment"),
            ("github/csrc/renesis_main.c", C, "RENESIS_VERSION")):
        if not os.path.exists(path):
            bad("%s is missing -- the release image is incomplete" % path)
            continue
        text = open(path, errors="replace").read()
        m = re.search(pat, text, re.M)
        if not m:
            bad("%s: cannot find the %s" % (path, what))
            continue
        if m.group(1) == v:
            ok("%s banner is v%s" % (path, v))
        else:
            bad("%s banner says v%s but this cut is v%s -- every run of "
                "this bundle would print the wrong version"
                % (path, m.group(1), v))


def check_versions(v):
    print("[1] version currency (v%s)" % v)
    musts = [
        ("CHECKPOINT-V%s.md" % v, None),
        ("VALIDATE-V%s.md" % v, None),
        ("HISTORY.md", r"^## v%s\b" % re.escape(v)),
        ("PACKAGING-NOTE.md", r"^## v%s\b" % re.escape(v)),
        ("README.md", r"v%s" % re.escape(v)),
        ("RENESIS-TODO.md", r"v%s" % re.escape(v)),
        ("NEW-SESSION-PRIMER.md", r"v%s" % re.escape(v)),
        ("github/README.md", None),
    ]
    for path, pat in musts:
        if not os.path.exists(path):
            bad("%s missing" % path)
            continue
        if pat is None:
            ok("%s present" % path)
            continue
        text = open(path, errors="replace").read()
        if re.search(pat, text, re.M):
            ok("%s mentions v%s" % (path, v))
        else:
            bad("%s does not mention v%s" % (path, v))


def check_source_headers(v):
    """v92.1 (owner's standing rule): every source file's header `Modified:`
    line carries THIS cut's version.  `Created:` is never touched -- it
    records the cut a file first appeared in and is history, not currency.

    The owner found v89.11 stamps inside the v92 image while reviewing it for
    the public push.  They were harmless and they were also the first thing a
    reader sees at the top of every file, which is exactly the kind of detail
    the repository is judged on.  Mechanical, so it is checked mechanically."""
    print("[1b] source header currency (v%s)" % v)
    pat = re.compile(r"Modified:\s+\d{4}-\d{2}-\d{2}\s+\(Renesis v([0-9.]+)\)")
    stale, seen = [], 0
    for root, dirs, files in os.walk("."):
        if ".git" in dirs:
            dirs.remove(".git")
        for f in files:
            if not f.endswith((".c", ".h", ".py", ".cpp", ".hpp")):
                continue
            path = os.path.join(root, f)
            try:
                text = open(path, errors="replace").read(4096)
            except Exception:
                continue
            m = pat.search(text)
            if not m:
                continue
            seen += 1
            if m.group(1) != v:
                stale.append("%s (v%s)" % (path, m.group(1)))
    if not seen:
        bad("no source header carries a Modified version token")
    elif stale:
        bad("%d of %d source header(s) not stamped v%s" % (len(stale), seen, v))
        for h in stale[:8]:
            print("        | %s" % h)
        if len(stale) > 8:
            print("        | ... and %d more" % (len(stale) - 8))
    else:
        ok("all %d source headers stamped v%s" % (seen, v))


def check_version_unused(v):
    """v92.1 (owner's standing rule, set after it was broken): a version
    number is never reused.  SHIPPED-VERSIONS.md holds one row per bundle
    that has left the container; cutting a version that already has a row
    fails here.

    The rule survives a request that names an already-shipped version: such a
    request is about which fix goes in, and advancing the number is the cut's
    own job.  Recorded mechanically because the failure mode is a long
    session in which the number simply carries forward unnoticed."""
    print("[1c] version number not already shipped (v%s)" % v)
    led = "SHIPPED-VERSIONS.md"
    if not os.path.exists(led):
        bad("%s missing -- the ledger is how reuse is detected" % led)
        return
    rows = []
    for line in open(led, errors="replace"):
        m = re.match(r"\|\s*v([0-9][0-9.]*)\s*\|", line)
        if m:
            rows.append(m.group(1))
    if not rows:
        bad("%s has no version rows" % led)
    elif v in rows:
        bad("v%s has ALREADY SHIPPED (%d row(s) in %s) -- cut the next "
            "number instead" % (v, rows.count(v), led))
        print("        | shipped so far: %s" % ", ".join("v" + r for r in rows))
    else:
        ok("v%s is unused (%d prior cut(s) recorded)" % (v, len(rows)))


def check_options_expect():
    """v92.3: the D15 EXPECT block states an option count.  It said 48 while
    the table has held 49 since `prescreen` landed in v91.3, and the Mac and
    the Spark both printed 49 for three cuts without the drift being caught.
    A number a human maintains beside a number a program computes will drift,
    so this check computes it and compares."""
    print("[1d] D15 option count matches the shipped table")
    tbl = os.path.join("config", "renesis_options.json")
    docs = [f for f in os.listdir(".")
            if f.startswith("VALIDATE-V") and f.endswith(".md")]
    if not os.path.exists(tbl) or not docs:
        bad("cannot check: missing %s or VALIDATE doc"
            % ("config/renesis_options.json" if not os.path.exists(tbl)
               else "VALIDATE-V*.md"))
        return
    try:
        d = json.load(open(tbl))
    except Exception as e:
        bad("options table unreadable: %s" % e)
        return
    n = sum(1 for s_, b in d.items() if not s_.startswith("_")
            for k, r in b.items() if isinstance(r, dict))
    h = sum(1 for s_, b in d.items() if not s_.startswith("_")
            for k, r in b.items() if isinstance(r, dict) and r.get("help"))
    want = "options %d with help %d" % (n, h)
    stale = []
    for f in docs:
        text = open(f, errors="replace").read()
        for m in re.finditer(r"options (\d+) with help (\d+)", text):
            if m.group(0) != want:
                stale.append("%s says %r" % (f, m.group(0)))
    if n != h:
        bad("%d of %d options carry no help text" % (n - h, n))
    elif stale:
        bad("D15 EXPECT stale, table says %r" % want)
        for x in stale[:4]:
            print("        | %s" % x)
    else:
        ok("D15 EXPECT agrees with the table (%s)" % want)


def check_hygiene():
    print("[2] tree hygiene")
    hits = []
    for root, dirs, files in os.walk("."):
        if ".git" in dirs:
            dirs.remove(".git")
        for d in list(dirs):
            if d in FORBIDDEN_NAMES:
                hits.append(os.path.join(root, d))
                dirs.remove(d)
        for f in files:
            p = os.path.join(root, f)
            if f in FORBIDDEN_NAMES or f.endswith(FORBIDDEN_SUFFIX) or is_elf(p):
                hits.append(p)
    # the --only lesson: no top-level entry may look like a stray option token
    for e in os.listdir("."):
        if e.startswith("-"):
            hits.append("./" + e + "  (top-level entry named like an option)")
    if hits:
        for h in sorted(hits)[:40]:
            bad("forbidden in bundle: %s" % h)
    else:
        ok("no ELF / objects / pycache / stray-option entries")


def check_mirror():
    print("[3] mirror identity (%d files)" % len(MIRRORED))
    drift = []
    for f in MIRRORED:
        a, b = os.path.join("scripts", f), os.path.join("scripts_adiabatic", f)
        if not (os.path.exists(a) and os.path.exists(b)):
            drift.append(f + " (missing on one side)")
        elif open(a, "rb").read() != open(b, "rb").read():
            drift.append(f)
    if drift:
        bad("mirror drift: %s" % ", ".join(drift))
    else:
        ok("scripts/ and scripts_adiabatic/ mirror-identical")


def check_validate_doc(v):
    print("[4] validation document discipline")
    path = "VALIDATE-V%s.md" % v
    if not os.path.exists(path):
        bad("%s missing" % path)
        return
    text = open(path, errors="replace").read()
    if "EXPECT-PLACEHOLDER" in text:
        bad("%s contains an unfilled EXPECT placeholder" % path)
    else:
        ok("no EXPECT placeholders")
    if "executed verbatim" in text:
        ok("carries the executed-verbatim statement")
    else:
        bad("%s lacks the executed-verbatim statement -- EXPECT blocks must "
            "be pasted from a run on a clean extraction" % path)


def check_image_buildable():
    """v91.3.  The image must contain every source its own Makefiles name.

    v91.2 shipped a github image whose csrc/Makefile still listed the fourteen
    vsim translation units while the files themselves were not copied in, so
    `make -C csrc` died on its first target -- "No rule to make target
    'vsim.o'" -- and D17 (build the image from scratch, run its suite) printed
    NOTHING, because its command is one && chain.  It was silent on the
    owner's M3 Max and on the Spark alike; the container missed it because the
    tree being re-copied still held stale .o files, so make never needed the
    .c files.  A false pass.

    This check is the cheap structural version of D17 and runs at every cut,
    before anyone spends a validation cycle: for each Makefile in the image,
    take every NAME.o it mentions and require NAME.c (or NAME.cpp, or a
    sibling under the source dirs the Makefile includes) to exist."""
    print("[5b] github image source completeness (Makefile names vs files)")
    missing = []
    for mk in ("github/csrc/Makefile", "github/tools/adshim/Makefile"):
        if not os.path.exists(mk):
            bad("%s is missing -- the image cannot be built" % mk)
            continue
        d = os.path.dirname(mk)
        roots = [d, os.path.join(d, "..", "exorcism", "source")]
        text = open(mk, errors="replace").read()
        want = set()
        # sources named outright:  CORE = vsim.c vsim_tags.c ...
        for m in re.finditer(r"([A-Za-z0-9_][A-Za-z0-9_.\-]*)\.(c|cpp|cc)\b",
                             text):
            want.add((m.group(1), "." + m.group(2)))
        # objects named outright:  vsim: $(OBJ) vsim_main.o
        for m in re.finditer(r"([A-Za-z0-9_][A-Za-z0-9_.\-]*)\.o\b", text):
            base = m.group(1)
            if not any((base, e) in want for e in (".c", ".cpp", ".cc")):
                want.add((base, None))
        for base, ext in sorted(want):
            exts = [ext] if ext else [".c", ".cpp", ".cc"]
            if any(os.path.exists(os.path.join(r, base + e))
                   for r in roots for e in exts):
                continue
            missing.append("%s: %s%s named by %s but not in the image"
                           % (d, base, ext or ".[c|cpp]",
                              os.path.basename(mk)))
    if missing:
        for m in missing:
            bad(m)
    else:
        ok("every source the image's Makefiles name is present")


def check_image_docs(v):
    """v92.  The image's own documents must be current and self-contained.

    v91.3 was cut with `**v91.0 -- first release.**` at the top of the image
    README and `**v91.0.**` at the top of the image manual, and with the
    manual's "where to read more" pointing at five documents the image does
    not ship (docs/USER-GUIDE-OPTIONS.md, RENESIS-CONTENTS-AUDIT.md,
    docs/TECHNICAL-MANUAL.md, APPROXIMATIONS.md, RENESIS-TODO.md,
    VALIDATE-V87.1.md) plus two research drivers.  The owner found the version
    lines by reading; nothing would have caught the dangling references.

    This is the v89.7 banner lesson a third time: a currency check that was
    written for the bundle and never extended to the image.  Two rules, both
    mechanical:

      1. A document's SELF-DESCRIBING version -- a `**vX.Y**` in its opening
         lines -- must be the version being cut.  Historical references in the
         body ("as of v90.7", "through v91.1") are legitimate and untouched.
      2. No image document, and no help text in the image's options table, may
         reference a file that is not in the image.  A public reader cannot
         follow a pointer into a bundle they do not have."""
    print("[5c] github image documents (currency + no dangling references)")
    import glob as _g
    here = os.getcwd()
    stale, dangling = [], []
    docs = sorted(_g.glob("github/*.md") + _g.glob("github/*/*.md"))
    selfver = re.compile(r"^\*\*v([0-9][0-9.]*)\b")
    ref = re.compile(r"`([A-Za-z0-9_][A-Za-z0-9_./-]*\.(?:md|sh|py|json|command|txt|1))`")
    for d in docs:
        text = open(d, errors="replace").read()
        head = "\n".join(text.split("\n")[:8])
        m = selfver.search(head, 0) or selfver.search(head.replace("\n", "\n"), 0)
        for line in head.split("\n"):
            mm = selfver.match(line)
            if mm:
                if mm.group(1).rstrip(".") != v:
                    stale.append("%s says v%s at the top; this cut is v%s"
                                 % (d, mm.group(1).rstrip("."), v))
                break
    # dangling references, in the docs and in the shipped options table
    targets = list(docs) + ["github/config/renesis_options.json"]
    for d in targets:
        if not os.path.exists(d):
            continue
        text = open(d, errors="replace").read()
        for name in sorted(set(ref.findall(text))):
            base = os.path.basename(name)
            # placeholders the docs use in command templates, not real files
            if (name == "127.0.0.1"
                    or name.startswith(("BASE", "OUT", "NAME", "FILE", "base"))
                    or re.search(r"\b(NAME|FILE|BASE|OUT|out|your)\b", name)):
                continue
            if os.path.exists(os.path.join("github", name)):
                continue
            if _g.glob(os.path.join("github", "**", base), recursive=True):
                continue
            dangling.append("%s references %s, which the image does not ship"
                            % (d, name))
    # the options table names files in prose too (no backticks)
    ot = "github/config/renesis_options.json"
    if os.path.exists(ot):
        for name in sorted(set(re.findall(r"\b([A-Z][A-Z0-9-]+\.md)\b",
                                          open(ot, errors="replace").read()))):
            if not _g.glob(os.path.join("github", "**", name), recursive=True):
                dangling.append("%s names %s in help text, which the image "
                                "does not ship" % (ot, name))
    # v92.1: the man page's .TH line names a version too, and it shipped
    # saying v91.3 inside the v92 image -- the md-only check above missed it.
    for mp in ("github/renesis.1", "renesis.1"):
        if os.path.exists(mp):
            m = re.search(r'"Renesis v([0-9.]+)"', open(mp, errors="replace").read())
            if m and m.group(1) != v:
                stale.append("%s .TH says Renesis v%s; this cut is v%s"
                             % (mp, m.group(1), v))
    for msg in stale + dangling:
        bad(msg)
    if not stale and not dangling:
        ok("%d image document(s) + man page: version current, no dangling "
           "references" % len(docs))


def check_github(v):
    """v91.0 repo policy (owner, 2026-08-14): the image is a CURATED release
    -- sources, manuals, examples, bench, config -- with NO internal
    development record (no CHECKPOINT/VALIDATE/history/TODO/sweep).  The
    image uses the bundle's own layout (scripts/, scripts_adiabatic/,
    csrc/, tools/) so every relative path works unchanged, and it must
    build and pass run_tests.sh BY ITSELF (verified at every cut)."""
    print("[5] github image currency (spot)")
    stale = []
    for f in MIRRORED:
        t = os.path.join("scripts", f)
        for g in (os.path.join("github", "scripts", f),
                  os.path.join("github", "scripts_adiabatic", f)):
            if not os.path.exists(g):
                stale.append(g + " (missing)")
            elif open(t, "rb").read() != open(g, "rb").read():
                stale.append(g)
    for req in ("README.md", "LICENSE", "PATENTS.md", "RENESIS-MANUAL.md",
                "WEB-UI-HOWTO.md", "MACOS-SETUP.md", "renesis",
                "csrc/run_tests.sh", "examples/EightBitHashTable.pla",
                "examples/TwelveBitHash.pla",
                "config/renesis_options.json"):
        if not os.path.exists(os.path.join("github", req)):
            stale.append("github/%s (missing -- release content)" % req)
    for banned in ("validate_all.sh", "history", "RENESIS-TODO.md",
                   "renesis_sweep.py", "BDEC-NOTES.md",
                   "other_options_validate.sh"):
        if os.path.exists(os.path.join("github", banned)):
            stale.append("github/%s (INTERNAL -- must not ship)" % banned)
    import glob as _g
    for pat in ("github/CHECKPOINT-*.md", "github/VALIDATE-*.md"):
        for hit in _g.glob(pat):
            stale.append(hit + " (INTERNAL -- must not ship)")
    if stale:
        for s in stale[:20]:
            bad("github image: %s" % s)
    else:
        ok("image mirrors current; release content present; no internal docs")


def check_manual_coverage(strict):
    print("[6] manual coverage (options table vs RENESIS-MANUAL.md)")
    try:
        table = json.load(open("config/renesis_options.json"))
    except Exception as e:
        bad("cannot read options table: %s" % e)
        return
    manual = open("RENESIS-MANUAL.md", errors="replace").read()
    # the table is sectioned: top-level keys are metadata (underscore-
    # prefixed) or sections; each section maps option name -> record.
    keys = []
    for sect, body in table.items():
        if sect.startswith("_") or not isinstance(body, dict):
            continue
        keys.extend(k for k, v in body.items() if isinstance(v, dict))
    # accepted alias spellings: the CLI takes both forms, the manual
    # documents the short one (verified against the driver, v89.6)
    ALIASES = {"cover_mode": "--cover", "chain_idx": "--chain",
               "elim": "--factor", "elim_min_gain": "--factor-min-gain",
               "elim_value_limit": "--factor-value-limit"}
    missing = []
    for key in keys:
        flag = "--" + key.replace("_", "-")
        alt = ALIASES.get(key)
        if flag not in manual and ("`%s`" % key) not in manual \
           and not (alt and alt in manual):
            missing.append(flag)
    if not missing:
        ok("every options-table key appears in the manual")
    else:
        msg = ("%d option(s) undocumented in RENESIS-MANUAL.md: %s"
               % (len(missing), ", ".join(sorted(missing))))
        if strict:
            bad(msg)
        else:
            warn(msg + "   (WARN until the manual rewrite lands; "
                       "--strict-manual to enforce)")


def check_validate_all(v):
    """validate_all.sh must be exactly what the VALIDATE doc generates.

    v89.9, from a real miss: the owner ran 'everything in the script' for
    v89.8 and D6 was not in what he ran.  The script is now GENERATED from
    the doc's command fences, and this check regenerates and compares, so
    the runnable procedure can never silently omit a block the doc has."""
    print("[9] validate_all.sh freshness (generated from VALIDATE-V%s.md)" % v)
    if not os.path.exists("validate_all.sh"):
        bad("validate_all.sh missing -- generate it: python3 "
            "scripts_adiabatic/gen_validate_all.py --version %s" % v)
        return
    r = subprocess.run([sys.executable,
                        "scripts_adiabatic/gen_validate_all.py",
                        "--version", v, "--stdout"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        bad("gen_validate_all failed: %s" % r.stderr.strip()[:80])
        return
    if r.stdout == open("validate_all.sh", errors="replace").read():
        ok("validate_all.sh matches the doc (%d lines)"
           % len(r.stdout.splitlines()))
    else:
        bad("validate_all.sh does not match VALIDATE-V%s.md -- regenerate "
            "it; the runnable procedure has drifted from the doc" % v)


def check_spice_cells(mode):
    """spice/ is GENERATED from the tool; it must match a regeneration.

    v89.10.  The cell decks are the tool's own synthesis of the canonical
    two-input gate per family; a hand-edited or stale deck would ship a
    topology the tool does not produce.  Validate mode regenerates and
    compares; prepack skips (a pristine tree cannot run the flow -- the
    adshim is not built) and the validate run covers it."""
    print("[10] spice/ cell library vs regeneration")
    if mode == "prepack":
        print("  SKIPPED (prepack mode -- regeneration needs the built "
              "adshim; the validate-mode run covers this)")
        return
    if not os.path.isdir("spice"):
        bad("spice/ missing -- generate it: python3 "
            "scripts_adiabatic/gen_spice_cells.py")
        return
    r = subprocess.run([sys.executable,
                        "scripts_adiabatic/gen_spice_cells.py", "--check"],
                       capture_output=True, text=True,
                       env=dict(os.environ, PYTHONHASHSEED="0"))
    if r.returncode == 0:
        ok("spice/ matches a fresh regeneration")
    else:
        bad("spice/ drifted from the tool: %s"
            % r.stdout.strip().splitlines()[-1][:90])


def check_prepack():
    print("[7] prepack (derived artifacts + manual defaults)")
    r = subprocess.run([sys.executable,
                        "scripts_adiabatic/prepack_check.py"],
                       env=dict(os.environ, PYTHONHASHSEED="0"))
    if r.returncode == 0:
        ok("prepack_check clean")
    else:
        bad("prepack_check failed (see output above)")


def check_tests():
    print("[8] the C test suite (58f: SOME TESTS FAILED is unshippable)")
    r = subprocess.run(["bash", "run_tests.sh"], cwd="csrc",
                       env=dict(os.environ, PYTHONHASHSEED="0"),
                       capture_output=True, text=True)
    tail = "\n".join(r.stdout.splitlines()[-3:])
    if "ALL TESTS PASSED" in r.stdout and r.returncode == 0:
        ok("run_tests.sh: ALL TESTS PASSED")
    else:
        bad("run_tests.sh did not pass:\n%s" % tail)


PY_ONLY = [
    "NO re-synthesis passes -- the pass list is fully bilingual as of "
    "v90.5 (prefix v90.1, elim/factor v90.2, bdec v90.3, davio v90.4, "
    "linwin/mowin v90.5)",
    "NO orchestration surfaces -- all ported in v90.6 (the optimize() "
    "dispatcher with --pass-order and per-pass budget maps, the pass "
    "Budget/wall_s, the K-ladder, drive-model tags --pi-drive/--saif, "
    "and the --spice-gen/--schematic FILE exports)",
    "constructive reversible synthesis (the .real/.tfc pipeline)",
    "the web UI, and the schematic SVG RENDERING (the graphviz/"
    "netlistsvg subprocess convenience; the exported .dot/.json files "
    "themselves are bilingual byte contracts)",
    "--optimize-all as a spelled flag (Python CLI sugar over four "
    "option bools; the C driver takes the same options directly)",
]
C_HAS = [
    "all front-end parsers (.v/.isc/.pla/.aig/.aag/.bench/.blif)",
    "the vsim layer and the exact ladder",
    "technology mapping + energy accounting (bit-identical to Python, "
    "2496/2496 parity cells; the s1488 shallow-candidate degeneracy "
    "fixed v90.2, suite stage [13])",
    "technologies-as-data (config/technology/*.json)",
    "the PREFIX re-synthesis pass (v90.1, ropt.c; suite stage [12])",
    "the ELIM/FACTOR re-synthesis pass (v90.2, ropt_elim.c: SOP network, "
    "bounded elimination, kernel + rectangle extraction, both "
    "polarities; endpoint byte-identical on c17/reconv24/c432/c880; "
    "suite stage [14])",
    "the BDEC re-synthesis pass (v90.3, ropt_bdec.c: invertible GF(2) "
    "output re-encoding, hill-climb row-add search, two-budget pricing, "
    "merged map REPLACES the mapping stage; reject/accept endpoints "
    "byte-identical on c17/bdtoy2, the c1238 E2 alias candidate "
    "rejected not fatal (BUG-V90-04); suite stage [15])",
    "the DAVIO re-synthesis pass (v90.4, ropt_davio.c: affine-cut "
    "extraction over K-feasible cuts, the 2/3/4/6/uncapped width "
    "ladder, per-width fixpoint, both-tables gate -- carrying a "
    "bit-exact CPython 3.11 set-table emulation because the Python "
    "pass's cut choice and XOR leaf order follow frozenset iteration "
    "order under PYTHONHASHSEED=0; endpoints byte-identical on "
    "c17/c432/c880/c1355; suite stage [16])",
    "the LINWIN + MOWIN re-synthesis passes (v90.5, ropt_win.c: "
    "single-output fanout-closed / multi-output promotion-closed "
    "affine windows over the same order-tracked cut machinery, "
    "deterministic first-improvement affine search, shared-dictionary "
    "activity score computed in EXACT float arithmetic, near-miss "
    "records with Python round(x, 9) semantics; window lists, "
    "searches and pass endpoints byte-identical on the fixture set; "
    "suite stage [17])",
    "tag generation (bit-exact MT19937 forward sweep since v83; "
    "--tags FILE also accepted)",
    "the optimize() DISPATCHER (v90.6, renesis_main.c: --option "
    "pass_order with canon factor->elim and Python's verbatim unknown/"
    "omitted errors; per-pass price_cap/passes budget maps in "
    "parse_budget's scalar and map forms; suite stage [18])",
    "the pass BUDGET (v90.6, ropt.c RoptBudget: wall_s honoured, ONE "
    "budget spans the optimize() chain and bdec builds its own, "
    "CLOCK_MONOTONIC, check_cut every-64 sites mirrored per pass; "
    "budgeted runs validate by equivalence + verdict-class + "
    "budget-honoured, NEVER byte parity -- the C is not throttled; "
    "unbudgeted paths stay byte-identical; suite stage [18])",
    "the K-LADDER (v90.6, renesis_main.c: champion loop over --option "
    "k_ladder rungs, both/t2 acceptance, per-rung receipts, wall "
    "budget with SKIPPED-not-dropped receipts; unbudgeted ladders "
    "byte-parity, budgeted verdict-class; suite stage [18])",
    "drive-model tags (v90.6, renesis_drive.c: SAIF s-expression "
    "reader with the strict validity bound and Python's verbatim "
    "errors, stationary lag-one chain vector draws in the tag sweep "
    "AND energy_report via bit-exact genrand_res53; drive runs are "
    "deterministic, so BYTE parity applies, proven through "
    "cover=switching and bdec; suite stage [18])",
    "--spice-gen / --schematic FILE exports (v90.6, rsynth_tech.c + "
    "renesis_netio.c: the ngspice deck, both .dot files and the "
    "Yosys-JSON netlist byte-identical across all shipped families "
    "incl. NPASS/XC/ACLK cells; SVG rendering stays Python; suite "
    "stage [18])",
    "the C-only build gate (make standalone-check, v90.2)",
]


def check_parity_boundary(v):
    """[11] language parity boundary: disclosed, and still true.

    v89.12, after the v89.11 conversation in which the owner discovered
    the C tree lacks the re-synthesis passes.  Two duties, every cut:
    (a) the C driver must still REFUSE the Python-only passes -- a
    silent no-op is unshippable; and (b) the cut's CHECKPOINT must
    carry the disclosure section, so no bundle can ship without
    stating what is and is not in the C tree.  The gate prints the
    boundary into its own log on every run so it appears in every
    validation console the owner reads.
    """
    print("[11] language parity boundary (disclosure + C-side refusal)")
    print("  Python-only today:")
    for x in PY_ONLY:
        print("    - " + x)
    print("  C tree has:")
    for x in C_HAS:
        print("    - " + x)
    src = open(os.path.join("csrc", "renesis_main.c"),
               encoding="utf-8").read()
    # v90.6: no Python-only surfaces remain that the C driver could
    # silently no-op -- the duty flips from REFUSING them to proving the
    # ported wiring is still there.  (An unknown flag still errors, which
    # covers whatever Python grows next until it is ported or listed.)
    for needle, what in [
            ("rb_parse_order",         "the pass_order dispatcher"),
            ("ropt_budget_init",       "the wall_s Budget"),
            ("parse_k_ladder_c",       "the K-ladder"),
            ("rdrive_from_saif",       "drive-model tags (--pi-drive saif)"),
            ("tech_write_spice_c",     "--spice-gen"),
            ("tech_write_mapped_dot_c", "--schematic")]:
        if needle in src:
            ok("C driver wires %s (v90.6)" % what)
        else:
            bad("csrc/renesis_main.c does not reference %s -- %s came "
                "unwired; PY_ONLY above would be a lie" % (needle, what))
    if "ropt_prefix_resynth" in src:
        ok("C driver runs the ported prefix pass (v90.1)")
    else:
        bad("csrc/renesis_main.c does not invoke ropt_prefix_resynth -- "
            "the ported pass came unwired; C_HAS above would be a lie")
    if "ropt_elim_resynth" in src:
        ok("C driver runs the ported elim pass (v90.2)")
    else:
        bad("csrc/renesis_main.c does not invoke ropt_elim_resynth -- "
            "the ported pass came unwired; C_HAS above would be a lie")
    if "ropt_bdec_run" in src:
        ok("C driver runs the ported bdec pass (v90.3)")
    else:
        bad("csrc/renesis_main.c does not invoke ropt_bdec_run -- "
            "the ported pass came unwired; C_HAS above would be a lie")
    if "ropt_davio_resynth" in src:
        ok("C driver runs the ported davio pass (v90.4)")
    else:
        bad("csrc/renesis_main.c does not invoke ropt_davio_resynth -- "
            "the ported pass came unwired; C_HAS above would be a lie")
    if src.count("ropt_win_resynth") >= 2:
        ok("C driver runs the ported linwin AND mowin passes (v90.5)")
    else:
        bad("csrc/renesis_main.c does not invoke ropt_win_resynth for "
            "both window passes -- a ported pass came unwired; C_HAS "
            "above would be a lie")
    cp = "CHECKPOINT-V%s.md" % v
    if not os.path.exists(cp):
        bad("%s missing -- cannot verify the disclosure section" % cp)
    elif "## Language parity boundary" in open(cp, encoding="utf-8").read():
        ok("%s carries the parity-boundary disclosure section" % cp)
    else:
        bad("%s lacks the '## Language parity boundary' section -- add it "
            "(the list this gate just printed)" % cp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True,
                    help="bundle version being cut, e.g. 89.5")
    ap.add_argument("--mode", choices=["validate", "prepack"],
                    default="validate",
                    help="validate: built work tree (tests, no hygiene). "
                         "prepack: pristine packing tree (hygiene, no tests)")
    ap.add_argument("--skip-tests", action="store_true",
                    help="skip the C suite (doc iteration only -- the final "
                         "pre-pack run must not skip)")
    ap.add_argument("--strict-manual", action="store_true",
                    help="manual coverage gaps FAIL instead of WARN")
    a = ap.parse_args()

    check_banner(a.version)
    check_versions(a.version)
    check_source_headers(a.version)
    check_version_unused(a.version)
    check_options_expect()
    if a.mode == "prepack":
        check_hygiene()
    else:
        print("[2] tree hygiene: SKIPPED (validate mode -- build products "
              "expected; the prepack-mode run covers this)")
    check_mirror()
    check_validate_doc(a.version)
    check_github(a.version)
    check_image_buildable()
    check_image_docs(a.version)
    check_manual_coverage(a.strict_manual)
    check_validate_all(a.version)
    check_spice_cells(a.mode)
    check_parity_boundary(a.version)
    check_prepack()
    if a.mode == "prepack":
        print("[8] C suite: SKIPPED (prepack mode -- a pristine tree has no "
              "binaries and run_tests.sh would silently skip its stages; "
              "the validate-mode run covers this)")
    elif a.skip_tests:
        print("[8] SKIPPED (--skip-tests) -- the final validate run must "
              "not skip")
    else:
        check_tests()

    print()
    if fails:
        print("release_gate: %d FAILURE(S) -- do not cut." % len(fails))
        return 1
    if warns:
        print("release_gate: clean, %d warning(s)." % len(warns))
        return 0
    print("release_gate: clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

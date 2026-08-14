# ---------------------------------------------------------------------------
#  renesis_config.py -- Options table + technology description loader
#  Renesis: energy-aware reversible / adiabatic logic synthesis
#
#  The option surface used to live in three places at once: library
#  keyword defaults, per-harness overrides, and conventions patched in at
#  runtime (the `tgate_sl6` clone being the clearest case). A user could
#  not see it, and a caller of the library got something different from
#  what we certify.
#  This module makes the surface external and singular:
#
#  Author:      Mitchell A. Thornton
#  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
#  Modified:    2026-08-10  (Renesis v89.11)
#  Created:     Renesis v82 (earliest version token in file)
# ---------------------------------------------------------------------------
"""Options table + technology description loader.

The option surface used to live in three places at once: library keyword
defaults, per-harness overrides, and conventions patched in at runtime (the
`tgate_sl6` clone being the clearest case).  A user could not see it, and a
caller of the library got something different from what we certify.

This module makes the surface external and singular:

    config/renesis_options.json      every option, its default, legal range,
                                     help text, and -- where the library's own
                                     default differs -- an AUDIT note saying so
    config/technology/<name>.json    one file per target technology: cell
                                     model, structural constraints, clocking,
                                     and the provenance of the constants

THE DEFAULTS RULE (owner, 2026-08-03): where a library keyword default and
the release validation disagree, the value assumed by the 20-circuit
validation wins.  The old keyword default is preserved as `code_default` so
the difference is documented rather than lost.

Behaviour contract: loading a shipped technology file must reproduce the
numbers the runtime-patched family produced.  `verify_technology_neutral()`
asserts it, and the v82 validation runs it.
"""
from __future__ import annotations

import json
import os

BUNDLE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPTIONS_FILE = os.path.join(BUNDLE, "config", "renesis_options.json")
TECH_DIR = os.path.join(BUNDLE, "config", "technology")


class Options:
    """Flat view over the grouped options table.

    Access by bare name (`opt["k"]`) or qualified name (`opt["cover.k"]`);
    bare names must be unique across groups, which the table enforces.
    """

    def __init__(self, table):
        self.table = table
        self.flat = {}
        self.group_of = {}
        for grp, entries in table.items():
            if grp.startswith("_"):
                continue
            for name, spec in entries.items():
                if name.startswith("_"):
                    continue
                if name in self.flat:
                    raise ValueError(
                        "duplicate option name %r in groups %r and %r -- bare "
                        "names must be unique" % (name, self.group_of[name], grp))
                self.flat[name] = spec["default"]
                self.group_of[name] = grp

    # -- access --------------------------------------------------------
    def __getitem__(self, key):
        if "." in key:
            grp, name = key.split(".", 1)
            return self.table[grp][name]["default"]
        return self.flat[key]

    def __contains__(self, key):
        return key in self.flat

    def get(self, key, default=None):
        return self.flat.get(key, default)

    def spec(self, name):
        return self.table[self.group_of[name]][name]

    # -- mutation ------------------------------------------------------
    def set(self, name, value):
        """Set one option, coercing a string to the declared type."""
        if name not in self.flat:
            raise KeyError("unknown option %r (see config/renesis_options.json)"
                           % name)
        cur = self.flat[name]
        if isinstance(value, str) and not isinstance(cur, str):
            v = value.strip().lower()
            if isinstance(cur, bool) or cur is None and v in ("true", "false"):
                if v not in ("true", "false", "1", "0", "on", "off",
                             "yes", "no"):
                    raise ValueError("option %r expects a boolean, got %r"
                                     % (name, value))
                value = v in ("true", "1", "on", "yes")
            elif isinstance(cur, int) and not isinstance(cur, bool):
                value = int(value)
            elif isinstance(cur, float):
                value = float(value)
            elif cur is None:
                value = None if v in ("none", "null", "") else float(value)
        self._check_legal(name, value)
        self.flat[name] = value
        return value

    def _check_legal(self, name, value):
        """Enforce the table's `legal` field for enumerated string options.

        v87.1.  Until now `legal` was documentation and nothing read it, so a
        typo in a string-valued option was not an error -- it was a silently
        different run.  `cover_mode=flowmap` meant `switching`; `route=bdd`
        meant structural; neither said so.  Only ENUMERATED legality (a list in
        the table) is enforced here; free-form entries like "integer >= 1" stay
        advisory, because parsing prose to validate a number would be its own
        source of wrong answers."""
        try:
            spec = self.spec(name)
        except KeyError:
            return
        legal = spec.get("legal")
        if not isinstance(legal, list) or not legal:
            return
        if isinstance(value, str):
            allowed = [x for x in legal if isinstance(x, str)]
            if allowed and value not in allowed:
                raise ValueError(
                    "option %r: %r is not one of %s.\n"
                    "        (Values outside this list used to be accepted and "
                    "then silently ignored; v87.1 makes them an error.)"
                    % (name, value, ", ".join(repr(x) for x in allowed)))

    def apply(self, assignments):
        """Apply ["name=value", ...] and return the names that changed."""
        changed = []
        for a in assignments or []:
            if "=" not in a:
                raise ValueError("expected NAME=VALUE, got %r" % a)
            n, v = a.split("=", 1)
            n = n.strip()
            if "." in n:
                n = n.split(".", 1)[1]
            self.set(n, v)
            changed.append(n)
        return changed

    def non_default(self):
        """Options whose current value differs from the shipped default."""
        out = {}
        for name, val in self.flat.items():
            if val != self.spec(name)["default"]:
                out[name] = val
        return out

    def audits(self):
        """Options where the library's own default differs from ours."""
        return {n: (self.spec(n).get("code_default"), self.spec(n)["default"])
                for n in self.flat if "code_default" in self.spec(n)
                and self.spec(n)["code_default"] != self.spec(n)["default"]}

    def as_dict(self):
        return dict(self.flat)


def load_options(path=None):
    path = path or OPTIONS_FILE
    if not os.path.exists(path):
        raise FileNotFoundError(
            "options table not found at %s -- renesis will not guess its "
            "defaults" % path)
    return Options(json.load(open(path)))


def load_technology(name, tech_dir=None):
    """Read a technology description file."""
    d = tech_dir or TECH_DIR
    path = os.path.join(d, "%s.json" % name)
    if not os.path.exists(path):
        avail = sorted(f[:-5] for f in os.listdir(d) if f.endswith(".json")) \
            if os.path.isdir(d) else []
        raise FileNotFoundError(
            "no technology description %r in %s. Available: %s"
            % (name, d, ", ".join(avail) or "(none)"))
    t = json.load(open(path))
    t["_path"] = path
    return t


def list_technologies(tech_dir=None):
    d = tech_dir or TECH_DIR
    if not os.path.isdir(d):
        return []
    return sorted(f[:-5] for f in os.listdir(d) if f.endswith(".json"))


def register_technology(tech):
    """Install a technology description into the runtime family registry.

    Returns the family key to pass to `tech_synth`.  This is what replaces
    the harnesses' runtime `tgate_sl6` clone: the cap now travels in the
    technology description, so the family the mapper sees is exactly what
    the file says.
    """
    import tech_families as tf
    if tech.get("role") == "comparison_baseline":
        return tech.get("parameters", {}).get("family_for_pricing", "tgate")
    name = tech["target_technology"]
    # A technology description is a base mapper family plus parameter
    # overrides; a derived target (tgate_sl6) names the family it maps with.
    base = tech.get("mapper_family", name)
    rec = dict(tf.FAMILIES.get(base, {}))
    rec.update(tech.get("parameters", {}))
    rec["kind"] = tech.get("mapper_kind", rec.get("kind", ""))
    rec["desc"] = tech.get("description", rec.get("desc", ""))
    # v89.8: `cap` no longer exists here.  Through v89.7 this function
    # overrode EVERY family's series_limit with the --cap value -- a
    # leftover from replacing the runtime tgate_sl6 clone, where the two
    # numbers coincide.  Result: the Python driver mapped every family at
    # series limit 6 while the family files say 4 and the C driver honored
    # the 4 -- a silent cross-language divergence on every family except
    # tgate_sl6, invisible to the matrix because the matrix lives on the
    # rsynth path.  The mapper's series_limit now comes from the technology
    # description alone (the file's `parameters` already carry it); --cap
    # drives buffer insertion, which is what its help text always said.
    key = "%s_cfg" % name
    tf.FAMILIES[key] = rec
    return key


def verify_technology_neutral(name="tgate_sl6", cap=6, tech_dir=None):
    """Assert the file-driven family matches the runtime-patched one.

    The historical construction was: clone FAMILIES['tgate'] and override
    series_limit to 6, calling it 'tgate_sl6'.  A file-driven family with
    the same parameters must be identical field for field, or the numbers
    would move for reasons unrelated to synthesis.  v89.8: the file checked
    is tgate_sl6's own description (which CARRIES series_limit 6) rather
    than tgate's plus a cap override -- register_technology no longer takes
    a cap, because the override it performed was the cross-language
    series-limit divergence this cut removes.
    """
    import tech_families as tf
    legacy = dict(tf.FAMILIES["tgate"])
    legacy["series_limit"] = cap
    key = register_technology(load_technology(name, tech_dir))
    got = dict(tf.FAMILIES[key])
    diffs = {k: (legacy.get(k), got.get(k))
             for k in set(legacy) | set(got) if legacy.get(k) != got.get(k)}
    return (not diffs), diffs

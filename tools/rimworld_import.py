#!/usr/bin/env python3
"""Imports RimWorld's Defs XML as Rim3D JSON defs and short reference summaries.

Runs on a machine that has RimWorld installed (task D3). Reads `<install>/Data/<Pack>/Defs/**/*.xml`
(Core first, expansions later), resolves RimWorld's XML inheritance (Name / ParentName), keeps the
defs listed in `rimworld_import_spec.py`, converts durations and speeds to game seconds, and writes

  data/defs/<Kind>/<Kind>.rimworld.json    one flat file per kind, no `parent` (already merged)
  docs/reference/<Kind>.md                 one compact table per kind
  docs/reference/import-report.md          what was found, missing, pulled in or dropped

RimWorld's proper names are then swapped for ours from the table in docs/design/naming.md.
Only the converted numbers and short labels are written. RimWorld's descriptions and raw XML are
never copied. Standard library only. See docs/design/rimworld-import.md.
"""
from __future__ import annotations

import argparse
import copy
import fnmatch
import json
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter, OrderedDict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import naming  # noqa: E402
import rimworld_import_spec as spec  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
IMPORT_SUFFIX = ".rimworld.json"


# --- Reading XML ----------------------------------------------------------------------------
@dataclass
class Node:
    cls: str                  # RimWorld class, the XML element name (ThingDef, TerrainDef...)
    el: ET.Element
    pack: str                 # Core, Royalty...
    path: Path
    name: Optional[str]       # the Name attribute (what ParentName points at)
    parent_name: Optional[str]
    abstract: bool

    @property
    def def_name(self) -> Optional[str]:
        el = self.el.find("defName")
        return el.text.strip() if el is not None and el.text else None


def find_defs_dir(root: Path, pack: str) -> Optional[Path]:
    """Accepts the install folder, its Data folder, or a pack's folder."""
    for candidate in (root / "Data" / pack / "Defs", root / pack / "Defs", root / "Defs"):
        if candidate.is_dir():
            return candidate
    return None


def load_nodes(defs_dir: Path, pack: str, warnings: List[str]) -> List[Node]:
    nodes: List[Node] = []
    for path in sorted(defs_dir.rglob("*.xml")):
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as err:
            warnings.append(f"{path}: unreadable XML ({err})")
            continue
        if root.tag != "Defs":
            continue
        for child in root:
            if not isinstance(child.tag, str):
                continue  # comment
            nodes.append(Node(
                cls=child.tag, el=child, pack=pack, path=path,
                name=child.get("Name"), parent_name=child.get("ParentName"),
                abstract=(child.get("Abstract") or "").lower() == "true",
            ))
    return nodes


# --- XML inheritance ------------------------------------------------------------------------
def _elems(el: ET.Element) -> List[ET.Element]:
    return [c for c in el if isinstance(c.tag, str)]


def _merge(parent: ET.Element, child: ET.Element) -> ET.Element:
    """RimWorld's rule: the child's fields win, objects merge field by field, and a list the
    child writes replaces the parent's list. `Inherit="False"` on a child field drops the
    parent's version of it."""
    out = ET.Element(child.tag, dict(child.attrib))
    out.text = child.text
    child_elems = _elems(child)
    parent_elems = _elems(parent)

    if any(c.tag == "li" for c in child_elems) or not parent_elems:
        for c in child_elems:
            out.append(copy.deepcopy(c))
        return out

    by_tag: Dict[str, ET.Element] = {}
    for c in child_elems:
        by_tag.setdefault(c.tag, c)
    used = set()
    for p in parent_elems:
        c = by_tag.get(p.tag)
        if c is None:
            out.append(copy.deepcopy(p))
            continue
        used.add(p.tag)
        if (c.get("Inherit") or "").lower() == "false" or not _elems(c):
            out.append(copy.deepcopy(c))
        else:
            out.append(_merge(p, c))
    for c in child_elems:
        if c.tag not in used:
            out.append(copy.deepcopy(c))
    return out


def resolve_inheritance(nodes: List[Node], warnings: List[str]) -> Dict[Tuple[str, str], Tuple[Node, ET.Element]]:
    """Returns (class, defName) -> (node, fully merged element) for every concrete def. A def in a
    later pack replaces the same def from an earlier one."""
    by_name: Dict[str, Node] = {}
    by_class_def: Dict[Tuple[str, str], Node] = {}
    for n in nodes:
        if n.name:
            by_name[n.name] = n
        if n.def_name:
            by_class_def[(n.cls, n.def_name)] = n

    memo: Dict[int, ET.Element] = {}

    def resolved(n: Node, stack: Tuple[int, ...]) -> ET.Element:
        key = id(n)
        if key in memo:
            return memo[key]
        if not n.parent_name:
            memo[key] = n.el
            return n.el
        if key in stack:
            warnings.append(f"{n.path.name}: inheritance cycle at {n.name or n.def_name}")
            memo[key] = n.el
            return n.el
        parent = by_name.get(n.parent_name) or by_class_def.get((n.cls, n.parent_name))
        if parent is None:
            warnings.append(f"{n.path.name}: {n.cls} {n.def_name or n.name} has unknown ParentName "
                            f"'{n.parent_name}'; imported without it")
            memo[key] = n.el
            return n.el
        merged = _merge(resolved(parent, stack + (key,)), n.el)
        merged.attrib.pop("Inherit", None)
        memo[key] = merged
        return merged

    out: Dict[Tuple[str, str], Tuple[Node, ET.Element]] = {}
    for n in nodes:
        if n.abstract or not n.def_name:
            continue
        out[(n.cls, n.def_name)] = (n, resolved(n, ()))
    return out


# --- XML to plain values --------------------------------------------------------------------
_INT = re.compile(r"^-?\d+$")
_FLOAT = re.compile(r"^-?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$")
_RANGE = re.compile(r"^\(?\s*(-?\d*\.?\d+)\s*~\s*(-?\d*\.?\d+)\s*\)?$")
_TUPLE = re.compile(r"^\(\s*-?[\d.]+(\s*,\s*-?[\d.]+)+\s*\)$")


def _num(text: str):
    if _INT.match(text):
        return int(text)
    return float(text)


def coerce(text: Optional[str], key: str = ""):
    if text is None:
        return None
    text = text.strip()
    if text == "":
        return None
    if key in spec.STRING_FIELDS:
        return text
    low = text.lower()
    if low == "true":
        return True
    if low == "false":
        return False
    if _INT.match(text) or _FLOAT.match(text):
        return _num(text)
    m = _RANGE.match(text)
    if m:
        return [_num(m.group(1)), _num(m.group(2))]
    if _TUPLE.match(text):
        return [_num(x.strip()) for x in text.strip("()").split(",")]
    return text


def lower_first(s: str) -> str:
    return s[:1].lower() + s[1:]


def rename_ticks(key: str) -> str:
    if "Ticks" in key:
        return key.replace("Ticks", "Seconds")
    if key.startswith("ticks"):
        return "seconds" + key[5:]
    return key


def tidy_number(x):
    if isinstance(x, bool) or not isinstance(x, (int, float)):
        return x
    r = round(float(x), 6)
    return int(r) if r == int(r) else r


class Converter:
    """Turns merged XML elements into plain JSON values, counting what it drops and converts."""

    def __init__(self, drops: Counter, conversions: Counter):
        self.drops = drops
        self.conversions = conversions

    def value(self, el: ET.Element, key: str, ctx: str, kind: str):
        children = [c for c in el if isinstance(c.tag, str)]
        if not children:
            return coerce(el.text, key)
        if all(c.tag == "li" for c in children):
            items = []
            for c in children:
                v = self.item(c, ctx if key == "li" else key, kind)
                if v is not None:
                    items.append(v)
            return items
        return self.mapping(el, key, kind, class_attr=el.get("Class"))

    def item(self, li: ET.Element, ctx: str, kind: str):
        kids = [c for c in li if isinstance(c.tag, str)]
        if not kids:
            if li.get("Class"):
                return OrderedDict(type=re.sub(r"^CompProperties_", "", li.get("Class")))
            return coerce(li.text, "li")
        if all(c.tag == "li" for c in kids):
            return self.value(li, "li", ctx, kind)
        return self.mapping(li, ctx, kind, class_attr=li.get("Class"))

    def mapping(self, el: ET.Element, ctx: str, kind: str, class_attr: Optional[str] = None):
        out: "OrderedDict[str, Any]" = OrderedDict()
        if class_attr:
            out["type"] = re.sub(r"^CompProperties_", "", class_attr)
        is_stat_dict = ctx in spec.STAT_DICTS
        for c in el:
            if not isinstance(c.tag, str):
                continue
            tag = c.tag
            if tag in spec.DROP_FIELDS:
                self.drops[(kind, tag)] += 1
                continue
            v = self.value(c, tag, ctx if tag == "li" else tag, kind)
            if v is None:
                continue
            unit = spec.UNITS.get((ctx, tag)) or spec.UNITS.get(("*", tag))
            if unit and isinstance(v, (int, float)) and not isinstance(v, bool):
                v = spec.UNIT_CONVERTERS[unit](v)
                self.conversions[(kind, tag, unit)] += 1
                name = rename_ticks(tag)
            else:
                name = tag
            if is_stat_dict:
                name = lower_first(name)
            else:
                name = spec.RENAMES.get(name, name)
            out[name] = self._tidy(v)
        return out

    def _tidy(self, v):
        if isinstance(v, list):
            return [self._tidy(x) for x in v]
        if isinstance(v, dict):
            return OrderedDict((k, self._tidy(x)) for k, x in v.items())
        return tidy_number(v)

    def convert_def(self, el: ET.Element, kind_spec: "spec.KindSpec", pack: str) -> "OrderedDict[str, Any]":
        flat = self.mapping(el, "", kind_spec.folder)
        out: "OrderedDict[str, Any]" = OrderedDict()
        out["defName"] = flat.pop("defName")
        if "label" in flat:
            out["label"] = flat.pop("label")
        out["source"] = pack
        for k, v in flat.items():
            k = kind_spec.rename_fields.get(k, k)
            if k in ("parent", "abstract"):
                k = "rw" + k[0].upper() + k[1:]   # these two words mean something to our own defs
            out[k] = v
        return out


# --- Skill gains come in two shapes in different RimWorld versions --------------------------
def normalise_backstory(d: dict) -> dict:
    gains = d.get("skillGains")
    if isinstance(gains, list):
        d["skillGains"] = OrderedDict(
            (g["def"], g.get("amount", 0)) for g in gains if isinstance(g, dict) and "def" in g)
    return d


NORMALISERS = {"Backstories": normalise_backstory}


# --- Selection ------------------------------------------------------------------------------
def select(kind_spec: "spec.KindSpec", resolved: Dict[Tuple[str, str], Tuple[Node, ET.Element]]) -> Tuple[List[str], List[str]]:
    """Returns (defNames chosen in order, patterns that matched nothing)."""
    available = sorted(n for (cls, n) in resolved if cls == kind_spec.rw_class)
    chosen: List[str] = []
    missing: List[str] = []
    for pattern in kind_spec.names:
        hits = [n for n in available if fnmatch.fnmatchcase(n, pattern)]
        if not hits:
            missing.append(pattern)
        for h in hits:
            if h not in chosen:
                chosen.append(h)
    return chosen, missing


def get_path(d: Any, dotted: str):
    for part in dotted.split("."):
        if isinstance(d, dict) and part in d:
            d = d[part]
        else:
            return None
    return d


# Fields that hold a dictionary keyed by def name (the rest of the reference fields hold names).
DICT_KEYED_FIELDS = {"costs", "products", "smeltProducts", "butcherProducts"}


def names_in(field_name: str, value: Any) -> List[str]:
    """The def names a reference field points at."""
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        return [v for v in value if isinstance(v, str)]
    if isinstance(value, dict) and field_name in DICT_KEYED_FIELDS:
        return list(value.keys())
    return []


def walk_fields(obj: Any, wanted: Iterable[str]) -> Iterable[Tuple[str, Any]]:
    wanted = set(wanted)
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k in wanted:
                yield k, v
            yield from walk_fields(v, wanted)
    elif isinstance(obj, list):
        for v in obj:
            yield from walk_fields(v, wanted)


# --- The import -----------------------------------------------------------------------------
@dataclass
class KindResult:
    spec: "spec.KindSpec"
    defs: List[dict]
    missing: List[str]
    dependencies: List[str]


@dataclass
class ImportResult:
    kinds: List[KindResult]
    packs: List[str]
    version: str
    warnings: List[str]
    drops: Counter
    conversions: Counter
    unresolved: Dict[str, List[str]]   # "Kind/defName.field" -> missing names
    renamed: Dict[str, str]            # RimWorld defName -> ours, for the names that occurred


def apply_renames(results: List[KindResult], renames: Dict[str, "naming.Rename"]) -> Dict[str, str]:
    """Swaps RimWorld's proper names for ours (docs/design/naming.md): a renamed def gets our
    defName and label, and every string or dictionary key elsewhere that is exactly a renamed
    defName follows it. Returns the renames that were used."""
    used: Dict[str, str] = {}

    def swap(name: str) -> str:
        r = renames.get(name)
        if r is None:
            return name
        used[name] = r.def_name
        return r.def_name

    def walk(obj: Any) -> Any:
        if isinstance(obj, dict):
            return OrderedDict((swap(k) if isinstance(k, str) else k, walk(v)) for k, v in obj.items())
        if isinstance(obj, list):
            return [walk(v) for v in obj]
        if isinstance(obj, str):
            return swap(obj)
        return obj

    for r in results:
        new_defs = []
        for d in r.defs:
            old = d.get("defName")
            d = walk(d)
            if old in renames and "label" in d:
                d["label"] = renames[old].label
            new_defs.append(d)
        r.defs = new_defs
    return used


def run_import(rimworld: Path, packs: List[str]) -> ImportResult:
    warnings: List[str] = []
    nodes: List[Node] = []
    used_packs: List[str] = []
    for pack in packs:
        defs_dir = find_defs_dir(rimworld, pack)
        if defs_dir is None:
            warnings.append(f"pack '{pack}': no Defs folder under {rimworld}")
            continue
        used_packs.append(pack)
        nodes += load_nodes(defs_dir, pack, warnings)
    resolved = resolve_inheritance(nodes, warnings)

    drops: Counter = Counter()
    conversions: Counter = Counter()
    conv = Converter(drops, conversions)

    results: List[KindResult] = []
    for ks in spec.KINDS:
        chosen, missing = select(ks, resolved)
        defs = []
        for name in chosen:
            node, el = resolved[(ks.rw_class, name)]
            d = conv.convert_def(el, ks, node.pack)
            normaliser = NORMALISERS.get(ks.folder)
            if normaliser:
                normaliser(d)
            if ks.where and not ks.where(d):
                continue
            defs.append(d)
        if ks.limit:
            field_name, n = ks.limit
            seen: Counter = Counter()
            kept = []
            for d in sorted(defs, key=lambda x: x["defName"]):
                if seen[d.get(field_name)] < n:
                    seen[d.get(field_name)] += 1
                    kept.append(d)
            defs = kept
        results.append(KindResult(ks, defs, missing, []))

    # Pull in the Things that imported defs depend on (projectiles, harvested items, ...).
    things = next(r for r in results if r.spec.folder == "Things")
    have = {d["defName"] for d in things.defs}
    things_spec = things.spec
    for _ in range(4):
        wanted: List[str] = []
        for r in results:
            for d in r.defs:
                for field_name, value in walk_fields(d, spec.FOLLOW_FIELDS):
                    for n in names_in(field_name, value):
                        if n not in have and n not in wanted and ("ThingDef", n) in resolved:
                            wanted.append(n)
        if not wanted:
            break
        for n in sorted(wanted):
            node, el = resolved[("ThingDef", n)]
            things.defs.append(conv.convert_def(el, things_spec, node.pack))
            things.dependencies.append(n)
            have.add(n)

    renamed = apply_renames(results, naming.load_renames())
    for r in results:
        r.defs.sort(key=lambda d: d["defName"])

    # References to defs that are not in the output.
    out_names = {r.spec.folder: {d["defName"] for d in r.defs} for r in results}
    unresolved: Dict[str, List[str]] = {}
    for r in results:
        for d in r.defs:
            for field_name, value in walk_fields(d, spec.REFERENCE_FIELDS):
                target = spec.REFERENCE_FIELDS[field_name]
                bad = sorted({n for n in names_in(field_name, value) if n not in out_names.get(target, set())})
                if bad:
                    unresolved.setdefault(f"{r.spec.folder}/{d['defName']}.{field_name}", []).extend(bad)

    version = ""
    for candidate in (rimworld / "Version.txt", rimworld.parent / "Version.txt"):
        if candidate.is_file():
            version = candidate.read_text(encoding="utf-8", errors="replace").strip().splitlines()[0]
            break
    return ImportResult(results, used_packs, version, warnings, drops, conversions, unresolved, renamed)


# --- Output ---------------------------------------------------------------------------------
def dumps(defs: List[dict]) -> str:
    return json.dumps(defs, indent=2, ensure_ascii=False) + "\n"


def brief(value: Any, limit: int = 60) -> str:
    if isinstance(value, dict):
        text = ", ".join(f"{k}:{brief(v, 20)}" for k, v in list(value.items())[:4])
    elif isinstance(value, list):
        text = "[" + ", ".join(brief(v, 20) for v in value[:3]) + (", ..." if len(value) > 3 else "") + "]"
    else:
        text = str(value)
    return text if len(text) <= limit else text[: limit - 1] + "…"


def reference_markdown(r: KindResult, result: ImportResult) -> str:
    ks = r.spec
    lines = [
        f"# {ks.title or ks.folder}",
        "",
        f"_Generated by `tools/rimworld_import.py` from RimWorld {', '.join(result.packs)}"
        f"{' ' + result.version if result.version else ''} (`{ks.rw_class}`). Do not edit by hand._",
        "",
        f"{len(r.defs)} defs, in `data/defs/{ks.folder}/{ks.folder}{IMPORT_SUFFIX}`. Durations are game seconds, "
        "speeds are metres per game second (see `docs/reference/README.md`).",
        "",
    ]
    cols = ks.summary
    lines.append("| defName | label | " + " | ".join(cols) + " |")
    lines.append("| --- | --- | " + " | ".join("---" for _ in cols) + " |")
    for d in r.defs:
        cells = []
        for c in cols:
            v = get_path(d, c)
            cells.append("" if v is None else brief(v).replace("|", "\\|"))
        label = str(d.get("label", "")).replace("|", "\\|")
        lines.append(f"| {d['defName']} | {label} | " + " | ".join(cells) + " |")
    lines.append("")
    return "\n".join(lines)


def report_markdown(result: ImportResult) -> str:
    L = ["# Import report", "",
         f"_Generated by `tools/rimworld_import.py`. Packs: {', '.join(result.packs) or 'none'}"
         f"{'. RimWorld ' + result.version if result.version else ''}._", ""]
    L += ["## Counts", "", "| kind | defs | not found | pulled in as dependencies |", "| --- | --- | --- | --- |"]
    for r in result.kinds:
        L.append(f"| {r.spec.folder} | {len(r.defs)} | {len(r.missing) or ''} | {', '.join(r.dependencies) or ''} |")
    L.append("")
    L += ["## Names or patterns that matched nothing", ""]
    anything = False
    for r in result.kinds:
        for m in r.missing:
            L.append(f"- {r.spec.folder}: `{m}`")
            anything = True
    if not anything:
        L.append("None.")
    L.append("")
    L += ["## References to defs that are not in the output", ""]
    if result.unresolved:
        for k in sorted(result.unresolved):
            L.append(f"- `{k}` -> {', '.join(sorted(set(result.unresolved[k])))}")
    else:
        L.append("None.")
    L.append("")
    L += ["## Renamed to our names (docs/design/naming.md)", ""]
    L += [f"- `{k}` -> `{v}`" for k, v in sorted(result.renamed.items())] or ["None."]
    L.append("")
    L += ["## Fields dropped (presentation and prose)", ""]
    if result.drops:
        per_kind: Dict[str, List[str]] = {}
        for (kind, field_name), n in sorted(result.drops.items()):
            per_kind.setdefault(kind, []).append(f"{field_name} x{n}")
        for kind in sorted(per_kind):
            L.append(f"- {kind}: {', '.join(per_kind[kind])}")
    else:
        L.append("None.")
    L.append("")
    L += ["## Numbers converted", "", "| kind | field | conversion | count |", "| --- | --- | --- | --- |"]
    for (kind, field_name, unit), n in sorted(result.conversions.items()):
        L.append(f"| {kind} | {field_name} | {unit} | {n} |")
    L.append("")
    L += ["## Warnings", ""]
    L += [f"- {w}" for w in result.warnings] or ["None."]
    L.append("")
    return "\n".join(L)


def check_collisions(result: ImportResult, out_defs: Path) -> List[str]:
    """Hand-written defs in the same kind folder must not share a defName with imported ones."""
    problems = []
    for r in result.kinds:
        folder = out_defs / r.spec.folder
        if not folder.is_dir():
            continue
        imported = {d["defName"] for d in r.defs}
        for path in sorted(folder.glob("*.json")):
            if path.name.endswith(IMPORT_SUFFIX):
                continue
            try:
                entries = json.loads(path.read_text(encoding="utf-8"))
            except (json.JSONDecodeError, OSError):
                continue
            for e in entries if isinstance(entries, list) else []:
                if isinstance(e, dict) and e.get("defName") in imported:
                    problems.append(f"{path}: defines '{e['defName']}', which the import also writes. "
                                    f"Delete or rename the hand-written one.")
    return problems


def write_outputs(result: ImportResult, out_defs: Path, out_ref: Path) -> List[Path]:
    written = []
    out_ref.mkdir(parents=True, exist_ok=True)
    for r in result.kinds:
        folder = out_defs / r.spec.folder
        folder.mkdir(parents=True, exist_ok=True)
        target = folder / f"{r.spec.folder}{IMPORT_SUFFIX}"
        target.write_text(dumps(r.defs), encoding="utf-8")
        written.append(target)
        ref = out_ref / f"{r.spec.folder}.md"
        ref.write_text(reference_markdown(r, result), encoding="utf-8")
        written.append(ref)
    report = out_ref / "import-report.md"
    report.write_text(report_markdown(result), encoding="utf-8")
    written.append(report)
    return written


# --- Command line ---------------------------------------------------------------------------
def find_command(rimworld: Path, packs: List[str], pattern: str, cls: Optional[str]) -> int:
    warnings: List[str] = []
    nodes: List[Node] = []
    for pack in packs:
        d = find_defs_dir(rimworld, pack)
        if d:
            nodes += load_nodes(d, pack, warnings)
    resolved = resolve_inheritance(nodes, warnings)
    rx = re.compile(pattern, re.IGNORECASE)
    hits = sorted((c, n) for (c, n) in resolved if rx.search(n) and (cls is None or c == cls))
    for c, n in hits:
        print(f"{c}\t{n}")
    print(f"{len(hits)} match(es)")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Import RimWorld Defs XML as Rim3D JSON defs.")
    ap.add_argument("--rimworld", required=True, type=Path,
                    help="RimWorld install folder (the one that contains Data/Core/Defs)")
    ap.add_argument("--packs", nargs="+", default=["Core"], help="Core, Royalty, Ideology... in load order")
    ap.add_argument("--out-defs", type=Path, default=REPO / "data" / "defs")
    ap.add_argument("--out-ref", type=Path, default=REPO / "docs" / "reference")
    ap.add_argument("--dry-run", action="store_true", help="read and report, write nothing")
    ap.add_argument("--strict", action="store_true", help="exit 1 if any listed name matched nothing")
    ap.add_argument("--find", metavar="REGEX", help="list defs whose name matches, then exit")
    ap.add_argument("--class", dest="cls", help="with --find: only this class, e.g. ThingDef")
    args = ap.parse_args(argv)

    if args.find:
        return find_command(args.rimworld, args.packs, args.find, args.cls)

    result = run_import(args.rimworld, args.packs)
    if not result.packs:
        print("No RimWorld Defs folder found. Pass the install folder with --rimworld.", file=sys.stderr)
        return 2
    problems = check_collisions(result, args.out_defs)
    for p in problems:
        print("ERROR " + p, file=sys.stderr)
    if problems:
        return 2

    for w in result.warnings:
        print("warning: " + w)
    total = 0
    for r in result.kinds:
        total += len(r.defs)
        extra = f", pulled in: {', '.join(r.dependencies)}" if r.dependencies else ""
        miss = f", NOT FOUND: {', '.join(r.missing)}" if r.missing else ""
        print(f"{r.spec.folder:16} {len(r.defs):4} defs{extra}{miss}")
    print(f"{total} defs from {', '.join(result.packs)}; {len(result.unresolved)} field(s) refer to defs not imported")
    if not args.dry_run:
        written = write_outputs(result, args.out_defs, args.out_ref)
        print(f"wrote {len(written)} files; see {args.out_ref / 'import-report.md'}")
    missing_any = any(r.missing for r in result.kinds)
    return 1 if (args.strict and missing_any) else 0


if __name__ == "__main__":
    sys.exit(main())

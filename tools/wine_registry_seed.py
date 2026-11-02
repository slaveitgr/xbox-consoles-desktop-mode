#!/usr/bin/env python3
"""Build the guest registry seed for stock Wine modules, without running Wine.

On a normal prefix, wineboot installs wine.inf (AddReg/DelReg) and calls
DllRegisterServer for every [RegisterDllsSection] module; most of those just
replay their WINE_REGISTRY (.rgs) resources through atl100's registrar. The
Xbox bridge cannot run wineboot before its own registry exists, so this tool
replays both offline and emits JSON for GuestRegistry seeding:

    wine_registry_seed.py --inf <build>/loader/wine.inf --arch ntamd64 \
        --dll-dir <xcdm-build-output> [--wine-source <wine>] \
        --require-clsid 71f96385-ddd6-48d3-a0c1-ae06e8b055fb \
        --output registry-seed.json

It also accepts raw scripts with --rgs FILE=module.dll (tests, debugging).

The rgs grammar and semantics mirror Wine dlls/atl/registrar.c (register
direction): NoRemove / ForceRemove / val / Delete, '=' s|d|b values,
%MODULE% / %SystemRoot% / %% replacement. HKCR writes are stored under
HKLM\\Software\\Classes, which is where Wine's registrar lands for the
machine scope. INF AddReg supports the FLG_ADDREG flags wine.inf uses
(type word, NOCLOBBER, APPEND, KEYONLY, DELVAL), [Strings] and DIRID
substitution, Needs= and architecture-decorated sections.

Limits, reported as warnings rather than hidden: DllRegisterServer code that
does more than replay resources (detected with --wine-source), typelib
registration, and services/fake-dll/file-copy directives are not replayed.

Exit status: 0 on success, 1 if a script fails to parse or a required CLSID
is not registered with an InprocServer32, 2 on usage errors.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

WINDOWS = "C:\\windows"
SYSTEM32 = WINDOWS + "\\system32"
DIRIDS = {
    "10": WINDOWS, "11": SYSTEM32, "12": SYSTEM32 + "\\drivers", "17": WINDOWS + "\\inf",
    "18": WINDOWS + "\\help", "20": WINDOWS + "\\fonts", "24": "C:\\", "25": WINDOWS,
    "30": "C:\\", "16422": "C:\\Program Files", "16426": "C:\\Program Files (x86)",
    "16427": "C:\\Program Files\\Common Files", "16428": "C:\\Program Files (x86)\\Common Files",
}
ENV_TOKENS = {"systemroot", "windir", "systemdrive", "programfiles", "programfiles(x86)",
              "commonprogramfiles", "userprofile", "appdata", "localappdata", "temp", "tmp",
              "allusersprofile", "programdata", "public", "path"}
ROOTS = {
    "hkey_classes_root": "HKCR", "hkcr": "HKCR", "hkey_current_user": "HKCU", "hkcu": "HKCU",
    "hkey_local_machine": "HKLM", "hklm": "HKLM", "hkey_users": "HKU", "hku": "HKU",
    "hkey_current_config": "HKCC", "hkcc": "HKCC", "hkey_performance_data": "HKPD", "hkpd": "HKPD",
    "hkey_dyn_data": "HKDD", "hkdd": "HKDD",
}
REG_SZ, REG_EXPAND_SZ, REG_BINARY, REG_DWORD, REG_MULTI_SZ, REG_QWORD, REG_NONE = 1, 2, 3, 4, 7, 11, 0
TYPE_NAMES = {0: "REG_NONE", 1: "REG_SZ", 2: "REG_EXPAND_SZ", 3: "REG_BINARY", 4: "REG_DWORD",
              7: "REG_MULTI_SZ", 11: "REG_QWORD"}


class ScriptError(Exception):
    pass


# --------------------------------------------------------------- registry tree

class Key:
    __slots__ = ("name", "children", "values")

    def __init__(self, name: str):
        self.name = name
        self.children: dict[str, Key] = {}
        self.values: dict[str, tuple[str, int, object]] = {}


class Registry:
    def __init__(self) -> None:
        self.roots = {name: Key(name) for name in ("HKLM", "HKCU", "HKU", "HKCC", "HKPD", "HKDD")}

    @staticmethod
    def _split(root: str, path: str) -> tuple[str, list[str]]:
        root = ROOTS.get(root.lower(), root.upper())
        parts = [p for p in path.replace("/", "\\").split("\\") if p]
        if root == "HKCR":
            return "HKLM", ["Software", "Classes"] + parts
        if root not in ("HKLM", "HKCU", "HKU", "HKCC", "HKPD", "HKDD"):
            raise ScriptError(f"unknown root key {root}")
        return root, parts

    def open(self, root: str, path: str, create: bool) -> Key | None:
        root, parts = self._split(root, path)
        key = self.roots[root]
        for part in parts:
            child = key.children.get(part.lower())
            if child is None:
                if not create:
                    return None
                child = key.children[part.lower()] = Key(part)
            key = child
        return key

    def delete_tree(self, root: str, path: str) -> None:
        root, parts = self._split(root, path)
        if not parts:
            return
        key = self.roots[root]
        for part in parts[:-1]:
            key = key.children.get(part.lower())
            if key is None:
                return
        key.children.pop(parts[-1].lower(), None)

    def export(self) -> dict:
        out: dict[str, dict] = {}

        def walk(key: Key, path: str) -> None:
            if key.values or not key.children:
                out[path] = {name: {"type": TYPE_NAMES.get(t, str(t)), "data": data}
                             for _, (name, t, data) in sorted(key.values.items())}
            for _, child in sorted(key.children.items()):
                walk(child, path + "\\" + child.name)

        for name, root in self.roots.items():
            for _, child in sorted(root.children.items()):
                walk(child, name + "\\" + child.name)
        return out


def set_value(key: Key, name: str, kind: int, data: object) -> None:
    key.values[name.lower()] = (name, kind, data)


# ---------------------------------------------------------------- rgs (atl)

def rgs_preprocess(text: str, replacements: dict[str, str]) -> str:
    out = []
    i = 0
    while True:
        j = text.find("%", i)
        if j < 0:
            out.append(text[i:])
            return "".join(out)
        out.append(text[i:j])
        k = text.find("%", j + 1)
        if k < 0:
            raise ScriptError("unterminated %replacement%")
        token = text[j + 1:k]
        if token == "":
            out.append("%")
        else:
            value = next((v for n, v in replacements.items() if n.lower() == token.lower()), None)
            if value is None:
                raise ScriptError(f"no replacement for %{token}%")
            out.append(value)
        i = k + 1


class RgsLexer:
    def __init__(self, text: str):
        self.s = text
        self.i = 0

    def peek_char(self) -> str:
        return self.s[self.i] if self.i < len(self.s) else ""

    def word(self) -> str:
        s, n = self.s, len(self.s)
        while self.i < n and s[self.i].isspace():
            self.i += 1
        if self.i >= n:
            return ""
        c = s[self.i]
        if c in "}=":
            self.i += 1
            w = c
        elif c == "'":
            parts = []
            while True:
                start = self.i + 1
                end = s.find("'", start)
                if end < 0:
                    raise ScriptError("unexpected end of script in quoted string")
                parts.append(s[start:end])
                if end + 1 < n and s[end + 1] == "'":
                    parts.append("'")
                    self.i = end + 1
                    continue
                self.i = end + 1
                break
            w = "".join(parts)
        else:
            start = self.i
            while self.i < n and not s[self.i].isspace():
                self.i += 1
            w = s[start:self.i]
        while self.i < n and s[self.i].isspace():
            self.i += 1
        return w

    def at_block(self) -> bool:
        # registrar.c: *iter == '{' && iswspace(iter[1])
        return self.peek_char() == "{" and self.i + 1 < len(self.s) and self.s[self.i + 1].isspace()


def rgs_process_key(lex: RgsLexer, reg: Registry, root: str, path: str) -> None:
    word = lex.word()
    while word != "}":
        if word == "":
            raise ScriptError("unexpected end of script inside a key block")
        kind = "normal"
        lw = word.lower()
        if lw in ("noremove", "forceremove", "val", "delete"):
            kind = {"noremove": "noremove", "forceremove": "force", "val": "val", "delete": "delete"}[lw]
            word = lex.word()
        name = word
        sub = path + "\\" + name if path else name
        key = None
        if kind == "val":
            key = reg.open(root, path, create=True)
        elif kind == "delete":
            reg.delete_tree(root, sub)
        else:
            if kind == "force":
                reg.delete_tree(root, sub)
            key = reg.open(root, sub, create=True)

        if kind != "delete" and lex.peek_char() == "=":
            lex.i += 1
            vtype = lex.word()
            if len(vtype) != 1:
                raise ScriptError(f"wrong registry type {vtype!r}")
            data = lex.word()
            value_name = name if kind == "val" else ""
            if vtype == "s":
                set_value(key, value_name, REG_SZ, data)
            elif vtype == "d":
                m = re.match(r"\s*(\d*)", data)
                set_value(key, value_name, REG_DWORD, int(m.group(1) or 0) & 0xFFFFFFFF)
            elif vtype == "b":
                if len(data) % 2 or not re.fullmatch(r"[0-9a-fA-F]*", data):
                    raise ScriptError(f"bad binary value {data!r}")
                set_value(key, value_name, REG_BINARY, data.lower())
            else:
                raise ScriptError(f"wrong resource type {vtype!r}")
        elif kind == "val":
            raise ScriptError(f"value {name!r} not set")

        if kind not in ("val", "delete") and lex.at_block():
            lex.word()  # '{'
            rgs_process_key(lex, reg, root, sub)
        word = lex.word()


def apply_rgs(reg: Registry, text: str, replacements: dict[str, str]) -> None:
    lex = RgsLexer(rgs_preprocess(text, replacements))
    word = lex.word()
    while word:
        root = ROOTS.get(word.lower())
        if not root:
            raise ScriptError(f"wrong root key name {word!r}")
        if lex.word() != "{":
            raise ScriptError("expected '{' after root key")
        rgs_process_key(lex, reg, root, "")
        word = lex.word()


# ---------------------------------------------------------------- INF

def inf_parse(text: str) -> dict[str, list[list[str] | tuple[str, list[str]]]]:
    sections: dict[str, list] = {}
    current = None
    logical: list[str] = []
    pending = ""
    for raw in text.splitlines():
        line = pending + raw
        pending = ""
        stripped = strip_comment(line).rstrip()
        if stripped.endswith("\\"):
            pending = stripped[:-1]
            continue
        logical.append(stripped)
    if pending:
        logical.append(pending)
    for line in logical:
        s = line.strip()
        if not s:
            continue
        if s.startswith("[") and s.endswith("]"):
            current = s[1:-1].strip().lower()
            sections.setdefault(current, [])
            continue
        if current is None:
            continue
        key, fields = split_entry(s)
        sections[current].append((key, fields) if key is not None else fields)
    return sections


def strip_comment(line: str) -> str:
    quoted = False
    for i, c in enumerate(line):
        if c == '"':
            quoted = not quoted
        elif c == ";" and not quoted:
            return line[:i]
    return line


def split_fields(s: str) -> list[str]:
    fields, cur, quoted, had_quotes = [], [], False, False
    i = 0
    while i < len(s):
        c = s[i]
        if c == '"':
            if quoted and i + 1 < len(s) and s[i + 1] == '"':
                cur.append('"')
                i += 2
                continue
            quoted = not quoted
            had_quotes = True
        elif c == "," and not quoted:
            fields.append("".join(cur) if had_quotes else "".join(cur).strip())
            cur, had_quotes = [], False
        else:
            cur.append(c)
        i += 1
    fields.append("".join(cur) if had_quotes else "".join(cur).strip())
    return fields


def split_entry(s: str) -> tuple[str | None, list[str]]:
    quoted = False
    for i, c in enumerate(s):
        if c == '"':
            quoted = not quoted
        elif c == "=" and not quoted:
            return s[:i].strip().lower(), split_fields(s[i + 1:])
        elif c == "," and not quoted:
            break
    return None, split_fields(s)


class Inf:
    def __init__(self, text: str, warnings: list[str]):
        self.sections = inf_parse(text)
        self.warnings = warnings
        self.strings = {}
        for entry in self.sections.get("strings", []):
            if isinstance(entry, tuple):
                self.strings[entry[0]] = entry[1][0] if entry[1] else ""
        if "@" in text and re.search(r"@[A-Z_]+@", text):
            warnings.append("INF still contains @PLACEHOLDER@ tokens; use the generated build/loader/wine.inf")

    def subst(self, value: str) -> str:
        def repl(m: re.Match) -> str:
            token = m.group(1)
            if token == "":
                return "%"
            if token in DIRIDS:
                return DIRIDS[token]
            if token.lower() in self.strings:
                return self.strings[token.lower()]
            # Like setupapi, unknown tokens stay literal: REG_EXPAND_SZ data
            # uses environment references such as %SystemRoot% on purpose.
            if token.isdigit():
                note = f"unknown DIRID %{token}% kept literally"
            elif token.lower() in ENV_TOKENS or not re.fullmatch(r"\w+", token):
                # Environment references and %1/%*-style command arguments.
                return m.group(0)
            else:
                note = f"unknown string %{token}% kept literally"
            if note not in self.warnings:
                self.warnings.append(note)
            return m.group(0)
        return re.sub(r"%([^%]*)%", repl, value)

    def section(self, name: str, arch: str) -> tuple[str, list] | None:
        for candidate in (f"{name}.{arch}".lower(), name.lower()):
            if candidate in self.sections:
                return candidate, self.sections[candidate]
        return None

    def directives(self, install: str, arch: str, seen: set[str] | None = None) -> dict[str, list[str]]:
        seen = seen if seen is not None else set()
        found = self.section(install, arch)
        out: dict[str, list[str]] = {"addreg": [], "delreg": [], "registerdlls": []}
        if not found or found[0] in seen:
            if not found:
                self.warnings.append(f"install section {install} not found")
            return out
        seen.add(found[0])
        entries = found[1]
        for entry in entries:
            if isinstance(entry, tuple) and entry[0] == "needs":
                for need in entry[1]:
                    sub = self.directives(need.strip(), arch, seen)
                    for k in out:
                        out[k] += sub[k]
        for entry in entries:
            if isinstance(entry, tuple) and entry[0] in out:
                out[entry[0]] += [f.strip() for f in entry[1] if f.strip()]
        return out


FLG_BINVALUETYPE, FLG_NOCLOBBER, FLG_DELVAL, FLG_APPEND, FLG_KEYONLY, FLG_OVERWRITEONLY = 1, 2, 4, 8, 0x10, 0x20


REG_LINK = 6
TYPE_NAMES[REG_LINK] = "REG_LINK"


def inf_value(flags: int, data: list[str], warnings: list[str], where: str):
    """Mirror Wine setupapi do_reg_operation(): FLG_ADDREG_TYPE_* or the high
    word as a raw registry type; text data unless BINVALUETYPE is set (a
    single-field REG_DWORD is always parsed as an integer)."""
    masked = flags & (0xFFFF0000 | FLG_BINVALUETYPE)
    kind = {0x00000000: REG_SZ, 0x00010000: REG_MULTI_SZ, 0x00020000: REG_EXPAND_SZ,
            0x00000001: REG_BINARY, 0x00010001: REG_DWORD, 0x00020001: REG_NONE}.get(masked, flags >> 16)
    if kind == REG_NONE:
        return REG_NONE, None
    if not flags & FLG_BINVALUETYPE or (kind == REG_DWORD and len(data) == 1):
        if kind == REG_MULTI_SZ:
            return kind, [d for d in data if d != ""]
        if kind == REG_DWORD:
            text = data[0].strip() if data else "0"
            try:
                return REG_DWORD, int(text or "0", 0) & 0xFFFFFFFF
            except ValueError:
                raise ScriptError(f"{where}: bad DWORD {text!r}") from None
        if kind not in (REG_SZ, REG_EXPAND_SZ, REG_LINK):
            warnings.append(f"{where}: text data for registry type {kind}")
        return kind, data[0] if data else ""
    try:
        raw = bytes(int(b, 16) for b in data if b.strip() != "")
    except ValueError:
        raise ScriptError(f"{where}: bad binary bytes {data}") from None
    if kind == REG_DWORD:
        return REG_DWORD, int.from_bytes(raw[:4].ljust(4, b"\0"), "little")
    if kind == REG_QWORD:
        return REG_QWORD, int.from_bytes(raw[:8].ljust(8, b"\0"), "little")
    return kind, raw.hex()


def apply_inf(reg: Registry, inf: Inf, install: str, arch: str) -> list[str]:
    plan = inf.directives(install, arch)
    for section_name in plan["delreg"]:
        found = inf.section(section_name, arch)
        for entry in (found[1] if found else []):
            if isinstance(entry, tuple):
                continue
            f = [inf.subst(x) for x in entry] + ["", "", ""]
            if f[2] == "":
                reg.delete_tree(f[0], f[1])
            else:
                key = reg.open(f[0], f[1], create=False)
                if key:
                    key.values.pop(f[2].lower(), None)
    for section_name in plan["addreg"]:
        found = inf.section(section_name, arch)
        if not found:
            inf.warnings.append(f"AddReg section {section_name} not found")
            continue
        for number, entry in enumerate(found[1], 1):
            if isinstance(entry, tuple):
                continue
            f = [inf.subst(x) for x in entry]
            if len(f) < 2:
                continue
            root, path = f[0], f[1]
            name = f[2] if len(f) > 2 else ""
            flags_text = f[3].strip() if len(f) > 3 and f[3].strip() else "0"
            try:
                flags = int(flags_text, 0)
            except ValueError:
                raise ScriptError(f"[{found[0]}] line {number}: bad flags {flags_text!r}") from None
            data = f[4:]
            if flags & FLG_KEYONLY:
                reg.open(root, path, create=True)
                continue
            if flags & FLG_DELVAL:
                key = reg.open(root, path, create=False)
                if key:
                    key.values.pop(name.lower(), None)
                continue
            existing = reg.open(root, path, create=False)
            if flags & FLG_OVERWRITEONLY and (not existing or name.lower() not in existing.values):
                continue
            key = reg.open(root, path, create=True)
            if flags & FLG_NOCLOBBER and name.lower() in key.values:
                continue
            kind, value = inf_value(flags, data, inf.warnings, f"[{found[0]}] line {number}")
            if flags & FLG_APPEND and kind == REG_MULTI_SZ and name.lower() in key.values:
                old = key.values[name.lower()][2]
                value = list(old) + [v for v in value if v not in old]
            set_value(key, name, kind, value)
    registered = []
    for section_name in plan["registerdlls"]:
        found = inf.section(section_name, arch)
        for entry in (found[1] if found else []):
            if isinstance(entry, tuple):
                continue
            if len(entry) >= 3 and entry[2]:
                registered.append(entry[2].strip().lower())
    return registered


# ---------------------------------------------------------------- modules

def wine_registry_scripts(path: Path) -> list[str]:
    import pefile  # noqa: PLC0415
    pe = pefile.PE(str(path), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    scripts = []
    root = getattr(pe, "DIRECTORY_ENTRY_RESOURCE", None)
    for type_entry in getattr(root, "entries", []) or []:
        if str(type_entry.name) != "WINE_REGISTRY":
            continue
        for name_entry in type_entry.directory.entries:
            for lang in name_entry.directory.entries:
                data = pe.get_data(lang.data.struct.OffsetToData, lang.data.struct.Size)
                scripts.append(data.decode("utf-8", "replace").rstrip("\0"))
    return scripts


def custom_registration(wine: Path, module: str) -> str | None:
    stem = module.rsplit(".", 1)[0]
    base = next((d for d in (wine / "dlls" / stem, wine / "programs" / stem) if d.is_dir()), None)
    if not base:
        return None
    for path in base.glob("*.c"):
        text = path.read_text(encoding="utf-8", errors="ignore")
        m = re.search(r"HRESULT\s+WINAPI\s+DllRegisterServer\s*\(\s*void\s*\)\s*\{(.*?)\n\}", text, re.S)
        if m:
            body = re.sub(r"/\*.*?\*/|//[^\n]*", "", m.group(1), flags=re.S)
            calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", body)) - {
                "__wine_register_resources", "TRACE", "FIXME", "WARN", "ERR", "FAILED", "SUCCEEDED"}
            if calls:
                return f"{path.name}: DllRegisterServer also calls {', '.join(sorted(calls))}"
            return None
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inf", type=Path, help="generated wine.inf")
    ap.add_argument("--install", default="DefaultInstall")
    ap.add_argument("--arch", default="ntamd64")
    ap.add_argument("--dll-dir", type=Path, help="directory with built PE modules")
    ap.add_argument("--dll", action="append", default=[], help="extra module to register (name)")
    ap.add_argument("--rgs", action="append", default=[], help="FILE=module.dll raw script")
    ap.add_argument("--wine-source", type=Path)
    ap.add_argument("--require-clsid", action="append", default=[])
    ap.add_argument("--output", type=Path)
    args = ap.parse_args()
    if not (args.inf or args.dll_dir or args.rgs):
        ap.error("nothing to do: give --inf, --dll-dir or --rgs")

    reg = Registry()
    warnings: list[str] = []
    registered: list[dict] = []
    order: list[str] = []
    try:
        if args.inf:
            inf = Inf(args.inf.read_text(encoding="utf-8", errors="replace"), warnings)
            order = apply_inf(reg, inf, args.install, args.arch)
        order += [d.lower() for d in args.dll]
        if args.dll_dir:
            available = {p.name.lower(): p for p in args.dll_dir.rglob("*") if p.is_file()}
            if not args.inf and not args.dll:
                order = sorted(n for n in available if n.endswith((".dll", ".exe", ".ocx", ".ax")))
            for module in dict.fromkeys(order):
                path = available.get(module)
                if not path:
                    continue
                scripts = wine_registry_scripts(path)
                replacements = {"MODULE": f"{SYSTEM32}\\{module}", "SystemRoot": WINDOWS}
                for script in scripts:
                    apply_rgs(reg, script, replacements)
                note = custom_registration(args.wine_source, module) if args.wine_source else None
                if note:
                    warnings.append(f"{module}: custom registration not replayed ({note})")
                registered.append({"module": module, "scripts": len(scripts)})
            missing = [m for m in dict.fromkeys(order) if m not in available]
            if missing:
                warnings.append(f"{len(missing)} RegisterDlls module(s) not in --dll-dir: "
                                + ", ".join(missing[:20]) + (" ..." if len(missing) > 20 else ""))
        for item in args.rgs:
            file, _, module = item.partition("=")
            if not module:
                ap.error("--rgs expects FILE=module.dll")
            apply_rgs(reg, Path(file).read_text(encoding="utf-8"),
                      {"MODULE": f"{SYSTEM32}\\{module.lower()}", "SystemRoot": WINDOWS})
            registered.append({"module": module.lower(), "scripts": 1})
    except ScriptError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    failures = 0
    for clsid in args.require_clsid:
        guid = clsid.strip("{}").upper()
        key = reg.open("HKCR", f"CLSID\\{{{guid}}}\\InprocServer32", create=False)
        default = key.values.get("") if key else None
        if not default or not default[2]:
            print(f"FAIL: CLSID {{{guid}}} has no InprocServer32")
            failures += 1
        else:
            print(f"ok: CLSID {{{guid}}} -> {default[2]}")

    seed = {"format": "xcdm-registry-seed/1", "keys": reg.export(), "registered": registered, "warnings": warnings}
    if args.output:
        args.output.write_text(json.dumps(seed, indent=1, ensure_ascii=False), encoding="utf-8")
    for w in warnings:
        print(f"warning: {w}")
    print(f"registry seed: {len(seed['keys'])} keys, {len(registered)} modules registered, "
          f"{len(warnings)} warning(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Bounded, metadata-only AMD64 PE32+ dependency inventory.

No OS image loader, executable mapping, guest code, or external command is used.
This inventories declarations, not a complete dynamically loaded closure and not
loader/ABI compatibility. A successful parse is NOT permission to execute.

Layout reference: https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_SECTIONS = 96
MAX_DIRECTORIES = 128
MAX_DESCRIPTORS = 4096
MAX_SYMBOLS = 262144
MAX_STRING_BYTES = 16384
SAFETY = "NOT SAFE TO EXECUTE"
DYNAMIC_UNKNOWN = {
    "status": "unknown",
    "reason": "Runtime LoadLibrary/GetProcAddress, COM, plugins and other computed dependencies are not determined by PE declarations.",
}


class PEFormatError(ValueError):
    """Invalid, ambiguous, unsupported, or over-budget PE metadata."""


def _require(condition, message):
    if not condition:
        raise PEFormatError(message)


def _module_key(name):
    # Preserve the declaration separately; this key is for case-insensitive
    # dependency comparison only and does not perform filesystem resolution.
    key = name.replace("\\", "/").rsplit("/", 1)[-1].lower()
    _require(bool(key), "empty dependency library")
    return key if "." in key else key + ".dll"


class _PE:
    def __init__(self, data):
        self.data = data
        self.symbol_budget = MAX_SYMBOLS
        _require(self.raw(0, 2) == b"MZ", "missing DOS MZ signature")
        pe = self.unpack("<I", 0x3c)[0]
        _require(pe >= 0x40, "PE header overlaps DOS header")
        _require(self.raw(pe, 4) == b"PE\0\0", "missing PE signature")
        machine, count, _, _, _, optional_size, _ = self.unpack("<HHIIIHH", pe + 4)
        _require(machine == 0x8664, "only AMD64 machine 0x8664 is supported")
        _require(1 <= count <= MAX_SECTIONS, "invalid or excessive section count")
        optional = pe + 24
        self.raw(optional, optional_size)
        _require(optional_size >= 112, "truncated PE32+ optional header")
        _require(self.unpack("<H", optional)[0] == 0x20b, "only PE32+ optional headers are supported")
        self.image_base = self.unpack("<Q", optional + 24)[0]
        self.image_size, self.headers_size = self.unpack("<II", optional + 56)
        _require(self.image_size > 0, "empty image address space")
        _require(self.image_base + self.image_size <= 1 << 64, "image VA range overflows 64 bits")
        directory_count = self.unpack("<I", optional + 108)[0]
        _require(directory_count <= MAX_DIRECTORIES, "excessive data directory count")
        _require(112 + directory_count * 8 <= optional_size, "data directories exceed optional header")
        self.directories = [self.unpack("<II", optional + 112 + i * 8) for i in range(directory_count)]
        table = optional + optional_size
        table_end = table + count * 40
        self.raw(table, count * 40)
        _require(table_end <= self.headers_size <= len(data), "SizeOfHeaders does not contain the headers in raw file")
        _require(self.headers_size <= self.image_size, "headers exceed image address space")
        self.sections = []
        raw_ranges = [(0, self.headers_size)]
        virtual_ranges = [(0, self.headers_size)]
        for i in range(count):
            entry = table + i * 40
            virtual_size, rva, raw_size, raw_offset = self.unpack("<IIII", entry + 8)
            mapped_size = max(virtual_size, raw_size)
            if mapped_size:
                _require(rva + mapped_size <= self.image_size, "section exceeds image address space")
                virtual_ranges.append((rva, rva + mapped_size))
            if raw_size:
                self.raw(raw_offset, raw_size)
                raw_ranges.append((raw_offset, raw_offset + raw_size))
            self.sections.append((rva, mapped_size, raw_offset, raw_size))
        for ranges, label in ((raw_ranges, "raw"), (virtual_ranges, "virtual")):
            ranges.sort()
            _require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), "overlapping %s section/header ranges" % label)

    def raw(self, offset, size):
        _require(offset >= 0 and size >= 0 and offset + size <= len(self.data), "raw file range is out of bounds")
        return self.data[offset:offset + size]

    def unpack(self, fmt, offset):
        return struct.unpack(fmt, self.raw(offset, struct.calcsize(fmt)))

    def _chunk(self, rva):
        _require(0 <= rva < self.image_size, "RVA is outside image address space")
        if rva < self.headers_size:
            return rva, self.headers_size - rva
        for start, mapped_size, raw_offset, raw_size in self.sections:
            if start <= rva < start + mapped_size:
                delta = rva - start
                _require(delta < raw_size, "metadata RVA points into virtual zero-fill rather than raw file bytes")
                return raw_offset + delta, raw_size - delta
        raise PEFormatError("RVA falls in an unmapped image gap")

    def rva(self, rva, size):
        _require(size >= 0 and rva >= 0 and rva + size <= self.image_size, "RVA range exceeds image address space")
        _require(size <= len(self.data), "RVA range exceeds raw-file budget")
        pieces = []
        while size:
            offset, available = self._chunk(rva)
            take = min(size, available)
            pieces.append(self.raw(offset, take))
            rva += take
            size -= take
        return b"".join(pieces)

    def validate_rva(self, rva, size=1):
        # An exported data address can legitimately be in virtual zero-fill.
        _require(0 <= rva and size > 0 and rva + size <= self.image_size, "virtual RVA range exceeds image address space")
        regions = [(0, self.headers_size)] + [(start, start + span) for start, span, _, _ in self.sections if span]
        cursor = rva
        end = rva + size
        for start, stop in sorted(regions):
            if start <= cursor < stop:
                cursor = min(end, stop)
                if cursor == end:
                    return
        raise PEFormatError("virtual RVA range is in an unmapped image gap")

    def ru(self, fmt, rva):
        return struct.unpack(fmt, self.rva(rva, struct.calcsize(fmt)))

    def cstring(self, rva, end=None):
        limit = min(MAX_STRING_BYTES, self.image_size - rva)
        if end is not None:
            limit = min(limit, end - rva)
        _require(limit > 0, "string RVA/range is invalid")
        pieces = []
        cursor = rva
        remaining = limit
        while remaining:
            offset, available = self._chunk(cursor)
            chunk = self.raw(offset, min(remaining, available))
            terminator = chunk.find(b"\0")
            if terminator >= 0:
                pieces.append(chunk[:terminator])
                value = b"".join(pieces)
                _require(bool(value), "empty metadata string")
                try:
                    return value.decode("ascii")
                except UnicodeDecodeError as exc:
                    raise PEFormatError("non-ASCII metadata string") from exc
            pieces.append(chunk)
            cursor += len(chunk)
            remaining -= len(chunk)
        raise PEFormatError("metadata string is unterminated or exceeds string budget")

    def directory(self, index, minimum):
        if index >= len(self.directories):
            return None
        rva, size = self.directories[index]
        if not rva and not size:
            return None
        _require(bool(rva) and size >= minimum, "invalid data directory RVA/size")
        self.rva(rva, size)  # Validate the complete declared span, not only its prefix.
        return rva, size

    def consume_symbols(self, count):
        _require(0 <= count <= self.symbol_budget, "symbol count exceeds inventory budget")
        self.symbol_budget -= count

    def va_to_rva(self, va):
        _require(va >= self.image_base, "legacy delay-load VA is below image base")
        rva = va - self.image_base
        _require(rva < self.image_size and rva <= 0xffffffff, "legacy delay-load VA is outside image")
        return rva

    def thunks(self, table, name_mode="rva"):
        _require(table != 0, "missing import lookup table")
        symbols = []
        for i in range(MAX_SYMBOLS + 1):
            value = self.ru("<Q", table + i * 8)[0]
            if value == 0:
                return symbols
            self.consume_symbols(1)
            if value & (1 << 63):
                _require(value & ~((1 << 63) | 0xffff) == 0, "ordinal thunk has reserved bits set")
                symbols.append({"ordinal": value & 0xffff})
            else:
                if name_mode == "va":
                    name_rva = self.va_to_rva(value)
                else:
                    _require(value <= 0x7fffffff, "name thunk has non-RVA/reserved bits set")
                    name_rva = value
                hint = self.ru("<H", name_rva)[0]
                symbols.append({"name": self.cstring(name_rva + 2), "hint": hint})
        raise PEFormatError("import lookup table is unterminated or exceeds symbol budget")

    def imports(self, delay=False):
        width = 32 if delay else 20
        directory = self.directory(13 if delay else 1, width)
        if directory is None:
            return []
        start, size = directory
        descriptors = []
        for i in range(min(size // width, MAX_DESCRIPTORS + 1)):
            fields = self.ru("<8I" if delay else "<5I", start + i * width)
            if not any(fields):
                return descriptors
            _require(i < MAX_DESCRIPTORS, "import descriptor count exceeds budget")
            if delay:
                attrs, name, module, iat, lookup, bound, unload, _ = fields
                _require(attrs & ~1 == 0, "delay-load descriptor has reserved attributes set")
                mode = "rva" if attrs & 1 else "va"
                addresses = [name, module, iat, lookup, bound, unload]
                if mode == "va":
                    addresses = [self.va_to_rva(value) if value else 0 for value in addresses]
                name, module, iat, lookup, bound, unload = addresses
                _require(name != 0 and lookup != 0 and iat != 0, "missing required delay-load descriptor address")
                dll = self.cstring(name)
                symbols = self.thunks(lookup, mode)
                # IAT/optional tables contain pointer-sized cells, not names.
                # Module storage may be zero-fill; INT and table bytes may not.
                self.rva(iat, (len(symbols) + 1) * 8)
                for address in (bound, unload):
                    if address:
                        self.rva(address, (len(symbols) + 1) * 8)
                if module:
                    self.validate_rva(module, 8)
                descriptors.append({"dll": dll, "attributes": attrs, "address_mode": mode, "symbols": symbols})
            else:
                lookup, timestamp, _, name, iat = fields
                _require(name != 0 and iat != 0, "missing required import descriptor address")
                # An IAT already bound to function VAs cannot recover names.
                _require(lookup != 0 or timestamp == 0, "bound IAT has no independent import lookup table; symbols are unknown")
                dll = self.cstring(name)
                symbols = self.thunks(lookup or iat)
                self.rva(iat, (len(symbols) + 1) * 8)
                descriptors.append({"dll": dll, "symbols": symbols})
        raise PEFormatError("import descriptor table has no terminator inside its declared directory")

    def exports(self):
        directory = self.directory(0, 40)
        if directory is None:
            return None, []
        start, size = directory
        _, _, _, _, name, base, function_count, name_count, functions, names, ordinals = self.ru("<IIHHIIIIIII", start)
        dll = self.cstring(name)
        self.consume_symbols(function_count + name_count)
        _require(base + function_count <= 1 << 32, "export ordinal range overflows 32 bits")
        function_data = self.rva(functions, function_count * 4)
        name_data = self.rva(names, name_count * 4)
        ordinal_data = self.rva(ordinals, name_count * 2)
        aliases = {}
        seen = set()
        for i in range(name_count):
            index = struct.unpack_from("<H", ordinal_data, i * 2)[0]
            _require(index < function_count, "export name ordinal index exceeds address table")
            symbol = self.cstring(struct.unpack_from("<I", name_data, i * 4)[0])
            _require(symbol not in seen, "duplicate export name")
            seen.add(symbol)
            aliases.setdefault(index, []).append(symbol)
        exports = []
        for i in range(function_count):
            rva = struct.unpack_from("<I", function_data, i * 4)[0]
            if rva == 0:
                _require(i not in aliases, "named export points to an empty address table slot")
                continue
            self.validate_rva(rva)
            entry = {"ordinal": base + i, "names": aliases.get(i, []), "rva": rva,
                     "forwarder": None, "forwarder_module": None,
                     "forwarder_name": None, "forwarder_ordinal": None}
            if start <= rva < start + size:
                forwarder = self.cstring(rva, start + size)
                module, separator, target = forwarder.rpartition(".")
                _require(bool(separator and module and target), "invalid forwarded export syntax")
                entry["forwarder"] = forwarder
                entry["forwarder_module"] = _module_key(module)
                if target.startswith("#"):
                    _require(target[1:].isascii() and target[1:].isdigit(), "invalid forwarded export ordinal")
                    _require(len(target) <= 6 and int(target[1:]) <= 0xffff, "forwarded export ordinal exceeds 16 bits")
                    entry["forwarder_ordinal"] = int(target[1:])
                else:
                    entry["forwarder_name"] = target
            exports.append(entry)
        return dll, exports


def inspect_pe(path):
    """Return JSON metadata; missing/invalid files return status=error and null tables.

    A whole-image failure deliberately discards partial tables so a caller cannot
    mistake an incomplete declaration list for a successful empty inventory.
    """
    path = Path(path).expanduser().absolute()
    result = {
        "path": str(path), "bytes": None, "sha256": None, "status": "error",
        "parsedDLL": None, "machine": None, "pe_format": None, "image_base": None,
        "imports": None, "delay_imports": None, "exports": None,
        "dependency_libraries": None, "error": None, "safety": SAFETY,
        "dynamic_load_closure": dict(DYNAMIC_UNKNOWN),
    }
    try:
        with path.open("rb") as stream:
            data = stream.read(MAX_FILE_BYTES + 1)
        _require(len(data) <= MAX_FILE_BYTES, "file exceeds inventory byte budget")
        result["bytes"] = len(data)
        result["sha256"] = hashlib.sha256(data).hexdigest()
        pe = _PE(data)
        imports = pe.imports()
        delay = pe.imports(delay=True)
        dll, exports = pe.exports()
        dependencies = {_module_key(item["dll"]) for item in imports + delay}
        dependencies.update(item["forwarder_module"] for item in exports if item["forwarder_module"])
        result.update(status="ok", parsedDLL=dll, machine="AMD64", pe_format="PE32+",
                      image_base=pe.image_base, imports=imports, delay_imports=delay,
                      exports=exports, dependency_libraries=sorted(dependencies))
    except (OSError, PEFormatError, struct.error) as exc:
        result["error"] = str(exc)
    return result


def inventory(paths):
    """Inventory supplied binaries only; never find, load, or execute dependencies."""
    binaries = [inspect_pe(path) for path in paths]
    ok = sum(item["status"] == "ok" for item in binaries)
    errors = len(binaries) - ok
    return {
        "schema_version": 1, "safety": SAFETY,
        "dynamic_load_closure": dict(DYNAMIC_UNKNOWN),
        "status": "partial" if ok and errors else ("error" if errors else "ok"),
        "counts": {"ok": ok, "errors": errors}, "binaries": binaries,
        "dependency_libraries": sorted({name for item in binaries if item["status"] == "ok" for name in item["dependency_libraries"]}),
        "dependency_union_complete_for_inputs": not errors,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", action="append", required=True, help="AMD64 PE32+ file; repeat for multiple inputs")
    parser.add_argument("--output", type=Path, required=True, help="JSON report path, or - for stdout")
    args = parser.parse_args(argv)
    report = inventory(args.binary)
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    try:
        if str(args.output) == "-":
            sys.stdout.write(payload)
        else:
            args.output.write_text(payload, encoding="utf-8")
    except OSError as exc:
        parser.exit(2, "Cannot write inventory: %s\n" % exc)
    return 0 if report["counts"]["errors"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Read-only qualification of pinned source evidence; never build/run a guest.

The curated port batch is NOT a complete link/dependency closure. The report
always keeps rendering, UWP runtime and rootfs admission unqualified.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import xml.etree.ElementTree as ET

PIN = '58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4'
PROJECT = 'project/msvc/BoxedWine/BoxedWine/BoxedWine.vcxproj'
CORE_BATCH = [
    'source/emulation/cpu/cpu64.cpp',
    'source/emulation/cpu/cpu64RunElf.cpp',
    'source/emulation/cpu/cpu64SelfTest.cpp',
    'source/emulation/cpu/normal/ripSampler.cpp',
    'source/kernel/kmemory64.cpp',
    'source/kernel/syscall64.cpp',
    'source/kernel/loader/loader64.cpp',
    'source/x11wire/xwireconnection.cpp',
    'source/x11wire/xwireserver.cpp',
    'source/x11wire/xwirepresentSDL.cpp',
]
NS = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def normalize_project_path(path):
    path = path.replace('\\', '/')
    while path.startswith('../'):
        path = path[3:]
    return path

def inspect_source(source, lock):
    errors = []
    if lock.get('commit') != PIN:
        errors.append('Source evidence commit is not the selected pin')
    entries = lock.get('files', [])
    verified = []
    for entry in entries:
        path = Path(entry.get('path', ''))
        if path.is_absolute() or '..' in path.parts or not path.parts:
            errors.append('Unsafe source evidence path')
            continue
        file = source / path
        if not file.is_file() or digest(file) != entry.get('snapshot_sha256'):
            errors.append('Missing or modified evidence file: ' + path.as_posix())
        else:
            verified.append(path.as_posix())
    required = set(CORE_BATCH + [PROJECT, 'platform/windows/platform.cpp'])
    missing_evidence = sorted(required - set(verified))
    if missing_evidence:
        errors.append('Required source evidence is missing or unverified')
    report = {
        'commit': PIN, 'scope': 'Curated source inventory, not a complete link closure',
        'verified_evidence_count': len(verified), 'missing_evidence': missing_evidence,
        'evidence_errors': errors, 'core_port_batch': CORE_BATCH,
        'selected_cpu': 'BOXEDWINE_GUEST_X64 interpreter; no native-code JIT',
        'selected_renderer': 'GDI/X11 BGRA pixels via UWP D3D11; GL64 excluded initially',
        'guest_code_executed': False, 'backend_linked': False, 'safe_to_boot_wine': False,
        'complete_link_closure_proven': False,
    }
    if errors:
        return report
    try:
        project = ET.parse(source / PROJECT)
    except ET.ParseError as error:
        report['evidence_errors'].append('Invalid Windows project XML: ' + str(error))
        return report
    compiled = {normalize_project_path(node.attrib.get('Include', ''))
                for node in project.findall('.//m:ClCompile', NS) if 'Include' in node.attrib}
    definitions = ' '.join(node.text or '' for node in project.findall('.//m:PreprocessorDefinitions', NS))
    app_types = [node.text for node in project.findall('.//m:ApplicationType', NS)]
    containers = [node.text for node in project.findall('.//m:AppContainerApplication', NS)]
    cpu = (source / CORE_BATCH[0]).read_text()
    platform = (source / 'platform/windows/platform.cpp').read_text()
    report.update({
        'windows_declared_compile_entries': len(compiled),
        'missing_core_compile_entries': [path for path in CORE_BATCH if path not in compiled],
        'windows_enables_guest_x64': any(token.strip().split('=', 1)[0] == 'BOXEDWINE_GUEST_X64'
                                        for token in definitions.split(';')),
        'windows_uwp_target': 'Windows Store' in app_types and 'true' in containers,
        'requires_128bit_arithmetic_port': '__int128' in cpu or '__uint128_t' in cpu,
        'desktop_memory_api_candidates': [name for name in ['VirtualAlloc(', 'VirtualProtect('] if name in platform],
        'remaining_runtime_gates': ['Windows guest-x64 compile', 'UWP API partition',
            'X11 pixel presenter and input', 'ELF64 selftests on UWP',
            'licensed pinned rootfs', 'actual Wine64 window', 'actual Explorer desktop'],
    })
    return report

def host_dependencies():
    # Trusted host availability probes only; no installs, configure or builds.
    result = {'sdl2-config': shutil.which('sdl2-config'), 'pkg-config': shutil.which('pkg-config')}
    for package in ['sdl2', 'minizip', 'libcurl', 'openssl']:
        result[package] = bool(result['pkg-config']) and subprocess.run(
            [result['pkg-config'], '--exists', package], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL).returncode == 0
    return result

def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'external/Boxedwine64-full')
    parser.add_argument('--lock', type=Path, default=root / 'backend-source-files.lock.json')
    parser.add_argument('--output', type=Path, default=root / 'artifacts/port-qualification.json')
    args = parser.parse_args()
    report = inspect_source(args.source, json.loads(args.lock.read_text()))
    report['host_dependencies'] = host_dependencies()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report.get(key) for key in ['verified_evidence_count',
        'missing_core_compile_entries', 'windows_enables_guest_x64', 'windows_uwp_target',
        'host_dependencies', 'safe_to_boot_wine', 'evidence_errors']}, indent=2))
    # A successful inventory is not a successful backend gate.
    return 1 if report['evidence_errors'] else 0

if __name__ == '__main__':
    raise SystemExit(main())

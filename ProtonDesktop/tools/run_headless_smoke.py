#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the pinned Linux core and run only our own static ELF64 diagnostic.

Does not fetch dependencies, execute upstream installers, mount a rootfs, start
Wine or deploy anything. The upstream CPU self-tests also run in-process.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import subprocess

PIN = '58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4'
EXPECTED = 'ProtonDesktop guest-x64 interpreter: PASS'

def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def verify_guest_log(log, expected_status, expect_marker):
    exits = re.findall(r'^CPU64: exit(?:_group)? syscall, status=(-?\d+)[ \t]*$', log, re.M)
    stopped = re.findall(r'^--x64-run-elf: stopped after (\d+) instructions \(yield=1 [^\n]*\)$', log, re.M)
    clean = re.findall(r'^--x64-run-elf: exit reached cleanly[ \t]*$', log, re.M)
    return (exits == [str(expected_status)] and len(stopped) == 1
            and 0 < int(stopped[0]) < 10000 and len(clean) == 1
            and (EXPECTED in log) == expect_marker)

def begin_report(path):
    # A previous success must never describe a new failed/interrupted run.
    path.write_text(json.dumps({'status': 'incomplete',
        'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}) + '\n')

def run(argv, log, *, env=None, timeout=600):
    with log.open('w') as stream:
        result = subprocess.run([str(arg) for arg in argv], stdout=stream,
                                stderr=subprocess.STDOUT, env=env, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{argv[0]} failed ({result.returncode}); see {log}')
    return log.read_text(errors='replace')

def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=root / 'external/Boxedwine64-full')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 16:
        parser.error('--jobs must be 1..16')
    source = args.source.resolve()
    build = root / 'build/headless-core'
    output = root / 'artifacts'
    output.mkdir(exist_ok=True)
    report_path = output / 'headless-core-result.json'
    begin_report(report_path)
    try:
        return perform(root, source, build, output, args.jobs)
    except Exception as error:
        report_path.write_text(json.dumps({'status': 'failed', 'error': str(error),
            'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}) + '\n')
        raise

def perform(root, source, build, output, jobs):
    run(['cmake', '-S', root / 'tools/headless-core', '-B', build, '-G', 'Ninja',
         '-DBOXEDWINE_SOURCE=' + str(source), '-DCMAKE_BUILD_TYPE=Release'],
        output / 'headless-core-configure.log')
    run(['cmake', '--build', build, '--parallel', jobs],
        output / 'headless-core-build.log', timeout=1800)
    assembly = root / 'tools/headless-core/guest64-smoke.S'
    payload = build / 'guest64-smoke.elf'
    negative = build / 'guest64-negative.elf'
    for target, definitions in [(payload, []), (negative, ['--defsym', 'NEGATIVE_CONTROL=1'])]:
        obj = target.with_suffix('.o')
        run(['as', '--64', *definitions, '-o', obj, assembly], output / (target.stem + '-assemble.log'))
        run(['ld', '-m', 'elf_x86_64', '-static', '-o', target, obj], output / (target.stem + '-link.log'))
    # Prevent inherited diagnostic tuning/rootfs/library-path variables from
    # changing the test. No guest libc or files outside our ELF are needed.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('BW64_', 'BOXEDWINE', 'LD_'))}
    env.update({'BW64_INSN_LIMIT': '10000', 'SDL_VIDEODRIVER': 'dummy',
                'SDL_AUDIODRIVER': 'dummy', 'LC_ALL': 'C'})
    executable = build / 'boxedwine64-headless'
    positive = run([executable, '--x64-run-elf', payload], output / 'guest64-smoke.log', env=env, timeout=30)
    rejected = run([executable, '--x64-run-elf', negative], output / 'guest64-negative.log', env=env, timeout=30)
    # Upstream runner returns zero on any yielded exit, including nonzero guest
    # status. Check the actual syscall trace and success marker, not that alone.
    positive_ok = verify_guest_log(positive, 0, True)
    negative_ok = verify_guest_log(rejected, 9, False)
    if not positive_ok or not negative_ok:
        raise RuntimeError('Guest result/negative control failed; see guest64 logs')
    selftest = run([executable, '--x64-selftest'], output / 'guest64-selftest.log', env=env, timeout=120)
    summary = re.search(r'self-test summary: (\d+) passed, (\d+) failed', selftest)
    if not summary or int(summary.group(1)) == 0 or int(summary.group(2)) != 0:
        raise RuntimeError('Missing or failing CPU64 self-test summary')
    commands = json.loads((build / 'compile_commands.json').read_text())
    cache = (build / 'CMakeCache.txt').read_text()
    compiler = re.search(r'^CMAKE_CXX_COMPILER:FILEPATH=(.+)$', cache, re.M).group(1)
    compiler_version = subprocess.check_output([compiler, '--version'], text=True).splitlines()[0]
    report = {
        'status': 'passed',
        'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'source_commit': PIN,
        'host': {'system': platform.system(), 'machine': platform.machine(),
                 'compiler': compiler, 'compiler_version': compiler_version},
        'qualifies_windows_or_xbox': False,
        'mode': 'Real BOXEDWINE_GUEST_X64 interpreter, upstream memory/ELF loader/syscalls',
        'source_overlay': {'path': 'tools/headless-core/gcc-packed-log.patch',
                           'sha256': sha256(root / 'tools/headless-core/gcc-packed-log.patch')},
        'compiled_translation_units': len(commands),
        'upstream_cpu64_checks_passed': int(summary.group(1)),
        'upstream_cpu64_checks_failed': int(summary.group(2)),
        'positive_guest_smoke': positive_ok, 'negative_guest_exit_control': negative_ok,
        'payload_source_sha256': sha256(assembly), 'payload_elf_sha256': sha256(payload),
        'executable_sha256': sha256(executable),
        'selftest_summary': [line for line in selftest.splitlines()
                             if re.search(r'passed|failed|self.test|===', line, re.I)][-8:],
        'wine_executed': False, 'rootfs_downloaded': False,
        'uwp_backend_linked': False, 'xbox_tested': False,
    }
    (output / 'headless-core-result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

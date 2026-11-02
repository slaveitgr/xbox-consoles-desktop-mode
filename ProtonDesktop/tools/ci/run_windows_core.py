#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run actual Windows interpreter diagnostics; this is not a UWP/Wine test."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import subprocess
import sys

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from make_elf64_smoke import make_elf
from run_headless_smoke import verify_guest_log, begin_report


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = output / 'windows-core-result.json'
    begin_report(report)
    try:
        if platform.system() != 'Windows':
            raise RuntimeError('Windows execution is required for this evidence')
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('BW64_', 'BOXEDWINE', 'LD_'))}
        env.update(BW64_INSN_LIMIT='10000', SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')

        def run(name, argv):
            with (output / (name + '.log')).open('w') as stream:
                result = subprocess.run([str(executable), *map(str, argv)],
                                        stdout=stream, stderr=subprocess.STDOUT,
                                        timeout=120, env=env)
            text = (output / (name + '.log')).read_text(errors='replace')
            if result.returncode != 0:
                raise RuntimeError(f'{name} returned host status {result.returncode}')
            return text

        payload_hashes = {}
        for negative in (False, True):
            name = 'guest64-negative' if negative else 'guest64-smoke'
            payload = output / (name + '.elf')
            payload.write_bytes(make_elf(negative))
            payload_hashes[name] = sha256(payload)
            log = run(name, ['--x64-run-elf', payload])
            if not verify_guest_log(log, 9 if negative else 0, not negative):
                raise RuntimeError(f'{name} did not reach its exact expected guest exit')
        selftest = run('guest64-selftest', ['--x64-selftest'])
        summaries = re.findall(r'self-test summary: (\d+) passed, (\d+) failed', selftest)
        if len(summaries) != 1 or int(summaries[0][0]) == 0 or int(summaries[0][1]) != 0:
            raise RuntimeError('CPU64 self-test summary missing, ambiguous or failing')
        result = {
            'status': 'passed',
            'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'host': {'system': platform.system(), 'machine': platform.machine(),
                     'version': platform.version()},
            'source_commit': '58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4',
            'executable_sha256': sha256(executable),
            'payload_sha256': payload_hashes,
            'cpu64_checks_passed': int(summaries[0][0]),
            'cpu64_checks_failed': int(summaries[0][1]),
            'positive_guest_exit': 0, 'negative_guest_exit': 9,
            'uwp_executed': False, 'wine_executed': False, 'xbox_tested': False,
        }
        report.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    except BaseException as error:
        report.write_text(json.dumps({'status': 'failed', 'error': str(error),
            'timestamp_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}) + '\n')
        raise


if __name__ == '__main__':
    main()

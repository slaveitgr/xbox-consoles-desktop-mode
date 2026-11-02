#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify the frozen rootfs plan against signed Debian metadata.

Optional fetching is restricted to the exact SHA256-addressed metadata indexes
in the lock. No package/archive/rootfs binaries or Wine are downloaded/executed.
"""
from pathlib import Path
import argparse
import datetime
import json
import lzma
import re
import subprocess
import tempfile
from lock_debian_metadata import paragraphs, sha, fetch

HERE = Path(__file__).resolve().parent

def require(condition, message):
    if not condition: raise RuntimeError(message)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch-indexes', action='store_true')
    parser.add_argument('--no-report', action='store_true', help='Verify without rewriting the tracked metadata report')
    args = parser.parse_args()
    lock = json.loads((HERE / 'debian-bookworm.lock.json').read_text())
    cache = HERE / '.cache'; cache.mkdir(exist_ok=True)
    metadata = HERE / 'metadata'
    release = metadata / 'InRelease'
    require(sha(release.read_bytes()) == lock['distribution']['inrelease_sha256'], 'InRelease hash mismatch')
    with tempfile.TemporaryDirectory(prefix='verify-gnupg-', dir=cache) as temporary:
        home = Path(temporary)
        for key in lock['trust']['keys']:
            path = metadata / key['url'].rsplit('/',1)[1]
            require(sha(path.read_bytes()) == key['sha256'], 'Key file hash mismatch')
            listing = subprocess.check_output(['gpg', '--batch', '--no-options', '--homedir', str(home),
                '--with-colons', '--show-keys', str(path)], text=True, stderr=subprocess.DEVNULL)
            require(re.findall(r'^fpr:::::::::([A-F0-9]+):', listing, re.M)[0] == key['primary_fingerprint'],
                    'Debian primary fingerprint mismatch')
            subprocess.run(['gpg', '--batch', '--no-options', '--homedir', str(home), '--import', str(path)],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        keyring = home / 'trusted.gpg'
        keyring.write_bytes(subprocess.check_output(['gpg', '--batch', '--no-options', '--homedir', str(home),
            '--export', *[key['primary_fingerprint'] for key in lock['trust']['keys']]]))
        checked = subprocess.run(['gpgv', '--keyring', str(keyring), '--status-fd', '1', str(release)],
                                 text=True, capture_output=True)
        require(checked.returncode == 0, 'Debian signature verification failed')
        valid = re.findall(r'^\[GNUPG:\] VALIDSIG (.+)$', checked.stdout, re.M)
        require(len(valid) == 3, 'Expected three Debian Release signatures')
    fields = next(paragraphs(release.read_text().split('\n\n',1)[1].split('-----BEGIN PGP SIGNATURE-----')[0]))
    signed_hashes = {line.split()[2]:line.split()[:2] for line in fields['SHA256'].splitlines() if line.strip()}
    contents = {}
    for index in lock['indexes']:
        require(signed_hashes[index['path']] == [index['sha256'], str(index['bytes'])], 'Index not anchored in signed release')
        path = cache / index['path'].rsplit('/',1)[1]
        if not path.exists() and args.fetch_indexes:
            require('/by-hash/SHA256/' + index['sha256'] in index['url'], 'Non-content-addressed metadata URL')
            path.write_bytes(fetch(index['url']))
        require(path.exists(), 'Missing metadata index; use --fetch-indexes')
        data = path.read_bytes()
        require(len(data) == index['bytes'] and sha(data) == index['sha256'], 'Index hash mismatch')
        contents[index['path']] = list(paragraphs(lzma.decompress(data).decode('utf-8')))
    binaries = {item['Package']:item for item in contents['main/binary-amd64/Packages.xz']}
    sources = {item['Package']:item for item in contents['main/source/Sources.xz']}
    for item in lock['packages']:
        actual = binaries[item['package']]
        for saved, field in [('version','Version'),('architecture','Architecture'),('sha256','SHA256'),('filename','Filename')]:
            require(item[saved] == actual[field], 'Binary package metadata mismatch: ' + item['package'])
        require(item['bytes'] == int(actual['Size']), 'Binary size metadata mismatch')
    for item in lock['sources']:
        actual = sources[item['source']]
        require(item['version'] == actual['Version'], 'Source version mismatch')
        expected = {line.split()[2]:(line.split()[0],int(line.split()[1]))
                    for line in actual['Checksums-Sha256'].splitlines() if line.strip()}
        require(len(expected) == len(item['archives']), 'Source archive set mismatch')
        for archive in item['archives']:
            require(expected[archive['filename']] == (archive['sha256'],archive['bytes']), 'Source archive hash metadata mismatch')
    upstream = json.loads((HERE / 'upstream-sources.lock.json').read_text())
    for item in upstream['files']:
        require(sha((HERE / item['local']).read_bytes()) == item['sha256'], 'Pinned upstream recipe source changed')
    plan = json.loads((HERE / 'assembly-plan.json').read_text())
    require(plan['wine_executed'] is False and
            plan['accepted_layer_sha256'] is None, 'Plan overstates Wine execution/artifact acceptance')
    result = {'status':'passed','timestamp_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'signed_release_signatures':len(valid),'binary_package_metadata_records':len(lock['packages']),
        'source_package_metadata_records':len(lock['sources']), 'upstream_source_files':len(upstream['files']),
        'scope':'signed metadata and pinned references only; no payload qualification',
        'package_binaries_downloaded_by_this_verifier':False,
        'wine_executed_by_this_verifier':False,'rootfs_assembled_by_this_verifier':False}
    if not args.no_report:
        (metadata / 'lock-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
if __name__ == '__main__': main()

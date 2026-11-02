#!/usr/bin/env python3
"""Fetch the exact official SDK packages used by Microsoft's Xbox WebView2 sample."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = [
    ('microsoft.ui.xaml', '2.8.7', '79207b10fe243eb1a8dcdc29beceba2f472f145ed31f147f7ae3f43b0659c9f7'),
    ('microsoft.web.webview2', '1.0.3719.77', '2f6be3a10a1a8d6d1fde986af4131dab344f8181fffe75590824e0f4b037ed73'),
]

def main():
    output=ROOT/'artifacts/webview2-sdk';output.mkdir(parents=True,exist_ok=True)
    for name,version,digest in PACKAGES:
        archive=output/(name+'.nupkg')
        if not archive.exists():
            url=f'https://api.nuget.org/v3-flatcontainer/{name}/{version}/{name}.{version}.nupkg'
            urllib.request.urlretrieve(url,archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest()!=digest:
            raise SystemExit('NuGet SDK digest mismatch: '+name)
        destination=output/name
        with zipfile.ZipFile(archive) as z:
            if sum(i.file_size for i in z.infolist())>256*1024*1024:
                raise SystemExit('Unexpected SDK archive size')
            for i in z.infolist():
                path=PurePosixPath(i.filename)
                if path.is_absolute() or '..' in path.parts or '\\' in i.filename:
                    raise SystemExit('Unsafe SDK archive member')
            z.extractall(destination)
    (output/'sdk-pins.json').write_text(json.dumps(PACKAGES,indent=2)+'\n')
    print('Verified official WinUI 2.8.7 and WebView2 1.0.3719.77 SDKs')

if __name__=='__main__':main()

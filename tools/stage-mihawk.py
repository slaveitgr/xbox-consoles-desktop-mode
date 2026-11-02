#!/usr/bin/env python3
"""Stage XWindows OS and its original branding plus pinned licensed assets."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / 'external/MihawkOS'
PIN = 'f81a028a668b36f5bcb51b09e66d526979382f1d'


def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def assets(destination):
    actual = subprocess.check_output(['git', '-C', str(UPSTREAM), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != PIN:
        raise SystemExit(f'Mihawk asset pin mismatch: {actual}')
    destination.mkdir(parents=True, exist_ok=True)
    copy(ROOT / 'desktop/branding/logo.svg', destination / 'xwindows-logo.svg')
    copy(ROOT / 'desktop/branding/logo.svg', destination / 'start.svg')
    copy(ROOT / 'desktop/branding/wallpaper.svg', destination / 'xwindows-wallpaper.svg')
    copy(UPSTREAM / 'prototypes/windows11-shell/css/tokens.css', destination / 'tokens.css')
    for icon in (UPSTREAM / 'prototypes/windows11-shell/assets/icons/object').glob('*.svg'):
        copy(icon, destination / 'icons' / icon.name)
    copy(UPSTREAM / 'themes/windows11/fonts/upstream/selawk.ttf', destination / 'fonts/selawk.ttf')
    (destination / 'fonts.css').write_text('@font-face{font-family:Selawik;src:url("fonts/selawk.ttf") format("truetype");font-style:normal;font-weight:100 900;}\n', encoding='utf-8')


def mark(size):
    # Original XWindows OS mark, matching desktop/branding/logo.svg (MIT).
    scale = size * 4 / 100
    im = Image.new('RGBA', (size * 4, size * 4))
    draw = ImageDraw.Draw(im)
    box = lambda values: tuple(round(v * scale) for v in values)
    draw.rounded_rectangle(box((8, 8, 92, 92)), radius=round(22 * scale), fill='#087ce5')
    for start, end in [((32, 29), (68, 71)), ((68, 29), (32, 71))]:
        draw.line([box(start), box(end)], fill='white', width=round(10 * scale))
        for x, y in (start, end):
            draw.ellipse(box((x - 5, y - 5, x + 5, y + 5)), fill='white')
    return im.resize((size, size), Image.Resampling.LANCZOS)


def package_icons(destination):
    destination.mkdir(parents=True, exist_ok=True)
    for name, width, height, glyph in [('Square44x44Logo.png', 44, 44, 40), ('Square150x150Logo.png', 150, 150, 120),
        ('StoreLogo.png', 50, 50, 46), ('Wide310x150Logo.png', 310, 150, 120), ('SplashScreen.png', 620, 300, 150)]:
        im = Image.new('RGBA', (width, height), '#102748')
        icon = mark(glyph)
        im.alpha_composite(icon, ((width - glyph) // 2, (height - glyph) // 2))
        im.save(destination / name)


def notices(destination):
    files = {
        'Mihawk-GPL-3.0.txt': UPSTREAM / 'LICENSE',
        'Mihawk-NOTICE.md': UPSTREAM / 'NOTICE.md',
        'Mihawk-LICENSING.md': UPSTREAM / 'LICENSING.md',
        'Mihawk-art-LGPL-2.1.txt': UPSTREAM / 'themes/windows11/COPYING.LIB',
        'Selawik-OFL.txt': UPSTREAM / 'themes/windows11/fonts/upstream/LICENSE.txt',
        'Object-icons-provenance.json': UPSTREAM / 'prototypes/windows11-shell/assets/icons/object/provenance.json',
        'Fluent-MIT-LICENSE.txt': UPSTREAM / 'themes/windows11/icons/fluent/LICENSE',
        'Fluent-NOTICE.txt': UPSTREAM / 'themes/windows11/icons/fluent/NOTICE',
        'DesktopMode-MIT.txt': ROOT / 'external/DesktopMode_uwp/LICENSE',
        'XboxDesktop-MIT.txt': ROOT / 'LICENSE',
    }
    for name in ['LICENSE', 'LICENSE_INFO', 'XCDM-INTEGRATION.md']:
        files['libx86emu-' + name + '.txt'] = ROOT / 'third_party/libx86emu' / name
    for name, source in files.items():
        copy(source, destination / name)
    (destination / 'XWINDOWS-OS.txt').write_text(
        'XWindows OS for Xbox Developer Mode, a community UWP desktop.\n'
        f'MihawkOS asset/source pin: {PIN}\n'
        'https://github.com/mihawk-99/PS5_MihawkOS\n'
        'Corresponding Xbox source and build scripts:\n'
        'https://github.com/slaveitgr/xbox-consoles-desktop-mode/tree/codex/x86-runtime\n'
        'The desktop adaptation is GPL-3.0-or-later. Upstream assets keep their licenses.\n'
        'Original XWindows OS logo and abstract wallpaper: MIT, supplied in desktop/branding.\n'
        'Selawik: OFL-1.1. Mihawk-derived frontend and object icons retain their component licenses.\n'
        'No Bubble City artist artwork, PS5 runtime, Wine/Proton binary or game is included.\n'
        'Package identity is retained for community DesktopMode update compatibility.\n'
        'This development signature is not an official Microsoft signature.\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets-only', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--host', type=Path)
    parser.add_argument('--bridge', type=Path)
    parser.add_argument('--webview2', type=Path)
    args = parser.parse_args()
    if args.assets_only:
        assets(args.output or ROOT / 'desktop/mihawk/assets')
        return
    if not args.output or not args.host or not args.bridge:
        parser.error('Package staging requires --output, --host and --bridge')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name in ['index.html', 'shell.css', 'shell.js']:
        copy(ROOT / 'desktop/mihawk' / name, out / 'Mihawk' / name)
    assets(out / 'Mihawk/assets')
    package_icons(out / 'Assets')
    copy(args.host, out / 'MihawkDesktop.exe')
    copy(ROOT / 'samples/return42.exe', out / 'Diagnostics/return42.exe')
    for name in ['Win32Bridge.Runtime.dll', 'Win32Bridge.winmd']:
        copy(args.bridge / name, out / name)
    copy(ROOT / 'desktop/native/Package.appxmanifest', out / 'AppxManifest.xml')
    notices(out / 'ThirdParty')
    source_commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    (out / 'ThirdParty/SOURCE-COMMIT.txt').write_text(source_commit + '\n', encoding='utf-8')
    if args.webview2:
        sdk = args.webview2
        for source, target in [
            ('microsoft.web.webview2/runtimes/win-x64/native_uap/Microsoft.Web.WebView2.Core.dll', 'Microsoft.Web.WebView2.Core.dll'),
            ('microsoft.web.webview2/lib/Microsoft.Web.WebView2.Core.winmd', 'Microsoft.Web.WebView2.Core.winmd'),
            ('microsoft.web.webview2/LICENSE.txt', 'ThirdParty/WebView2-LICENSE.txt'),
            ('microsoft.web.webview2/NOTICE.txt', 'ThirdParty/WebView2-NOTICE.txt'),
            ('microsoft.ui.xaml/license.txt', 'ThirdParty/WinUI-MIT-LICENSE.txt'),
            ('sdk-pins.json', 'ThirdParty/WebView2-sdk-pins.json'),
        ]:
            copy(sdk / source, out / target)
        copy(sdk / 'microsoft.ui.xaml/tools/AppX/x64/Release/Microsoft.UI.Xaml.2.8.appx',
             out.parent / 'Dependencies/Microsoft.UI.Xaml.2.8.x64.appx')
    records = {str(p.relative_to(out)).replace('\\', '/'): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in sorted(out.rglob('*')) if p.is_file()}
    (out.parent / 'payload-files.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
    print(f'Staged {len(records)} XWindows OS files; upstream asset pin {PIN}')


if __name__ == '__main__':
    main()

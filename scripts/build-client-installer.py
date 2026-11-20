#!/usr/bin/env python3
"""Build a per-user Windows installer from a validated control-center bundle."""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def quote(value):
    return '"' + str(value).replace('$', '$$').replace('"', '$\\"') + '"'


def payload(source):
    manifest = source / 'SHA256SUMS.txt'
    files = []
    for line in manifest.read_text(encoding='utf-8').splitlines():
        digest, name = line.split('  ', 1)
        relative = PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts or any(char in name for char in '\\:"'):
            raise ValueError(f'Invalid Windows payload path: {name}')
        path = source / relative
        with path.open('rb') as content:
            if hashlib.file_digest(content, 'sha256').hexdigest() != digest:
                raise ValueError(f'Payload does not match its checksum: {path}')
        files.append(relative)
    if not {PurePosixPath('Waybridge.Control.exe'), PurePosixPath('waybridge-client.exe')}.issubset(files):
        raise ValueError('The payload must contain the control center and streaming worker')
    files.append(PurePosixPath(manifest.name))

    previous = None
    for relative in sorted(files):
        if relative.parent != previous:
            parts = relative.parent.parts
            yield 'SetOutPath "$Stage' + (('\\' + '\\'.join(parts)) if parts else '') + '"'
            yield 'IfErrors install_error'
            previous = relative.parent
        yield f'File {quote(source / relative)}'
        yield 'IfErrors install_error'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('architecture', choices=('x64', 'arm64'))
    parser.add_argument('--makensis', default=os.environ.get('MAKENSIS', 'makensis'))
    args = parser.parse_args()
    executable = shutil.which(args.makensis)
    if not executable:
        parser.error('makensis was not found. Install NSIS 3.11+ or pass --makensis.')

    arch = args.architecture
    build = ROOT / 'build'
    source = build / f'control-center-{arch}'
    if not (source / 'SHA256SUMS.txt').is_file():
        parser.error(f'Build and package the {arch} control center first: {source}')
    version = tomllib.loads((ROOT / 'Cargo.toml').read_text(encoding='utf-8'))['package']['version']
    output = build / f'Waybridge-Setup-win-{arch}.exe'
    with tempfile.TemporaryDirectory(prefix='setup-', dir=build) as temp:
        directory = Path(temp)
        include = directory / 'payload.nsh'
        include.write_text('\n'.join(payload(source)) + '\n', encoding='utf-8')
        staged = directory / output.name
        definitions = {'ARCH': arch, 'VERSION': version, 'OUTPUT': staged, 'PAYLOAD': include,
                       'ICON': ROOT / 'assets/branding/waybridge.ico'}
        prefix = '/' if os.name == 'nt' else '-'
        subprocess.run([executable, prefix + 'V2', prefix + 'WX',
                        *(f'{prefix}D{name}={value}' for name, value in definitions.items()),
                        str(ROOT / 'client/installer.nsi')], check=True, cwd=ROOT)
        staged.replace(output)
    print(f'Built {output} ({output.stat().st_size // (1024 * 1024)} MiB)')


if __name__ == '__main__':
    main()

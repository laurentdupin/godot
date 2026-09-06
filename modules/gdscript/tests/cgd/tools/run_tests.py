#!/usr/bin/env python3
"""Compile and run the exact engine frontend in a standalone harness (Python 3.9+)."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default='c++')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--build-dir', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir or Path(tempfile.mkdtemp(prefix='gdc-tests-'))
    build.mkdir(parents=True, exist_ok=True)
    include = root.parents[1]
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-fno-exceptions', '-fno-rtti', '-g']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    core = include / 'gdc_transpiler_core.cpp'
    for name, src in [('test_core', root / 'tests/test_core.cpp'), ('gdc', root / 'tools/gdc.cpp')]:
        subprocess.run([args.compiler, *flags, '-I' + str(include), str(core), str(src), '-o', str(build / name)], check=True)
    subprocess.run([str(build / 'test_core')], check=True)
    for source in sorted((root / 'examples').rglob('*.cgd')):
        subprocess.run([str(build / 'gdc'), str(source), str(build / (source.name + '.gd'))], check=True)
    print(f'Build and example outputs: {build}')


if __name__ == '__main__':
    main()

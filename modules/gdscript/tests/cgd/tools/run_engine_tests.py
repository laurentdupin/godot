#!/usr/bin/env python3
"""Run the bundled project using an already-built patched Godot editor/template.

This does not build Godot or download export templates. Each subprocess has a
finite timeout and tests require success markers as well as clean exit codes.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(command, marker=None, timeout=180):
    print('+', ' '.join(str(arg) for arg in command), flush=True)
    proc = subprocess.run([str(arg) for arg in command], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    print(proc.stdout, end='')
    if proc.returncode != 0 or 'SCRIPT ERROR:' in proc.stdout or 'Parse Error:' in proc.stdout or 'GD-C smoke failure:' in proc.stdout:
        raise RuntimeError(f'Command failed with exit status {proc.returncode}')
    if marker and marker not in proc.stdout:
        raise RuntimeError(f'Missing success marker {marker}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--godot', required=True, type=Path, help='Patched editor executable.')
    parser.add_argument('--template', type=Path, help='Patched template executable; also test all three pack export modes.')
    parser.add_argument('--native-tests', action='store_true', help='Editor was built with tests=yes; run [GDC] native unit cases too.')
    args = parser.parse_args()
    editor = args.godot.resolve(strict=True)
    template = args.template.resolve(strict=True) if args.template else None
    bundle = Path(__file__).resolve().parents[1]
    if args.native_tests:
        run([editor, '--headless', '--test', '--test-case=[GDC]*'])
    with tempfile.TemporaryDirectory(prefix='gdc-engine-tests-') as folder:
        work = Path(folder)
        project = work / 'project'
        shutil.copytree(bundle / 'examples/smoke', project)
        if os.name == 'nt':
            presets = project / 'export_presets.cfg'
            presets.write_text(presets.read_text(encoding='utf-8').replace('platform="Linux"', 'platform="Windows Desktop"'), encoding='utf-8')
        run([editor, '--headless', '--editor', '--path', project, '--import'])
        run([editor, '--headless', '--path', project, '--quit-after', '600'], 'GDC_SMOKE_OK')
        run([editor, '--headless', '--path', project, '--script', 'res://reload_test.gd', '--quit-after', '600'], 'GDC_RELOAD_OK')
        if template:
            for name in ('Text', 'Binary', 'Compressed'):
                pack = work / (name.lower() + '.pck')
                run([editor, '--headless', '--editor', '--path', project, '--export-pack', 'GD-C ' + name, pack])
                if not pack.is_file() or not pack.stat().st_size:
                    raise RuntimeError(f'Export did not create {pack}')
                # Test the normal deployed layout. Release builds may disable
                # --main-pack/path overrides, so put a matching PCK beside the executable.
                deployed = work / name.lower()
                deployed.mkdir()
                executable = deployed / template.name
                shutil.copy2(template, executable)
                main_executable = executable
                if template.name.endswith('.console.exe'):
                    main_source = template.with_name(template.name.replace('.console.exe', '.exe'))
                    main_executable = deployed / main_source.name
                    shutil.copy2(main_source, main_executable)
                shutil.copy2(pack, main_executable.with_suffix('.pck'))
                run([executable, '--headless'], 'GDC_SMOKE_OK')
            (project / 'broken.cgd').write_text('extends Node;\nvoid broken() {', encoding='utf-8')
            failed = subprocess.run([str(editor), '--headless', '--editor', '--path', str(project), '--export-pack', 'GD-C Binary', str(work / 'invalid.pck')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace', timeout=180)
            print(failed.stdout)
            if failed.returncode == 0 or 'GD-C: Unclosed delimiter.' not in failed.stdout:
                raise RuntimeError('Invalid GD-C source did not fail the export as expected')
            print('PASS: invalid GD-C source fails export.')
    print('PASS: requested engine integration tests completed.')


if __name__ == '__main__':
    main()

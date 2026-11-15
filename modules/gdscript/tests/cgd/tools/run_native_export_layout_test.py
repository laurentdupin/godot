#!/usr/bin/env python3
"""Export with custom cgd_native/export settings and run the exported game."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*args, expected_success=True):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace", timeout=300)
    if (result.returncode == 0) != expected_success:
        raise SystemExit(result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, required=True)
    parser.add_argument("--template", type=Path, required=True)
    options = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Run this test on Windows x86-64.")
    with tempfile.TemporaryDirectory(prefix="cgd-layout-") as folder:
        root = Path(folder)
        project = root / "project"
        project.mkdir()

        def write(name, value):
            (project / name).write_text(value, encoding="utf-8")

        write("export_presets.cfg", f'''[preset.0]
name="Layout"
platform="Windows Desktop"
export_filter="all_resources"
script_export_mode=2

[preset.0.options]
custom_template/release="{options.template.resolve().as_posix()}"
binary_format/architecture="x86_64"
debug/export_console_wrapper=2
application/modify_resources=false
''')
        write("main.tscn", '''[gd_scene format=3]
[ext_resource type="Script" path="res://main.gd" id="1"]
[node name="Main" type="Node"]
script = ExtResource("1")
''')
        write("main.gd", '''extends Node
func _ready():
    print("LAYOUT_VALUE=", load("res://probe.cgd").probe())
    get_tree().quit()
''')
        write("probe.cgd", "native {\nstatic int base(void) { return 40; }\n}\nstatic native int probe() { return base() + 2; }\n")

        def configure(settings):
            write("project.godot", 'config_version=5\n[application]\nrun/main_scene="res://main.tscn"\n'
                  '[rendering]\nrenderer/rendering_method="gl_compatibility"\n[cgd_native]\n' + settings)

        def export(name, expected_success=True):
            output = root / name
            output.mkdir()
            log = run(str(options.godot.resolve()), "--headless", "--path", str(project),
                      "--export-release", "Layout", str(output / "Layout.exe"), expected_success=expected_success)
            return output, log

        cases = (
            ("NextToExe", 'export/library_name="game_native"\nexport/directory=""\n', "game_native.dll"),
            ("Nested", 'export/directory="libs/native"\n', "libs/native/cgd_native.dll"),
        )
        for name, settings, expected in cases:
            configure(settings)
            output, _ = export(name)
            if not (output / expected).is_file():
                raise SystemExit(f"{name}: expected {expected}; exported files: {sorted(str(p.relative_to(output)) for p in output.rglob('*'))}")
            if expected != "cgd_native/cgd_native.dll" and (output / "cgd_native").exists():
                raise SystemExit(f"{name}: the default cgd_native folder was still created")
            result = run(str(output / "Layout.console.exe"), "--headless", "--quit-after", "30")
            if "LAYOUT_VALUE=42" not in result:
                raise SystemExit(f"{name}: the exported game did not load {expected}:\n{result}")

        configure('export/directory="../outside"\n')
        _, log = export("Invalid", expected_success=False)
        if "Invalid cgd_native/export/directory" not in log:
            raise SystemExit("An invalid export directory was not reported:\n" + log)
        print("CGD_EXPORT_LAYOUT_OK")


if __name__ == "__main__":
    main()

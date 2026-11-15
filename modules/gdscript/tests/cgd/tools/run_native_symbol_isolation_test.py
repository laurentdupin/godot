#!/usr/bin/env python3
"""Exercise CGD symbol isolation with a matching desktop export template."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*args, expected_success=True):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace", timeout=150)
    if (result.returncode == 0) != expected_success:
        raise SystemExit(result.stdout)
    return result.stdout


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--godot", type=Path, required=True)
    args.add_argument("--template", type=Path, required=True)
    options = args.parse_args()
    windows = sys.platform == "win32"
    if not windows and sys.platform != "darwin":
        args.error("Run this test on Windows x86-64 or macOS arm64.")
    platform, arch = ("Windows Desktop", "x86_64") if windows else ("macOS", "arm64")
    with tempfile.TemporaryDirectory(prefix="cgd-isolation-") as folder:
        root = Path(folder)
        project = root / "project"
        project.mkdir()

        def write(name, value):
            (project / name).write_text(value, encoding="utf-8")

        preset = f'''[preset.0]
name="Isolation"
platform="{platform}"
export_filter="all_resources"
script_export_mode=2

[preset.0.options]
custom_template/release="{options.template.resolve().as_posix()}"
binary_format/architecture="{arch}"
'''
        preset += ('debug/export_console_wrapper=2\napplication/modify_resources=false\n' if windows else
                   'application/bundle_identifier="org.godotengine.cgdisolation"\ncodesign/codesign=3\ncodesign/identity="-"\n')
        write("export_presets.cfg", preset)
        write("main.tscn", '''[gd_scene format=3]
[ext_resource type="Script" path="res://main.gd" id="1"]
[node name="Main" type="Node"]
script = ExtResource("1")
''')
        write("main.gd", '''extends Node
func _ready():
    var a = load("res://a.cgd")
    var b = load("res://b.cgd")
    var c = load("res://c.cgd")
    var d = load("res://d.cgd")
    print("ISOLATED=", [a.probe(), b.probe(), a.probe(), b.probe(), c.probe(), d.probe()])
    get_tree().quit()
''')
        for name, value in (("a", 0), ("b", 10)):
            write(f"{name}.cgd", f"""native {{
int counter;
int helper(void) {{ return ++counter + {value}; }}
}}
static native int probe() {{ return helper(); }}
""")
        for name, value in (("c", 41), ("d", 42)):
            write(f"{name}.cgd", f"""native cpp {{
struct Parser {{ int parse() {{ return {value}; }} }};
}}
static native cpp int probe() {{ return Parser{{}}.parse(); }}
""")

        def export(name, expected_success=True):
            dest = root / (name + (".exe" if windows else ".app"))
            output = run(str(options.godot.resolve()), "--headless", "--path", str(project),
                         "--export-release", "Isolation", str(dest), expected_success=expected_success)
            return dest, output

        for index, flag in enumerate(("/Od", "/O2") if windows else ("-O0", "-O2")):
            target = "windows/x86_64" if windows else "macos/arm64"
            write("project.godot", f'''config_version=5
[application]
run/main_scene="res://main.tscn"
[rendering]
renderer/rendering_method="gl_compatibility"
textures/vram_compression/import_etc2_astc=true
[cgd_native]
{target}/cxx_flags=PackedStringArray("{flag}")
''')
            dest, _ = export(f"Isolation{index}")
            binary = (dest.with_suffix(".console.exe") if windows else
                      next((dest / "Contents/MacOS").iterdir()))
            output = run(str(binary), "--headless", "--quit-after", "20")
            if "ISOLATED=[1, 11, 2, 12, 41, 42]" not in output:
                raise SystemExit(output)

        write("a.cgd", '''native {
int DllMain(void) { return 0; }
}
static native int probe() { return DllMain(); }
''')
        _, output = export("ReservedSymbol", expected_success=False)
        if "res://a.cgd: 2: Runtime entry point or replacement 'DllMain'" not in output:
            raise SystemExit(output)
        print("CGD_SYMBOL_ISOLATION_OK")


if __name__ == "__main__":
    main()

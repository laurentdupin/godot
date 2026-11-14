#!/usr/bin/env python3
"""Measure cold, incremental and warm Run preflight for five native scripts."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

FILES = {'project.godot': 'config_version=5\n[application]\nrun/main_scene="res://main.tscn"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n',
    'main.tscn': '[gd_scene format=3]\n[ext_resource type="Script" path="res://main.gd" id="1"]\n[node name="Main" type="Node"]\nscript = ExtResource("1")\n',
    'main.gd': 'extends Node\nfunc _ready():\n    var file = FileAccess.open("res://values.txt", FileAccess.READ_WRITE if FileAccess.file_exists("res://values.txt") else FileAccess.WRITE)\n    file.seek_end()\n    file.store_line(str(load("res://native0.cgd").value()))\n    file.close()\n    get_tree().quit()\n',
    'addons/check/plugin.cfg': '[plugin]\nname="Preflight probe"\nscript="plugin.gd"\n',
    'addons/check/plugin.gd': '@tool\nextends EditorPlugin\nfunc _enter_tree():\n    run_test.call_deferred()\nfunc run_test():\n    await get_tree().create_timer(1.0).timeout\n    while EditorInterface.get_resource_filesystem().is_scanning():\n        await get_tree().process_frame\n    for i in range(3):\n        if i == 1:\n            var path = "res://native0.cgd"\n            var source = FileAccess.get_file_as_string(path)\n            var file = FileAccess.open(path, FileAccess.WRITE)\n            file.store_string(source.replace("COMMON + 0", "COMMON + 10"))\n            file.close()\n        EditorInterface.play_main_scene()\n        for j in range(100):\n            await get_tree().create_timer(0.1).timeout\n            if not EditorInterface.is_playing_scene():\n                break\n    get_tree().quit()\n'}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cgd-preflight-") as folder:
        root = Path(folder)
        for name, contents in FILES.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        (root / "common.h").write_text("#define COMMON 1\n", encoding="utf-8")
        for index in range(5):
            (root / f"native{index}.cgd").write_text(
                'extends RefCounted;\nnative {\n#include "common.h"\n}\n'
                f'static native int value() {{ return COMMON + {index}; }}\n', encoding="utf-8")
        result = subprocess.run([str(args.godot.resolve()), "--headless", "--editor", "--verbose", "--path", str(root)],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=150)
        output = result.stdout + result.stderr
        times = re.findall(r"CGD native preflight: ([\d.]+) ms", output)
        values = root / "values.txt"
        if result.returncode or len(times) != 3 or output.count("CGD native export object cache hit:") != 9 or not values.exists() or values.read_text().splitlines() != ["1", "11", "11"]:
            raise SystemExit(output)
        print(f"CGD_PREFLIGHT_OK cold={times[0]} ms one_changed={times[1]} ms warm={times[2]} ms")


if __name__ == "__main__":
    main()

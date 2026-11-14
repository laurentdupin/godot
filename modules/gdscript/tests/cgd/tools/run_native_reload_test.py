#!/usr/bin/env python3
"""Verify CGD native reloads in the game process started by the editor debugger."""
import argparse
from pathlib import Path
import subprocess
import tempfile

SOURCE = """extends Node;
native int value() { static int calls = 0; return 10 + (++calls == 1 ? 0 : 1); }
int marker() { return 1; }
void _process(float delta) {
    FileAccess output = FileAccess.open("res://result.tmp", FileAccess.WRITE);
    output.store_string(str(marker()) + ":" + str(value()));
    output.close();
    DirAccess.rename_absolute("res://result.tmp", "res://result.txt");
}
"""
PLUGIN = r"""@tool
extends EditorPlugin

func _enter_tree():
    run_test.call_deferred()

func wait_for(expected):
    for attempt in range(120):
        await get_tree().create_timer(0.1).timeout
        if FileAccess.file_exists("res://result.txt") and FileAccess.get_file_as_string("res://result.txt") == expected:
            return true
    push_error("CGD_DEBUGGER_RELOAD_TIMEOUT " + expected)
    return false

func save_edit(script, source):
    script.source_code = source
    if ResourceSaver.save(script) != OK:
        return false
    var editor_node = EditorInterface.get_base_control()
    while editor_node != null and not editor_node.has_signal("resource_saved"):
        editor_node = editor_node.get_parent()
    if editor_node == null:
        return false
    editor_node.emit_signal("resource_saved", script)
    return true

func run_test():
    await get_tree().create_timer(1.0).timeout
    while EditorInterface.get_resource_filesystem().is_scanning():
        await get_tree().process_frame
    var script = load("res://main.cgd")
    EditorInterface.play_main_scene()
    var passed = await wait_for("1:11")
    if passed:
        passed = save_edit(script, "// CGD-only edit\n" + script.source_code.replace("return 1;", "return 2;"))
    if passed:
        passed = await wait_for("2:11")
    if passed:
        passed = save_edit(script, script.source_code.replace("return 10 +", "return 20 +"))
    if passed:
        passed = await wait_for("2:21")
    EditorInterface.stop_playing_scene()
    print("CGD_DEBUGGER_RELOAD_OK" if passed else "CGD_DEBUGGER_RELOAD_FAILED")
    get_tree().quit(0 if passed else 1)
"""

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cgd-debugger-reload-") as folder:
        root = Path(folder)
        addon = root / "addons/reload_probe"
        addon.mkdir(parents=True)
        (addon / "plugin.cfg").write_text('[plugin]\nname="Reload probe"\nscript="plugin.gd"\n', encoding="utf-8")
        (addon / "plugin.gd").write_text(PLUGIN, encoding="utf-8")
        (root / "main.cgd").write_text(SOURCE, encoding="utf-8")
        (root / "main.tscn").write_text('[gd_scene format=3]\n[ext_resource type="Script" path="res://main.cgd" id="1"]\n[node name="Main" type="Node"]\nscript = ExtResource("1")\n', encoding="utf-8")
        (root / "project.godot").write_text('config_version=5\n[application]\nrun/main_scene="res://main.tscn"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/reload_probe/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n', encoding="utf-8")
        result = subprocess.run([str(args.godot.resolve()), "--headless", "--editor", "--verbose", "--path", str(root)], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=80)
        output = result.stdout + result.stderr
        print(output)
        manifests = list((root / ".godot" / "cgd_native").glob("*/*/*/current"))
        generations = sum(len(list(manifest.parent.glob("build_*"))) for manifest in manifests)
        if result.returncode or "CGD_DEBUGGER_RELOAD_OK" not in output or len(manifests) != 2 or generations != 2:
            raise SystemExit(1)

if __name__ == "__main__":
    main()

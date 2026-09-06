#!/usr/bin/env python3
"""Exercise the real FileSystem creation menu and script dialog in a headless editor."""
import argparse
from pathlib import Path
import subprocess
import tempfile

PLUGIN = r'''@tool
extends EditorPlugin

func _enter_tree():
    run_test.call_deferred()

func descendants(node):
    var found = [node]
    for child in node.get_children(true):
        found.append_array(descendants(child))
    return found

func fail(message):
    push_error("CGD_CREATION_FAILED: " + message)
    get_tree().quit(1)

func run_test():
    for frame in range(30):
        await get_tree().process_frame
    var dock = EditorInterface.get_file_system_dock()
    var tree = null
    for node in descendants(dock):
        if node is Tree:
            tree = node
            break
    if tree == null:
        fail("FileSystem tree missing")
        return
    tree.emit_signal("empty_clicked", Vector2(10, 10), MOUSE_BUTTON_RIGHT)
    var selected = false
    for node in descendants(dock):
        if node is PopupMenu:
            for index in range(node.item_count):
                if "GD-C Script" in node.get_item_text(index):
                    node.emit_signal("id_pressed", node.get_item_id(index))
                    node.hide()
                    selected = true
                    break
        if selected:
            break
    if not selected:
        fail("GD-C creation menu entry missing")
        return
    await get_tree().process_frame
    var dialog = null
    for node in descendants(dock):
        if node.get_class() == "ScriptCreateDialog":
            dialog = node
            break
    if dialog == null or not dialog.visible:
        fail("Script dialog did not open")
        return
    var dialect_found = false
    var custom_template_found = false
    for node in descendants(dialog):
        if node is OptionButton and node.get_item_text(node.selected) == "GD-C (.cgd)":
            dialect_found = true
        if node is OptionButton and node.accessibility_name == "Template":
            for index in range(node.item_count):
                if "Custom CGD" in node.get_item_text(index):
                    custom_template_found = true
            for index in range(node.item_count):
                if "Node: Default" in node.get_item_text(index):
                    node.select(index)
                    node.emit_signal("item_selected", index)
        if node is LineEdit and node.text.ends_with(".cgd"):
            node.text = "res://created_from_menu.cgd"
            node.emit_signal("text_changed", node.text)
    if not dialect_found:
        fail("GD-C dialect not selected")
        return
    if not custom_template_found:
        fail("Project CGD template missing")
        return
    for frame in range(3):
        await get_tree().process_frame
    if dialog.get_ok_button().disabled:
        fail("Create is disabled")
        return
    dialog.get_ok_button().emit_signal("pressed")
    for frame in range(5):
        await get_tree().process_frame
    var path = "res://created_from_menu.cgd"
    if not FileAccess.file_exists(path):
        fail("Script file was not saved")
        return
    var source = FileAccess.get_file_as_string(path)
    if not source.begins_with("extends Node;") or "func " in source or "pass;" in source or "->" in source:
        fail("Wrong source dialect: " + source)
        return
    if "void _ready()" not in source or "void _process(float delta)" not in source:
        fail("C-style Node template was not created: " + source)
        return
    var script = load(path)
    if script == null or script.reload() != OK or script.get_instance_base_type() != "Node":
        fail("Created script does not compile")
        return
    print("CGD_EDITOR_CREATION_OK")
    get_tree().quit(0)
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--godot", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cgd-editor-creation-") as folder:
        root = Path(folder)
        addon = root / "addons/cgd_test"
        addon.mkdir(parents=True)
        (addon / "plugin.gd").write_text(PLUGIN, encoding="utf-8")
        (addon / "plugin.cfg").write_text('[plugin]\nname="CGD Creation Test"\ndescription=""\nauthor=""\nversion="1"\nscript="plugin.gd"\n', encoding="utf-8")
        (root / "project.godot").write_text('config_version=5\n[application]\nconfig/name="CGD Creation Test"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/cgd_test/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n', encoding="utf-8")
        templates = root / "script_templates/Node"
        templates.mkdir(parents=True)
        (templates.parent / ".gdignore").write_text("", encoding="utf-8")
        (templates / "custom.cgd").write_text("// meta-name: Custom CGD\n// meta-description: Project template\nextends _BASE_;\nvoid _ready() {}\n", encoding="utf-8")
        result = subprocess.run([str(args.godot.resolve()), "--headless", "--editor", "--path", str(root), "--quit-after", "300"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", timeout=120)
        print(result.stdout)
        if result.returncode or "CGD_EDITOR_CREATION_OK" not in result.stdout or "SCRIPT ERROR:" in result.stdout:
            raise SystemExit(1)
        (root / "verify.gd").write_text('extends SceneTree\nfunc _initialize():\n\tvar script = load("res://created_from_menu.cgd")\n\tvar instance = script.new()\n\tif not instance is Node:\n\t\tquit(1)\n\t\treturn\n\tinstance.free()\n\tprint("CGD_CREATED_SCRIPT_RUNS")\n\tquit()\n', encoding="utf-8")
        runtime = subprocess.run([str(args.godot.resolve()), "--headless", "--path", str(root), "--script", "res://verify.gd"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", timeout=60)
        print(runtime.stdout)
        if runtime.returncode or "CGD_CREATED_SCRIPT_RUNS" not in runtime.stdout or "SCRIPT ERROR:" in runtime.stdout:
            raise SystemExit(1)

if __name__ == "__main__":
    main()

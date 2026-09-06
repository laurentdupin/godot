#!/usr/bin/env python3
"""Exercise CGD autocomplete with incomplete buffers in the actual script editor."""
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
    push_error("CGD_EDITING_FAILED: " + message)
    get_tree().quit(1)

func run_test():
    for frame in range(30):
        await get_tree().process_frame
    var script = load("res://node_3d.cgd")
    EditorInterface.edit_script(script)
    for frame in range(5):
        await get_tree().process_frame
    var editor = EditorInterface.get_script_editor().get_current_editor()
    if editor == null or not editor.get_base_editor() is CodeEdit:
        push_error("CGD_EDITING_FAILED: Script editor missing")
        get_tree().quit(1)
        return
    var code = editor.get_base_editor()
    var original = code.text
    code.text = "extends Node3D;\nvoid _process(float delta) {\n    posit\n}"
    code.set_caret_line(2)
    code.set_caret_column(9)
    code.request_code_completion(true)
    await get_tree().process_frame
    var position_found = false
    for option in code.get_code_completion_options():
        position_found = position_found or option.display_text == "position"
    if not position_found:
        fail("Autocomplete popup does not contain position")
        return
    for source in ["extends Node3D;\nvoid _ready(", "extends Node3D;\nvoid _ready() {\nprint(\"unfinished", "extends Node3D;\nvoid _ready() {\nint value ="]:
        code.text = source
        code.set_caret_line(code.get_line_count() - 1)
        code.set_caret_column(code.get_line(code.get_caret_line()).length())
        code.request_code_completion(true)
        await get_tree().process_frame
    code.text = original
    code.request_code_completion(true)
    for frame in range(5):
        await get_tree().process_frame
    EditorInterface.open_scene_from_path("res://test.tscn")
    for frame in range(5):
        await get_tree().process_frame
    var root = EditorInterface.get_edited_scene_root()
    EditorInterface.get_selection().clear()
    EditorInterface.get_selection().add_node(root)
    for frame in range(3):
        await get_tree().process_frame
    for node in descendants(EditorInterface.get_base_control()):
        if node is Button and node.tooltip_text.begins_with("Attach a new or existing script"):
            node.emit_signal("pressed")
            break
    await get_tree().process_frame
    var dialog = null
    for node in descendants(EditorInterface.get_base_control()):
        if node.get_class() == "ScriptCreateDialog" and node.visible:
            dialog = node
            break
    if dialog == null:
        fail("Attach Script dialog did not open")
        return
    for node in descendants(dialog):
        if node is OptionButton:
            for index in range(node.item_count):
                if node.get_item_text(index) == "GDScript":
                    node.select(index)
                    node.emit_signal("item_selected", index)
        if node is CheckBox and node.accessibility_name == "Built-in Script:" and node.button_pressed:
            node.button_pressed = false
            node.emit_signal("pressed")
    for node in descendants(dialog):
        if node is Button and node.accessibility_name == "Select File":
            node.emit_signal("pressed")
            break
    await get_tree().process_frame
    var picker = null
    for node in descendants(dialog):
        if node.get_class() == "EditorFileDialog" and node.visible:
            picker = node
            break
    if picker == null or not "*.cgd" in str(picker.filters) or not "*.gd" in str(picker.filters):
        fail("Open Script filter does not include both gd and cgd")
        return
    picker.emit_signal("file_selected", "res://node_3d.cgd")
    picker.hide()
    for frame in range(3):
        await get_tree().process_frame
    if dialog.get_ok_button().disabled:
        fail("Selecting CGD from the GDScript dialog did not enable attachment")
        return
    dialog.get_ok_button().emit_signal("pressed")
    for frame in range(5):
        await get_tree().process_frame
    if root.get_script() == null or root.get_script().resource_path != "res://node_3d.cgd":
        fail("CGD script was not attached to the node")
        return
    print("CGD_EDITOR_ATTACHMENT_OK")
    var plugin_dialog = null
    for node in descendants(EditorInterface.get_base_control()):
        if node.get_class() == "PluginConfigDialog":
            plugin_dialog = node
            break
    if plugin_dialog == null:
        fail("Plugin creation dialog missing")
        return
    plugin_dialog.popup_centered()
    for node in descendants(plugin_dialog):
        if node is LineEdit and node.accessibility_name == "Plugin Name:":
            node.text = "CGD Test Plugin"
            node.emit_signal("text_changed", node.text)
        if node is OptionButton and node.accessibility_name == "Language:":
            for index in range(node.item_count):
                if node.get_item_text(index) == "GD-C (.cgd)":
                    node.select(index)
                    node.emit_signal("item_selected", index)
    for frame in range(3):
        await get_tree().process_frame
    if plugin_dialog.get_ok_button().disabled:
        fail("CGD plugin creation is disabled")
        return
    plugin_dialog.get_ok_button().emit_signal("pressed")
    for frame in range(10):
        await get_tree().process_frame
    var plugin_path = "res://addons/cgd_test_plugin/cgd_test_plugin.cgd"
    if not FileAccess.file_exists(plugin_path):
        fail("Plugin wizard did not create CGD source")
        return
    var created_plugin = load(plugin_path)
    if created_plugin == null or created_plugin.reload() != OK or created_plugin.get_instance_base_type() != "EditorPlugin":
        fail("Generated CGD plugin did not compile")
        return
    print("CGD_EDITOR_PLUGIN_CREATION_OK")
    print("CGD_EDITOR_EDITING_OK")
    get_tree().quit(0)
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--godot", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cgd-editor-editing-") as folder:
        root = Path(folder)
        addon = root / "addons/cgd_test"
        addon.mkdir(parents=True)
        (addon / "plugin.gd").write_text(PLUGIN, encoding="utf-8")
        (addon / "plugin.cfg").write_text('[plugin]\nname="CGD Editing Test"\ndescription=""\nauthor=""\nversion="1"\nscript="plugin.gd"\n', encoding="utf-8")
        (root / "project.godot").write_text('config_version=5\n[application]\nconfig/name="CGD Editing Test"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/cgd_test/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n', encoding="utf-8")
        (root / "node_3d.cgd").write_text("extends Node3D;\n\nvoid _ready() {\n}\n\nvoid _process(float delta) {\n}\n", encoding="utf-8")
        (root / "test.tscn").write_text('[gd_scene format=3]\n[node name="Test" type="Node3D"]\n', encoding="utf-8")
        result = subprocess.run([str(args.godot.resolve()), "--headless", "--editor", "--path", str(root), "--quit-after", "300"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", timeout=120)
        print(result.stdout)
        if result.returncode or "CGD_EDITOR_EDITING_OK" not in result.stdout or "CGD_EDITOR_ATTACHMENT_OK" not in result.stdout or "SCRIPT ERROR:" in result.stdout:
            raise SystemExit(1)


if __name__ == "__main__":
    main()

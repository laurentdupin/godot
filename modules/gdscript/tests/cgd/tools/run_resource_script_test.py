#!/usr/bin/env python3
"""Exercise the Inspector script picker on Shader and other Resources."""
import argparse
from pathlib import Path
import subprocess
import tempfile

PLUGIN = r'''@tool
extends EditorPlugin

var target_resource

func _enter_tree():
    run_test.call_deferred()

func descendants(node):
    var found = [node]
    for child in node.get_children(true):
        found.append_array(descendants(child))
    return found

func fail(message):
    push_error("RESOURCE_SCRIPT_FAILED: " + message)
    get_tree().quit(1)

func run_test():
    for frame in range(30):
        await get_tree().process_frame
    for resource in [load("res://cube.gdshader"), ShaderMaterial.new(), Resource.new()]:
        if not await test_resource(resource):
            return
    # Opening a built-in script whose owner is a shader reenters main-editor selection.
    var legacy = GDScript.new()
    legacy.resource_path = "res://cube.gdshader::GDScript_reentry"
    EditorInterface.inspect_object(legacy)
    for frame in range(10):
        await get_tree().process_frame
    print("RESOURCE_SCRIPT_REENTRY_OK")
    print("RESOURCE_SCRIPT_CGD_OK")
    get_tree().quit(0)

func test_resource(resource):
    target_resource = resource
    EditorInterface.inspect_object(target_resource)
    for frame in range(5):
        await get_tree().process_frame
    var picker = null
    for node in descendants(EditorInterface.get_inspector()):
        if node is EditorResourcePicker and node.base_type == "Script":
            picker = node
            break
    if picker == null:
        fail("Script picker missing")
        return false
    for child in picker.get_children():
        if child is Button and child.tooltip_text == "":
            child.emit_signal("pressed")
    var menu = null
    for child in descendants(picker):
        if child is PopupMenu:
            menu = child
            break
    if menu == null:
        fail("Script menu missing")
        return false
    var selected = false
    for index in range(menu.item_count):
        if menu.get_item_text(index) == "New Script...":
            menu.emit_signal("id_pressed", menu.get_item_id(index))
            menu.hide()
            selected = true
            break
    if not selected:
        fail("New Script item missing")
        return false
    await get_tree().process_frame
    var dialog = null
    for node in descendants(picker):
        if node.get_class() == "ScriptCreateDialog" and node.visible:
            dialog = node
            break
    if dialog == null:
        fail("Resource script creation dialog missing")
        return false
    var cgd_found = false
    for node in descendants(dialog):
        if node is OptionButton:
            for index in range(node.item_count):
                if node.get_item_text(index) == "GD-C (.cgd)":
                    node.select(index)
                    node.emit_signal("item_selected", index)
                    cgd_found = true
    var path = "res://" + resource.get_class().to_snake_case() + "_behavior.cgd"
    for node in descendants(dialog):
        if node is LineEdit and node.accessibility_name == "Path:":
            node.text = path
            node.emit_signal("text_changed", path)
    for frame in range(3):
        await get_tree().process_frame
    if not cgd_found or dialog.get_ok_button().disabled:
        fail("CGD creation unavailable for " + resource.get_class())
        return false
    dialog.get_ok_button().emit_signal("pressed")
    for frame in range(10):
        await get_tree().process_frame
    var script = resource.get_script()
    if script == null or script.resource_path != path or script.get_instance_base_type() != resource.get_class():
        fail("Wrong script attachment for " + resource.get_class())
        return false
    var save_path = "res://" + resource.get_class().to_snake_case() + "_scripted.tres"
    if ResourceSaver.save(resource, save_path) != OK:
        fail("Could not save scripted resource")
        return false
    var reloaded = ResourceLoader.load(save_path, "", ResourceLoader.CACHE_MODE_IGNORE)
    if reloaded.get_script() == null or reloaded.get_script().resource_path != path:
        fail("Script attachment was lost on reload")
        return false
    EditorInterface.inspect_object(resource)
    for frame in range(5):
        await get_tree().process_frame
    for node in descendants(EditorInterface.get_inspector()):
        if node is EditorResourcePicker and node.base_type == "Script":
            picker = node
            break
    for child in picker.get_children():
        if child is Button and child.tooltip_text == "":
            child.emit_signal("pressed")
    for child in descendants(picker):
        if child is PopupMenu:
            menu = child
            break
    selected = false
    for index in range(menu.item_count):
        if menu.get_item_text(index) == "Extend Script...":
            menu.emit_signal("id_pressed", menu.get_item_id(index))
            menu.hide()
            selected = true
            break
    if not selected:
        fail("Extend Script item missing")
        return false
    await get_tree().process_frame
    for node in descendants(picker):
        if node.get_class() == "ScriptCreateDialog" and node.visible:
            dialog = node
            break
    for frame in range(3):
        await get_tree().process_frame
    if dialog.get_ok_button().disabled:
        fail("Extending resource script is disabled")
        return false
    dialog.get_ok_button().emit_signal("pressed")
    for frame in range(10):
        await get_tree().process_frame
    script = resource.get_script()
    if script == null or script.get_base_script() == null or script.get_base_script().resource_path != path:
        fail("Resource script inheritance was not preserved")
        return false
    print("RESOURCE_SCRIPT_ATTACHED_AND_RELOADED: ", resource.get_class())
    return true
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--godot", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cgd-resource-script-") as folder:
        root = Path(folder)
        addon = root / "addons/test"
        addon.mkdir(parents=True)
        (addon / "plugin.gd").write_text(PLUGIN, encoding="utf-8")
        (addon / "plugin.cfg").write_text('[plugin]\nname="Resource Script Test"\ndescription=""\nauthor=""\nversion="1"\nscript="plugin.gd"\n', encoding="utf-8")
        (root / "project.godot").write_text('config_version=5\n[application]\nconfig/name="Resource Script Test"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/test/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n', encoding="utf-8")
        (root / "cube.gdshader").write_text("shader_type spatial;\nvoid fragment() { ALBEDO = vec3(1.0); }\n", encoding="utf-8")
        result = subprocess.run([str(args.godot.resolve()), "--headless", "--editor", "--path", str(root), "--quit-after", "300"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", timeout=120)
        print(result.stdout)
        print("Editor exit code:", result.returncode)
        if result.returncode or "RESOURCE_SCRIPT_CGD_OK" not in result.stdout or "SCRIPT ERROR:" in result.stdout:
            raise SystemExit(1)


if __name__ == "__main__":
    main()

import subprocess
import sys
import tempfile
from pathlib import Path

with tempfile.TemporaryDirectory(prefix="cgd-check-") as folder:
    root = Path(folder)
    addon = root / "addons" / "check"
    addon.mkdir(parents=True)
    (root / "project.godot").write_text('config_version=5\n[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
    (addon / "plugin.cfg").write_text('[plugin]\nname="Check probe"\nscript="plugin.gd"\n')
    (root / "main.cgd").write_text('extends Node;\nnative int value() { return 12; }\n')
    (root / "broken.cgd").write_text('extends Node;\nnative int value() { return project_check_bad; }\n')
    # Restore broken.cgd as a background tab: its editor isn't enabled until shown,
    # so Check All routes a native error to an editor without gutters yet.
    (root / ".godot" / "editor").mkdir(parents=True)
    (root / ".godot" / "editor" / "editor_layout.cfg").write_text('[ScriptEditor]\n\nopen_scripts=["res://broken.cgd", "res://mention.cgd"]\nselected_script="res://mention.cgd"\n')
    # Mentions the word only (a comment and a path): not native code.
    (root / "mention.cgd").write_text('extends Node;\n// Wraps the native helper.\nconst auto HELPER = "res://native_helper.cgd";\n')
    (addon / "plugin.gd").write_text(r"""@tool
extends EditorPlugin
func _enter_tree():
    run_test.call_deferred()
func run_test():
    await get_tree().create_timer(1).timeout
    print("CGD_OPEN_SCRIPTS ", EditorInterface.get_script_editor().get_open_scripts().map(func(s): return s.resource_path))
    # Regaining focus re-analyzes native scripts, including tabs never shown.
    get_tree().root.propagate_notification(NOTIFICATION_APPLICATION_FOCUS_IN)
    await get_tree().create_timer(3).timeout
    EditorInterface.set_main_screen_editor("Script")
    EditorInterface.edit_script(load("res://mention.cgd"))
    await get_tree().create_timer(1).timeout
    var mention_hidden = compile_button() == null
    EditorInterface.edit_script(load("res://main.cgd"))
    await get_tree().create_timer(1).timeout
    EditorInterface.get_editor_settings().set_setting("text_editor/cgd/live_native_diagnostics", false)
    var script_editor = EditorInterface.get_script_editor().get_current_editor()
    var code = script_editor.get_base_editor()
    code.text = "extends Node;\nnative int value() { return real_check_bad; }\n"
    code.text_changed.emit()
    await get_tree().create_timer(2).timeout
    var toolbar = false
    var button = compile_button()
    if button:
        toolbar = true
        button.pressed.emit()
    await get_tree().create_timer(2).timeout
    var positioned = false
    for child in script_editor.find_children("*", "RichTextLabel", true, false):
        var text = child.get_parsed_text()
        positioned = positioned or ("Line 2, column 29:" in text and "real_check_bad" in text)
    var found_check_all = false
    for menu in EditorInterface.get_script_editor().find_children("*", "PopupMenu", true, false):
        for i in menu.item_count:
            if menu.get_item_text(i) == "Check All Native Scripts":
                menu.id_pressed.emit(menu.get_item_id(i))
                found_check_all = true
                break
    await get_tree().create_timer(3).timeout
    # The background tab shows the stored Check All error once it is opened.
    EditorInterface.edit_script(load("res://broken.cgd"))
    await get_tree().create_timer(1).timeout
    var restored = false
    for child in EditorInterface.get_script_editor().get_current_editor().find_children("*", "RichTextLabel", true, false):
        restored = restored or "project_check_bad" in child.get_parsed_text()
    print("CGD_CHECK_TOOLBAR ", toolbar, " POSITIONED ", positioned, " CHECK_ALL ", found_check_all, " MENTION_HIDDEN ", mention_hidden, " RESTORED ", restored)
    EditorInterface.get_editor_settings().set_setting("text_editor/cgd/live_native_diagnostics", true)
    get_tree().quit(0 if toolbar and positioned and found_check_all and mention_hidden and restored else 1)
func compile_button():
    for button in EditorInterface.get_script_editor().find_children("*", "Button", true, false):
        if button.text == "Compile" and button.is_visible_in_tree():
            return button
    return null
""")
    result = subprocess.run([str(Path(sys.argv[1]).resolve()), "--headless", "--editor", "--verbose", "--path", str(root)],
                            capture_output=True, encoding="utf-8", errors="replace", timeout=40)
    output = result.stdout + result.stderr
    print(output)
    if (result.returncode or "project_check_bad" not in output or "p_gutter = -1" in output
            or "CGD_CHECK_TOOLBAR true POSITIONED true CHECK_ALL true MENTION_HIDDEN true RESTORED true" not in output):
        raise SystemExit(1)

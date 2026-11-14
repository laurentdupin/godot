import sys, tempfile, subprocess
from pathlib import Path
with tempfile.TemporaryDirectory(prefix="cgd-analysis-") as folder:
    root = Path(folder)
    addon = root / "addons" / "check"
    addon.mkdir(parents=True)
    (root / "project.godot").write_text('config_version=5\n[editor_plugins]\nenabled=PackedStringArray("res://addons/check/plugin.cfg")\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
    (addon / "plugin.cfg").write_text('[plugin]\nname="Analysis probe"\nscript="plugin.gd"\n')
    (root / "main.cgd").write_text('extends Node;\nnative {\n#include "probe.h"\n}\nnative int value() { return MAGIC; }\n')
    (root / "probe.h").write_text('#define MAGIC 12\n')
    (addon / "plugin.gd").write_text(r"""@tool
extends EditorPlugin
func _enter_tree():
    run_test.call_deferred()
func run_test():
    await get_tree().create_timer(1).timeout
    EditorInterface.edit_script(load("res://main.cgd"))
    await get_tree().create_timer(3).timeout
    var script_editor = EditorInterface.get_script_editor().get_current_editor()
    var code = script_editor.get_base_editor()
    code.text = "// CGD-only edit\n" + code.text
    code.text_changed.emit()
    await get_tree().create_timer(3).timeout
    var header = FileAccess.open("res://probe.h", FileAccess.WRITE)
    header.store_string("#define MAGIC missing_symbol\n")
    header.close()
    script_editor.notification(NOTIFICATION_APPLICATION_FOCUS_IN)
    await get_tree().create_timer(3).timeout
    var has_error = false
    for child in script_editor.find_children("*", "RichTextLabel", true, false):
        has_error = has_error or "missing_symbol" in child.get_parsed_text()
    print("CGD_HEADER_REFRESH ", has_error)
    code.text = "extends Node;\nnative {\n#define strcmp missing_in_wrapper\n}\nnative int value() { return 12; }\n"
    code.text_changed.emit()
    await get_tree().create_timer(3).timeout
    var generated_error = false
    for child in script_editor.find_children("*", "RichTextLabel", true, false):
        generated_error = generated_error or "In generated wrapper for value:" in child.get_parsed_text()
    print("CGD_GENERATED_ERROR ", generated_error)
    get_tree().quit(0 if has_error and generated_error else 1)
""")
    result = subprocess.run([str(Path(sys.argv[1]).resolve()), "--headless", "--editor", "--verbose", "--path", str(root)],
                            capture_output=True, encoding="utf-8", errors="replace", timeout=35)
    output = result.stdout + result.stderr
    print(output)
    if result.returncode or output.count("Analyzing CGD native C: res://main.cgd") != 3 or "CGD native analysis cache hit: res://main.cgd" not in output:
        raise SystemExit(1)

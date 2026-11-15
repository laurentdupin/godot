#!/usr/bin/env python3
"""Check native console output from source, editor tools, and a packaged game."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

C_SOURCE = '''static native void log_value() {
    cgd_print("value=%d", 42);
    cgd_print_warning("native warning");
    cgd_print_error("native error");
    cgd_print("%s", "LONG_MESSAGE");
}
'''
CPP_SOURCE = '''native cpp {
#include <thread>
#include <vector>
}
static native cpp void log_values(bool stress) {
    cgd::print("n=", 3, " x=", 1.5, " ", std::string("ok"));
    cgd::print_warning("cpp warning");
    cgd::print_error("cpp error");
    if (!stress) return;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([worker] {
            for (int index = 0; index < 1000; ++index) cgd_print("worker=%d index=%d", worker, index);
        });
    }
    for (auto &worker : workers) worker.join();
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, required=True)
    parser.add_argument("--template", type=Path)
    args = parser.parse_args()
    godot = str(args.godot.resolve())
    windows = sys.platform == "win32"
    long_message = "long:" + "x" * 1600

    def run(*command):
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                encoding="utf-8", errors="replace", timeout=180)
        if result.returncode:
            raise SystemExit(result.stdout)
        return result.stdout

    def verify(output, stress=False):
        expected = ("value=42", "n=3 x=1.5 ok", long_message, "WARNING: native warning",
                    "ERROR: native error", "res://logger.cgd:3", "res://logger.cgd:4",
                    "WARNING: cpp warning", "ERROR: cpp error", "res://cpp.cgd:7", "res://cpp.cgd:8")
        missing = [message for message in expected if message not in output]
        if missing:
            raise SystemExit(f"Missing {missing!r}\n{output[:4000]}")
        if stress:
            lines = re.findall(r"^worker=(\d+) index=(\d+)$", output, re.MULTILINE)
            if len(lines) != 8000 or len(set(lines)) != 8000:
                raise SystemExit("Missing or interleaved worker messages")

    with tempfile.TemporaryDirectory(prefix="cgd-logging-") as directory:
        root = Path(directory)
        project = root / "project"
        project.mkdir()

        def write(path, text):
            (project / path).write_text(text, encoding="utf-8")

        config = '''config_version=5
[application]
run/main_scene="res://main.tscn"
[rendering]
renderer/rendering_method="gl_compatibility"
textures/vram_compression/import_etc2_astc=true
'''
        write("project.godot", config)
        write("logger.cgd", C_SOURCE.replace("LONG_MESSAGE", long_message))
        write("cpp.cgd", CPP_SOURCE)
        write("main.tscn", '''[gd_scene format=3]
[ext_resource type="Script" path="res://main.gd" id="1"]
[node name="Main" type="Node"]
script = ExtResource("1")
''')
        write("main.gd", '''extends Node
func _ready():
    load("res://logger.cgd").log_value()
    load("res://cpp.cgd").log_values(OS.get_cmdline_user_args().has("--stress"))
    get_tree().quit()
''')
        verify(run(godot, "--headless", "--path", directory + "/project", "--", "--stress"), stress=True)
        if args.template:
            platform, arch = ("Windows Desktop", "x86_64") if windows else ("macOS", "arm64")
            settings = ('debug/export_console_wrapper=2\napplication/modify_resources=false\n' if windows else
                        'application/bundle_identifier="org.godotengine.cgdlogging"\ncodesign/codesign=3\ncodesign/identity="-"\n')
            write("export_presets.cfg", f'''[preset.0]
name="Logging"
platform="{platform}"
export_filter="all_resources"
include_filter=""
exclude_filter=""
script_export_mode=2
[preset.0.options]
custom_template/release="{args.template.resolve().as_posix()}"
binary_format/architecture="{arch}"
{settings}''')
            app = root / ("Logging.exe" if windows else "Logging.app")
            run(godot, "--headless", "--path", str(project), "--export-release", "Logging", str(app))
            binary = app.with_suffix(".console.exe") if windows else next((app / "Contents/MacOS").iterdir())
            log = root / "export.log"
            run(str(binary), "--headless", "--log-file", str(log), "--quit-after", "30")
            verify(log.read_text(encoding="utf-8"))

        addon = project / "addons/probe"
        addon.mkdir(parents=True)
        write("project.godot", config + '[editor_plugins]\nenabled=PackedStringArray("res://addons/probe/plugin.cfg")\n')
        write("addons/probe/plugin.cfg", '[plugin]\nname="CGD logging probe"\nscript="probe.cgd"\n')
        write("addons/probe/probe.cgd", '''@tool
extends EditorPlugin;
void _enter_tree() { _run.call_deferred(); }
void _run() {
    await get_tree().create_timer(1).timeout;
    while (EditorInterface.get_resource_filesystem().is_scanning()) {
        await get_tree().process_frame;
    }
    native_log();
    print("CGD_TOOL_LOG_OK");
    EditorInterface.play_main_scene();
    await get_tree().create_timer(0.5).timeout;
    while (EditorInterface.is_playing_scene()) {
        await get_tree().process_frame;
    }
    get_tree().quit();
}
native void native_log() { cgd_print("tool-native=%d", 42); }
''')
        output = run(godot, "--headless", "--editor", "--verbose", "--path", str(project))
        for expected in ("tool-native=42", "CGD_TOOL_LOG_OK", "Linking CGD native Run library"):
            if expected not in output:
                raise SystemExit(output)
        print("CGD_NATIVE_LOGGING_OK")


if __name__ == "__main__":
    main()

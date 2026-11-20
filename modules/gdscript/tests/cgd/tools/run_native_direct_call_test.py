#!/usr/bin/env python3
"""Check direct calls to `static native` functions: results, fallbacks, reload, and that they skip the wrapper."""
import argparse
from pathlib import Path
import subprocess
import tempfile

MATH_V1 = '''extends RefCounted;
class_name NativeMath;

static native int add(int a, int b) {
    return a + b;
}

static native float scale(float value, float factor) {
    return value * factor;
}

static native int version() {
    return 1;
}

native {
static int64_t bump_count = 0;
}

static native void bump() {
    bump_count++;
}

static native int bumps() {
    return bump_count;
}

static native bool is_even(int value) {
    return value % 2 == 0;
}

static int add_one(int value) {
    return add(value, 1);
}
'''

OTHER = '''extends RefCounted;

const auto MATH = preload("res://math.cgd");

static int read_version() {
    return MATH.version();
}
'''

RUNNER = '''extends SceneTree;

const auto MATH = preload("res://math.cgd");
const auto OTHER = preload("res://other.cgd");

bool failed = false;

void check(bool condition, String what) {
    if (!condition) {
        failed = true;
        push_error("CGD_DIRECT_CALL_FAILED: " + what);
    }
}

void _initialize() {
    check(MATH.add(2, 3) == 5, "preloaded constant call");
    check(NativeMath.add(4, 5) == 9, "class_name call");
    check(is_equal_approx(MATH.scale(2, 1.5), 3.0), "int argument for a float parameter");
    check(MATH.add_one(1) == 2, "unqualified call inside the native script");
    Variant untyped = 7;
    check(MATH.add(untyped, 1) == 8, "untyped argument through the wrapper");
    check(Callable(MATH, "add").call(1, 1) == 2, "call by name through the wrapper");
    check(MATH.version() == 1 && OTHER.read_version() == 1, "version before reload");
    MATH.bump();
    MATH.bump();
    check(MATH.bumps() == 2, "void function through the typed call");
    check(MATH.is_even(4) && !MATH.is_even(7), "bool function through the typed call");
    Variant untyped_even = 6;
    check(MATH.is_even(untyped_even), "bool function through the wrapper");

    // Direct calls must skip the wrapper, which costs a full script call.
    const int N = 200000;
    int direct_total = 0;
    int started = Time.get_ticks_usec();
    for (int i = 0; i < N; i++) { direct_total += MATH.add(i, 1); }
    int direct_us = Time.get_ticks_usec() - started;
    int wrapper_total = 0;
    started = Time.get_ticks_usec();
    for (int i = 0; i < N; i++) { Variant value = i; wrapper_total += MATH.add(value, 1); }
    int wrapper_us = Time.get_ticks_usec() - started;
    print("CGD_DIRECT_CALL_TIMING direct_us=", direct_us, " wrapper_us=", wrapper_us, " total=", direct_total);
    check(direct_total == wrapper_total, "typed and wrapper calls return the same results");
    check(direct_us < wrapper_us * 0.85, "direct calls are not faster than wrapper calls");

    // Reload only the native script: callers compiled earlier must reach the new code.
    String source = FileAccess.get_file_as_string("res://math_v2.cgd.txt");
    FileAccess file = FileAccess.open("res://math.cgd", FileAccess.WRITE);
    file.store_string(source);
    file.close();
    Script math = load("res://math.cgd");
    math.source_code = source;
    check(math.reload(true) == OK, "reload of the native script");
    check(MATH.version() == 2, "direct call in this script after reload");
    check(OTHER.read_version() == 2, "direct call in another script after reload");
    check(MATH.add(20, 22) == 42, "unchanged function after reload");

    if (!failed) {
        print("CGD_DIRECT_CALL_OK");
    }
    quit(failed ? 1 : 0);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, required=True)
    args = parser.parse_args()
    godot = str(args.godot.resolve())

    with tempfile.TemporaryDirectory(prefix="cgd-direct-call-") as directory:
        project = Path(directory)
        (project / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        (project / "math.cgd").write_text(MATH_V1, encoding="utf-8")
        (project / "math_v2.cgd.txt").write_text(MATH_V1.replace("return 1;", "return 2;"), encoding="utf-8")
        (project / "other.cgd").write_text(OTHER, encoding="utf-8")
        (project / "runner.cgd").write_text(RUNNER, encoding="utf-8")

        def run(*arguments):
            return subprocess.run([godot, "--headless", "--path", str(project), *arguments], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, encoding="utf-8", errors="replace", timeout=300)

        # Register the class_name in the global class cache.
        imported = run("--import")
        if imported.returncode:
            raise SystemExit(imported.stdout)
        result = run("--script", "res://runner.cgd")
        print(result.stdout)
        if result.returncode or "CGD_DIRECT_CALL_OK" not in result.stdout or "CGD_DIRECT_CALL_FAILED" in result.stdout:
            raise SystemExit(1)


if __name__ == "__main__":
    main()

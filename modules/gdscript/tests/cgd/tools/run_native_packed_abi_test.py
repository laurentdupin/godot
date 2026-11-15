#!/usr/bin/env python3
"""Round-trip native C packed inputs and outputs in the engine."""
import argparse
from pathlib import Path
import subprocess
import tempfile

TYPES = (
    ("PackedByteArray", "1, 255"),
    ("PackedInt32Array", "-2000000000, 17"),
    ("PackedInt64Array", "-5000000000, 5000000000"),
    ("PackedFloat32Array", "1.25, -2.5"),
    ("PackedFloat64Array", "0.125, -3.25"),
    ("PackedVector2Array", "Vector2(1, 2), Vector2(-3, 4)"),
    ("PackedVector3Array", "Vector3(1, 2, 3), Vector3(-4, 5, 6)"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cgd-packed-abi-") as folder:
        root = Path(folder)
        (root / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        source = ["extends RefCounted;"]
        driver = ["extends SceneTree", 'const Native = preload("res://native.cgd")', "func _initialize():"]
        for index, (array, values) in enumerate(TYPES):
            source.append(f"""static native {array} copy_{index}({array} value) {{
    void *result = cgd_out_alloc(cgd_out, value.size);
    if (result) {{
        memcpy(result, value.data, (size_t)value.size * sizeof(*value.data));
    }}
}}""")
            driver.append(f"    var a{index} = {array}([{values}])")
            driver.append(f"    assert(Native.copy_{index}(a{index}) == a{index})")
            driver.append(f"    assert(Native.copy_{index}({array}()).is_empty())")
        source.append("""static native String copy_string(String value) {
    char *result = cgd_out_alloc(cgd_out, value.length);
    if (result) {
        memcpy(result, value.utf8, (size_t)value.length);
    }
}
static native PackedByteArray replace_output() {
    uint8_t *first = cgd_out_alloc(cgd_out, 2);
    if (first) {
        first[0] = 1;
        first[1] = 2;
    }
    uint8_t *second = cgd_out_alloc(cgd_out, 1);
    if (second) {
        second[0] = 9;
    }
}""")
        driver.extend(['    assert(Native.copy_string("é日本語🙂") == "é日本語🙂")',
                       '    assert(Native.copy_string("").is_empty())',
                       '    assert(Native.replace_output() == PackedByteArray([9]))',
                       '    print("CGD_PACKED_ABI_OK")', '    quit()'])
        (root / "native.cgd").write_text("\n".join(source) + "\n", encoding="utf-8")
        (root / "driver.gd").write_text("\n".join(driver) + "\n", encoding="utf-8")
        run = subprocess.run([str(args.godot.resolve()), "--headless", "--path", folder, "--script",
                              "res://driver.gd", "--quit-after", "300"], capture_output=True, encoding="utf-8",
                             errors="replace", timeout=45)
        output = run.stdout + run.stderr
        print(output)
        if run.returncode or "CGD_PACKED_ABI_OK" not in output or "SCRIPT ERROR:" in output:
            raise SystemExit(1)


if __name__ == "__main__":
    main()

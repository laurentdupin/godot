# GD-C syntax (C-style source only)

`.cgd` accepts C-style source declarations and blocks. GDScript-style aliases are rejected, including during editor completion. The engine translates ordinary GD-C code to GDScript and runs it on the existing VM. Explicit `native` declarations can contain inline C on supported targets.

## Functions and variables

```c
extends Node2D;
class_name MovingActor;

@export float speed = 250.0;
const int LIMIT = 10;
int count = 0;
auto label = "Player";

void _ready() {
}

void _process(float delta) {
    if (Input.is_action_pressed("ui_right") && !Input.is_action_pressed("ui_left")) {
        position.x += speed * delta;
    }
}

int twice(int value) {
    return value * 2;
}
```

Types precede names. `void f(void)` is also accepted for a function with no parameters. `auto name = value` retains Godot's hard type inference; it requires an initializer. `dynamic name = value` retains its soft inference, and `Variant name = value` explicitly declares a Variant. These distinctions matter when the analyzer checks subsequent expressions. Constants use `const Type`, `const auto` (inferred), or `const dynamic` (without an explicit type or inference marker).

Parameters require a type prefix. `auto amount = 3` infers a parameter type from its default; `dynamic value` leaves it untyped; `Variant value` explicitly annotates it. Functions require a return prefix: use `void`, a concrete type, `Variant`, or `dynamic`. A `dynamic` return preserves an unannotated GDScript return, including implicit null on paths with no return statement. Explicit `Variant` returns still require a value on every path.

Use `{}` for empty bodies or `;` for an empty statement. `pass` is not source syntax. Simple statements require semicolons; indentation is optional. Annotations retain their Godot spelling. Static declarations, nested classes, enums, and Godot container types such as `Array[int]` and `Dictionary[String, int]` remain available.

## Control flow

```c
if (ready && !paused) {
    run();
} else if (waiting) {
    wait_for_event();
} else {
    stop();
}

while (running) {
    tick();
}

for (int i = 0; i < 10; i++) {
    if (i == 5) { continue; }
    print(i);
}

for (Node child : get_children()) {
    child.queue_free();
}

int result = condition ? 10 : 20;
```

Conditions need parentheses and bodies need braces. Range loops use `for (Type name : iterable)` or `for (auto name : iterable)`. Three-clause loops preserve update execution after `continue`, skip it after `break`, and scope the initializer to the loop.

Use `&&`, `||`, `!` and `condition ? yes : no`. `++name`, `name++`, `--name`, and `name--` are allowed only as whole statements on a bare identifier, including a loop update. For fields or array entries use `+= 1` or `-= 1`.

## Lambdas and engine extensions

```c
auto callback = [](int value) -> int { return value * 2; };

signal changed(int value);

int health = 100 {
    get { return health; }
    set(value) { health = max(value, 0); }
}
```

Lambdas use `[](Type name) { ... }`, optionally with a C++-style trailing return type: `[](int value) -> int { return value * 2; }`. The `[]` introducer does not change Godot's capture or object-lifetime semantics. Explicit capture lists are not supported. Trailing return types are allowed only on lambdas.

`cast<Type>(value)` retains Godot's safe `as` cast, and `type_is<Type>(value)` retains its type test and analyzer narrowing. `is_in(value, container)` tests membership and evaluates the value before the container, matching the original operator's evaluation order. These are compiler intrinsics, not ordinary helper functions. GDScript `as`, `is`, and `in` operators remain rejected in source.

`extends`, `class_name`, `signal`, annotations, property accessors, `await`, Godot types, array/dictionary values and engine APIs remain Godot-specific features of the dialect. They do not enable GDScript declaration syntax.

The braced selection form is `switch (value) { case 1, 2: { ... } default: { ... } }`. It uses the engine's pattern-selection behavior: branches do not fall through and do not need a trailing `break`. It is not a C switch with fallthrough; GDScript `match` and `var` pattern bindings are rejected.

## Strings and comments

Use double-quoted strings with escapes such as `\n`. Use `StringName("name")`, `NodePath("Node/Child")`, and `get_node("Child")` instead of GDScript literal/node shorthands. Single-quoted, raw-prefixed and triple-quoted strings are rejected.

Comments use `//`, `/* ... */`, or `///` for documentation. `#` and `##` comments are rejected. Block comments do not nest. UTF-8, Unicode identifiers, a leading BOM, and CRLF line endings are supported.

Translation extraction recognizes `// NO_TRANSLATE` and `// TRANSLATORS: ...` either inline or on the immediately preceding comment lines, following Godot's native rules. Blank lines end a preceding comment block. These comments retain their original source lines and are not included in exported bytecode.

## Rejected compatibility syntax

- `func`, `fn`, `var`, `pass`.
- `name: Type`, `:=`, and ordinary function declarations with `-> ReturnType`.
- Untyped parameters and constants without a type or `auto`.
- `elif`, `and`, `or`, `not`, `a if condition else b`.
- `for (... in ...)`, `match`, `is`, and `as`.
- GDScript comment, string, `$Node` and `%Node` shorthand forms described above.

These checks operate on source tokens, so strings containing words such as "func" are unaffected and methods such as `String.match()` remain callable. Existing `.gd` files still use normal GDScript.

## Inline C

```c
native {
    #include <windows.h>
}

native int monitor_count() {
    return GetSystemMetrics(SM_CMONITORS);
}
```

Put native declarations at script scope. A `native { ... }` block adds C declarations to the generated translation unit. A typed `native` function can be called like any other script method. The current ABI accepts `int`, `float` and `bool` parameters and returns, plus `void` returns. See [native build configuration](CGD_INTEGRATION.md#inline-c) for compiler and library settings.

Use `native cpp { ... }` and `native cpp int f() { ... }` for C++ (`cgd_native/cpp_standard`, C++20 by default). On macOS, use `native objcpp` for Objective-C++. Each script uses a single native language across its blocks and functions. Plain `native` takes the project default, which is C17 unless `cgd_native/default_language` is set to `"cpp"`.

In a native body, use `cgd_print("value=%d", value)`, `cgd_print_warning(...)`, or `cgd_print_error(...)` for Godot output. These functions take printf-style format strings. C++ and Objective-C++ can also use `cgd::print("value=", value)`, `cgd::print_warning(...)`, and `cgd::print_error(...)` to join printable arguments. Warnings and errors report the CGD file and line.



Use `static native` to call a native function without a script instance. Calls to a `static native` function made unqualified from its own script, or through its class (`NativeMath.add(...)` or a preloaded constant), skip the generated wrapper when every argument already has the parameter's type (an `int` may be passed for a `float`). These direct calls follow hot reloads of the native script. Other calls, such as those with untyped arguments or by name, go through the wrapper. A `PackedByteArray` parameter is exposed to C as `CGDBytes`, with a read-only `data` pointer and an `int64_t size` in bytes. The view shares the Godot array's storage for the duration of the call. An empty view has size zero. C code must keep accesses within that size and release any references to the view before returning.

```c
static native int first_byte(PackedByteArray bytes) {
    return bytes.size > 0 ? bytes.data[0] : -1;
}
```

Native functions also accept `PackedInt32Array`, `PackedInt64Array`, `PackedFloat32Array`, `PackedFloat64Array`, `PackedVector2Array`, `PackedVector3Array`, and `String`. Array views expose `data` and `size`. Vector data uses `data[index][component]` with the engine's real-number precision. A `CGDString` exposes `utf8` and its byte `length`.

For a packed array or String return, the C body returns void and writes to storage from `cgd_out_alloc(cgd_out, count)`. Count is in elements for arrays and UTF-8 bytes for strings. A second allocation replaces the first and invalidates its pointer. Zero count or a body that never allocates produces an empty value. Check the returned pointer before writing. Invalid counts or allocation failures report a native call error.

```c
static native String copy_string(String value) {
    char *output = cgd_out_alloc(cgd_out, value.length);
    if (output) {
        memcpy(output, value.utf8, (size_t)value.length);
    }
}
```

## Runtime limits

Ordinary GD-C retains Godot's Variant values, reference semantics, type system and script permissions. The C compiler handles pointers, structs and preprocessing inside native blocks and bodies.

The frontend enforces input, output and nesting limits. It preserves source coordinates through generated-code maps; exported binary columns remain limited by the native token format. The `__gdc_` identifier prefix is reserved for generated locals.

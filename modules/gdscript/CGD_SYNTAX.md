# GD-C syntax (C-style source only)

`.cgd` accepts C-style source declarations and blocks. GDScript-style aliases are rejected, including during editor completion. The engine still translates GD-C to GDScript internally and runs it on the existing VM; this is not a C/C++ compiler.

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

## Rejected compatibility syntax

- `func`, `fn`, `var`, `pass`.
- `name: Type`, `:=`, and ordinary function declarations with `-> ReturnType`.
- Untyped parameters and constants without a type or `auto`.
- `elif`, `and`, `or`, `not`, `a if condition else b`.
- `for (... in ...)`, `match`, `is`, and `as`.
- GDScript comment, string, `$Node` and `%Node` shorthand forms described above.

These checks operate on source tokens, so strings containing words such as "func" are unaffected and methods such as `String.match()` remain callable. Existing `.gd` files still use normal GDScript.

## Runtime limits

GD-C retains Godot's Variant values, reference semantics, type system and script permissions. It adds no pointers, manual memory management, preprocessor, exceptions, C-style casts, overloads, structs, namespaces, or separate native runtime. Not every C/C++ construct is supported.

The frontend enforces input, output and nesting limits. It preserves source coordinates through generated-code maps; exported binary columns remain limited by the native token format. The `__gdc_` identifier prefix is reserved for generated locals.

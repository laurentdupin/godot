// SPDX-License-Identifier: MIT
#include "gdc/transpiler.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

static unsigned checks = 0;
static void check(bool value, const std::string &message) {
	++checks;
	if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static GDC::Result good(const std::u32string &source, const std::string &name) {
	auto r = GDC::transpile(source);
	check(r.ok(), name + (r.ok() ? "" : ": " + r.diagnostics.front().message));
	check(!r.map.empty(), name + ": map exists");
	return r;
}
static void contains(const GDC::Result &r, const std::u32string &part, const std::string &name) {
	check(r.code.find(part) != std::u32string::npos, name + "\n" + GDC::encode_utf8(r.code));
}
static void bad(const std::u32string &source, const std::string &name) {
	auto r = GDC::transpile(source);
	check(!r.ok() && r.code.empty() && r.map.empty(), name);
	check(r.diagnostics[0].position.line >= 1 && r.diagnostics[0].position.column >= 1, name + ": positioned");
}
int main() {
	check(good(U"", "empty").code.empty(), "empty source");
	check(good(U"void f() {}", "empty function").code == U"func f() -> void:\n    pass\n", "empty block emits pass");
	check(good(U"void f(){; // blank\n}", "empty semicolon block").code == U"func f() -> void:\n    pass\n", "empty semicolon emits pass");
	contains(good(U"void f() { /// documentation\n }", "doc only"), U"pass", "doc only emits pass");
	contains(good(U"extends Node; class_name Hero;", "metadata"), U"class_name Hero\n", "metadata output");
	contains(good(U"@tool; @export_range(0, 10) int health = 10;", "annotations"), U"var health: int = 10", "C declaration");
	contains(good(U"const int MAX = 3; static float speed = 1.5;", "storage"), U"static var speed: float = 1.5", "static declaration");
	contains(good(U"static int twice(int n) { return n * 2; }", "C function"), U"static func twice(n: int) -> int:", "typed signature");
	bad(U"fn f(x: int = 2) -> int { return x; }", "fn alias rejected");
	contains(good(U"signal hit(int damage, Node source);", "signal"), U"signal hit(damage: int, source: Node)", "typed signal");
	contains(good(U"Array[Dictionary[String, int]] values = [];", "generic type"), U"var values: Array[Dictionary[String, int]]", "container type");
	contains(good(U"class Inner extends RefCounted { int x = 1; }", "inner class"), U"class Inner extends RefCounted:\n    var x: int = 1", "inner class output");
	contains(good(U"enum State { IDLE, RUN = 2 };", "enum"), U"enum State {IDLE, RUN = 2}", "enum braces retained");
	contains(good(U"void f(){ if (a && !b) {} else if (c || d) {} else {} }", "control"), U"elif c  or  d:", "else-if mapping");
	contains(good(U"void f(){while(a){break;} }", "while"), U"while a:\n        break", "while block");
	contains(good(U"void f(){auto d = {\"nested\": {\"x\": [1, 2]}, answer = 42};}", "dictionary"), U"{\"nested\": {\"x\": [1, 2]}, answer = 42}", "dictionary retained");
	contains(good(U"void f(){for (int i : range(3)) { print(i); }}", "typed foreach"), U"for i: int in range(3):", "typed foreach output");
	contains(good(U"void f(){for (auto i : [1,2]) { continue; }}", "var foreach"), U"for i in [1, 2]:", "var stripped in foreach");
	auto loop = good(U"void f(){for(int i=0;i<3;i++){if(i==1){continue;}print(i);}}", "C for");
	contains(loop, U"if true:", "for scope");
	contains(loop, U"else:\n                i += 1\n            if not (i<3):", "update before next condition");
	contains(good(U"void f(){for(;;){break;}}", "infinite for"), U"while true:", "infinite loop");
	contains(good(U"void f(){int n=0;++n;n--;}", "increments"), U"n += 1\n    n -= 1", "standalone updates");
	contains(good(U"void f(){return !a == b;}", "not precedence"), U"return (not a) == b", "C unary not precedence");
	contains(good(U"void f(){return !thing.call()[0].value;}", "not postfix"), U"(not thing.call()[0].value)", "unary includes postfix chain");
	contains(good(U"void f(){return !get_node(\"Child\").visible;}", "not node"), U"(not get_node(\"Child\").visible)", "unary node shorthand");
	contains(good(U"void f(){return c ? a : b;}", "ternary"), U"return (a if c else b)", "conditional lowering");
	contains(good(U"void f(){return a ? b : c ? d : e;}", "nested ternary"), U"(b if a else (d if c else e))", "right associativity");
	contains(good(U"void f(){call(a?b:c, d?e:f);}", "argument ternaries"), U"call((b if a else c), (e if d else f))", "comma boundary");
	contains(good(U"void f(){x += a ? b : c;}", "compound ternary"), U"x += (b if a else c)", "assignment precedence");
	contains(good(U"void f(){auto d={\"x\": a?b:c};}", "dictionary ternary"), U"{\"x\": (b if a else c)}", "key value boundary");
	contains(good(U"void f(){auto cb=[](int n){return n+1;};}", "lambda"), U"func(n: int):\n        return n+1", "lambda lowering");
	contains(good(U"void f(){xs.map([](Variant x){return x*2;});}", "callback lambda"), U"return x*2\n    )", "callback closing delimiter");
	contains(good(U"int hp=1 {get{return hp;}set(v){hp=v;}}", "property"), U"set(v):\n        hp = v", "accessors");
	contains(good(U"void f(){switch(x){case 1,2: {return true;} default:{return false;}}}", "match"), U"1, 2:\n            return true", "match cases");
    bad(U"void f(){match(x){case {\"x\": var v}: {return v;}}}", "GDScript pattern binding rejected");
	auto strings = good(U"auto a=\"// { ; && }\"; auto b=\"a\\b\"; auto c=StringName(\"name\"); auto d=NodePath(\"Node/Child\");", "strings");
	contains(strings, U"\"// { ; && }\"", "string punctuation untouched");
	contains(strings, U"\"a\\b\"", "escaped string retained");
	contains(strings, U"StringName(\"name\")", "StringName constructor");
	bad(U"String text = \"\"\"native multiline\"\"\";", "triple quoted strings rejected");
	contains(good(U"auto a=1; /* ignored { ; */ auto b=2; // tail\n", "comments"), U"var b: = 2", "comments stripped safely");
	contains(good(U"int f(auto n=2){ auto local=n; return local; }", "inferred declarations"), U"func f(n: = 2) -> int:", "auto parameter retains inference");
	contains(good(U"Variant value = 2;", "dynamic declaration"), U"var value: Variant = 2", "Variant remains dynamic");
	contains(good(U"auto cb=[](int n)->int{return n;};", "typed lambda"), U"func(n: int) -> int:", "lambda return type retained");
	contains(good(U"bool f(Variant x){return !type_is<Dictionary>(x);}", "negated type test"), U"(not (x is Dictionary))", "type narrowing operation retained");
	contains(good(U"Node f(Variant x){return cast<Node>(x);}", "safe cast"), U"(x as Node)", "safe cast retained");
	contains(good(U"bool f(){return is_in(first(), second());}", "membership order"), U"(first() in second())", "membership evaluates value before container");
	bad(U"bool f(){return type_is<Node>();}", "empty type test");
	bad(U"void f(){is_in(1);}", "missing membership operand");
	bad(U"void f(auto x){}", "inference needs a default");
	bad(U"void f()->void{}", "trailing return only for lambdas");
	contains(good(U"dynamic f(){ return; }", "implicit dynamic return"), U"func f():", "dynamic return allows implicit null");
	contains(good(U"dynamic value = 1;", "soft inferred declaration"), U"var value = 1", "dynamic retains soft inference");
	contains(good(U"\uFEFFextends Node;\r\nvoid f(){\r\nreturn;\r\n}", "BOM CRLF"), U"func f() -> void:", "BOM and CRLF accepted");
	contains(good(U"int café = 2; void f(){return café;}", "unicode"), U"var café: int", "unicode identifiers");
	for (const auto &input : std::vector<std::u32string>{ U"void f() {", U"func f(] {}", U"auto s=\"bad;", U"/* bad", U"extends Node", U"void f(){return n++;}", U"int __gdc_x=0;", U"void f(){if x {}}", U"void f(){for(a;b){}}", U"void f(){switch(x){}}", U"void f(){return a?b;}", U"void f(){for(i=0,j=0;i<2;i++){}}" }) {
		bad(input, "invalid: " + GDC::encode_utf8(input));
	}
	for (const auto &input : std::vector<std::u32string>{
		U"func f() {}", U"func f() -> void {}", U"fn f() {}", U"var x = 1;", U"var x: int = 1;",
		U"int x := 1;", U"x: int = 1;", U"void f(x: int) {}", U"void f(x) {}", U"void f(){pass;}",
		U"void f(){if(a){} elif(b){}}", U"bool f(){return a and b;}", U"bool f(){return a or b;}",
		U"bool f(){return not a;}", U"int f(){return a if b else c;}", U"void f(){for(int x in xs){}}",
		U"# comment", U"## doc comment", U"String x = r\"raw\";", U"StringName x = &\"name\";", U"NodePath x = ^\"path\";", U"String x = 'text';", U"Node child = $Child;", U"auto cb = func(int n) { return n; };", U"const X = 1;"
	}) {
		bad(input, "GDScript compatibility removed: " + GDC::encode_utf8(input));
		GDC::Options editor; editor.completion = true;
		check(!GDC::transpile(input, editor).ok(), "editor completion cannot enable GDScript aliases");
	}
	contains(good(U"String text = \"func var pass and or not\";", "keyword literals"), U"\"func var pass and or not\"", "strings are not keyword checked");
	contains(good(U"bool test() { return text.match(\"pattern\"); }", "member keyword"), U"text.match", "member methods remain callable");
	bad(U"Node child = %Child;", "unique node shorthand rejected");
	contains(good(U"int remainder(int value) { return value % 2; }", "modulo operator"), U"value % 2", "modulo remains valid");
	bad(std::u32string(1, U'\0'), "embedded NUL");
	GDC::Options limited; limited.max_source_chars = 2;
	check(!GDC::transpile(U"auto x;", limited).ok(), "input size limit");
	limited = {}; limited.max_output_chars = 10;
	check(!GDC::transpile(U"void f() {}", limited).ok(), "output size limit");
	limited = {}; limited.max_nesting = 4;
	check(!GDC::transpile(U"void f(){return (((((x)))));}", limited).ok(), "depth limit");
	GDC::Options completion; completion.completion = true;
	check(GDC::transpile(U"void f(){return 1", completion).ok(), "completion closes braces and missing semicolon");
	completion.completion_line = 3;
	const std::u32string editing = U"extends Node3D;\nvoid f(){\nposit\nprint(1);\n}";
	check(GDC::transpile(editing, completion).ok(), "completion recovers missing semicolon on cursor line");
	check(!GDC::transpile(editing).ok(), "completion recovery does not relax executable syntax");
	check(GDC::transpile(U"extends Node3D;\nvoid f(){\nposition.\n}", completion).ok(), "completion preserves unfinished member expression");
	auto map_test = good(U"// header\nvoid f() {\n    return café;\n}\n", "positions");
	check(map_test.map.original(2, 12).line == 3, "generated to source line");
	check(map_test.map.original(2, 12).column == 12, "Unicode column");
	check(map_test.map.generated({3, 12}).line == 2, "reverse line map");
	GDC::Position span_start, span_end;
	auto reordered = good(U"void f(){return condition ? yes : no;}", "span remapping");
	reordered.map.original_range({2, 12}, {2, 38}, span_start, span_end);
	check(span_start.line == 1 && span_start.column <= 17 && span_end.column >= 35, "reordered span contains original conditional");
	std::vector<uint8_t> native { 'G', 'D', 'S', 'C', 100, 0, 0, 0 };
	auto binary = GDC::encode_binary(native, map_test.map);
	std::vector<uint8_t> decoded;
	GDC::SourceMap decoded_map;
	std::string err;
	check(GDC::decode_binary(binary, decoded, decoded_map, err), "binary round trip");
	check(decoded == native, "native token payload unchanged");
	check(decoded_map.original(2, 12) == map_test.map.original(2, 12), "binary source map retained");
	for (size_t i = 0; i < binary.size(); ++i) {
		std::vector<uint8_t> truncated(binary.begin(), binary.begin() + i);
		check(!GDC::decode_binary(truncated, decoded, decoded_map, err), "truncated binary rejected");
	}
	auto corrupt = binary; corrupt[4] = 99;
	check(!GDC::decode_binary(corrupt, decoded, decoded_map, err), "unknown version rejected");
	corrupt = binary; corrupt[20] = 255; corrupt[21] = 255;
	check(!GDC::decode_binary(corrupt, decoded, decoded_map, err), "forged length rejected");
	corrupt = binary; corrupt.push_back(0);
	check(!GDC::decode_binary(corrupt, decoded, decoded_map, err), "trailing payload rejected");
	std::u32string text;
	check(GDC::decode_utf8("caf\xc3\xa9 \xf0\x9f\x98\x80", text, err), "UTF8 decode");
	check(GDC::encode_utf8(text) == "caf\xc3\xa9 \xf0\x9f\x98\x80", "UTF8 encode");
	check(!GDC::decode_utf8("\xc0\xaf", text, err), "UTF8 overlong rejected");
	check(!GDC::decode_utf8("\xed\xa0\x80", text, err), "UTF8 surrogate rejected");
	check(!GDC::decode_utf8("\xf4\x90\x80\x80", text, err), "UTF8 range rejected");
	check(!GDC::decode_utf8("\xe2\x82", text, err), "UTF8 truncation rejected");
	// Deterministic malformed-input fuzz smoke test. Run under ASan/UBSan too.
	std::mt19937 random(0x474443);
	const std::u32string alphabet = U"abc auto func 012 \n\t{}[]();@?:!&|=+-/*\\\"'#";
	for (int n = 0; n < 10000; ++n) {
		std::u32string sample;
		unsigned len = random() % 200;
		for (unsigned j = 0; j < len; ++j) { sample.push_back(alphabet[random() % alphabet.size()]); }
		auto r = GDC::transpile(sample);
		check(r.ok() || r.code.empty(), "fuzz error is atomic");
		std::vector<uint8_t> bytes(random() % 200);
		for (auto &byte : bytes) { byte = static_cast<uint8_t>(random()); }
		GDC::decode_binary(bytes, decoded, decoded_map, err);
	}
	// Mutate valid envelopes too: random non-magic bytes exercise only the header guard.
	for (int n = 0; n < 5000; ++n) {
		auto mutated = binary;
		for (unsigned j = 0, edits = 1 + random() % 4; j < edits; ++j) {
			mutated[random() % mutated.size()] ^= static_cast<uint8_t>(1 + random() % 255);
		}
		bool valid = GDC::decode_binary(mutated, decoded, decoded_map, err);
		check(valid || (decoded.empty() && decoded_map.empty()), "mutated envelope failure is atomic");
		if (valid) {
			check(decoded.size() >= 4 && decoded[0] == 'G' && decoded[3] == 'C', "accepted envelope keeps native payload magic");
			check(decoded_map.original(1, 1).line >= 1, "accepted mutated map is queryable");
		}
	}
	std::cout << "PASS: " << checks << " checks; 10000 source fuzz cases, 10000 random binary cases, 5000 valid-envelope mutations.\n";
	return 0;
}

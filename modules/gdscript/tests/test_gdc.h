// SPDX-License-Identifier: MIT
#ifndef TEST_GDC_H
#define TEST_GDC_H

#include "tests/test_macros.h"

#include "modules/gdscript/gdc_frontend.h"
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_analyzer.h"
#include "modules/gdscript/gdscript_parser.h"
#ifdef TOOLS_ENABLED
#include "modules/gdscript/editor/gdscript_editor_language.h"
#endif

namespace TestGDC {

TEST_CASE("[GDC] Native parser accepts C syntax and preserves original line numbers") {
	GDScriptParser parser;
	const String source = "extends RefCounted;\n\nint answer() {\n    return 42;\n}\n";
	CHECK(parser.parse(source, "res://test.cgd", false) == OK);
	REQUIRE(parser.get_tree() != nullptr);
	REQUIRE(parser.get_tree()->members.size() == 1);
	const GDScriptParser::FunctionNode *function = parser.get_tree()->members[0].function;
	REQUIRE(function != nullptr);
	CHECK(function->start_line == 3);
	CHECK(function->end_line >= 4);
	CHECK(function->end_line <= 5);
}

TEST_CASE("[GDC] Analyzer locations outside enclosing node spans are mapped") {
	GDScriptParser parser;
	CHECK(parser.parse("extends RefCounted;\nenum Mode { FIRST, SECOND };\n\nint answer() { int value = 42; return value; }\n", "res://locations.cgd", false) == OK);
	REQUIRE(parser.get_tree() != nullptr);
	const auto *tree = parser.get_tree();
	CHECK(tree->extends_start_line == 1);
	CHECK(tree->extends_end_line == 1);
	REQUIRE(tree->members.size() == 2);
	const auto *function = tree->members[1].function;
	REQUIRE(function != nullptr);
	CHECK(function->header_end_line == 4);
	REQUIRE(function->body != nullptr);
	REQUIRE(!function->body->locals.is_empty());
	CHECK(function->body->locals[0].start_line == 4);
	CHECK(function->body->locals[0].end_line == 4);
}

TEST_CASE("[GDC] Existing GDScript parsing is unchanged") {
	GDScriptParser parser;
	CHECK(parser.parse("extends RefCounted\nfunc answer():\n\treturn 42\n", "res://test.gd", false) == OK);
	CHECK(parser.parse("func answer() { return 42; }", "res://test.gd", false) != OK);
}

TEST_CASE("[GDC] GDScript syntax is rejected in CGD source") {
	for (const String &source : {
				 "func run() {}", "func run() -> void {}", "fn run() {}", "var value = 1;", "int value := 1;",
				 "void run(value: int) {}", "void run(value) {}", "void run(){pass;}", "void run(){if(true){} elif(false){}}",
				 "bool run(){return true and false;}", "bool run(){return not false;}", "int run(){return 1 if true else 2;}",
				 "void run(){for(int item in [1,2]){}}", "# GDScript comment" }) {
		GDScriptParser parser;
		CHECK(parser.parse(source, "res://strict.cgd", false) == ERR_PARSE_ERROR);
		CHECK_FALSE(parser.get_errors().is_empty());
		CHECK(parser.parse(source, "res://strict.cgd", true) == ERR_PARSE_ERROR);
	}
}

TEST_CASE("[GDC] Source errors always contain a positioned diagnostic") {
	GDScriptParser parser;
	CHECK(parser.parse("extends RefCounted;\nvoid broken() {", "res://test.cgd", false) == ERR_PARSE_ERROR);
	REQUIRE(!parser.get_errors().is_empty());
	CHECK(parser.get_errors().front()->get().start_line == 2);
}

TEST_CASE("[GDC] C-style inference casts and typed lambdas preserve native operations") {
	GDScriptParser parser;
	CHECK(parser.parse("extends RefCounted;\nauto value = 2;\nint calculate(auto amount = 3) { auto callback = [](int n) -> int { return n * 2; }; Variant data = {\"x\": 1}; if (!type_is<Dictionary>(data) || !is_in(\"x\", data)) { return 0; } return cast<Dictionary>(data)[\"x\"] + callback.call(amount); }", "res://port_features.cgd", false) == OK);
	CHECK(parser.get_errors().is_empty());
}

TEST_CASE("[GDC] Both native token compression modes preserve GD-C source maps") {
	for (int mode = 0; mode < 2; ++mode) {
		const auto compression = mode == 0 ? GDScriptTokenizerBuffer::COMPRESS_NONE : GDScriptTokenizerBuffer::COMPRESS_ZSTD;
		String error;
		Vector<uint8_t> binary = GDCFrontend::compile_binary("\n\nint answer(){return 42;}", "res://test.cgd", compression, error);
		CHECK(error.is_empty());
		REQUIRE(!binary.is_empty());
		CHECK(GDC::has_binary_envelope(binary.ptr(), binary.size()));
		GDScriptParser parser;
		CHECK(parser.parse_binary(binary, "res://test.cgd") == OK);
		REQUIRE(parser.get_tree() != nullptr);
		REQUIRE(parser.get_tree()->members.size() == 1);
		CHECK(parser.get_tree()->members[0].function->start_line == 3);
	}
}

TEST_CASE("[GDC] Legacy native token buffers still load") {
	const Vector<uint8_t> native = GDScriptTokenizerBuffer::parse_code_string("func answer():\n    return 42\n", GDScriptTokenizerBuffer::COMPRESS_NONE);
	GDScriptParser parser;
	CHECK(parser.parse_binary(native, "res://test.gd") == OK);
	CHECK(parser.parse_binary(native, "res://test.gdc") == OK);
}

TEST_CASE("[GDC] Corrupt binary produces a diagnostic rather than an empty error list") {
	Vector<uint8_t> bad;
	bad.push_back('G');
	bad.push_back('D');
	bad.push_back('C');
	bad.push_back('B');
	GDScriptParser parser;
	CHECK(parser.parse_binary(bad, "res://test.cgd") != OK);
	CHECK(!parser.get_errors().is_empty());
}

TEST_CASE("[GDC] Source aliases and compiled canonical paths do not collide") {
	REQUIRE(ScriptServer::get_language_for_extension("gd") != nullptr);
	CHECK(ScriptServer::get_language_for_extension("gd") == ScriptServer::get_language_for_extension("cgd"));
	CHECK(GDScript::canonicalize_path("res://test.cgd.gdbin") == "res://test.cgd");
	CHECK(GDScript::canonicalize_path("res://test.gdc") == "res://test.gd");
	CHECK(GDScript::canonicalize_path("res://nonexistent.cgd") == "res://nonexistent.cgd");
}

TEST_CASE("[GDC] CGD source and native GDC binaries have separate extension routes") {
	CHECK(GDCFrontend::is_source_path("res://script.cgd"));
	CHECK(GDCFrontend::is_source_path("res://script.CGD"));
	CHECK(!GDCFrontend::is_source_path("res://script.gd"));
	CHECK(!GDCFrontend::is_source_path("res://script.gdc"));
	CHECK(!GDCFrontend::is_source_path("res://script.cgd.gdbin"));
	CHECK(!GDCFrontend::is_binary_path("res://nonexistent.cgd"));
	CHECK(!GDCFrontend::is_binary_path("res://script.gd"));
	CHECK(GDCFrontend::is_binary_path("res://nonexistent.gdc"));
	CHECK(GDCFrontend::is_binary_path("res://script.GDC"));
	CHECK(GDCFrontend::is_binary_path("res://script.cgd.gdbin"));
	CHECK(ScriptServer::get_language_for_extension("CGD") == ScriptServer::get_language_for_extension("gd"));
	CHECK(!GDScriptLanguage::get_singleton()->handles_extension("gdc"));
	CHECK(GDScript::canonicalize_path("res://script.CGD") == "res://script.CGD");
}

#ifdef TOOLS_ENABLED
TEST_CASE("[GDC][SceneTree][Editor] Every built-in template has a valid C-style counterpart") {
	GDScriptLanguage *language = GDScriptLanguage::get_singleton();
	for (const String &base : { "Object", "Node", "CharacterBody2D", "CharacterBody3D", "EditorPlugin", "EditorScript", "EditorScenePostImport", "RichTextEffect", "VisualShaderNodeCustom" }) {
		const auto templates = language->get_built_in_templates_for_path("res://template.cgd", base);
		CHECK(templates.size() == language->get_built_in_templates(base).size());
		REQUIRE(!templates.is_empty());
		for (const auto &entry : templates) {
			const Ref<Script> script = language->make_template(entry.content, "CGDTemplateTest", base);
			GDScriptParser parser;
			CHECK(parser.parse(script->get_source_code(), "res://template.cgd", false) == OK);
			REQUIRE(parser.get_tree() != nullptr);
			GDScriptAnalyzer analyzer(&parser);
			CHECK(analyzer.analyze() == OK);
		}
	}
}

TEST_CASE("[GDC][SceneTree][Editor] Completion suggests inherited properties and typed members while typing") {
	EditorLanguage *editor = GDScriptLanguage::get_singleton()->get_editor_language();
	const String caret = String::chr(0xFFFF);
	for (const String &tail : { "\n}", "\n    print(1);\n}", ";\n}" }) {
		List<ScriptLanguage::CodeCompletionOption> options;
		bool forced = false;
		String hint;
		const String source = "extends Node3D;\nvoid _process(float delta) {\n    posit" + caret + tail;
		CHECK(editor->complete_code(source, "res://completion.cgd", nullptr, &options, forced, hint) == OK);
		bool position = false;
		bool global_position = false;
		for (const auto &option : options) {
			position |= option.display == "position";
			global_position |= option.display == "global_position";
		}
		CHECK(position);
		CHECK(global_position);
	}
	for (const String &expression : { "position.", "offset." }) {
		List<ScriptLanguage::CodeCompletionOption> options;
		bool forced = false;
		String hint;
		const String source = "extends Node3D;\nvoid _ready() {\n    Vector3 offset = Vector3.ZERO;\n    " + expression + caret + "\n}";
		CHECK(editor->complete_code(source, "res://completion.cgd", nullptr, &options, forced, hint) == OK);
		bool x = false;
		for (const auto &option : options) {
			x |= option.display == "x";
		}
		CHECK(x);
	}
}

TEST_CASE("[GDC][SceneTree][Editor] Incomplete source is safe during completion and symbol lookup") {
	EditorLanguage *editor = GDScriptLanguage::get_singleton()->get_editor_language();
	int rejected_before_parsing = 0;
	for (const String &source : { "extends Node2D;\nvoid _ready() {\n    int value =", "extends Node2D;\nvoid _ready(", "extends Node2D;\nvoid _ready() {\n    print(\"unfinished" }) {
		GDScriptParser parser;
		REQUIRE(parser.parse(source, "res://editing.cgd", true) == ERR_PARSE_ERROR);
		const bool has_tree = parser.get_tree() != nullptr;
		rejected_before_parsing += !has_tree;
		List<ScriptLanguage::CodeCompletionOption> options;
		bool forced = true;
		String hint;
		Error completion_error = editor->complete_code(source + String::chr(0xFFFF), "res://editing.cgd", nullptr, &options, forced, hint);
		if (!has_tree) {
			CHECK(completion_error == ERR_PARSE_ERROR);
			CHECK(options.is_empty());
			CHECK_FALSE(forced);
		}
		EditorLanguage::LookupResult result;
		Error lookup_error = editor->lookup_code(source + String::chr(0xFFFF), "value", "res://editing.cgd", nullptr, result);
		if (!has_tree) {
			CHECK(lookup_error == ERR_PARSE_ERROR);
		}
	}
	CHECK(rejected_before_parsing > 0);

	// Exercise each intermediate buffer when typing the Node3D starter script.
	const String starter = "extends Node3D;\n\nvoid _ready() {\n}\n\nvoid _process(float delta) {\n}\n";
	for (int length = 0; length <= starter.length(); length++) {
		const String source = starter.left(length) + String::chr(0xFFFF);
		List<ScriptLanguage::CodeCompletionOption> options;
		bool forced = false;
		String hint;
		Error completion_error = editor->complete_code(source, "res://editing.cgd", nullptr, &options, forced, hint);
		CHECK((completion_error == OK || completion_error == ERR_PARSE_ERROR));
		EditorLanguage::LookupResult result;
		Error lookup_error = editor->lookup_code(source, "_ready", "res://editing.cgd", nullptr, result);
		CHECK((lookup_error == OK || lookup_error == ERR_PARSE_ERROR || lookup_error == ERR_CANT_RESOLVE));
	}
}

TEST_CASE("[GDC][SceneTree][Editor] Editor helpers preserve the source dialect") {
	GDScriptLanguage *language = GDScriptLanguage::get_singleton();
	EditorLanguage *editor = language->get_editor_language();
	CHECK(language->get_comment_delimiters_for_path("res://test.cgd").has("//"));
	CHECK_FALSE(language->get_comment_delimiters_for_path("res://test.gd").has("//"));
	PackedStringArray args;
	args.push_back("value:int");
	String callback = language->make_function_for_path("res://test.cgd", "", "changed", args);
	GDScriptParser parser;
	CHECK(callback.begins_with("void changed(int value) {"));
	CHECK_FALSE(callback.contains("pass"));
	CHECK(parser.parse(callback, "res://test.cgd", false) == OK);
	CHECK(editor->find_function_for_path("res://test.cgd", "changed", "// Header\n\n" + callback) == 3);
	CHECK(editor->find_function_for_path("res://test.cgd", "missing", callback) == -1);
	String native = language->make_function_for_path("res://test.gd", "", "changed", args);
	CHECK(parser.parse(native, "res://test.gd", false) == OK);
	String source = "void run() {\nprint(\"}\");\nif (true) {\n;\n}\n}\n";
	editor->format_code_for_path("res://test.cgd", source, 0, 6);
	CHECK(parser.parse(source, "res://test.cgd", false) == OK);
	CHECK((source.split("\n")[2].begins_with("\t") || source.split("\n")[2].begins_with(" ")));
	CHECK(source.split("\n")[5] == "}");
	String literal = "void run() {\n/* first\n  } untouched\nlast */\n}\n";
	editor->format_code_for_path("res://test.cgd", literal, 0, 5);
	CHECK(literal.contains("/* first\n  } untouched\nlast */"));
	String selection = "void run() {\n;\n}\n";
	editor->format_code_for_path("res://test.cgd", selection, 1, 1);
	CHECK(selection.begins_with("void run() {\n"));
	CHECK(selection.ends_with("\n}\n"));
}
#endif

} // namespace TestGDC
#endif // TEST_GDC_H

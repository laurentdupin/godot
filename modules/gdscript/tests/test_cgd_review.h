// Regression tests for the CGD review follow-up. MIT license; see bundle LICENSE.
#pragma once

#include "modules/gdscript/gdc_frontend.h"
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_parser.h"
#include "modules/gdscript/gdscript_tokenizer_buffer.h"
#include "tests/test_macros.h"

#ifdef TOOLS_ENABLED
#include "modules/gdscript/editor/gdscript_translation_parser_plugin.h"
#endif

namespace TestCGDReview {

#ifdef TOOLS_ENABLED
TEST_CASE("[GDScript][CGDReview] Translation comments match native extraction") {
	const String cgd = "extends RefCounted;\n"
			"void messages() {\n"
			"    tr(\"Hidden inline\"); // NO_TRANSLATE\n"
			"    // NO_TRANSLATE: internal identifier\n"
			"    tr(\"Hidden above\");\n"
			"    tr(\"Visible inline\"); // TRANSLATORS: Menu label\n"
			"    // TRANSLATORS: First line\n"
			"    // Second line\n"
			"    tr(\"Visible above\");\n"
			"    // NO_TRANSLATE\n"
			"\n"
			"    tr(\"Visible after blank\");\n"
			"    tr(\"Literal // NO_TRANSLATE\"); /* // NO_TRANSLATE */\n"
			"}\n";
	const String gd = cgd.replace("extends RefCounted;", "extends RefCounted")
			.replace("void messages() {", "func messages():")
			.replace("}\n", "")
			.replace("; //", " #")
			.replace("    //", "    #")
			.replace("; /* // NO_TRANSLATE */", "");
	Ref<GDScript> native;
	native.instantiate();
	native->set_source_code(gd);
	native->set_path("res://review_translation_comments.gd");
	Ref<GDScript> script;
	script.instantiate();
	script->set_source_code(cgd);
	script->set_path("res://review_translation_comments.cgd");
	Ref<GDScriptEditorTranslationParserPlugin> extractor;
	extractor.instantiate();
	Vector<Vector<String>> expected;
	Vector<Vector<String>> actual;
	REQUIRE(extractor->parse_file(native->get_path(), &expected) == OK);
	REQUIRE(extractor->parse_file(script->get_path(), &actual) == OK);
	REQUIRE(expected.size() == 4);
	CHECK(actual == expected);
	CHECK(expected[0][3] == "Menu label");
	CHECK(expected[1][3] == "First line\nSecond line");
	CHECK(expected[2][3].is_empty());
	CHECK(expected[3][0] == "Literal // NO_TRANSLATE");
}
#endif

TEST_CASE("[GDScript][CGDReview] Extension matching is case-insensitive") {
	GDScriptLanguage *language = GDScriptLanguage::get_singleton();
	REQUIRE(language != nullptr);
	CHECK(language->handles_extension("gd"));
	CHECK(language->handles_extension("GD"));
	CHECK(language->handles_extension("cgd"));
	CHECK(language->handles_extension("CgD"));
	CHECK_FALSE(language->handles_extension("gdc"));
	CHECK_FALSE(language->handles_extension("gdbin"));
}

static void check_anonymous_enum(GDScriptParser &p_parser) {
	const GDScriptParser::ClassNode *tree = p_parser.get_tree();
	REQUIRE(tree != nullptr);
	for (const StringName &name : { StringName("REVIEW_ALPHA"), StringName("REVIEW_BETA") }) {
		REQUIRE(tree->has_member(name));
		const GDScriptParser::ClassNode::Member member = tree->get_member(name);
		REQUIRE(member.type == GDScriptParser::ClassNode::Member::ENUM_VALUE);
		CHECK(member.enum_value.line == 3);
	}
}

TEST_CASE("[GDScript][CGDReview] Anonymous enum member copies retain original lines") {
	const String source = "extends RefCounted;\nvoid unused() { return; }\nenum { REVIEW_ALPHA, REVIEW_BETA };\n";
	GDScriptParser text_parser;
	REQUIRE(text_parser.parse(source, "res://review_enum.cgd", false) == OK);
	check_anonymous_enum(text_parser);
	for (GDScriptTokenizerBuffer::CompressMode mode : { GDScriptTokenizerBuffer::COMPRESS_NONE, GDScriptTokenizerBuffer::COMPRESS_ZSTD }) {
		String error;
		const Vector<uint8_t> binary = GDCFrontend::compile_binary(source, "res://review_enum.cgd", mode, error);
		REQUIRE(error.is_empty());
		REQUIRE_FALSE(binary.is_empty());
		GDScriptParser binary_parser;
		REQUIRE(binary_parser.parse_binary(binary, "res://review_enum.cgd") == OK);
		check_anonymous_enum(binary_parser);
	}
}

} // namespace TestCGDReview

// Regression tests for the CGD review follow-up. MIT license; see bundle LICENSE.
#pragma once

#include "modules/gdscript/gdc_frontend.h"
#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_parser.h"
#include "modules/gdscript/gdscript_tokenizer_buffer.h"
#include "tests/test_macros.h"

namespace TestCGDReview {

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

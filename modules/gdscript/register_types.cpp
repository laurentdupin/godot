/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "register_types.h"
#include "gdc_frontend.h"
#include "gdc_native.h"

#include "gdscript.h"
#include "gdscript_cache.h"
#include "gdscript_parser.h"
#include "gdscript_resource_format.h"
#include "gdscript_tokenizer_buffer.h"
#include "gdscript_utility_functions.h"

#ifdef TOOLS_ENABLED
#include "editor/gdscript_editor_language.h"
#include "editor/gdscript_highlighter.h"
#include "editor/gdscript_translation_parser_plugin.h"
#include "editor/script/script_editor_plugin.h"

#ifndef GDSCRIPT_NO_LSP
#include "language_server/gdscript_language_protocol.h"
#include "language_server/gdscript_language_server.h"
#endif
#endif // TOOLS_ENABLED

#ifdef TESTS_ENABLED
#include "tests/test_cgd_review.h"
#include "tests/test_gdscript.h"
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"

#ifdef TOOLS_ENABLED
#include "editor/editor_node.h"
#include "editor/export/editor_export.h"
#include "editor/export/editor_export_platform.h"
#include "editor/translations/editor_translation_parser.h"

#endif // TOOLS_ENABLED

#ifdef TESTS_ENABLED
#include "tests/test_macros.h"
#endif

GDScriptLanguage *script_language_gd = nullptr;
Ref<ResourceFormatLoaderGDScript> resource_loader_gd;
Ref<ResourceFormatSaverGDScript> resource_saver_gd;
GDScriptCache *gdscript_cache = nullptr;

#ifdef TOOLS_ENABLED

Ref<GDScriptEditorTranslationParserPlugin> gdscript_translation_parser_plugin;

class GDScriptExportPlugin : public EditorExportPlugin {
	GDSOFTCLASS(GDScriptExportPlugin, EditorExportPlugin);

	static constexpr EditorExportPreset::ScriptExportMode DEFAULT_SCRIPT_MODE = EditorExportPreset::MODE_SCRIPT_BINARY_TOKENS_COMPRESSED;
	EditorExportPreset::ScriptExportMode script_mode = DEFAULT_SCRIPT_MODE;
	HashMap<String, Vector<GDCNative::NativeUnit>> native_units;
	Vector<Ref<DirAccess>> native_export_workspaces;

protected:
	virtual void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override {
		script_mode = DEFAULT_SCRIPT_MODE;
		native_units.clear();

		const Ref<EditorExportPreset> &preset = get_export_preset();
		if (preset.is_valid()) {
			script_mode = preset->get_script_export_mode();
		}
	}

	virtual void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) override {
		const String extension = p_path.get_extension().to_lower();
		if ((extension != "gd" && extension != "cgd") || GDCFrontend::is_binary_path(p_path)) {
			return;
		}

		Error read_error = OK;
		Vector<uint8_t> file = FileAccess::get_file_as_bytes(p_path, &read_error);
		String source;
		String error;
		if (read_error != OK || (!file.is_empty() && source.append_utf8(reinterpret_cast<const char *>(file.ptr()), file.size()) != OK)) {
			error = vformat("%s: Cannot read valid UTF-8 script source.", p_path);
		} else if (script_mode == EditorExportPreset::MODE_SCRIPT_TEXT) {
			if (extension == "cgd") {
				GDScriptParser parser;
				if (parser.parse(source, p_path, false) != OK) {
					const GDScriptParser::ParserError &failure = parser.get_errors().front()->get();
					error = vformat("%s:%d:%d: %s", p_path, failure.start_line, failure.start_column, failure.message);
				}
			}
			// Text mode keeps the original .cgd; the patched template transpiles at load time.
		} else {
			const GDScriptTokenizerBuffer::CompressMode mode = script_mode == EditorExportPreset::MODE_SCRIPT_BINARY_TOKENS_COMPRESSED ? GDScriptTokenizerBuffer::COMPRESS_ZSTD : GDScriptTokenizerBuffer::COMPRESS_NONE;
			file = GDCFrontend::compile_binary(source, p_path, mode, error);
			if (error.is_empty() && file.is_empty()) {
				error = vformat("%s: Script compilation produced an empty token buffer.", p_path);
			}
			if (error.is_empty()) {
				// Keep ordinary .gd exports on the existing .gdc route.
				const String compiled_path = extension == "cgd" ? p_path + ".gdbin" : p_path.get_basename() + ".gdc";
				add_file(compiled_path, file, true);
			}
		}
		if (extension == "cgd" && error.is_empty()) {
			String generated;
			String native_source;
			String native_id;
			GDC::SourceMap map;
			int line = 1;
			int column = 1;
			if (GDCFrontend::transpile(source, generated, map, error, line, column, false, { 0, 0 }, p_path, &native_source, nullptr, &native_id) == OK && !native_source.is_empty()) {
				const Ref<EditorExportPreset> preset = get_export_preset();
				const String platform = preset->get_platform()->get_os_name().to_lower();
				if (!GDCNative::supports_source(native_source, platform)) {
					return;
				}
				const bool android = platform == "android";
				Vector<String> targets;
				if (android) {
					for (const char *arch : { "arm32", "arm64", "x86_32", "x86_64" }) {
						if (p_features.has(arch)) {
							targets.push_back(arch);
						}
					}
				} else if (platform == "windows" || platform == "linux" || platform == "macos") {
					targets.push_back(preset->get("binary_format/architecture"));
				}
				if (targets.is_empty()) {
					error = p_path + ": CGD native C requires a supported desktop or Android export target.";
				}
				for (const String &arch : targets) {
					native_units[arch].push_back({ p_path, native_id, native_source });
				}
			}
		}
		if (!error.is_empty()) {
			get_export_preset()->get_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "GDScript / GD-C", error);
			fail_export(ERR_INVALID_DATA); // Consumed by EditorExportPlatform's per-plugin check.
			skip(); // Never silently package failed source as successful compiled output.
		}
	}

	virtual void _export_finalize_files() override {
		if (native_units.is_empty()) {
			return;
		}
		const String platform = get_export_preset()->get_platform()->get_os_name().to_lower();
		for (const auto &entry : native_units) {
			String library;
			String error;
			if (GDCNative::get_singleton()->build_export(entry.value, platform, entry.key, library, error, native_export_workspaces) != OK) {
				get_export_preset()->get_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "GDScript / GD-C", error);
				fail_export(ERR_COMPILATION_FAILED);
				return;
			}
			const Vector<String> tags = platform == "android" ? Vector<String>{ entry.key } : Vector<String>();
			const String destination = platform == "windows" || platform == "linux" ? String("cgd_native") : String();
			add_shared_object(library, tags, destination);
			for (const String &runtime : GDCNative::runtime_libraries(platform, entry.key)) {
				add_shared_object(ProjectSettings::get_singleton()->globalize_path(runtime), tags, destination);
			}
		}
		native_units.clear();
	}

	virtual void _export_end() override {
		native_export_workspaces.clear();
		native_units.clear();
	}

public:
	virtual String get_name() const override { return "GDScript"; }
};

static void _editor_init() {
	EditorNode::add_build_callback(GDCNative::prepare_editor_run);
	Ref<GDScriptExportPlugin> gd_export;
	gd_export.instantiate();
	EditorExport::get_singleton()->add_export_plugin(gd_export);

#ifdef TOOLS_ENABLED
	Ref<GDScriptSyntaxHighlighter> gdscript_syntax_highlighter;
	gdscript_syntax_highlighter.instantiate();
	ScriptEditor::get_singleton()->register_syntax_highlighter(gdscript_syntax_highlighter);
#endif
}

#endif // TOOLS_ENABLED

void initialize_gdscript_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		GLOBAL_DEF(PropertyInfo(Variant::STRING, "cgd_native/c_standard", PROPERTY_HINT_ENUM, "c11,c17,c23,gnu11,gnu17,gnu23"), "c17");
		GLOBAL_DEF(PropertyInfo(Variant::STRING, "cgd_native/cpp_standard", PROPERTY_HINT_ENUM, "c++20,c++23,gnu++20,gnu++23"), "c++20");
		GDREGISTER_CLASS(GDScript);
		GDREGISTER_INTERNAL_CLASS(GDScriptFunctionState);
		GDREGISTER_ABSTRACT_CLASS(GDCNative);
		GDCNative *native = memnew(GDCNative);
		Engine::get_singleton()->add_singleton(Engine::Singleton("CGDNative", native, "GDCNative"));

		script_language_gd = memnew(GDScriptLanguage);
		ScriptServer::register_language(script_language_gd);

		resource_loader_gd.instantiate();
		ResourceLoader::add_resource_format_loader(resource_loader_gd);

		resource_saver_gd.instantiate();
		ResourceSaver::add_resource_format_saver(resource_saver_gd);

		gdscript_cache = memnew(GDScriptCache);

		GDScriptUtilityFunctions::register_functions();
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		EditorNode::add_init_callback(_editor_init);

		gdscript_translation_parser_plugin.instantiate();
		EditorTranslationParser::get_singleton()->add_parser(gdscript_translation_parser_plugin, EditorTranslationParser::STANDARD);
	} else if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		memnew(GDScriptEditorLanguage);

		GDREGISTER_CLASS(GDScriptSyntaxHighlighter);
#ifndef GDSCRIPT_NO_LSP
		register_lsp_types();
		memnew(GDScriptLanguageProtocol);
		EditorPlugins::add_by_type<GDScriptLanguageServer>();

		Engine::Singleton singleton("GDScriptLanguageProtocol", GDScriptLanguageProtocol::get_singleton());
		singleton.editor_only = true;
		Engine::get_singleton()->add_singleton(singleton);
#endif // !GDSCRIPT_NO_LSP
	}
#endif // TOOLS_ENABLED
}

void uninitialize_gdscript_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		ScriptServer::unregister_language(script_language_gd);
		Engine::get_singleton()->remove_singleton("CGDNative");
		memdelete(GDCNative::get_singleton());

		memdelete(gdscript_cache);
		memdelete(script_language_gd);

		ResourceLoader::remove_resource_format_loader(resource_loader_gd);
		resource_loader_gd.unref();

		ResourceSaver::remove_resource_format_saver(resource_saver_gd);
		resource_saver_gd.unref();

		GDScriptParser::cleanup();
		GDScriptUtilityFunctions::unregister_functions();
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorTranslationParser::get_singleton()->remove_parser(gdscript_translation_parser_plugin, EditorTranslationParser::STANDARD);
		gdscript_translation_parser_plugin.unref();
#ifndef GDSCRIPT_NO_LSP
		memdelete(GDScriptLanguageProtocol::get_singleton());
#endif // GDSCRIPT_NO_LSP
		memdelete(GDScriptEditorLanguage::get_singleton());
	}
#endif // TOOLS_ENABLED
}

#ifdef TESTS_ENABLED
void test_tokenizer() {
	GDScriptTests::test(GDScriptTests::TestType::TEST_TOKENIZER);
}

void test_tokenizer_buffer() {
	GDScriptTests::test(GDScriptTests::TestType::TEST_TOKENIZER_BUFFER);
}

void test_parser() {
	GDScriptTests::test(GDScriptTests::TestType::TEST_PARSER);
}

void test_compiler() {
	GDScriptTests::test(GDScriptTests::TestType::TEST_COMPILER);
}

void test_bytecode() {
	GDScriptTests::test(GDScriptTests::TestType::TEST_BYTECODE);
}

REGISTER_TEST_COMMAND("gdscript-tokenizer", &test_tokenizer);
REGISTER_TEST_COMMAND("gdscript-tokenizer-buffer", &test_tokenizer_buffer);
REGISTER_TEST_COMMAND("gdscript-parser", &test_parser);
REGISTER_TEST_COMMAND("gdscript-compiler", &test_compiler);
REGISTER_TEST_COMMAND("gdscript-bytecode", &test_bytecode);
#endif

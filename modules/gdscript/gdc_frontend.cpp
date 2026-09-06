// SPDX-License-Identifier: MIT
#include "gdc_frontend.h"

#include "gdscript_parser.h"

#include <utility>

namespace {
std::vector<uint8_t> to_std_bytes(const Vector<uint8_t> &p_bytes) {
	if (p_bytes.is_empty()) {
		return {};
	}
	return std::vector<uint8_t>(p_bytes.ptr(), p_bytes.ptr() + p_bytes.size());
}
Vector<uint8_t> to_godot_bytes(const std::vector<uint8_t> &p_bytes) {
	Vector<uint8_t> result;
	result.resize(p_bytes.size());
	uint8_t *out = result.ptrw();
	for (size_t i = 0; i < p_bytes.size(); ++i) {
		out[i] = p_bytes[i];
	}
	return result;
}
} // namespace

bool GDCFrontend::is_source_path(const String &p_path) {
	return p_path.get_extension().to_lower() == "cgd";
}

bool GDCFrontend::is_binary_path(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	// .gdc remains native compiled GDScript; .cgd is always source text.
	// .gdbin contains the compiled GD-C payload and its source map.
	return extension == "gdc" || extension == "gdbin";
}

Error GDCFrontend::transpile(const String &p_source, String &r_source, GDC::SourceMap &r_map,
		String &r_error, int &r_line, int &r_column, bool p_completion, GDC::Position p_completion_cursor) {
	r_source = String();
	r_map = GDC::SourceMap();
	r_error = String();
	r_line = 1;
	r_column = 1;
	std::u32string input;
	input.reserve(p_source.length());
	for (int i = 0; i < p_source.length(); ++i) {
		input.push_back(p_source[i]);
	}
	GDC::Options options;
	options.completion = p_completion;
	options.completion_cursor = p_completion_cursor;
	GDC::Result result = GDC::transpile(input, options);
	if (!result.ok() && p_completion && p_completion_cursor.line > 0) {
		// Retry only failed input, preserving valid multiline expressions as written.
		options.completion_line = p_completion_cursor.line;
		result = GDC::transpile(input, options);
	}
	if (!result.ok()) {
		const GDC::Diagnostic &diagnostic = result.diagnostics.front();
		r_error = String::utf8(diagnostic.message.c_str());
		r_line = diagnostic.position.line;
		r_column = diagnostic.position.column;
		return ERR_PARSE_ERROR;
	}
	const std::string utf8 = GDC::encode_utf8(result.code);
	r_source = String::utf8(utf8.c_str(), utf8.size());
	r_map = std::move(result.map);
	return OK;
}

Vector<uint8_t> GDCFrontend::compile_binary(const String &p_source, const String &p_path,
		GDScriptTokenizerBuffer::CompressMode p_mode, String &r_error) {
	r_error = String();
	String source = p_source;
	GDC::SourceMap map;
	if (is_source_path(p_path)) {
		// The native token serializer encodes ERROR tokens too. Validate syntax first,
		// otherwise a nonempty token buffer could conceal an invalid GD-C export.
		GDScriptParser parser;
		if (parser.parse(p_source, p_path, false) != OK) {
			const GDScriptParser::ParserError &failure = parser.get_errors().front()->get();
			r_error = vformat("%s:%d:%d: %s", p_path, failure.start_line, failure.start_column, failure.message);
			return Vector<uint8_t>();
		}
		String message;
		int line = 1;
		int column = 1;
		String generated;
		if (transpile(source, generated, map, message, line, column) != OK) {
			r_error = vformat("%s:%d:%d: %s", p_path, line, column, message);
			return Vector<uint8_t>();
		}
		source = generated;
	}
	Vector<uint8_t> tokens = GDScriptTokenizerBuffer::parse_code_string(source, p_mode);
	if (tokens.is_empty()) {
		r_error = vformat("%s: Native GDScript tokenization failed.", p_path);
		return tokens;
	}
	if (map.empty()) {
		return tokens; // Ordinary .gd retains the native token format.
	}
	Vector<uint8_t> wrapped = to_godot_bytes(GDC::encode_binary(to_std_bytes(tokens), map));
	if (wrapped.is_empty()) {
		r_error = vformat("%s: GD-C binary exceeds the supported size limit.", p_path);
	}
	return wrapped;
}

Error GDCFrontend::unpack_binary(const Vector<uint8_t> &p_binary, Vector<uint8_t> &r_tokens,
		GDC::SourceMap &r_map, String &r_error) {
	r_map = GDC::SourceMap();
	r_error = String();
	if (!GDC::has_binary_envelope(p_binary.ptr(), p_binary.size())) {
		r_tokens = p_binary;
		return OK; // Let the existing native decoder validate native/legacy files.
	}
	std::vector<uint8_t> native;
	std::string error;
	if (!GDC::decode_binary(to_std_bytes(p_binary), native, r_map, error)) {
		r_tokens.clear();
		r_error = String::utf8(error.c_str());
		return ERR_FILE_CORRUPT;
	}
	r_tokens = to_godot_bytes(native);
	return OK;
}

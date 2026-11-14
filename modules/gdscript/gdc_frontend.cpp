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
		String &r_error, int &r_line, int &r_column, bool p_completion, GDC::Position p_completion_cursor,
		const String &p_path, String *r_native_source, int *r_native_completion_offset, String *r_native_id,
		std::vector<GDC::Result::NativeBody> *r_native_bodies) {
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
	options.script_path.assign(p_path.get_data(), p_path.length());
	options.native_library_id.assign(64, U'0');
	if (r_native_id) {
		*r_native_id = String();
	}
	if (r_native_source) {
		*r_native_source = String();
	}
	options.completion = p_completion;
	options.completion_cursor = p_completion_cursor;
	options.native_completion = r_native_completion_offset != nullptr;
	if (r_native_completion_offset) {
		*r_native_completion_offset = -1;
	}
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
	if (!result.native_code.empty()) {
		const String precision = "#define CGD_REAL_BITS " + itos(sizeof(real_t) * 8) + "\n";
		std::u32string normalized = result.native_code;
		for (auto it = result.native_line_numbers.rbegin(); it != result.native_line_numbers.rend(); ++it) {
			normalized.replace(it->first, it->second, U"1");
		}
		const std::string native_utf8 = GDC::encode_utf8(normalized);
		const String id = (p_path + "\n" + precision + String::utf8(native_utf8.data(), native_utf8.size())).sha256_text();
		for (size_t offset : result.native_library_id_offsets) {
			result.code.replace(offset, 64, id.get_data(), 64);
		}
		for (size_t offset : result.native_wrapper_symbol_offsets) {
			result.code.replace(offset, 16, id.get_data(), 16);
		}
		for (size_t offset : result.native_c_symbol_offsets) {
			result.native_code.replace(offset, 16, id.get_data(), 16);
		}
		if (r_native_id) {
			*r_native_id = id;
		}
		if (r_native_source) {
			const std::string native_code = GDC::encode_utf8(result.native_code);
			*r_native_source = precision + String::utf8(native_code.data(), native_code.size());
		}
		if (result.native_completion_offset != std::u32string::npos) {
			result.native_completion_offset += precision.length();
		}
		for (auto &body : result.native_bodies) {
			body.offset += precision.length();
		}
	}
	const std::string utf8 = GDC::encode_utf8(result.code);
	r_source = String::utf8(utf8.c_str(), utf8.size());
	r_map = std::move(result.map);
	if (r_native_completion_offset && result.native_completion_offset != std::u32string::npos) {
		*r_native_completion_offset = int(result.native_completion_offset);
	}
	if (r_native_bodies) {
		*r_native_bodies = std::move(result.native_bodies);
	}
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
		if (transpile(source, generated, map, message, line, column, false, { 0, 0 }, p_path) != OK) {
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

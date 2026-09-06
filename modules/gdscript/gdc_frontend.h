// SPDX-License-Identifier: MIT
#ifndef GDC_FRONTEND_H
#define GDC_FRONTEND_H

#include "gdc/transpiler.h"
#include "gdscript_tokenizer_buffer.h"

// All engine-specific glue lives here. The structural frontend remains directly testable.
class GDCFrontend {
public:
	static bool is_source_path(const String &p_path);
	static bool is_binary_path(const String &p_path);
	static Error transpile(const String &p_source, String &r_source, GDC::SourceMap &r_map,
			String &r_error, int &r_line, int &r_column, bool p_completion = false, GDC::Position p_completion_cursor = { 0, 0 });
	static Vector<uint8_t> compile_binary(const String &p_source, const String &p_path,
			GDScriptTokenizerBuffer::CompressMode p_mode, String &r_error);
	static Error unpack_binary(const Vector<uint8_t> &p_binary, Vector<uint8_t> &r_tokens,
			GDC::SourceMap &r_map, String &r_error);
};

#endif // GDC_FRONTEND_H

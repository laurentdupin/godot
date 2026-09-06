// SPDX-License-Identifier: MIT
// GD-C: a source-to-source frontend for GDScript. No engine or runtime dependencies.
#ifndef GDC_TRANSPILER_H
#define GDC_TRANSPILER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GDC {

struct Position {
	uint32_t line = 1;
	uint32_t column = 1; // One-based Unicode code-point column, not a UTF-8 byte offset.
	bool operator==(const Position &p_other) const { return line == p_other.line && column == p_other.column; }
};

struct MapRun {
	uint32_t offset = 0;
	Position source;
	uint32_t step = 0; // 0 for inserted characters, 1 for a copied same-line run.
};

struct SourceMap {
	uint32_t generated_size = 0;
	std::vector<uint32_t> line_starts;
	std::vector<MapRun> runs;
	Position completion_source = { 0, 0 }; // Transient editor data, not serialized.
	Position completion_generated = { 0, 0 };
	bool empty() const { return runs.empty(); }
	Position original(uint32_t p_line, uint32_t p_column) const;
	void original_range(Position p_start, Position p_end, Position &r_start, Position &r_end) const;
	Position generated(Position p_original) const; // Best-effort reverse map for an editor cursor.
};

struct Diagnostic {
	Position position;
	std::string message;
};

struct Options {
	bool completion = false; // Close unfinished groups and allow a final missing semicolon.
	uint32_t completion_line = 0; // Recovery boundary for an unfinished editor statement.
	Position completion_cursor = { 0, 0 };
	uint32_t max_nesting = 128;
	uint32_t max_source_chars = 4 * 1024 * 1024;
	uint32_t max_output_chars = 16 * 1024 * 1024;
};

struct Result {
	std::u32string code;
	SourceMap map;
	std::vector<Diagnostic> diagnostics;
	bool ok() const { return diagnostics.empty(); }
};

Result transpile(const std::u32string &p_source, const Options &p_options = Options());

// The GDCB envelope contains native GDSC tokens plus a compact source map, NOT C source.
// Native token bytes remain unchanged (including their normal optional Zstandard compression).
std::vector<uint8_t> encode_binary(const std::vector<uint8_t> &p_tokens, const SourceMap &p_map);
bool has_binary_envelope(const uint8_t *p_data, size_t p_size);
bool decode_binary(const std::vector<uint8_t> &p_data, std::vector<uint8_t> &r_tokens,
		SourceMap &r_map, std::string &r_error);

// Strict UTF-8 helpers for the optional standalone tool.
bool decode_utf8(const std::string &p_bytes, std::u32string &r_text, std::string &r_error);
std::string encode_utf8(const std::u32string &p_text);

} // namespace GDC
#endif // GDC_TRANSPILER_H

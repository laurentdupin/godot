// SPDX-License-Identifier: MIT
// Optional developer CLI. Godot itself calls the C++ frontend directly.
#include "gdc/transpiler.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char **argv) {
	if (argc < 2 || argc > 3) {
		std::cerr << "Usage: gdc INPUT.cgd [OUTPUT.gd]\n";
		return 2;
	}
	std::ifstream in(argv[1], std::ios::binary);
	if (!in) { std::cerr << "Cannot read input file.\n"; return 2; }
	std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	std::u32string source;
	std::string error;
	if (!GDC::decode_utf8(bytes, source, error)) { std::cerr << error << '\n'; return 1; }
	GDC::Result result = GDC::transpile(source);
	if (!result.ok()) {
		for (const auto &d : result.diagnostics) {
			std::cerr << argv[1] << ':' << d.position.line << ':' << d.position.column << ": " << d.message << '\n';
		}
		return 1;
	}
	std::string code = GDC::encode_utf8(result.code);
	if (argc == 3) {
		std::error_code file_error;
		if (std::filesystem::equivalent(argv[1], argv[2], file_error) || std::string(argv[1]) == argv[2]) { std::cerr << "Refusing to overwrite the input.\n"; return 2; }
		std::ofstream out(argv[2], std::ios::binary);
		out.write(code.data(), static_cast<std::streamsize>(code.size()));
		if (!out) { std::cerr << "Cannot write output file.\n"; return 2; }
	} else { std::cout << code; }
	return 0;
}

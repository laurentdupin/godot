// SPDX-License-Identifier: MIT
#include "gdc/transpiler.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace GDC {
namespace {

constexpr size_t NO_INDEX = static_cast<size_t>(-1);
constexpr uint32_t MAX_BINARY_BYTES = 256 * 1024 * 1024;

enum class Kind { WORD,
	NUMBER,
	STRING,
	SYMBOL,
	DOC,
	END };
struct Token {
	Kind kind = Kind::END;
	std::u32string text;
	size_t offset = 0;
	size_t mate = NO_INDEX;
	Position position;
};

bool digit(char32_t c) {
	return c >= U'0' && c <= U'9';
}
bool word_start(char32_t c) {
	return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'_' ||
			(c >= 128 && c != 0xFEFF && c != 0xFFFF && !(c >= 0xD800 && c <= 0xDFFF));
}
bool word_char(char32_t c) {
	return word_start(c) || digit(c);
}
bool opening(const std::u32string &s) {
	return s == U"(" || s == U"[" || s == U"{";
}
std::u32string closing(const std::u32string &s) {
	return s == U"(" ? U")" : s == U"[" ? U"]"
										: U"}";
}

class Frontend {
	const std::u32string &source;
	const Options options;
	Result result;
	std::vector<Position> locations;
	std::vector<Position> output_locations;
	std::vector<Token> tokens;
	size_t cursor = 0;
	uint32_t indent = 0;
	uint32_t recursion = 0;
	uint32_t next_loop = 0;
	bool line_start = true;
	size_t completion_offset = NO_INDEX;

	bool failed() const { return !result.diagnostics.empty(); }
	const Token &token(size_t i) const { return tokens[std::min(i, tokens.size() - 1)]; }
	const Token &peek() const { return token(cursor); }
	bool at(const char32_t *s) const { return peek().text == s; }
	bool take(const char32_t *s) {
		if (!at(s)) {
			return false;
		}
		++cursor;
		return true;
	}
	void error(Position p, const std::string &message) {
		if (!failed()) {
			result.diagnostics.push_back({ p, message });
		}
	}
	bool expect(const char32_t *s) {
		if (take(s)) {
			return true;
		}
		error(peek().position, "Expected '" + encode_utf8(s) + "'.");
		return false;
	}
	bool enter(Position p) {
		if (++recursion > options.max_nesting) {
			error(p, "Maximum syntax nesting exceeded.");
			--recursion;
			return false;
		}
		return true;
	}
	void leave() { --recursion; }
	void append(char32_t c, Position p) {
		if (failed()) {
			return;
		}
		if (result.code.size() >= options.max_output_chars) {
			error(p, "Generated source exceeds the configured size limit.");
			return;
		}
		result.code.push_back(c);
		output_locations.push_back(p);
		line_start = c == U'\n';
	}
	void inserted(const std::u32string &s, Position p) {
		for (char32_t c : s) {
			append(c, p);
		}
	}
	void copied(size_t i) {
		const Token &t = token(i);
		for (size_t j = 0; j < t.text.size(); ++j) {
			if (options.completion && t.offset + j < source.size() && locations[t.offset + j] == options.completion_cursor) {
				completion_offset = result.code.size();
			}
			append(t.text[j], locations[std::min(t.offset + j, source.size())]);
		}
		if (options.completion && t.offset < source.size() && locations[std::min(t.offset + t.text.size(), source.size())] == options.completion_cursor) {
			completion_offset = result.code.size();
		}
	}
	void start(Position p) {
		if (line_start) {
			inserted(std::u32string(indent * 4, U' '), p);
		}
	}
	void newline(Position p) {
		if (!line_start) {
			append(U'\n', p);
		}
	}
	void line(const std::u32string &s, Position p) {
		start(p);
		inserted(s, p);
		newline(p);
	}
	void gap(size_t previous, size_t next) {
		if (line_start) {
			start(token(next).position);
		} else if (token(previous).offset + token(previous).text.size() < token(next).offset) {
			inserted(U" ", token(next).position);
		}
	}

	void lex() {
		locations.reserve(source.size() + 1);
		Position pos;
		for (char32_t c : source) {
			locations.push_back(pos);
			if (c == U'\n') {
				++pos.line;
				pos.column = 1;
			} else {
				++pos.column;
			}
		}
		locations.push_back(pos);
		size_t i = source.empty() || source[0] != 0xFEFF ? 0 : 1;
		auto ch = [&](size_t n) -> char32_t { return n < source.size() ? source[n] : U'\0'; };
		auto add = [&](Kind kind, size_t begin, size_t end) {
			tokens.push_back({ kind, source.substr(begin, end - begin), begin, NO_INDEX, locations[begin] });
		};
		while (i < source.size() && !failed()) {
			char32_t c = ch(i);
			if (c == U' ' || c == U'\t' || c == U'\r' || c == U'\n') {
				++i;
				continue;
			}
			if (c == U'\0' || c == 0xFFFF || c == 0xFEFF || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) {
				error(locations[i], "Invalid character in source.");
				break;
			}
			size_t begin = i;
			if (c == U'#') {
				error(locations[i], "Use // or /* */ comments in GD-C.");
				break;
			}
			if (c == U'/' && ch(i + 1) == U'/') {
				bool doc = ch(i + 2) == U'/';
				while (i < source.size() && ch(i) != U'\n') {
					++i;
				}
				if (doc) {
					add(Kind::DOC, begin, i);
				}
				continue;
			}
			if (c == U'/' && ch(i + 1) == U'*') {
				i += 2;
				while (i < source.size() && !(ch(i) == U'*' && ch(i + 1) == U'/')) {
					++i;
				}
				if (i == source.size()) {
					error(locations[begin], "Unterminated block comment.");
					break;
				}
				i += 2;
				continue;
			}
			bool prefixed = (c == U'r' || c == U'&' || c == U'^') && (ch(i + 1) == U'\'' || ch(i + 1) == U'"');
			if (prefixed || c == U'$' || c == U'\'') {
				error(locations[i], "Use double-quoted strings, StringName()/NodePath() constructors and get_node() in GD-C.");
				break;
			}
			if (c == U'"') {
				char32_t quote = ch(i);
				bool triple = ch(i + 1) == quote && ch(i + 2) == quote;
				if (triple) {
					error(locations[i], "Triple-quoted strings are not supported in GD-C; use escaped newlines.");
					break;
				}
				++i;
				bool ended = false;
				while (i < source.size()) {
					if (ch(i) == U'\\') {
						i += std::min<size_t>(2, source.size() - i);
						continue;
					}
					if (ch(i) == quote) {
						++i;
						ended = true;
						break;
					}
					if (ch(i) == U'\n' || ch(i) == U'\r') {
						break;
					}
					++i;
				}
				if (!ended) {
					error(locations[begin], "Unterminated string literal.");
					break;
				}
				add(Kind::STRING, begin, i);
				continue;
			}
			if (word_start(c)) {
				while (word_char(ch(i))) {
					++i;
				}
				add(Kind::WORD, begin, i);
				// Reject source aliases before lowering; strings, comments and member
				// names (such as String.match()) never take this keyword path.
				bool member = tokens.size() > 1 && tokens[tokens.size() - 2].text == U".";
				if (!member) {
					for (const char32_t *word : { U"func", U"fn", U"var", U"pass", U"elif", U"and", U"or", U"not", U"in", U"is", U"as", U"match" }) {
						if (tokens.back().text == word) {
							error(locations[begin], "GDScript-style '" + encode_utf8(word) + "' is not supported in GD-C. Use C-style syntax.");
							break;
						}
					}
				}
				if (tokens.back().text.compare(0, 6, U"__gdc_") == 0) {
					error(locations[begin], "The '__gdc_' prefix is reserved for generated local names.");
				}
				continue;
			}
			if (digit(c) || (c == U'.' && digit(ch(i + 1)))) {
				++i;
				while (digit(ch(i)) || word_start(ch(i)) ||
						(ch(i) == U'.' && ch(i + 1) != U'.' && ch(i - 1) != U'.') ||
						((ch(i) == U'+' || ch(i) == U'-') && (ch(i - 1) == U'e' || ch(i - 1) == U'E'))) {
					++i;
				}
				add(Kind::NUMBER, begin, i);
				continue;
			}
			if (c == U':' && ch(i + 1) == U'=') {
				error(locations[i], "Put the type before the name in GD-C; use '=' for initialization.");
				break;
			}
			static const char32_t *operators[] = {
				U"**=", U"<<=", U">>=", U"->", U"==", U"!=", U"<=", U">=", U"&&", U"||", U"++", U"--",
				U"+=", U"-=", U"*=", U"/=", U"%=", U"&=", U"|=", U"^=", U"<<", U">>", U"**", U".."
			};
			bool found = false;
			for (const char32_t *op : operators) {
				std::u32string s(op);
				if (source.compare(i, s.size(), s) == 0) {
					i += s.size();
					add(Kind::SYMBOL, begin, i);
					found = true;
					break;
				}
			}
			if (!found) {
				++i;
				add(Kind::SYMBOL, begin, i);
			}
		}
		std::vector<size_t> stack;
		for (size_t n = 0; n < tokens.size() && !failed(); ++n) {
			Token &t = tokens[n];
			if (t.kind != Kind::SYMBOL) {
				continue;
			}
			if (opening(t.text)) {
				stack.push_back(n);
				if (stack.size() > options.max_nesting) {
					error(t.position, "Maximum delimiter nesting exceeded.");
				}
			} else if (t.text == U")" || t.text == U"]" || t.text == U"}") {
				if (stack.empty() || closing(tokens[stack.back()].text) != t.text) {
					error(t.position, "Mismatched closing delimiter.");
					break;
				}
				t.mate = stack.back();
				tokens[stack.back()].mate = n;
				stack.pop_back();
			}
		}
		while (!stack.empty() && !failed()) {
			size_t index = stack.back();
			stack.pop_back();
			if (!options.completion) {
				error(tokens[index].position, "Unclosed delimiter.");
				break;
			}
			tokens[index].mate = tokens.size();
			tokens.push_back({ Kind::SYMBOL, closing(tokens[index].text), source.size(), index, locations.back() });
		}
		tokens.push_back({ Kind::END, U"", source.size(), NO_INDEX, locations.back() });
	}

	// Return the first top-level token matching s, ignoring nested groups.
	size_t find(size_t a, size_t b, const char32_t *s) const {
		for (size_t n = a; n < b; ++n) {
			if (token(n).text == s) {
				return n;
			}
			if (opening(token(n).text) && token(n).mate < b) {
				n = token(n).mate;
			}
		}
		return NO_INDEX;
	}
	std::vector<std::pair<size_t, size_t>> split(size_t a, size_t b, const char32_t *s) const {
		std::vector<std::pair<size_t, size_t>> out;
		size_t begin = a;
		while (begin < b) {
			size_t end = find(begin, b, s);
			if (end == NO_INDEX) {
				end = b;
			}
			out.emplace_back(begin, end);
			begin = end + 1;
		}
		return out;
	}
	bool reserved(const std::u32string &s) const {
		static const char32_t *words[] = { U"var", U"const", U"static", U"return", U"await", U"break", U"continue",
			U"pass", U"breakpoint", U"extends", U"class_name", U"signal", U"enum", U"func", U"fn", U"if", U"elif", U"else",
			U"for", U"while", U"switch", U"match", U"class", U"assert", U"preload", U"and", U"or", U"not", U"in", U"is", U"as" };
		for (const char32_t *w : words) {
			if (s == w) {
				return true;
			}
		}
		return false;
	}
	size_t type_end(size_t a, size_t b) const {
		if (a >= b || token(a).kind != Kind::WORD || reserved(token(a).text)) {
			return a;
		}
		size_t n = a + 1;
		while (n + 1 < b && token(n).text == U"." && token(n + 1).kind == Kind::WORD) {
			n += 2;
		}
		if (n < b && token(n).text == U"[" && token(n).mate < b) {
			n = token(n).mate + 1;
		}
		return n;
	}
	void parameters(size_t a, size_t b) {
		if (b == a + 1 && token(a).text == U"void") {
			return;
		}
		bool first = true;
		for (const auto &part : split(a, b, U",")) {
			if (!first) {
				inserted(U", ", token(part.first).position);
			}
			first = false;
			size_t end = type_end(part.first, part.second);
			if (end > part.first && end < part.second && token(end).kind == Kind::WORD &&
					(end + 1 == part.second || token(end + 1).text == U"=")) {
				copied(end);
				bool inferred = token(part.first).text == U"auto";
				if (inferred) {
					if (end + 1 == part.second) {
						error(token(part.first).position, "An auto parameter requires a default value.");
						return;
					}
					inserted(U":", token(end).position);
				} else if (token(part.first).text != U"dynamic") {
					inserted(U": ", token(end).position);
					expression(part.first, end);
				}
				if (end + 1 < part.second) {
					inserted(U" ", token(end + 1).position);
					expression(end + 1, part.second);
				}
			} else {
				error(token(part.first).position, "Parameters require C-style 'Type name' declarations; use Variant for a dynamic value.");
				return;
			}
		}
	}
	size_t unary_end(size_t a, size_t b) const {
		if (a >= b) {
			return a;
		}
		size_t n = a;
		while (n < b && (token(n).text == U"!" || token(n).text == U"~" || token(n).text == U"+" || token(n).text == U"-" || token(n).text == U"await")) {
			++n;
		}
		if (n == b) {
			return n;
		}
		if ((token(n).text == U"cast" || token(n).text == U"type_is") && token(n + 1).text == U"<") {
			size_t end_type = type_end(n + 2, b);
			if (token(end_type).text == U">" && token(end_type + 1).text == U"(" && token(end_type + 1).mate < b) {
				n = token(end_type + 1).mate + 1;
			} else {
				return n + 1;
			}
		} else if (opening(token(n).text)) {
			n = token(n).mate + 1;

		} else {
			++n;
		}
		while (n < b) {
			if (token(n).text == U"." && n + 1 < b) {
				n += 2;
			} else if ((token(n).text == U"(" || token(n).text == U"[") && token(n).mate < b) {
				n = token(n).mate + 1;
			} else {
				break;
			}
		}
		return n;
	}
	void expression(size_t a, size_t b) {
		if (a >= b || failed()) {
			return;
		}
		if (!enter(token(a).position)) {
			return;
		}
		// Split commas first: ternaries in one argument must not consume the next argument.
		const auto parts = split(a, b, U",");
		if (parts.size() > 1 || (b > a && token(b - 1).text == U",")) {
			for (size_t i = 0; i < parts.size(); ++i) {
				if (i) {
					inserted(U", ", token(parts[i].first).position);
				}
				expression(parts[i].first, parts[i].second);
			}
			if (token(b - 1).text == U",") {
				copied(b - 1);
			}
			leave();
			return;
		}
		// Assignment/typed declarations and dictionary key separators bind outside ternaries.
		size_t assign = NO_INDEX;
		for (const char32_t *op : { U"=", U"+=", U"-=", U"*=", U"/=", U"%=", U"**=", U"<<=", U">>=", U"&=", U"|=", U"^=" }) {
			size_t found = find(a, b, op);
			if (found < assign) {
				assign = found;
			}
		}
		if (assign != NO_INDEX) {
			expression(a, assign);
			if (assign > a) {
				inserted(U" ", token(assign).position);
			}
			copied(assign);
			inserted(U" ", token(assign).position);
			expression(assign + 1, b);
			leave();
			return;
		}
		size_t question = find(a, b, U"?");
		size_t colon = find(a, b, U":");
		if (colon != NO_INDEX && (question == NO_INDEX || colon < question)) {
			expression(a, colon);
			copied(colon);
			inserted(U" ", token(colon).position);
			expression(colon + 1, b);
			leave();
			return;
		}
		if (question != NO_INDEX) {
			int nested = 0;
			size_t separator = NO_INDEX;
			for (size_t n = question + 1; n < b; ++n) {
				if (opening(token(n).text)) {
					n = token(n).mate;
					continue;
				}
				if (token(n).text == U"?") {
					++nested;
				}
				if (token(n).text == U":" && nested-- == 0) {
					separator = n;
					break;
				}
			}
			if (separator == NO_INDEX || a == question || question + 1 == separator || separator + 1 == b) {
				error(token(question).position, "Expected 'condition ? true_value : false_value'.");
				leave();
				return;
			}
			inserted(U"(", token(question).position);
			expression(question + 1, separator);
			inserted(U" if ", token(question).position);
			expression(a, question);
			inserted(U" else ", token(separator).position);
			expression(separator + 1, b);
			inserted(U")", token(separator).position);
			leave();
			return;
		}
		for (size_t n = a; n < b && !failed(); ++n) {
			if (n > a) {
				gap(n - 1, n);
			}
			const Token &t = token(n);
			if (t.kind == Kind::DOC) {
				continue;
			}
			if ((t.text == U"if" || t.text == U"else") && (n == a || token(n - 1).text != U".")) {
				error(t.position, "Use 'condition ? yes : no' instead of a GDScript conditional expression.");
				break;
			}
			if (t.text == U"%" && (n == a || (token(n - 1).kind == Kind::SYMBOL && token(n - 1).text != U")" && token(n - 1).text != U"]"))) {
				error(t.position, "Use get_node() instead of the GDScript unique-node shorthand.");
				break;
			}
			if (t.text == U"++" || t.text == U"--") {
				error(t.position, "Increment/decrement is supported only as a standalone operation on a local name.");
				break;
			}
			if ((t.text == U"cast" || t.text == U"type_is") && (n == a || token(n - 1).text != U".") && token(n + 1).text == U"<") {
				size_t type_stop = type_end(n + 2, b);
				if (type_stop == n + 2 || token(type_stop).text != U">" || token(type_stop + 1).text != U"(" || token(type_stop + 1).mate >= b) {
					error(t.position, "Expected cast<Type>(value) or type_is<Type>(value).");
					break;
				}
				size_t end = token(type_stop + 1).mate;
				if (end == type_stop + 2 || split(type_stop + 2, end, U",").size() != 1) {
					error(t.position, "A cast or type test requires one value.");
					break;
				}
				inserted(U"(", t.position);
				expression(type_stop + 2, end);
				inserted(t.text == U"cast" ? U" as " : U" is ", t.position);
				expression(n + 2, type_stop);
				inserted(U")", t.position);
				n = end;
				continue;
			}
			if (t.text == U"is_in" && (n == a || token(n - 1).text != U".") && token(n + 1).text == U"(" && token(n + 1).mate < b) {
				size_t end = token(n + 1).mate;
				const auto args = split(n + 2, end, U",");
				if (args.size() != 2) {
					error(t.position, "Expected is_in(value, container).");
					break;
				}
				inserted(U"(", t.position);
				expression(args[0].first, args[0].second);
				inserted(U" in ", t.position);
				expression(args[1].first, args[1].second);
				inserted(U")", t.position);
				n = end;
				continue;
			}
			if (t.text == U"!") {
				size_t end = unary_end(n + 1, b);
				if (end == n + 1) {
					error(t.position, "Expected an operand after '!'.");
					break;
				}
				inserted(U"(not ", t.position);
				expression(n + 1, end);
				inserted(U")", t.position);
				n = end - 1;
			} else if (t.text == U"&&" || t.text == U"||") {
				inserted(t.text == U"&&" ? U" and " : U" or ", t.position);
			} else if (t.text == U"[" && t.mate == n + 1 && token(n + 2).text == U"(") {
				size_t saved = cursor;
				cursor = n;
				function(true);
				n = cursor - 1;
				cursor = saved;
				start(t.position);
			} else if (opening(t.text)) {
				if (t.mate >= b) {
					error(t.position, "Expression crosses a delimiter boundary.");
					break;
				}
				copied(n);
				expression(n + 1, t.mate);
				start(token(t.mate).position);
				copied(t.mate);
				n = t.mate;
			} else {
				copied(n);
			}
		}
		leave();
	}
	void declaration(size_t a, size_t b, bool loop_binding = false) {
		if (a == b) {
			return;
		}
		if (b - a == 2 && ((token(a).kind == Kind::WORD && (token(a + 1).text == U"++" || token(a + 1).text == U"--")) || (token(a + 1).kind == Kind::WORD && (token(a).text == U"++" || token(a).text == U"--")))) {
			size_t name = token(a).kind == Kind::WORD ? a : a + 1;
			size_t op = name == a ? a + 1 : a;
			copied(name);
			inserted(token(op).text == U"++" ? U" += 1" : U" -= 1", token(op).position);
			return;
		}
		if (token(a).text == U"static") {
			copied(a);
			inserted(U" ", token(a).position);
			++a;
		}
		bool constant = token(a).text == U"const";
		if (constant) {
			copied(a);
			inserted(U" ", token(a).position);
			++a;
		}

		size_t end = type_end(a, b);
		if (end > a && end < b && token(end).kind == Kind::WORD &&
				(end + 1 == b || token(end + 1).text == U"=")) {
			if (!constant && !loop_binding) {
				inserted(U"var ", token(a).position);
			}
			copied(end);
			if (token(a).text != U"auto" && token(a).text != U"dynamic") {
				inserted(U": ", token(end).position);
				expression(a, end);
			} else if (token(a).text == U"auto" && !loop_binding && end + 1 == b) {
				error(token(a).position, "An auto declaration requires an initializer.");
				return;
			}
			if (end + 1 < b) {
				if (!loop_binding && token(a).text == U"auto") {
					inserted(U":", token(end).position);
				}
				inserted(U" ", token(end + 1).position);
				expression(end + 1, b);
			}
		} else {
			if (constant || loop_binding || (a + 1 < b && token(a + 1).text == U":")) {
				error(token(a).position, "Declarations require 'Type name' or 'auto name = value'.");
				return;
			}
			expression(a, b);
		}
	}
	void semicolon() {
		const bool cursor_boundary = options.completion_line > 0 && cursor > 0 &&
				token(cursor - 1).position.line <= options.completion_line &&
				(peek().position.line > options.completion_line || at(U"}"));
		if (!take(U";") && !(options.completion && (peek().kind == Kind::END || peek().offset == source.size() || cursor_boundary))) {
			error(peek().position, "Expected ';' after a simple statement.");
		}
	}
	size_t simple_end(size_t a) const {
		for (size_t n = a; token(n).kind != Kind::END; ++n) {
			if (options.completion && options.completion_line > 0 && n > a &&
					token(n - 1).position.line <= options.completion_line && token(n).position.line > options.completion_line) {
				return n;
			}
			if (token(n).text == U";" || token(n).text == U"}") {
				return n;
			}
			if (token(n).text == U"{" &&
					((token(n + 1).text == U"get" && (token(n + 2).text == U"{" || token(n + 2).text == U"(")) ||
							(token(n + 1).text == U"set" && token(n + 2).text == U"("))) {
				return n;
			}
			if (opening(token(n).text)) {
				n = token(n).mate;
			}
		}
		return tokens.size() - 1;
	}
	void body(bool class_body = false) {
		if (!expect(U"{")) {
			return;
		}
		Position origin = token(cursor - 1).position;
		if (!enter(origin)) {
			return;
		}
		++indent;
		size_t count = 0;
		while (!at(U"}") && peek().kind != Kind::END && !failed()) {
			size_t old = cursor;
			bool executable = peek().kind != Kind::DOC && !at(U";") && !at(U"@");
			statement(class_body);
			if (cursor == old) {
				error(peek().position, "Parser made no progress.");
				break;
			}
			if (executable) {
				++count;
			}
		}
		if (!count) {
			line(U"pass", origin);
		}
		expect(U"}");
		--indent;
		leave();
	}
	void function(bool lambda = false) {
		size_t begin = cursor;
		Position origin = peek().position;
		if (!lambda) {
			start(origin);
		}
		bool is_static = take(U"static");
		if (is_static) {
			inserted(U"static ", origin);
		}
		size_t type_begin = cursor;
		size_t type_stop = cursor;
		if (lambda) {
			cursor += 2; // The C++-style [] lambda introducer.
		} else {
			type_stop = type_end(cursor, tokens.size() - 1);
			cursor = type_stop;
		}
		inserted(U"func", origin);
		if (!lambda) {
			if (peek().kind != Kind::WORD || token(type_begin).text == U"auto") {
				error(peek().position, "Expected an explicit return type and function name.");
				return;
			}
			inserted(U" ", peek().position);
			copied(cursor++);
		}
		size_t open = cursor;
		if (!expect(U"(")) {
			return;
		}
		size_t close = token(open).mate;
		inserted(U"(", token(open).position);
		parameters(cursor, close);
		inserted(U")", token(close).position);
		cursor = close + 1;
		if (!lambda && token(type_begin).text != U"dynamic") {
			inserted(U" -> ", token(type_begin).position);
			expression(type_begin, type_stop);
		} else if (lambda && take(U"->")) {
			size_t return_end = type_end(cursor, tokens.size() - 1);
			if (return_end == cursor || at(U"auto")) {
				error(peek().position, "Expected an explicit lambda return type.");
				return;
			}
			inserted(U" -> ", peek().position);
			expression(cursor, return_end);
			cursor = return_end;
		}
		if (at(U";") && !lambda) { // @abstract function declaration: native parser validates the annotation.
			++cursor;
			newline(origin);
			return;
		}
		inserted(U":", peek().position);
		newline(origin);
		body();
		if (!lambda) {
			take(U";");
		}
		if (cursor <= begin) {
			error(origin, "Invalid function.");
		}
	}
	std::pair<size_t, size_t> parenthesized() {
		size_t open = cursor;
		if (!expect(U"(")) {
			return { cursor, cursor };
		}
		size_t close = token(open).mate;
		cursor = close + 1;
		return { open + 1, close };
	}
	void control(const std::u32string &keyword) {
		Position origin = peek().position;
		++cursor;
		start(origin);
		inserted(keyword, origin);
		if (keyword != U"else") {
			auto condition = parenthesized();
			if (condition.first == condition.second) {
				error(origin, "A control-flow condition cannot be empty.");
				return;
			}
			inserted(U" ", origin);
			expression(condition.first, condition.second);
		}
		inserted(U":", peek().position);
		newline(origin);
		body();
	}
	void for_loop() {
		Position origin = peek().position;
		++cursor;
		auto header = parenthesized();
		size_t first = find(header.first, header.second, U";");
		if (first == NO_INDEX) {
			size_t in = find(header.first, header.second, U":");
			if (in == NO_INDEX || in == header.first || in + 1 == header.second) {
				error(origin, "Expected 'for (Type name : iterable)' or a three-clause C-style for loop.");
				return;
			}
			start(origin);
			inserted(U"for ", origin);
			declaration(header.first, in, true);
			inserted(U" in ", token(in).position);
			expression(in + 1, header.second);
			inserted(U":", origin);
			newline(origin);
			body();
			return;
		}
		size_t second = find(first + 1, header.second, U";");
		if (second == NO_INDEX || find(second + 1, header.second, U";") != NO_INDEX) {
			error(origin, "A C-style for loop requires exactly three clauses.");
			return;
		}
		if (find(header.first, first, U",") != NO_INDEX || find(second + 1, header.second, U",") != NO_INDEX) {
			error(origin, "Use a single initializer and a single update statement; comma operators are not supported.");
			return;
		}
		std::u32string flag = U"__gdc_first_";
		for (char c : std::to_string(next_loop++)) {
			flag.push_back(c);
		}
		line(U"if true:", origin);
		++indent; // A scope for the initializer, like C's for statement.
		if (header.first < first) {
			start(origin);
			declaration(header.first, first);
			newline(origin);
		}
		line(U"var " + flag + U" = true", origin);
		line(U"while true:", origin);
		++indent;
		line(U"if " + flag + U":", origin);
		++indent;
		line(flag + U" = false", origin);
		--indent;
		if (second + 1 < header.second) {
			line(U"else:", origin);
			++indent;
			start(token(second + 1).position);
			declaration(second + 1, header.second);
			newline(origin);
			--indent;
		}
		if (first + 1 < second) {
			start(token(first + 1).position);
			inserted(U"if not (", origin);
			expression(first + 1, second);
			inserted(U"):", origin);
			newline(origin);
			++indent;
			line(U"break", origin);
			--indent;
		}
		// body() normally adds an indentation level; here the while already supplied it.
		--indent;
		body();
		++indent;
		indent -= 2;
	}
	void match_statement() {
		Position origin = peek().position;
		++cursor;
		auto subject = parenthesized();
		start(origin);
		inserted(U"match ", origin);
		expression(subject.first, subject.second);
		inserted(U":", origin);
		newline(origin);
		if (!expect(U"{")) {
			return;
		}
		++indent;
		bool any = false;
		while (!at(U"}") && peek().kind != Kind::END && !failed()) {
			Position arm = peek().position;
			start(arm);
			if (take(U"default")) {
				inserted(U"_", arm);
			} else {
				if (!expect(U"case")) {
					break;
				}
				size_t end = find(cursor, tokens.size() - 1, U":");
				if (end == NO_INDEX || end == cursor) {
					error(arm, "Expected 'case pattern: { ... }'.");
					break;
				}
				expression(cursor, end);
				cursor = end;
			}
			if (!expect(U":")) {
				break;
			}
			inserted(U":", arm);
			newline(arm);
			body();
			any = true;
		}
		if (!any && !failed()) {
			error(origin, "A switch block needs at least one case.");
		}
		expect(U"}");
		--indent;
	}
	void property() {
		if (!expect(U"{")) {
			return;
		}
		++indent;
		while (!at(U"}") && peek().kind != Kind::END && !failed()) {
			Position origin = peek().position;
			start(origin);
			if (take(U"get")) {
				inserted(U"get", origin);
				if (take(U"(")) {
					expect(U")");
				}
			} else if (take(U"set")) {
				inserted(U"set(", origin);
				auto param = parenthesized();
				expression(param.first, param.second);
				inserted(U")", origin);
			} else {
				error(origin, "Expected a get or set accessor.");
				break;
			}
			inserted(U":", origin);
			newline(origin);
			body();
		}
		expect(U"}");
		take(U";");
		--indent;
	}
	void statement(bool class_body) {
		if (failed()) {
			return;
		}
		Position origin = peek().position;
		if (!enter(origin)) {
			return;
		}
		if (take(U";")) {
			leave();
			return;
		}
		if (peek().kind == Kind::DOC) {
			std::u32string text = peek().text;
			if (text.compare(0, 3, U"///") == 0) {
				text = U"##" + text.substr(3);
			}
			line(text, origin);
			++cursor;
			leave();
			return;
		}
		if (take(U"@")) {
			start(origin);
			inserted(U"@", origin);
			if (peek().kind != Kind::WORD) {
				error(peek().position, "Expected an annotation name.");
				leave();
				return;
			}
			copied(cursor++);
			if (at(U"(")) {
				size_t end = peek().mate;
				expression(cursor, end + 1);
				cursor = end + 1;
			}
			take(U";");
			newline(origin);
			leave();
			return;
		}
		if (at(U"if") || at(U"while")) {
			control(peek().text);
		} else if (at(U"else")) {
			if (token(cursor + 1).text == U"if") {
				++cursor;
				control(U"elif");
			} else {
				control(U"else");
			}
		} else if (at(U"for")) {
			for_loop();
		} else if (at(U"switch")) {
			match_statement();
		} else if (at(U"class")) {
			size_t begin = cursor++;
			while (!at(U"{") && peek().kind != Kind::END) {
				++cursor;
			}
			start(origin);
			expression(begin, cursor);
			inserted(U":", origin);
			newline(origin);
			body(true);
			take(U";");
		} else if (at(U"enum")) {
			size_t begin = cursor++;
			if (peek().kind == Kind::WORD) {
				++cursor;
			}
			if (!at(U"{")) {
				error(peek().position, "Expected an enum body.");
			} else {
				cursor = peek().mate + 1;
				start(origin);
				expression(begin, cursor);
				newline(origin);
				take(U";");
			}
		} else if (at(U"signal")) {
			start(origin);
			copied(cursor++);
			inserted(U" ", origin);
			if (peek().kind != Kind::WORD) {
				error(peek().position, "Expected a signal name.");
			} else {
				copied(cursor++);
				if (at(U"(")) {
					auto args = parenthesized();
					inserted(U"(", origin);
					parameters(args.first, args.second);
					inserted(U")", origin);
				}
				semicolon();
				newline(origin);
			}
		} else if (at(U"do") || at(U"try") || at(U"catch") || at(U"throw") || at(U"struct") || at(U"namespace")) {
			error(origin, "This construct is not supported by the GD-C dialect.");
		} else {
			size_t begin = cursor;
			size_t candidate = at(U"static") ? cursor + 1 : cursor;
			size_t end_type = type_end(candidate, tokens.size() - 1);
			bool crosses_cursor_line = options.completion && options.completion_line > 0 &&
					token(candidate).position.line <= options.completion_line && token(end_type).position.line > options.completion_line;
			bool typed_func = !crosses_cursor_line && end_type > candidate && token(end_type).kind == Kind::WORD && token(end_type + 1).text == U"(";
			if (typed_func) {
				function();
			} else {
				cursor = simple_end(begin);
				if (cursor == begin) {
					error(origin, "Unexpected token at the start of a statement.");
				} else {
					start(origin);
					if (token(begin).text == U"return") {
						copied(begin);
						if (begin + 1 < cursor) {
							inserted(U" ", origin);
							expression(begin + 1, cursor);
						}
					} else {
						declaration(begin, cursor);
					}
					if (at(U"{")) {
						if (!class_body) {
							error(peek().position, "Properties can only be declared in a class.");
						}
						inserted(U":", peek().position);
						newline(origin);
						property();
					} else {
						semicolon();
						newline(origin);
					}
				}
			}
		}
		leave();
	}
	void build_map() {
		result.map.generated_size = static_cast<uint32_t>(result.code.size());
		result.map.line_starts.push_back(0);
		for (size_t i = 0; i < result.code.size(); ++i) {
			if (result.code[i] == U'\n') {
				result.map.line_starts.push_back(static_cast<uint32_t>(i + 1));
			}
		}
		if (completion_offset != NO_INDEX) {
			auto it = std::upper_bound(result.map.line_starts.begin(), result.map.line_starts.end(), completion_offset);
			size_t index = size_t(it - result.map.line_starts.begin() - 1);
			result.map.completion_source = options.completion_cursor;
			result.map.completion_generated = { uint32_t(index + 1), uint32_t(completion_offset - result.map.line_starts[index] + 1) };
		}
		output_locations.push_back(locations.back());
		for (size_t i = 0; i < output_locations.size();) {
			uint32_t step = 0;
			if (i + 1 < output_locations.size() && output_locations[i + 1].line == output_locations[i].line &&
					output_locations[i + 1].column == output_locations[i].column + 1) {
				step = 1;
			}
			result.map.runs.push_back({ static_cast<uint32_t>(i), output_locations[i], step });
			size_t end = i + 1;
			while (end < output_locations.size() && output_locations[end].line == output_locations[i].line &&
					output_locations[end].column == output_locations[i].column + step * (end - i)) {
				++end;
			}
			i = end;
		}
	}

public:
	Frontend(const std::u32string &p_source, const Options &p_options) : source(p_source), options(p_options) {}
	Result run() {
		if (source.size() > options.max_source_chars) {
			error({}, "Source exceeds the configured size limit.");
			return std::move(result);
		}
		lex();
		while (!failed() && peek().kind != Kind::END) {
			statement(true);
		}
		if (failed()) {
			result.code.clear();
			result.map = SourceMap();
		} else {
			build_map();
		}
		return std::move(result);
	}
};

void put32(std::vector<uint8_t> &out, uint32_t value) {
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<uint8_t>(value >> (i * 8)));
	}
}
uint32_t get32(const std::vector<uint8_t> &data, size_t offset) {
	uint32_t value = 0;
	for (int i = 0; i < 4; ++i) {
		value |= static_cast<uint32_t>(data[offset + i]) << (i * 8);
	}
	return value;
}
} // namespace

Result transpile(const std::u32string &source, const Options &options) {
	return Frontend(source, options).run();
}

Position SourceMap::original(uint32_t line, uint32_t column) const {
	if (empty() || line_starts.empty()) {
		return { line, column };
	}
	uint32_t offset = generated_size;
	if (line >= 1 && line <= line_starts.size()) {
		uint64_t candidate = static_cast<uint64_t>(line_starts[line - 1]) + (column > 0 ? column - 1 : 0);
		uint32_t end = line < line_starts.size() ? line_starts[line] - 1 : generated_size;
		offset = static_cast<uint32_t>(std::min<uint64_t>(candidate, end));
	}
	auto it = std::upper_bound(runs.begin(), runs.end(), offset,
			[](uint32_t value, const MapRun &run) { return value < run.offset; });
	if (it == runs.begin()) {
		return it->source;
	}
	--it;
	return { it->source.line, it->source.column + (offset - it->offset) * it->step };
}

void SourceMap::original_range(Position start, Position end, Position &out_start, Position &out_end) const {
	out_start = original(start.line, start.column);
	out_end = original(end.line, end.column);
	auto less = [](Position a, Position b) { return a.line < b.line || (a.line == b.line && a.column < b.column); };
	if (empty() || line_starts.empty()) {
		return;
	}
	auto offset = [&](Position p) -> uint32_t {
		if (p.line < 1 || p.line > line_starts.size()) {
			return generated_size;
		}
		uint32_t limit = p.line < line_starts.size() ? line_starts[p.line] - 1 : generated_size;
		return static_cast<uint32_t>(std::min<uint64_t>(limit, static_cast<uint64_t>(line_starts[p.line - 1]) + (p.column ? p.column - 1 : 0)));
	};
	const uint32_t first = offset(start), last = offset(end);
	if (last <= first) {
		out_end = out_start;
		return;
	}
	// C signatures and conditional expressions reorder tokens. Mapping just their
	// endpoints can reverse a span or omit the original condition entirely.
	auto it = std::upper_bound(runs.begin(), runs.end(), first,
			[](uint32_t value, const MapRun &run) { return value < run.offset; });
	if (it != runs.begin()) {
		--it;
	}
	bool initialized = false;
	for (; it != runs.end() && it->offset < last; ++it) {
		uint32_t next = it + 1 != runs.end() ? (it + 1)->offset : generated_size + 1;
		uint32_t lo = std::max(first, it->offset), hi = std::min(last, next);
		if (lo >= hi) {
			continue;
		}
		Position a{ it->source.line, it->source.column + (lo - it->offset) * it->step };
		Position b{ it->source.line, it->source.column + (hi - 1 - it->offset) * it->step + 1 };
		if (!initialized || less(a, out_start)) {
			out_start = a;
		}
		if (!initialized || less(out_end, b)) {
			out_end = b;
		}
		initialized = true;
	}
	if (less(out_end, out_start)) {
		out_end = out_start;
	}
}

Position SourceMap::generated(Position original_pos) const {
	if (completion_generated.line > 0 && original_pos == completion_source) {
		return completion_generated;
	}
	if (empty() || line_starts.empty()) {
		return original_pos;
	}
	uint64_t best = std::numeric_limits<uint64_t>::max();
	uint32_t offset = 0;
	for (size_t i = 0; i < runs.size(); ++i) {
		const MapRun &run = runs[i];
		uint32_t length = (i + 1 < runs.size() ? runs[i + 1].offset : generated_size + 1) - run.offset;
		uint32_t delta = 0;
		if (run.step && original_pos.column >= run.source.column && length) {
			delta = std::min(original_pos.column - run.source.column, length - 1);
		}
		uint32_t col = run.source.column + delta;
		uint64_t distance = static_cast<uint64_t>(run.source.line > original_pos.line ? run.source.line - original_pos.line : original_pos.line - run.source.line) * (1ull << 32) +
				(col > original_pos.column ? col - original_pos.column : original_pos.column - col);
		if (distance < best) {
			best = distance;
			offset = run.offset + delta;
		}
	}
	auto it = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
	size_t index = it == line_starts.begin() ? 0 : static_cast<size_t>(it - line_starts.begin() - 1);
	return { static_cast<uint32_t>(index + 1), offset - line_starts[index] + 1 };
}

bool has_binary_envelope(const uint8_t *data, size_t size) {
	return size >= 4 && data[0] == 'G' && data[1] == 'D' && data[2] == 'C' && data[3] == 'B';
}

std::vector<uint8_t> encode_binary(const std::vector<uint8_t> &tokens, const SourceMap &map) {
	uint64_t total = 24ull + map.line_starts.size() * 4ull + map.runs.size() * 16ull + tokens.size();
	if (total > MAX_BINARY_BYTES || tokens.empty() || map.empty()) {
		return {};
	}
	std::vector<uint8_t> result;
	result.reserve(static_cast<size_t>(total));
	result.insert(result.end(), { 'G', 'D', 'C', 'B' });
	put32(result, 1);
	put32(result, static_cast<uint32_t>(tokens.size()));
	put32(result, map.generated_size);
	put32(result, static_cast<uint32_t>(map.line_starts.size()));
	put32(result, static_cast<uint32_t>(map.runs.size()));
	for (uint32_t start : map.line_starts) {
		put32(result, start);
	}
	for (const MapRun &run : map.runs) {
		put32(result, run.offset);
		put32(result, run.source.line);
		put32(result, run.source.column);
		put32(result, run.step);
	}
	result.insert(result.end(), tokens.begin(), tokens.end());
	return result;
}

bool decode_binary(const std::vector<uint8_t> &data, std::vector<uint8_t> &tokens, SourceMap &map, std::string &error) {
	tokens.clear();
	map = SourceMap();
	error.clear();
	auto fail = [&](const char *message) { error = message; return false; };
	if (data.size() < 24 || data.size() > MAX_BINARY_BYTES || !has_binary_envelope(data.data(), data.size())) {
		return fail("Invalid GDCB header.");
	}
	if (get32(data, 4) != 1) {
		return fail("Unsupported GDCB version.");
	}
	uint32_t token_size = get32(data, 8), code_size = get32(data, 12), lines = get32(data, 16), runs = get32(data, 20);
	uint64_t prefix = 24ull + lines * 4ull + runs * 16ull;
	if (!token_size || !lines || !runs || code_size > 16 * 1024 * 1024 || prefix + token_size != data.size() || lines > code_size + 1 || runs > code_size + 1) {
		return fail("Invalid GDCB sizes.");
	}
	SourceMap decoded;
	decoded.generated_size = code_size;
	size_t offset = 24;
	for (uint32_t i = 0; i < lines; ++i, offset += 4) {
		uint32_t value = get32(data, offset);
		if (value > code_size || (i == 0 && value != 0) || (i > 0 && value <= decoded.line_starts.back())) {
			return fail("Invalid GDCB line table.");
		}
		decoded.line_starts.push_back(value);
	}
	for (uint32_t i = 0; i < runs; ++i, offset += 16) {
		MapRun run{ get32(data, offset), { get32(data, offset + 4), get32(data, offset + 8) }, get32(data, offset + 12) };
		if (run.offset > code_size || (i == 0 && run.offset != 0) || (i > 0 && run.offset <= decoded.runs.back().offset) ||
				!run.source.line || !run.source.column || run.step > 1 || run.source.line > 0x7FFFFFFFu ||
				static_cast<uint64_t>(run.source.column) + code_size > 0x7FFFFFFFu) {
			return fail("Invalid GDCB mapping run.");
		}
		decoded.runs.push_back(run);
	}
	if (token_size < 4 || data[offset] != 'G' || data[offset + 1] != 'D' || data[offset + 2] != 'S' || data[offset + 3] != 'C') {
		return fail("GDCB payload is not native GDSC tokens.");
	}
	tokens.assign(data.begin() + static_cast<std::ptrdiff_t>(prefix), data.end());
	map = std::move(decoded);
	return true;
}

bool decode_utf8(const std::string &bytes, std::u32string &text, std::string &error) {
	text.clear();
	error.clear();
	for (size_t i = 0; i < bytes.size();) {
		uint8_t first = static_cast<uint8_t>(bytes[i++]);
		char32_t value = first;
		uint32_t extra = 0;
		if (first >= 0xC2 && first <= 0xDF) {
			value = first & 0x1F;
			extra = 1;
		} else if (first >= 0xE0 && first <= 0xEF) {
			value = first & 0x0F;
			extra = 2;
		} else if (first >= 0xF0 && first <= 0xF4) {
			value = first & 0x07;
			extra = 3;
		} else if (first >= 0x80) {
			error = "Invalid UTF-8 leading byte.";
			text.clear();
			return false;
		}
		if (i + extra > bytes.size()) {
			error = "Truncated UTF-8 sequence.";
			text.clear();
			return false;
		}
		for (uint32_t j = 0; j < extra; ++j) {
			uint8_t next = static_cast<uint8_t>(bytes[i++]);
			if ((next & 0xC0) != 0x80) {
				error = "Invalid UTF-8 continuation byte.";
				text.clear();
				return false;
			}
			value = (value << 6) | (next & 0x3F);
		}
		if ((extra == 1 && value < 0x80) || (extra == 2 && value < 0x800) || (extra == 3 && value < 0x10000) ||
				value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
			error = "Invalid UTF-8 scalar value.";
			text.clear();
			return false;
		}
		text.push_back(value);
	}
	return true;
}
std::string encode_utf8(const std::u32string &text) {
	std::string out;
	for (char32_t c : text) {
		if (c < 0x80) {
			out.push_back(static_cast<char>(c));
		} else if (c < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (c >> 6)));
			out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		} else if (c < 0x10000) {
			out.push_back(static_cast<char>(0xE0 | (c >> 12)));
			out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (c >> 18)));
			out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		}
	}
	return out;
}
} // namespace GDC

#pragma once

#include "../html_document.h"
#include "hcsr_scene.h"

#include "scene/resources/font.h"

// Host-owned fonts. The scene ABI receives only immutable glyph IDs and logical metrics.
class HCSRNewestText {
	struct AuthorFace {
		String family, source;
		int weight = 400;
		bool italic = false;
	};
	Ref<HTMLDocument> document;
	Vector<AuthorFace> authors;
	HashMap<String, Ref<Font>> fonts;
	HashMap<String, Ref<FontFile>> face_files;
	uint64_t configuration = 0;
	Vector<hcsr_shaped_glyph_t> scratch;
	Ref<Font> resolve(const String &family, int weight, bool italic);
	int shape(const hcsr_shape_request_t &request, hcsr_shape_result_t &result);

public:
	void configure(const Ref<HTMLDocument> &p_document, const String &css);
	void add_stylesheet(const String &css, const String &base);
	static int32_t HCSR_CALL callback(void *user, const hcsr_shape_request_t *request, hcsr_shape_result_t *result);
};

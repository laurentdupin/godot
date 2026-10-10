#pragma once

#include "../html_document.h"
#include "hcsr_font_engine.h"
#include "hcsr_scene.h"

#include "scene/resources/font.h"

// Host-owned fonts. The scene ABI receives only immutable glyph IDs and logical metrics.
class HCSRNewestText {
	struct AuthorFace {
		String family, source;
		int weight = 400;
		int maximum_weight = 400;
		bool italic = false;
	};
	Ref<HTMLDocument> document;
	Vector<AuthorFace> authors;
	HashMap<String, Ref<Font>> fonts;
	HashMap<String, Ref<SystemFont>> system_fonts;
	HashMap<String, bool> optical_sizing_support;
	HashMap<String, Ref<FontFile>> face_files;
	struct FontMetrics : hcsr_font_metrics {
		hcsr_font_decoration_metrics decoration = {};
		Ref<FontFile> source;
		Dictionary coordinates;
		int face_index = 0, weight = 400;
		hcsr_font_face_id caret_face = 0;
		bool has_vertical_metrics = true;
		bool authored = false;
		bool italic_face = false;
	};
	HashMap<RID, FontMetrics> font_metrics;
	void cache_font_metrics(const Ref<Font> &font, int weight);
	uint64_t configuration = 0;
	Vector<hcsr_shaped_glyph_t> scratch;
	Vector<hcsr_shape_run_t> scratch_runs;
	Vector<float> scratch_carets;
	Ref<Font> resolve(const hcsr_shape_request_t &request);
	int shape(const hcsr_shape_request_t &request, hcsr_shape_result_t &result);

public:
	~HCSRNewestText();
	static int32_t HCSR_CALL caret_callback(void *user, uint64_t face, uint32_t glyph, float size, uint32_t direction, hcsr_caret_result_t *result);
	void configure(const Ref<HTMLDocument> &p_document, const String &css);
	void add_stylesheet(const String &css, const String &base);
	static int32_t HCSR_CALL callback(void *user, const hcsr_shape_request_t *request, hcsr_shape_result_t *result);
};

#include "hcsr_newest_text.h"

#include "../bridge/html_asset_provider.h"

#include "core/string/regex.h"
#include "servers/text/text_server.h"

#include <cstdlib>

static String decode(hcsr_utf8_t value) {
	return String::utf8(value.data, value.length);
}

// CSS Fonts weight matching is directional, with a special 400..500 search.
static int weight_rank(int requested, int minimum, int maximum) {
	if (requested >= minimum && requested <= maximum) {
		return 0;
	}
	if (requested < 400) {
		return maximum < requested ? requested - maximum : 1000 + minimum - requested;
	}
	if (requested > 500) {
		return minimum > requested ? minimum - requested : 1000 + requested - maximum;
	}
	if (minimum > requested && minimum <= 500) {
		return minimum - requested;
	}
	return maximum < requested ? 1000 + requested - maximum : 2000 + minimum - 500;
}

void HCSRNewestText::configure(const Ref<HTMLDocument> &p_document, const String &css) {
	document = p_document;
	configuration++;
	authors.clear();
	add_stylesheet(css, p_document->get_html_file().get_base_dir());
}

void HCSRNewestText::add_stylesheet(const String &css, const String &base) {
	RegEx faces("(?is)@font-face\\s*\\{([^}]+)\\}");
	RegEx family("(?i)font-family\\s*:\\s*([^;]+)");
	RegEx source("(?i)url\\(\\s*['\"]?([^)'\"]+)");
	RegEx weight("(?i)font-weight\\s*:\\s*([^;}]+)");
	for (const Ref<RegExMatch> &match : faces.search_all(css)) {
		String body = match->get_string(1);
		Ref<RegExMatch> f = family.search(body), s = source.search(body), w = weight.search(body);
		if (f.is_valid() && s.is_valid()) {
			String value = w.is_valid() ? w->get_string(1).strip_edges().to_lower() : "normal";
			PackedStringArray bounds = value.replace("\t", " ").split(" ", false);
			int minimum = 400, maximum = 400;
			if (value == "bold") {
				minimum = maximum = 700;
			} else if (value != "normal") {
				if (bounds.is_empty() || bounds.size() > 2 || !bounds[0].is_valid_int() || (bounds.size() == 2 && !bounds[1].is_valid_int())) {
					continue;
				}
				minimum = bounds[0].to_int();
				maximum = bounds.size() == 2 ? int(bounds[1].to_int()) : minimum;
				if (minimum < 1 || maximum > 1000 || minimum > maximum) {
					continue;
				}
			}
			authors.push_back({ f->get_string(1).strip_edges().unquote().to_lower(), (s->get_string(1).contains("://") || s->get_string(1).is_absolute_path() || base.is_empty()) ? s->get_string(1).strip_edges() : base.path_join(s->get_string(1).strip_edges()).simplify_path(),
					minimum, maximum, body.to_lower().contains("italic") });
		}
	}
}

Ref<Font> HCSRNewestText::resolve(const String &family, int weight, bool italic) {
	String key = uitos(configuration) + "|" + family + "|" + itos(weight) + "|" + itos(italic);
	for (const AuthorFace &face : authors) {
		key += "|" + face.family + ":" + face.source + ":" + itos(face.weight) + ":" + itos(face.maximum_weight) + ":" + itos(face.italic);
	}
	if (const Ref<Font> *cached = fonts.getptr(key)) {
		return *cached;
	}
	PackedStringArray names;
	Vector<const AuthorFace *> selected_faces;
	for (const String &part : family.split(",")) {
		const AuthorFace *selected = nullptr;
		String name = part.strip_edges().unquote();
		names.push_back(name == "sans-serif" || name == "system-ui" ? "Arial" : name == "serif" ? "Times New Roman"
						: name == "monospace"													? "Consolas"
																								: name);
		for (const AuthorFace &face : authors) {
			if (face.family == name.to_lower() && (!selected || weight_rank(weight, face.weight, face.maximum_weight) + (face.italic != italic ? 10000 : 0) <= weight_rank(weight, selected->weight, selected->maximum_weight) + (selected->italic != italic ? 10000 : 0))) {
				selected = &face;
			}
		}
		if (selected) {
			selected_faces.push_back(selected);
		}
	}
	Ref<SystemFont> fallback;
	fallback.instantiate();
	fallback->set_font_names(names);
	fallback->set_font_weight(weight);
	fallback->set_font_italic(italic);
	fallback->set_hinting(TextServer::HINTING_NONE);
	fallback->set_subpixel_positioning(TextServer::SUBPIXEL_POSITIONING_DISABLED);
	fallback->set_oversampling(1);
	TypedArray<Font> resolved;
	for (const AuthorFace *selected : selected_faces) {
		const String face_key = uitos(configuration) + "|" + selected->source;
		Ref<FontFile> file;
		if (const Ref<FontFile> *cached = face_files.getptr(face_key)) {
			file = *cached;
		} else {
			HTMLAssetResource asset;
			if (HTMLGodotAssetProvider::load_asset(document, selected->source, asset) != OK) {
				continue;
			}
			file.instantiate();
			file->set_data(asset.bytes);
			file->set_hinting(TextServer::HINTING_NONE);
			file->set_subpixel_positioning(TextServer::SUBPIXEL_POSITIONING_DISABLED);
			file->set_oversampling(1);
			face_files.insert(face_key, file);
		}
		Ref<FontVariation> variation;
		variation.instantiate();
		variation->set_base_font(file);
		Dictionary axes;
		TextServer *ts = TextServerManager::get_singleton()->get_primary_interface().ptr();
		axes[ts->name_to_tag("weight")] = CLAMP(weight, selected->weight, selected->maximum_weight);
		axes[ts->name_to_tag("italic")] = italic ? 1 : 0;
		variation->set_variation_opentype(axes);
		// Keep shaping and rasterization on the same synthesized FontVariation RID.
		// Real bold faces and ranges containing bold must never be emboldened twice.
		if (weight >= 600 && selected->maximum_weight < 600) {
			variation->set_variation_embolden(0.5f);
		}
		resolved.push_back(variation);
	}
	resolved.push_back(fallback);
	Ref<Font> font = resolved[0];
	if (resolved.size() > 1) {
		TypedArray<Font> fallbacks;
		for (int i = 1; i < resolved.size(); i++) {
			fallbacks.push_back(resolved[i]);
		}
		font->set_fallbacks(fallbacks);
	}
	fonts.insert(key, font);
	return font;
}

int32_t HCSR_CALL HCSRNewestText::callback(void *user, const hcsr_shape_request_t *request, hcsr_shape_result_t *result) {
	return static_cast<HCSRNewestText *>(user)->shape(*request, *result);
}

void HCSRNewestText::cache_font_metrics(const Ref<Font> &font, int weight) {
	const TypedArray<RID> rids = font->get_rids();
	if (rids.is_empty()) {
		return;
	}
	const RID rid = rids[0];
	if (font_metrics.has(rid)) {
		return;
	}
	// Keep Godot's glyph IDs/shaping and atlas. Only the unrounded CSS metrics
	// come from the same codec used by the interactive host, once per font RID.
	Ref<Font> base = font;
	while (base.is_valid()) {
		Ref<FontVariation> variation = base;
		Ref<SystemFont> system = base;
		if (variation.is_valid()) {
			base = variation->_get_base_font_or_default();
		} else if (system.is_valid()) {
			base = system->_get_base_font_or_default();
		} else {
			break;
		}
	}
	hcsr_font_metrics metrics = {};
	Ref<FontFile> file = base;
	if (file.is_valid()) {
		const PackedByteArray data = file->get_data();
		hcsr_font_face_id face = 0;
		TextServer *ts = TextServerManager::get_singleton()->get_primary_interface().ptr();
		if (hcsr_font_register(data.ptr(), data.size(), ts->font_get_face_index(rid), &face)) {
			hcsr_font_get_metrics_variation(face, 1024, weight, &metrics);
			hcsr_font_unregister(face);
			metrics.ascent /= 1024;
			metrics.descent /= 1024;
			metrics.line_gap /= 1024;
			metrics.x_height /= 1024;
		}
	}
	font_metrics.insert(rid, metrics);
	for (const Ref<Font> &fallback : font->get_fallbacks()) {
		cache_font_metrics(fallback, weight);
	}
}

int HCSRNewestText::shape(const hcsr_shape_request_t &request, hcsr_shape_result_t &result) {
	TextServer *ts = TextServerManager::get_singleton()->get_primary_interface().ptr();
	Ref<Font> font = resolve(decode(request.family), request.weight, request.italic != 0);
	const String text = decode(request.text);
	const float scale = request.size / 64.0f;
	RID shaped = ts->create_shaped_text(request.rtl ? TextServer::DIRECTION_RTL : TextServer::DIRECTION_LTR);
	Dictionary features;
	RegEx feature("['\"]?([A-Za-z0-9]{4})['\"]?\\s*(?:=|\\s)\\s*(\\d+|on|off)");
	for (const Ref<RegExMatch> &match : feature.search_all(decode(request.features))) {
		String value = match->get_string(2);
		features[match->get_string(1)] = value == "on" ? 1 : value == "off" ? 0
																			: value.to_int();
	}
	bool ok = ts->shaped_text_add_string(shaped, text, font->get_rids(), 64, features, decode(request.language)) && ts->shaped_text_shape(shaped);
	if (!ok) {
		ts->free_rid(shaped);
		return 0;
	}
	cache_font_metrics(font, request.weight);
	float ascent = 0, descent = 0, gap = 0, x_height = request.size * .5f;
	RID previous_metrics_rid;
	Vector<int> utf16;
	utf16.resize(text.length() + 1);
	int offset = 0;
	for (int i = 0; i < text.length(); i++) {
		utf16.write[i] = offset;
		offset += text[i] > 0xffff ? 2 : 1;
	}
	utf16.write[text.length()] = offset;
	scratch.clear();
	const Glyph *glyphs = ts->shaped_text_get_glyphs(shaped);
	for (int i = 0; i < ts->shaped_text_get_glyph_count(shaped); i++) {
		const Glyph &g = glyphs[i];
		if (!g.font_rid.is_valid()) {
			continue;
		}
		if (g.font_rid != previous_metrics_rid) {
			const hcsr_font_metrics *metrics = font_metrics.getptr(g.font_rid);
			if (metrics && metrics->ascent + metrics->descent > 0) {
				ascent = MAX(ascent, metrics->ascent * request.size);
				descent = MAX(descent, metrics->descent * request.size);
				gap = MAX(gap, metrics->line_gap * request.size);
				if (!previous_metrics_rid.is_valid() && metrics->x_height > 0) {
					x_height = metrics->x_height * request.size;
				}
			} else {
				// Godot can select an implicit system fallback whose bytes are not
				// exposed by Font. Preserve its metrics rather than using another face.
				ascent = MAX(ascent, (float)ts->font_get_ascent(g.font_rid, 64) * scale);
				descent = MAX(descent, (float)ts->font_get_descent(g.font_rid, 64) * scale);
			}
			previous_metrics_rid = g.font_rid;
		}
		Vector2 origin = ts->font_get_glyph_offset(g.font_rid, Vector2i(64, 0), g.index) * scale;
		Vector2 size = ts->font_get_glyph_size(g.font_rid, Vector2i(64, 0), g.index) * scale;
		for (int repeat = 0; repeat < g.repeat; repeat++) {
			scratch.push_back({ g.font_rid.get_id(), (uint32_t)g.index, (uint32_t)utf16[CLAMP(g.start, 0, text.length())],
					g.x_off * scale, g.y_off * scale, g.advance * scale, 0, origin.x, origin.y, size.x, size.y });
		}
	}
	result = { scratch.ptr(), (size_t)scratch.size(), ascent, descent, gap, x_height };
	ts->free_rid(shaped);
	return 1;
}

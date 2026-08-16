/**************************************************************************/
/*  html_texture.cpp                                                      */
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

#include "html_texture.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/string/print_string.h"
#include "servers/rendering/rendering_server.h"

static bool html_css_texture_trace_enabled() {
	return OS::get_singleton() != nullptr && OS::get_singleton()->get_environment("HTML_CSS_GPU_TRACE") == "1";
}

static void html_css_texture_trace(const String &p_message) {
	if (html_css_texture_trace_enabled()) {
		print_line(vformat("HTML/CSS texture trace: %s", p_message));
	}
}

void HTMLTexture2D::_emit_changed_on_main_thread() {
	emit_changed();
}

void HTMLTexture2D::_notify_changed() {
	if (Thread::is_main_thread()) {
		emit_changed();
	} else {
		callable_mp(this, &HTMLTexture2D::_emit_changed_on_main_thread).call_deferred();
	}
}

void HTMLTexture2D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("update_placeholder", "size", "background", "marker"), &HTMLTexture2D::update_placeholder, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_latest_image"), &HTMLTexture2D::get_latest_image);
}

void HTMLTexture2D::update_placeholder(const Size2i &p_size, const Color &p_background, const String &p_marker) {
	Size2i new_size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	Ref<Image> image = Image::create_empty(new_size.x, new_size.y, false, Image::FORMAT_RGBA8);
	image->fill(p_background);

	// The marker is accepted so the API can stay stable when the real renderer
	// starts labeling debug frames; this placeholder does not rasterize text.
	(void)p_marker;
	update_from_image(image);
}

void HTMLTexture2D::update_from_image(const Ref<Image> &p_image) {
	ERR_FAIL_COND(p_image.is_null());
	ERR_FAIL_COND(p_image->is_empty());

	update_from_image(p_image, p_image->detect_alpha() != Image::ALPHA_NONE);
}

void HTMLTexture2D::update_from_image(const Ref<Image> &p_image, bool p_has_alpha) {
	ERR_FAIL_COND(p_image.is_null());
	ERR_FAIL_COND(p_image->is_empty());

	latest_image = p_image;
	external_texture_rid = RID();

	if (texture.is_null()) {
		texture.instantiate();
	}

	if (size == Size2i(p_image->get_width(), p_image->get_height()) && texture->get_rid().is_valid() && texture->get_format() == p_image->get_format()) {
		texture->update(p_image);
	} else {
		texture->set_image(p_image);
	}
	RenderingServer::get_singleton()->texture_proxy_update(proxy_texture_rid, texture->get_rid());

	size = Size2i(p_image->get_width(), p_image->get_height());
	alpha = p_has_alpha;
	_notify_changed();
}

void HTMLTexture2D::update_regions(const Size2i &p_size, const Vector<Rect2i> &p_regions, const Vector<Ref<Image>> &p_images, bool p_has_alpha) {
	ERR_FAIL_COND(p_size.x <= 0 || p_size.y <= 0);
	ERR_FAIL_COND(p_regions.size() != p_images.size() || p_regions.is_empty());

	const bool requires_full_initialization = latest_image.is_null()
			|| latest_image->is_empty()
			|| size != p_size
			|| latest_image->get_format() != Image::FORMAT_RGBA8
			|| texture.is_null()
			|| !texture->get_rid().is_valid();
	if (requires_full_initialization) {
		Ref<Image> image = Image::create_empty(p_size.x, p_size.y, false, Image::FORMAT_RGBA8);
		image->fill(Color(0, 0, 0, 0));
		for (int index = 0; index < p_regions.size(); index++) {
			ERR_FAIL_COND(p_images[index].is_null() || p_images[index]->is_empty());
			ERR_FAIL_COND(p_images[index]->get_format() != Image::FORMAT_RGBA8);
			ERR_FAIL_COND(p_regions[index].size != Size2i(p_images[index]->get_width(), p_images[index]->get_height()));
			ERR_FAIL_COND(!Rect2i(Vector2i(), p_size).encloses(p_regions[index]));
			image->blit_rect(p_images[index], Rect2i(Vector2i(), p_regions[index].size), p_regions[index].position);
		}
		update_from_image(image, p_has_alpha);
		return;
	}

	external_texture_rid = RID();
	for (int index = 0; index < p_regions.size(); index++) {
		ERR_FAIL_COND(p_images[index].is_null() || p_images[index]->is_empty());
		ERR_FAIL_COND(p_images[index]->get_format() != Image::FORMAT_RGBA8);
		ERR_FAIL_COND(p_regions[index].size != Size2i(p_images[index]->get_width(), p_images[index]->get_height()));
		ERR_FAIL_COND(!Rect2i(Vector2i(), p_size).encloses(p_regions[index]));
		latest_image->blit_rect(p_images[index], Rect2i(Vector2i(), p_regions[index].size), p_regions[index].position);
		RenderingServer::get_singleton()->texture_2d_update_region(texture->get_rid(), p_images[index], p_regions[index], 0);
	}
	RenderingServer::get_singleton()->texture_proxy_update(proxy_texture_rid, texture->get_rid());
	size = p_size;
	alpha = p_has_alpha;
	_notify_changed();
}

bool HTMLTexture2D::begin_region_candidate(const Size2i &p_size, bool p_has_alpha) {
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0, false);
	if (standby_texture.is_null()) {
		standby_texture.instantiate();
	}
	bool initialized = false;
	if (standby_image.is_null() || standby_image->get_size() != p_size || standby_image->get_format() != Image::FORMAT_RGBA8) {
		standby_image = Image::create_empty(p_size.x, p_size.y, false, Image::FORMAT_RGBA8);
		standby_image->fill(Color(0, 0, 0, 0));
		standby_texture->set_image(standby_image);
		initialized = true;
	}
	alpha = p_has_alpha;
	return initialized;
}

void HTMLTexture2D::update_candidate_region(const Rect2i &p_region, const Ref<Image> &p_image) {
	ERR_FAIL_COND(standby_image.is_null() || standby_texture.is_null());
	ERR_FAIL_COND(p_image.is_null() || p_image->is_empty() || p_image->get_format() != Image::FORMAT_RGBA8);
	ERR_FAIL_COND(p_region.size != p_image->get_size());
	ERR_FAIL_COND(!Rect2i(Vector2i(), standby_image->get_size()).encloses(p_region));
	standby_image->blit_rect(p_image, Rect2i(Vector2i(), p_region.size), p_region.position);
	RenderingServer::get_singleton()->texture_2d_update_region(standby_texture->get_rid(), p_image, p_region, 0);
}

void HTMLTexture2D::activate_region_candidate(bool p_notify) {
	ERR_FAIL_COND(standby_image.is_null() || standby_texture.is_null());
	SWAP(texture, standby_texture);
	SWAP(latest_image, standby_image);
	external_texture_rid = RID();
	size = latest_image->get_size();
	RenderingServer::get_singleton()->texture_proxy_update(proxy_texture_rid, texture->get_rid());
	if (p_notify) {
		_notify_changed();
	}
}

void HTMLTexture2D::notify_region_candidate_activation() {
	_notify_changed();
}

void HTMLTexture2D::cancel_region_candidate() {
	standby_texture.unref();
	standby_image.unref();
}

void HTMLTexture2D::set_external_texture(const RID &p_texture_rid, const Size2i &p_size, bool p_alpha) {
	html_css_texture_trace(vformat("set_external_texture: rid_valid=%s size=%dx%d alpha=%s", p_texture_rid.is_valid() ? "true" : "false", p_size.x, p_size.y, p_alpha ? "true" : "false"));
	external_texture_rid = p_texture_rid;
	RenderingServer::get_singleton()->texture_proxy_update(proxy_texture_rid, external_texture_rid);
	latest_image.unref();
	standby_image.unref();
	size = Size2i(MAX(1, p_size.x), MAX(1, p_size.y));
	alpha = p_alpha;
	_notify_changed();
}

void HTMLTexture2D::clear_external_texture() {
	if (!external_texture_rid.is_valid() && size == Size2i()) {
		return;
	}
	html_css_texture_trace("clear_external_texture");
	external_texture_rid = RID();
	Ref<Image> fallback = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
	fallback->fill(Color(0, 0, 0, 0));
	texture->set_image(fallback);
	RenderingServer::get_singleton()->texture_proxy_update(proxy_texture_rid, texture->get_rid());
	size = Size2i();
	_notify_changed();
}

void HTMLTexture2D::release_resources() {
	html_css_texture_trace(vformat("release_resources: proxy_valid=%s fallback_valid=%s", proxy_texture_rid.is_valid() ? "true" : "false", texture.is_valid() && texture->get_rid().is_valid() ? "true" : "false"));
	external_texture_rid = RID();
	latest_image.unref();
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	if (rendering_server != nullptr && proxy_texture_rid.is_valid()) {
		rendering_server->free_rid(proxy_texture_rid);
	}
	proxy_texture_rid = RID();
	texture.unref();
	standby_texture.unref();
	size = Size2i();
}

int HTMLTexture2D::get_width() const {
	return size.x;
}

int HTMLTexture2D::get_height() const {
	return size.y;
}

RID HTMLTexture2D::get_rid() const {
	return proxy_texture_rid;
}

bool HTMLTexture2D::has_alpha() const {
	return alpha;
}

Ref<Image> HTMLTexture2D::get_image() const {
	if (external_texture_rid.is_valid()) {
		RenderingServer *rendering_server = RenderingServer::get_singleton();
		ERR_FAIL_NULL_V(rendering_server, Ref<Image>());
		return rendering_server->texture_2d_get(external_texture_rid);
	}
	return latest_image;
}

Ref<Image> HTMLTexture2D::get_latest_image() const {
	return latest_image;
}

void HTMLTexture2D::draw(RID p_canvas_item, const Point2 &p_pos, const Color &p_modulate, bool p_transpose) const {
	if (external_texture_rid.is_valid()) {
		html_css_texture_trace(vformat("draw: external rid size=%dx%d", size.x, size.y));
		RenderingServer::get_singleton()->canvas_item_add_texture_rect(p_canvas_item, Rect2(p_pos, Size2(size)), proxy_texture_rid, false, p_modulate, p_transpose);
		return;
	}
	if (texture.is_valid()) {
		texture->draw(p_canvas_item, p_pos, p_modulate, p_transpose);
	}
}

void HTMLTexture2D::draw_rect(RID p_canvas_item, const Rect2 &p_rect, bool p_tile, const Color &p_modulate, bool p_transpose) const {
	if (external_texture_rid.is_valid()) {
		html_css_texture_trace(vformat("draw_rect: external rid rect=%s", p_rect));
		RenderingServer::get_singleton()->canvas_item_add_texture_rect(p_canvas_item, p_rect, proxy_texture_rid, p_tile, p_modulate, p_transpose);
		return;
	}
	if (texture.is_valid()) {
		texture->draw_rect(p_canvas_item, p_rect, p_tile, p_modulate, p_transpose);
	}
}

void HTMLTexture2D::draw_rect_region(RID p_canvas_item, const Rect2 &p_rect, const Rect2 &p_src_rect, const Color &p_modulate, bool p_transpose, bool p_clip_uv) const {
	if (external_texture_rid.is_valid()) {
		html_css_texture_trace(vformat("draw_rect_region: external rid rect=%s src=%s", p_rect, p_src_rect));
		RenderingServer::get_singleton()->canvas_item_add_texture_rect_region(p_canvas_item, p_rect, proxy_texture_rid, p_src_rect, p_modulate, p_transpose, p_clip_uv);
		return;
	}
	if (texture.is_valid()) {
		texture->draw_rect_region(p_canvas_item, p_rect, p_src_rect, p_modulate, p_transpose, p_clip_uv);
	}
}

bool HTMLTexture2D::is_pixel_opaque(int p_x, int p_y) const {
	if (external_texture_rid.is_valid()) {
		return true;
	}
	if (texture.is_null()) {
		return false;
	}
	return texture->is_pixel_opaque(p_x, p_y);
}

HTMLTexture2D::HTMLTexture2D() {
	texture.instantiate();
	Ref<Image> fallback = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
	fallback->fill(Color(0, 0, 0, 0));
	texture->set_image(fallback);
	proxy_texture_rid = RenderingServer::get_singleton()->texture_proxy_create(texture->get_rid());
}

HTMLTexture2D::~HTMLTexture2D() {
	release_resources();
}

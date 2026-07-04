/**************************************************************************/
/*  html_view.cpp                                                         */
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

#include "html_view.h"

#include "bridge/html_activation_engine.h"

#include "core/input/input_event.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/gui/color_rect.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

static constexpr int HTML_VIEW_MAX_BACKDROP_FILTER_REGIONS = 8;
static constexpr int HTML_VIEW_MAX_BACKDROP_FILTER_OPERATIONS = 64;

static bool html_view_input_trace_enabled() {
	return OS::get_singleton() != nullptr && OS::get_singleton()->get_environment("HTML_CSS_GPU_TRACE") == "1";
}

static void html_view_input_trace(const String &p_message) {
	if (html_view_input_trace_enabled()) {
		print_line(vformat("HTMLView input trace: %s", p_message));
	}
}

static double html_view_elapsed_ms(uint64_t p_start_usec) {
	if (OS::get_singleton() == nullptr || p_start_usec == 0) {
		return 0.0;
	}
	return (double)(OS::get_singleton()->get_ticks_usec() - p_start_usec) / 1000.0;
}

static const char *html_view_backdrop_filter_shader_code = R"(
shader_type canvas_item;
render_mode blend_mix, unshaded;

uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_linear_mipmap;
uniform sampler2D backdrop_mask_texture : repeat_disable, filter_nearest;
uniform bool use_mask_texture = false;
uniform vec2 view_size = vec2(1.0, 1.0);
uniform int region_count = 0;
uniform vec4 region_rects[8];
uniform vec4 region_radii[8];
uniform vec4 region_params[8];
uniform vec4 filter_ops[64];

const int FILTER_BLUR = 0;
const int FILTER_BRIGHTNESS = 1;
const int FILTER_CONTRAST = 2;
const int FILTER_SATURATE = 3;
const int FILTER_GRAYSCALE = 4;
const int FILTER_SEPIA = 5;
const int FILTER_INVERT = 6;
const int FILTER_HUE_ROTATE = 7;
const int FILTER_OPACITY = 8;

float corner_mask(vec2 p, vec2 center, float radius) {
	if (radius <= 0.0) {
		return 1.0;
	}
	float dist = length(p - center) - radius;
	return 1.0 - smoothstep(-1.0, 1.0, dist);
}

float rounded_rect_mask(vec2 p, vec4 rect, vec4 radii) {
	vec2 minp = rect.xy;
	vec2 maxp = rect.xy + rect.zw;
	if (p.x < minp.x || p.y < minp.y || p.x > maxp.x || p.y > maxp.y) {
		return 0.0;
	}

	float max_radius = max(0.0, min(rect.z, rect.w) * 0.5);
	radii = clamp(radii, vec4(0.0), vec4(max_radius));

	if (p.x < minp.x + radii.x && p.y < minp.y + radii.x) {
		return corner_mask(p, minp + vec2(radii.x, radii.x), radii.x);
	}
	if (p.x > maxp.x - radii.y && p.y < minp.y + radii.y) {
		return corner_mask(p, vec2(maxp.x - radii.y, minp.y + radii.y), radii.y);
	}
	if (p.x > maxp.x - radii.z && p.y > maxp.y - radii.z) {
		return corner_mask(p, maxp - vec2(radii.z, radii.z), radii.z);
	}
	if (p.x < minp.x + radii.w && p.y > maxp.y - radii.w) {
		return corner_mask(p, vec2(minp.x + radii.w, maxp.y - radii.w), radii.w);
	}
	return 1.0;
}

vec3 apply_saturate(vec3 color, float amount) {
	return mat3(
		vec3(0.213 + 0.787 * amount, 0.213 - 0.213 * amount, 0.213 - 0.213 * amount),
		vec3(0.715 - 0.715 * amount, 0.715 + 0.285 * amount, 0.715 - 0.715 * amount),
		vec3(0.072 - 0.072 * amount, 0.072 - 0.072 * amount, 0.072 + 0.928 * amount)
	) * color;
}

vec3 apply_sepia(vec3 color, float amount) {
	mat3 sepia = mat3(
		vec3(0.393, 0.349, 0.272),
		vec3(0.769, 0.686, 0.534),
		vec3(0.189, 0.168, 0.131)
	);
	return mix(color, sepia * color, amount);
}

vec3 apply_hue_rotate(vec3 color, float degrees) {
	float angle = radians(degrees);
	float c = cos(angle);
	float s = sin(angle);
	mat3 hue = mat3(
		vec3(0.213 + c * 0.787 - s * 0.213, 0.213 - c * 0.213 + s * 0.143, 0.213 - c * 0.213 - s * 0.787),
		vec3(0.715 - c * 0.715 - s * 0.715, 0.715 + c * 0.285 + s * 0.140, 0.715 - c * 0.715 + s * 0.715),
		vec3(0.072 - c * 0.072 + s * 0.928, 0.072 - c * 0.072 - s * 0.283, 0.072 + c * 0.928 + s * 0.072)
	);
	return hue * color;
}

void apply_color_filter(inout vec4 color, int filter_type, float amount) {
	if (filter_type == FILTER_BLUR) {
		return;
	}
	if (filter_type == FILTER_BRIGHTNESS) {
		color.rgb *= amount;
	} else if (filter_type == FILTER_CONTRAST) {
		color.rgb = (color.rgb - vec3(0.5)) * amount + vec3(0.5);
	} else if (filter_type == FILTER_SATURATE) {
		color.rgb = apply_saturate(color.rgb, amount);
	} else if (filter_type == FILTER_GRAYSCALE) {
		color.rgb = apply_saturate(color.rgb, 1.0 - amount);
	} else if (filter_type == FILTER_SEPIA) {
		color.rgb = apply_sepia(color.rgb, amount);
	} else if (filter_type == FILTER_INVERT) {
		color.rgb = mix(color.rgb, vec3(1.0) - color.rgb, amount);
	} else if (filter_type == FILTER_HUE_ROTATE) {
		color.rgb = apply_hue_rotate(color.rgb, amount);
	} else if (filter_type == FILTER_OPACITY) {
		color.a *= amount;
	}
}

void fragment() {
	vec2 local_pos = UV * view_size;
	float mask = 0.0;
	float blur_radius = 0.0;
	float opacity = 1.0;
	int op_start = 0;
	int op_count = 0;

	if (use_mask_texture) {
		vec4 mask_sample = texture(backdrop_mask_texture, UV);
		int effect_id = int(mask_sample.r * 255.0 + 0.5);
		mask = mask_sample.g;
		bool effect_found = false;
		for (int i = 0; i < 8; i++) {
			if (i >= region_count) {
				break;
			}
			if (int(region_rects[i].x + 0.5) == effect_id) {
				blur_radius = region_params[i].x;
				opacity = region_params[i].y;
				op_start = int(region_params[i].z + 0.5);
				op_count = int(region_params[i].w + 0.5);
				effect_found = true;
				break;
			}
		}
		if (!effect_found) {
			mask = 0.0;
		}
	} else {
		for (int i = 0; i < 8; i++) {
			if (i >= region_count) {
				break;
			}
			float region_mask = rounded_rect_mask(local_pos, region_rects[i], region_radii[i]);
			if (region_mask > mask) {
				mask = region_mask;
				blur_radius = region_params[i].x;
				opacity = region_params[i].y;
				op_start = int(region_params[i].z + 0.5);
				op_count = int(region_params[i].w + 0.5);
			}
		}
	}

	if (mask <= 0.0) {
		COLOR = vec4(0.0);
	} else {
		float lod = clamp(log2(max(blur_radius, 1.0)), 0.0, 6.0);
		vec4 filtered = textureLod(screen_texture, SCREEN_UV, lod);
		for (int i = 0; i < 64; i++) {
			if (i >= op_count) {
				break;
			}
			int op_index = op_start + i;
			if (op_index < 0 || op_index >= 64) {
				break;
			}
			apply_color_filter(filtered, int(filter_ops[op_index].x + 0.5), filter_ops[op_index].y);
		}
		COLOR = vec4(clamp(filtered.rgb, vec3(0.0), vec3(1.0)), mask * opacity * filtered.a);
	}
}
)";

static HTMLSurfaceBackendPreference html_view_to_surface_backend_preference(HTMLView::BackendPreference p_backend_preference) {
	switch (p_backend_preference) {
		case HTMLView::BACKEND_CPU:
			return HTML_SURFACE_BACKEND_CPU;
		case HTMLView::BACKEND_GPU_AUTO:
			return HTML_SURFACE_BACKEND_GPU_AUTO;
		case HTMLView::BACKEND_VULKAN:
			return HTML_SURFACE_BACKEND_VULKAN;
		case HTMLView::BACKEND_D3D12:
			return HTML_SURFACE_BACKEND_D3D12;
		case HTMLView::BACKEND_AUTO:
		default:
			return HTML_SURFACE_BACKEND_AUTO;
	}
}

static bool html_view_backend_preference_can_use_gpu(HTMLView::BackendPreference p_backend_preference) {
	return p_backend_preference == HTMLView::BACKEND_AUTO ||
			p_backend_preference == HTMLView::BACKEND_GPU_AUTO ||
			p_backend_preference == HTMLView::BACKEND_VULKAN ||
			p_backend_preference == HTMLView::BACKEND_D3D12;
}

static Dictionary html_form_control_state_to_dictionary(const HTMLFormControlState &p_state) {
	Dictionary state;
	state[SNAME("element_id")] = p_state.element_id;
	state[SNAME("tag_name")] = p_state.tag_name;
	state[SNAME("value")] = p_state.value;
	state[SNAME("checked")] = p_state.checked;
	state[SNAME("focused")] = p_state.focused;
	state[SNAME("selection_offsets_present")] = p_state.selection_offsets_present;
	state[SNAME("selection_start")] = p_state.selection_start;
	state[SNAME("selection_end")] = p_state.selection_end;
	return state;
}

static Dictionary html_backdrop_filter_region_to_dictionary(const HTMLBackdropFilterRegion &p_region) {
	Dictionary region;
	region[SNAME("element_id")] = p_region.element_id;
	region[SNAME("bounds")] = p_region.bounds;
	region[SNAME("blur_radius_css_px")] = p_region.blur_radius_css_px;
	region[SNAME("border_radius_top_left")] = p_region.border_radius_top_left;
	region[SNAME("border_radius_top_right")] = p_region.border_radius_top_right;
	region[SNAME("border_radius_bottom_right")] = p_region.border_radius_bottom_right;
	region[SNAME("border_radius_bottom_left")] = p_region.border_radius_bottom_left;
	region[SNAME("opacity")] = p_region.opacity;
	region[SNAME("flags")] = (int64_t)p_region.flags;
	region[SNAME("supported")] = !p_region.has_unsupported_flags();
	Array operations;
	for (const HTMLBackdropFilterOperation &operation : p_region.filter_operations) {
		Dictionary operation_data;
		operation_data[SNAME("type")] = operation.type;
		operation_data[SNAME("amount")] = operation.amount;
		operations.push_back(operation_data);
	}
	region[SNAME("filter_operations")] = operations;
	return region;
}

void HTMLView::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_document", "document"), &HTMLView::set_document);
	ClassDB::bind_method(D_METHOD("get_document"), &HTMLView::get_document);
	ClassDB::bind_method(D_METHOD("set_html", "html"), &HTMLView::set_html);
	ClassDB::bind_method(D_METHOD("get_html"), &HTMLView::get_html);
	ClassDB::bind_method(D_METHOD("set_css", "css"), &HTMLView::set_css);
	ClassDB::bind_method(D_METHOD("get_css"), &HTMLView::get_css);
	ClassDB::bind_method(D_METHOD("set_html_file", "html_file"), &HTMLView::set_html_file);
	ClassDB::bind_method(D_METHOD("get_html_file"), &HTMLView::get_html_file);
	ClassDB::bind_method(D_METHOD("set_css_files", "css_files"), &HTMLView::set_css_files);
	ClassDB::bind_method(D_METHOD("get_css_files"), &HTMLView::get_css_files);
	ClassDB::bind_method(D_METHOD("add_css_file", "css_file"), &HTMLView::add_css_file);
	ClassDB::bind_method(D_METHOD("remove_css_file", "css_file"), &HTMLView::remove_css_file);
	ClassDB::bind_method(D_METHOD("clear_css_files"), &HTMLView::clear_css_files);
	ClassDB::bind_method(D_METHOD("set_input_enabled", "input_enabled"), &HTMLView::set_input_enabled);
	ClassDB::bind_method(D_METHOD("is_input_enabled"), &HTMLView::is_input_enabled);
	ClassDB::bind_method(D_METHOD("set_focus_on_click", "focus_on_click"), &HTMLView::set_focus_on_click);
	ClassDB::bind_method(D_METHOD("is_focus_on_click_enabled"), &HTMLView::is_focus_on_click_enabled);
	ClassDB::bind_method(D_METHOD("set_accept_action", "action"), &HTMLView::set_accept_action);
	ClassDB::bind_method(D_METHOD("get_accept_action"), &HTMLView::get_accept_action);
	ClassDB::bind_method(D_METHOD("set_focus_next_action", "action"), &HTMLView::set_focus_next_action);
	ClassDB::bind_method(D_METHOD("get_focus_next_action"), &HTMLView::get_focus_next_action);
	ClassDB::bind_method(D_METHOD("set_focus_previous_action", "action"), &HTMLView::set_focus_previous_action);
	ClassDB::bind_method(D_METHOD("get_focus_previous_action"), &HTMLView::get_focus_previous_action);
	ClassDB::bind_method(D_METHOD("set_text_submit_action", "action"), &HTMLView::set_text_submit_action);
	ClassDB::bind_method(D_METHOD("get_text_submit_action"), &HTMLView::get_text_submit_action);
	ClassDB::bind_method(D_METHOD("set_text_backspace_action", "action"), &HTMLView::set_text_backspace_action);
	ClassDB::bind_method(D_METHOD("get_text_backspace_action"), &HTMLView::get_text_backspace_action);
	ClassDB::bind_method(D_METHOD("set_text_delete_action", "action"), &HTMLView::set_text_delete_action);
	ClassDB::bind_method(D_METHOD("get_text_delete_action"), &HTMLView::get_text_delete_action);
	ClassDB::bind_method(D_METHOD("set_backend_preference", "backend_preference"), &HTMLView::set_backend_preference);
	ClassDB::bind_method(D_METHOD("get_backend_preference"), &HTMLView::get_backend_preference);
	ClassDB::bind_method(D_METHOD("set_viewport_size_mode", "viewport_size_mode"), &HTMLView::set_viewport_size_mode);
	ClassDB::bind_method(D_METHOD("get_viewport_size_mode"), &HTMLView::get_viewport_size_mode);
	ClassDB::bind_method(D_METHOD("set_fixed_viewport_size", "fixed_viewport_size"), &HTMLView::set_fixed_viewport_size);
	ClassDB::bind_method(D_METHOD("get_fixed_viewport_size"), &HTMLView::get_fixed_viewport_size);
	ClassDB::bind_method(D_METHOD("set_use_document_minimum_size", "use_document_minimum_size"), &HTMLView::set_use_document_minimum_size);
	ClassDB::bind_method(D_METHOD("is_using_document_minimum_size"), &HTMLView::is_using_document_minimum_size);
	ClassDB::bind_method(D_METHOD("set_backdrop_filter_enabled", "backdrop_filter_enabled"), &HTMLView::set_backdrop_filter_enabled);
	ClassDB::bind_method(D_METHOD("is_backdrop_filter_enabled"), &HTMLView::is_backdrop_filter_enabled);
	ClassDB::bind_method(D_METHOD("get_backdrop_filter_regions"), &HTMLView::get_backdrop_filter_regions);
	ClassDB::bind_method(D_METHOD("get_texture"), &HTMLView::get_texture);
	ClassDB::bind_method(D_METHOD("local_to_html_position", "position"), &HTMLView::local_to_html_position);
	ClassDB::bind_method(D_METHOD("set_element_text", "id", "text"), &HTMLView::set_element_text);
	ClassDB::bind_method(D_METHOD("set_element_inner_html", "id", "html_fragment"), &HTMLView::set_element_inner_html);
	ClassDB::bind_method(D_METHOD("set_body_inner_html", "html_fragment"), &HTMLView::set_body_inner_html);
	ClassDB::bind_method(D_METHOD("set_element_attribute", "id", "name", "value"), &HTMLView::set_element_attribute);
	ClassDB::bind_method(D_METHOD("remove_element_attribute", "id", "name"), &HTMLView::remove_element_attribute);
	ClassDB::bind_method(D_METHOD("set_element_style", "id", "css_text"), &HTMLView::set_element_style);
	ClassDB::bind_method(D_METHOD("replace_stylesheet_text", "style_id", "css_text"), &HTMLView::replace_stylesheet_text);
	ClassDB::bind_method(D_METHOD("set_form_control_value", "id", "value"), &HTMLView::set_form_control_value);
	ClassDB::bind_method(D_METHOD("set_form_control_checked", "id", "checked"), &HTMLView::set_form_control_checked);
	ClassDB::bind_method(D_METHOD("focus_element", "id"), &HTMLView::focus_element);
	ClassDB::bind_method(D_METHOD("blur_focused_element"), &HTMLView::blur_focused_element);
	ClassDB::bind_method(D_METHOD("set_text_selection", "id", "start", "end"), &HTMLView::set_text_selection);
	ClassDB::bind_method(D_METHOD("get_form_control_state", "id"), &HTMLView::get_form_control_state);
	ClassDB::bind_method(D_METHOD("bind_action", "action", "callable"), &HTMLView::bind_action);
	ClassDB::bind_method(D_METHOD("unbind_action", "action"), &HTMLView::unbind_action);
	ClassDB::bind_method(D_METHOD("has_action", "action"), &HTMLView::has_action);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "document", PROPERTY_HINT_RESOURCE_TYPE, HTMLDocument::get_class_static()), "set_document", "get_document");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "html", PROPERTY_HINT_MULTILINE_TEXT), "set_html", "get_html");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "css", PROPERTY_HINT_MULTILINE_TEXT), "set_css", "get_css");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "html_file", PROPERTY_HINT_FILE, "*.html,*.htm"), "set_html_file", "get_html_file");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "css_files"), "set_css_files", "get_css_files");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "input_enabled"), "set_input_enabled", "is_input_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "focus_on_click"), "set_focus_on_click", "is_focus_on_click_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "accept_action"), "set_accept_action", "get_accept_action");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "focus_next_action"), "set_focus_next_action", "get_focus_next_action");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "focus_previous_action"), "set_focus_previous_action", "get_focus_previous_action");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "text_submit_action"), "set_text_submit_action", "get_text_submit_action");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "text_backspace_action"), "set_text_backspace_action", "get_text_backspace_action");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "text_delete_action"), "set_text_delete_action", "get_text_delete_action");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "backend_preference", PROPERTY_HINT_ENUM, "Auto,CPU,GPU Auto,Vulkan,D3D12"), "set_backend_preference", "get_backend_preference");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "viewport_size_mode", PROPERTY_HINT_ENUM, "Control Size,Screen Pixels,Fixed"), "set_viewport_size_mode", "get_viewport_size_mode");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "fixed_viewport_size", PROPERTY_HINT_RANGE, "0,16384,1,or_greater,suffix:px"), "set_fixed_viewport_size", "get_fixed_viewport_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_document_minimum_size"), "set_use_document_minimum_size", "is_using_document_minimum_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "backdrop_filter_enabled"), "set_backdrop_filter_enabled", "is_backdrop_filter_enabled");

	ADD_SIGNAL(MethodInfo("action_requested", PropertyInfo(Variant::STRING_NAME, "action"), PropertyInfo(Variant::DICTIONARY, "payload")));
	ADD_SIGNAL(MethodInfo("element_clicked", PropertyInfo(Variant::STRING_NAME, "element_id"), PropertyInfo(Variant::INT, "button")));
	ADD_SIGNAL(MethodInfo("render_error", PropertyInfo(Variant::STRING, "message")));

	BIND_ENUM_CONSTANT(BACKEND_AUTO);
	BIND_ENUM_CONSTANT(BACKEND_CPU);
	BIND_ENUM_CONSTANT(BACKEND_GPU_AUTO);
	BIND_ENUM_CONSTANT(BACKEND_VULKAN);
	BIND_ENUM_CONSTANT(BACKEND_D3D12);
	BIND_ENUM_CONSTANT(VIEWPORT_SIZE_CONTROL);
	BIND_ENUM_CONSTANT(VIEWPORT_SIZE_SCREEN_PIXELS);
	BIND_ENUM_CONSTANT(VIEWPORT_SIZE_FIXED);

	BIND_CONSTANT(HTML_BACKDROP_FILTER_ROUNDED_RECT);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_UNSUPPORTED_COMPLEX_CLIP);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_UNSUPPORTED_TRANSFORM);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_UNSUPPORTED_FILTER_OP);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_UNSUPPORTED_MASK_OR_BLEND);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_OPERATION_BLUR);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_OPERATION_BRIGHTNESS);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_OPERATION_CONTRAST);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_OPERATION_SATURATE);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_OPERATION_GRAYSCALE);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_OPERATION_SEPIA);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_OPERATION_INVERT);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_OPERATION_HUE_ROTATE);
	BIND_CONSTANT(HTML_BACKDROP_FILTER_OPERATION_OPACITY);
}

void HTMLView::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_connect_viewport_size_changed();
			_update_surface_size(false);
			_apply_surface_backend_preference();
			if (frame_render_pending) {
				set_process_internal(true);
			}
			_update_backdrop_filter_canvas();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_disconnect_viewport_size_changed();
		} break;

		case NOTIFICATION_DRAW: {
			_update_backdrop_filter_canvas();
			Ref<Texture2D> texture = surface->get_texture();
			if (texture.is_valid() && texture->get_width() > 0 && texture->get_height() > 0) {
				draw_texture_rect(texture, Rect2(Vector2(), get_size()));
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			if (!frame_render_pending) {
				set_process_internal(false);
				break;
			}

			if (frame_render_delay > 0) {
				frame_render_delay--;
				break;
			}

			frame_render_pending = false;
			bool waiting_for_async_completion = false;
			const bool poll_changed = surface->poll_pending_output(&waiting_for_async_completion);
			if (poll_changed) {
				_update_backdrop_filter_canvas();
				queue_redraw();
			}
			if (waiting_for_async_completion) {
				frame_render_pending = true;
				set_process_internal(true);
				break;
			}

			bool needs_output = true;
			bool needs_begin_frame = false;
			const bool had_pending_output = surface->has_pending_output();
			const uint64_t trace_sequence = pending_input_trace_sequence;
			if (trace_sequence != 0) {
				pending_input_trace_sequence = 0;
				html_view_input_trace(vformat("seq=%d internal_process begin", (int64_t)trace_sequence));
			}
			const double timeline_time_seconds = OS::get_singleton() != nullptr ? (double)OS::get_singleton()->get_ticks_usec() / 1000000.0 : 0.0;
			const uint64_t update_start_usec = trace_sequence != 0 && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
			const Error update_err = surface->update_compositor(timeline_time_seconds, &needs_output, &needs_begin_frame);
			if (trace_sequence != 0) {
				html_view_input_trace(vformat("seq=%d update_compositor exit err=%d needs_output=%s needs_begin_frame=%s elapsed_ms=%.3f",
						(int64_t)trace_sequence,
						(int)update_err,
						needs_output ? "true" : "false",
						needs_begin_frame ? "true" : "false",
						html_view_elapsed_ms(update_start_usec)));
			}
			if (update_err != OK) {
				needs_output = true;
				needs_begin_frame = false;
			}

			const bool should_render = needs_output || had_pending_output;
			if (should_render) {
				if (trace_sequence != 0) {
					html_view_input_trace(vformat("seq=%d render_now begin reason=%s", (int64_t)trace_sequence, needs_output ? "needs_output" : "pending_output"));
				}
				const uint64_t render_start_usec = trace_sequence != 0 && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
				surface->render_now("HTMLView");
				if (surface->has_pending_output()) {
					needs_begin_frame = true;
				}
				if (trace_sequence != 0) {
					html_view_input_trace(vformat("seq=%d render_now end pending_output=%s elapsed_ms=%.3f", (int64_t)trace_sequence, surface->has_pending_output() ? "true" : "false", html_view_elapsed_ms(render_start_usec)));
				}
			} else if (trace_sequence != 0) {
				html_view_input_trace(vformat("seq=%d render_now skipped reason=no_output", (int64_t)trace_sequence));
			}

			if (trace_sequence != 0 && needs_begin_frame) {
				pending_input_trace_sequence = trace_sequence;
			}
			frame_render_pending = needs_begin_frame;
			set_process_internal(frame_render_pending);
		} break;

		case NOTIFICATION_RESIZED: {
			_update_surface_size();
			_apply_surface_backend_preference();
			_update_backdrop_filter_canvas();
			update_minimum_size();
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			_update_surface_size(false);
			_apply_surface_backend_preference();
			_update_backdrop_filter_canvas();
		} break;
	}
}

bool HTMLView::_has_current_viewport_size() const {
	if (!is_inside_tree()) {
		return false;
	}

	if (viewport_size_mode == VIEWPORT_SIZE_FIXED && fixed_viewport_size.x > 0 && fixed_viewport_size.y > 0) {
		return true;
	}

	const Size2 control_size = get_size();
	return control_size.x > 0.0 && control_size.y > 0.0;
}

bool HTMLView::_should_defer_backend_activation() const {
	return html_view_backend_preference_can_use_gpu(backend_preference) && !_has_current_viewport_size();
}

void HTMLView::_apply_surface_backend_preference() {
	const HTMLSurfaceBackendPreference surface_backend_preference = _should_defer_backend_activation() ? HTML_SURFACE_BACKEND_CPU : html_view_to_surface_backend_preference(backend_preference);
	surface->set_backend_preference(surface_backend_preference);
}

void HTMLView::_surface_changed() {
	update_minimum_size();
	_update_backdrop_filter_canvas();
	queue_redraw();
}

void HTMLView::_connect_viewport_size_changed() {
	Viewport *viewport = get_viewport();
	if (viewport_size_changed_viewport == viewport) {
		return;
	}

	_disconnect_viewport_size_changed();
	if (viewport == nullptr) {
		return;
	}

	viewport->connect(SNAME("size_changed"), callable_mp(this, &HTMLView::_viewport_size_changed));
	viewport_size_changed_viewport = viewport;
}

void HTMLView::_disconnect_viewport_size_changed() {
	if (viewport_size_changed_viewport == nullptr) {
		return;
	}

	Callable callback = callable_mp(this, &HTMLView::_viewport_size_changed);
	if (viewport_size_changed_viewport->is_connected(SNAME("size_changed"), callback)) {
		viewport_size_changed_viewport->disconnect(SNAME("size_changed"), callback);
	}
	viewport_size_changed_viewport = nullptr;
}

void HTMLView::_viewport_size_changed() {
	if (viewport_size_mode == VIEWPORT_SIZE_SCREEN_PIXELS) {
		_update_surface_size();
	}
	_update_backdrop_filter_canvas();
}

void HTMLView::_ensure_document() {
	if (surface->get_document().is_null()) {
		Ref<HTMLDocument> document;
		document.instantiate();
		surface->set_document(document);
	}
}

void HTMLView::_ensure_backdrop_filter_canvas() {
	if (backdrop_filter_rect != nullptr) {
		return;
	}

	backdrop_filter_shader.instantiate();
	backdrop_filter_shader->set_code(html_view_backdrop_filter_shader_code);
	backdrop_filter_material.instantiate();
	backdrop_filter_material->set_shader(backdrop_filter_shader);

	backdrop_filter_rect = memnew(ColorRect);
	backdrop_filter_rect->set_name("_HTMLBackdropFilter");
	backdrop_filter_rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	backdrop_filter_rect->set_draw_behind_parent(true);
	backdrop_filter_rect->set_color(Color(1, 1, 1, 1));
	backdrop_filter_rect->set_material(backdrop_filter_material);
	backdrop_filter_rect->hide();
	add_child(backdrop_filter_rect, false, INTERNAL_MODE_BACK);
}

void HTMLView::_update_backdrop_filter_canvas() {
	_ensure_backdrop_filter_canvas();
	ERR_FAIL_NULL(backdrop_filter_rect);

	const Size2 control_size = get_size();
	if (!backdrop_filter_enabled || control_size.x <= 0.0 || control_size.y <= 0.0) {
		backdrop_filter_rect->hide();
		RS::get_singleton()->canvas_item_set_copy_to_backbuffer(backdrop_filter_rect->get_canvas_item(), false, Rect2());
		return;
	}

	const Size2i html_size = surface->get_size();
	if (html_size.x <= 0 || html_size.y <= 0) {
		backdrop_filter_rect->hide();
		RS::get_singleton()->canvas_item_set_copy_to_backbuffer(backdrop_filter_rect->get_canvas_item(), false, Rect2());
		return;
	}

	PackedVector4Array rects;
	PackedVector4Array radii;
	PackedVector4Array params;
	PackedVector4Array filter_ops;
	rects.resize(HTML_VIEW_MAX_BACKDROP_FILTER_REGIONS);
	radii.resize(HTML_VIEW_MAX_BACKDROP_FILTER_REGIONS);
	params.resize(HTML_VIEW_MAX_BACKDROP_FILTER_REGIONS);
	filter_ops.resize(HTML_VIEW_MAX_BACKDROP_FILTER_OPERATIONS);

	const float scale_x = control_size.x / (float)html_size.x;
	const float scale_y = control_size.y / (float)html_size.y;
	const float radius_scale = MAX(scale_x, scale_y);
	int supported_count = 0;
	int supported_op_count = 0;
	float max_blur_radius = 0.0f;
	bool use_gpu_mask = false;

	const HTMLGPUBackdropFrame &gpu_backdrop_frame = surface->get_gpu_backdrop_frame();
	if (gpu_backdrop_frame.is_valid()) {
		use_gpu_mask = true;
		for (const HTMLGPUBackdropEffect &effect : gpu_backdrop_frame.effects) {
			if (supported_count >= HTML_VIEW_MAX_BACKDROP_FILTER_REGIONS) {
				break;
			}
			if (effect.has_unsupported_flags() || effect.id == 0 || !effect.has_filter_operations()) {
				continue;
			}
			rects.set(supported_count, Vector4(effect.id, 0.0f, 0.0f, 0.0f));
			const int op_start = supported_op_count;
			float local_blur_radius = effect.blur_radius_css_px * radius_scale;
			for (const HTMLBackdropFilterOperation &operation : effect.filter_operations) {
				if (supported_op_count >= HTML_VIEW_MAX_BACKDROP_FILTER_OPERATIONS) {
					break;
				}
				float amount = operation.amount;
				if (operation.type == HTML_BACKDROP_FILTER_OPERATION_BLUR) {
					amount = MAX(0.0f, amount * radius_scale);
					local_blur_radius = MAX(local_blur_radius, amount);
				}
				filter_ops.set(supported_op_count, Vector4(operation.type, amount, 0.0f, 0.0f));
				supported_op_count++;
			}
			const int op_count = supported_op_count - op_start;
			if (op_count == 0 && local_blur_radius <= 0.0f) {
				continue;
			}
			params.set(supported_count, Vector4(local_blur_radius, effect.opacity, op_start, op_count));
			max_blur_radius = MAX(max_blur_radius, local_blur_radius);
			supported_count++;
		}
	} else {
		const Vector<HTMLBackdropFilterRegion> &regions = surface->get_backdrop_filter_regions();
		for (const HTMLBackdropFilterRegion &region : regions) {
			if (supported_count >= HTML_VIEW_MAX_BACKDROP_FILTER_REGIONS) {
				break;
			}
			if (region.has_unsupported_flags() || region.bounds.size.x <= 0.0f || region.bounds.size.y <= 0.0f || !region.has_filter_operations()) {
				continue;
			}

			const Rect2 local_rect(
					Point2(region.bounds.position.x * scale_x, region.bounds.position.y * scale_y),
					Size2(region.bounds.size.x * scale_x, region.bounds.size.y * scale_y));
			rects.set(supported_count, Vector4(local_rect.position.x, local_rect.position.y, local_rect.size.x, local_rect.size.y));
			radii.set(supported_count, Vector4(
											 region.border_radius_top_left * radius_scale,
											 region.border_radius_top_right * radius_scale,
											 region.border_radius_bottom_right * radius_scale,
											 region.border_radius_bottom_left * radius_scale));
			const int op_start = supported_op_count;
			float local_blur_radius = region.blur_radius_css_px * radius_scale;
			for (const HTMLBackdropFilterOperation &operation : region.filter_operations) {
				if (supported_op_count >= HTML_VIEW_MAX_BACKDROP_FILTER_OPERATIONS) {
					break;
				}
				float amount = operation.amount;
				if (operation.type == HTML_BACKDROP_FILTER_OPERATION_BLUR) {
					amount = MAX(0.0f, amount * radius_scale);
					local_blur_radius = MAX(local_blur_radius, amount);
				}
				filter_ops.set(supported_op_count, Vector4(operation.type, amount, 0.0f, 0.0f));
				supported_op_count++;
			}
			const int op_count = supported_op_count - op_start;
			if (op_count == 0 && local_blur_radius <= 0.0f) {
				continue;
			}
			params.set(supported_count, Vector4(local_blur_radius, region.opacity, op_start, op_count));
			max_blur_radius = MAX(max_blur_radius, local_blur_radius);
			supported_count++;
		}
	}

	if (supported_count == 0) {
		backdrop_filter_rect->hide();
		RS::get_singleton()->canvas_item_set_copy_to_backbuffer(backdrop_filter_rect->get_canvas_item(), false, Rect2());
		return;
	}

	backdrop_filter_rect->show();
	backdrop_filter_rect->set_position(Point2());
	backdrop_filter_rect->set_size(control_size);
	backdrop_filter_material->set_shader_parameter(SNAME("use_mask_texture"), use_gpu_mask);
	if (use_gpu_mask) {
		backdrop_filter_material->set_shader_parameter(SNAME("backdrop_mask_texture"), gpu_backdrop_frame.mask_texture);
	}
	backdrop_filter_material->set_shader_parameter(SNAME("view_size"), control_size);
	backdrop_filter_material->set_shader_parameter(SNAME("region_count"), supported_count);
	backdrop_filter_material->set_shader_parameter(SNAME("region_rects"), rects);
	backdrop_filter_material->set_shader_parameter(SNAME("region_radii"), radii);
	backdrop_filter_material->set_shader_parameter(SNAME("region_params"), params);
	backdrop_filter_material->set_shader_parameter(SNAME("filter_ops"), filter_ops);

	const Rect2 copy_rect = Rect2(Point2(), control_size).grow(max_blur_radius * 2.0f);
	RS::get_singleton()->canvas_item_set_copy_to_backbuffer(backdrop_filter_rect->get_canvas_item(), true, copy_rect);
	backdrop_filter_rect->queue_redraw();
}

Vector2 HTMLView::_get_screen_pixel_scale() const {
	if (!is_inside_tree()) {
		return Vector2(1, 1);
	}

	const Vector2 scale = (get_viewport_transform() * get_global_transform()).get_scale().abs();
	float scale_x = scale.x;
	float scale_y = scale.y;
	if (!Math::is_finite(scale_x) || scale_x <= 0.0f) {
		scale_x = 1.0f;
	}
	if (!Math::is_finite(scale_y) || scale_y <= 0.0f) {
		scale_y = 1.0f;
	}
	return Vector2(CLAMP(scale_x, 0.01f, 8.0f), CLAMP(scale_y, 0.01f, 8.0f));
}

Size2i HTMLView::_get_target_viewport_size() const {
	const Size2 control_size = get_size();
	Size2i target_size;

	switch (viewport_size_mode) {
		case VIEWPORT_SIZE_FIXED: {
			target_size = fixed_viewport_size;
			if (target_size.x <= 0 || target_size.y <= 0) {
				Ref<HTMLDocument> document = surface->get_document();
				target_size = document.is_valid() ? document->get_default_size() : Size2i(512, 512);
			}
		} break;

		case VIEWPORT_SIZE_SCREEN_PIXELS:
		case VIEWPORT_SIZE_CONTROL:
		default: {
			target_size = Size2i(Math::ceil(control_size.x), Math::ceil(control_size.y));
		} break;
	}

	if (target_size.x <= 0 || target_size.y <= 0) {
		Ref<HTMLDocument> document = surface->get_document();
		target_size = document.is_valid() ? document->get_default_size() : Size2i(512, 512);
	}
	return Size2i(MAX(1, target_size.x), MAX(1, target_size.y));
}

float HTMLView::_get_target_device_scale_factor() const {
	if (viewport_size_mode == VIEWPORT_SIZE_FIXED) {
		return 1.0f;
	}

	const Vector2 scale = _get_screen_pixel_scale();
	return CLAMP(MAX(scale.x, scale.y), 1.0f, 8.0f);
}

void HTMLView::_update_surface_size(bool p_force_render) {
	const bool changed = surface->set_viewport(_get_target_viewport_size(), _get_target_device_scale_factor(), false);
	if (changed || (p_force_render && surface->get_texture().is_null())) {
		_queue_frame_render();
	}
	queue_redraw();
}

Vector2 HTMLView::_local_to_html_position(const Vector2 &p_position) const {
	const Size2 control_size = get_size();
	const Size2i html_size = surface->get_size();
	if (control_size.x <= 0.0 || control_size.y <= 0.0 || html_size.x <= 0 || html_size.y <= 0) {
		return p_position;
	}

	return Vector2(
			p_position.x * (double)html_size.x / control_size.x,
			p_position.y * (double)html_size.y / control_size.y);
}

int HTMLView::_modifiers_from_event(const Ref<InputEvent> &p_event, MouseButton p_button, bool p_button_pressed) const {
	int modifiers = 0;

	Ref<InputEventWithModifiers> modifiers_event = p_event;
	if (modifiers_event.is_valid()) {
		if (modifiers_event->is_shift_pressed()) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_SHIFT;
		}
		if (modifiers_event->is_ctrl_pressed()) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_CONTROL;
		}
		if (modifiers_event->is_alt_pressed()) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_ALT;
		}
		if (modifiers_event->is_meta_pressed()) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_META;
		}
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid()) {
		const BitField<MouseButtonMask> button_mask = motion->get_button_mask();
		if (button_mask.has_flag(MouseButtonMask::LEFT)) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_LEFT_BUTTON_DOWN;
		}
		if (button_mask.has_flag(MouseButtonMask::MIDDLE)) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_MIDDLE_BUTTON_DOWN;
		}
		if (button_mask.has_flag(MouseButtonMask::RIGHT)) {
			modifiers |= HTML_SURFACE_INPUT_MODIFIER_RIGHT_BUTTON_DOWN;
		}
	}

	if (p_button_pressed) {
		switch (p_button) {
			case MouseButton::LEFT:
				modifiers |= HTML_SURFACE_INPUT_MODIFIER_LEFT_BUTTON_DOWN;
				break;
			case MouseButton::MIDDLE:
				modifiers |= HTML_SURFACE_INPUT_MODIFIER_MIDDLE_BUTTON_DOWN;
				break;
			case MouseButton::RIGHT:
				modifiers |= HTML_SURFACE_INPUT_MODIFIER_RIGHT_BUTTON_DOWN;
				break;
			default:
				break;
		}
	}

	return modifiers;
}

HTMLSurfaceMouseButton HTMLView::_to_html_mouse_button(MouseButton p_button) const {
	switch (p_button) {
		case MouseButton::LEFT:
			return HTML_SURFACE_MOUSE_BUTTON_LEFT;
		case MouseButton::MIDDLE:
			return HTML_SURFACE_MOUSE_BUTTON_MIDDLE;
		case MouseButton::RIGHT:
			return HTML_SURFACE_MOUSE_BUTTON_RIGHT;
		default:
			return HTML_SURFACE_MOUSE_BUTTON_NONE;
	}
}

HTMLSurfaceInputKey HTMLView::_to_html_input_key(Key p_key) const {
	switch (p_key) {
		case Key::BACKSPACE:
			return HTML_SURFACE_INPUT_KEY_BACKSPACE;
		case Key::TAB:
			return HTML_SURFACE_INPUT_KEY_TAB;
		case Key::ENTER:
		case Key::KP_ENTER:
			return HTML_SURFACE_INPUT_KEY_ENTER;
		case Key::KEY_DELETE:
			return HTML_SURFACE_INPUT_KEY_DELETE;
		default:
			return HTML_SURFACE_INPUT_KEY_UNKNOWN;
	}
}

bool HTMLView::_hit_test(const Vector2 &p_html_position, HTMLElementHit &r_hit) const {
	const bool hit = surface->hit_test(Point2(p_html_position.x, p_html_position.y), r_hit);
	if (html_view_input_trace_enabled()) {
		const HTMLFrameMetadata &frame_metadata = surface->get_frame_metadata();
		html_view_input_trace(vformat("hit_test position=%s result=%s hit_count=%d element_id=%s",
				p_html_position,
				hit ? "true" : "false",
				frame_metadata.hits.size(),
				hit ? String(r_hit.element_id) : String()));
	}
	return hit;
}

bool HTMLView::_same_activation_target(const HTMLElementHit &p_pressed, const HTMLElementHit &p_released) const {
	if (p_pressed.disabled || p_released.disabled) {
		return false;
	}

	if (p_pressed.element_id != StringName() || p_released.element_id != StringName()) {
		return p_pressed.element_id == p_released.element_id;
	}

	const String pressed_action = p_pressed.get_attribute(SNAME("data-godot-action"));
	const String released_action = p_released.get_attribute(SNAME("data-godot-action"));
	if (!pressed_action.is_empty() || !released_action.is_empty()) {
		return pressed_action == released_action && p_pressed.bounds == p_released.bounds;
	}

	return p_pressed.tag_name == p_released.tag_name && p_pressed.bounds == p_released.bounds;
}

void HTMLView::_emit_activation(const HTMLElementHit &p_hit, const Vector2 &p_html_position, MouseButton p_button) {
	if (p_hit.disabled) {
		return;
	}

	const Point2i document_position(Math::floor(p_html_position.x), Math::floor(p_html_position.y));
	HTMLActionActivation activation = HTMLActivationEngine::activate(p_hit, document_position, p_button);
	if (!activation.has_action) {
		return;
	}

	emit_signal(SNAME("element_clicked"), activation.element_id, (int)p_button);
	emit_signal(SNAME("action_requested"), activation.action, activation.payload);
	_call_bound_action(activation.action, activation.payload);
}

bool HTMLView::_send_action_key_event(const Ref<InputEvent> &p_event) {
	struct ActionKeyMapping {
		StringName action;
		HTMLSurfaceInputKey key = HTML_SURFACE_INPUT_KEY_UNKNOWN;
		int extra_modifiers = 0;
	};

	const int base_modifiers = _modifiers_from_event(p_event);
	const ActionKeyMapping mappings[] = {
		{ focus_previous_action, HTML_SURFACE_INPUT_KEY_TAB, HTML_SURFACE_INPUT_MODIFIER_SHIFT },
		{ focus_next_action, HTML_SURFACE_INPUT_KEY_TAB, 0 },
		{ text_backspace_action, HTML_SURFACE_INPUT_KEY_BACKSPACE, 0 },
		{ text_delete_action, HTML_SURFACE_INPUT_KEY_DELETE, 0 },
		{ text_submit_action, HTML_SURFACE_INPUT_KEY_ENTER, 0 },
		{ accept_action, HTML_SURFACE_INPUT_KEY_ENTER, 0 },
	};

	for (const ActionKeyMapping &mapping : mappings) {
		if (mapping.action == StringName() || mapping.key == HTML_SURFACE_INPUT_KEY_UNKNOWN) {
			continue;
		}

		const int modifiers = base_modifiers | mapping.extra_modifiers;
		if (p_event->is_action_pressed(mapping.action, false, true)) {
			return surface->key_down(mapping.key, modifiers) == OK;
		}
		if (p_event->is_action_released(mapping.action, true)) {
			return surface->key_up(mapping.key, modifiers) == OK;
		}
	}

	return false;
}

bool HTMLView::_send_key_event(const Ref<InputEventKey> &p_event) {
	if (p_event.is_null()) {
		return false;
	}

	bool handled = false;
	const HTMLSurfaceInputKey key = _to_html_input_key(p_event->get_keycode());
	if (key != HTML_SURFACE_INPUT_KEY_UNKNOWN) {
		if (p_event->is_pressed()) {
			handled = surface->key_down(key, _modifiers_from_event(p_event)) == OK;
		} else {
			handled = surface->key_up(key, _modifiers_from_event(p_event)) == OK;
		}
	}

	if (p_event->is_pressed() && !p_event->is_echo() && !p_event->is_ctrl_pressed() && !p_event->is_alt_pressed() && !p_event->is_meta_pressed() && p_event->get_unicode() >= 32) {
		handled = surface->text_input(String::chr(p_event->get_unicode())) == OK || handled;
	}

	return handled;
}

void HTMLView::_call_bound_action(const StringName &p_action, const Dictionary &p_payload) {
	const Callable *callable_ptr = action_bindings.getptr(p_action);
	if (!callable_ptr || !callable_ptr->is_valid()) {
		return;
	}

	const Callable callable = *callable_ptr;
	const Variant args[1] = { p_payload };
	const Variant *argptrs[1] = { &args[0] };
	Variant ret;
	Callable::CallError ce;
	callable.callp(argptrs, 1, ret, ce);
	if (ce.error != Callable::CallError::CALL_OK) {
		String message = vformat("HTML action '%s' callable failed with error %d.", String(p_action), ce.error);
		ERR_PRINT(message);
		emit_signal(SNAME("render_error"), message);
	}
}

void HTMLView::_queue_frame_render() {
	if (frame_render_pending) {
		return;
	}

	frame_render_pending = true;
	frame_render_delay = 1;
	if (is_inside_tree()) {
		set_process_internal(true);
	}
}

void HTMLView::set_document(const Ref<HTMLDocument> &p_document) {
	surface->set_document(p_document);
	_update_surface_size();
}

Ref<HTMLDocument> HTMLView::get_document() const {
	return surface->get_document();
}

void HTMLView::set_html(const String &p_html) {
	_ensure_document();
	surface->get_document()->set_html(p_html);
}

String HTMLView::get_html() const {
	Ref<HTMLDocument> document = surface->get_document();
	return document.is_valid() ? document->get_html() : String();
}

void HTMLView::set_css(const String &p_css) {
	_ensure_document();
	surface->get_document()->set_css(p_css);
}

String HTMLView::get_css() const {
	Ref<HTMLDocument> document = surface->get_document();
	return document.is_valid() ? document->get_css() : String();
}

void HTMLView::set_html_file(const String &p_html_file) {
	_ensure_document();
	surface->get_document()->set_html_file(p_html_file);
}

String HTMLView::get_html_file() const {
	Ref<HTMLDocument> document = surface->get_document();
	return document.is_valid() ? document->get_html_file() : String();
}

void HTMLView::set_css_files(const PackedStringArray &p_css_files) {
	_ensure_document();
	surface->get_document()->set_css_files(p_css_files);
}

PackedStringArray HTMLView::get_css_files() const {
	Ref<HTMLDocument> document = surface->get_document();
	return document.is_valid() ? document->get_css_files() : PackedStringArray();
}

void HTMLView::add_css_file(const String &p_css_file) {
	_ensure_document();
	surface->get_document()->add_css_file(p_css_file);
}

void HTMLView::remove_css_file(const String &p_css_file) {
	_ensure_document();
	surface->get_document()->remove_css_file(p_css_file);
}

void HTMLView::clear_css_files() {
	_ensure_document();
	surface->get_document()->clear_css_files();
}

void HTMLView::set_input_enabled(bool p_input_enabled) {
	input_enabled = p_input_enabled;
}

bool HTMLView::is_input_enabled() const {
	return input_enabled;
}

void HTMLView::set_focus_on_click(bool p_focus_on_click) {
	focus_on_click = p_focus_on_click;
}

bool HTMLView::is_focus_on_click_enabled() const {
	return focus_on_click;
}

void HTMLView::set_accept_action(const StringName &p_action) {
	accept_action = p_action;
}

StringName HTMLView::get_accept_action() const {
	return accept_action;
}

void HTMLView::set_focus_next_action(const StringName &p_action) {
	focus_next_action = p_action;
}

StringName HTMLView::get_focus_next_action() const {
	return focus_next_action;
}

void HTMLView::set_focus_previous_action(const StringName &p_action) {
	focus_previous_action = p_action;
}

StringName HTMLView::get_focus_previous_action() const {
	return focus_previous_action;
}

void HTMLView::set_text_submit_action(const StringName &p_action) {
	text_submit_action = p_action;
}

StringName HTMLView::get_text_submit_action() const {
	return text_submit_action;
}

void HTMLView::set_text_backspace_action(const StringName &p_action) {
	text_backspace_action = p_action;
}

StringName HTMLView::get_text_backspace_action() const {
	return text_backspace_action;
}

void HTMLView::set_text_delete_action(const StringName &p_action) {
	text_delete_action = p_action;
}

StringName HTMLView::get_text_delete_action() const {
	return text_delete_action;
}

void HTMLView::set_backend_preference(BackendPreference p_backend_preference) {
	ERR_FAIL_INDEX((int)p_backend_preference, 5);
	backend_preference = p_backend_preference;
	_apply_surface_backend_preference();
}

HTMLView::BackendPreference HTMLView::get_backend_preference() const {
	return backend_preference;
}

void HTMLView::set_viewport_size_mode(ViewportSizeMode p_viewport_size_mode) {
	ERR_FAIL_INDEX((int)p_viewport_size_mode, 3);
	if (viewport_size_mode == p_viewport_size_mode) {
		return;
	}
	viewport_size_mode = p_viewport_size_mode;
	_update_surface_size();
	update_minimum_size();
}

HTMLView::ViewportSizeMode HTMLView::get_viewport_size_mode() const {
	return viewport_size_mode;
}

void HTMLView::set_fixed_viewport_size(const Size2i &p_fixed_viewport_size) {
	const Size2i new_size = Size2i(MAX(0, p_fixed_viewport_size.x), MAX(0, p_fixed_viewport_size.y));
	if (fixed_viewport_size == new_size) {
		return;
	}
	fixed_viewport_size = new_size;
	_update_surface_size();
	update_minimum_size();
}

Size2i HTMLView::get_fixed_viewport_size() const {
	return fixed_viewport_size;
}

void HTMLView::set_use_document_minimum_size(bool p_use_document_minimum_size) {
	if (use_document_minimum_size == p_use_document_minimum_size) {
		return;
	}
	use_document_minimum_size = p_use_document_minimum_size;
	update_minimum_size();
}

bool HTMLView::is_using_document_minimum_size() const {
	return use_document_minimum_size;
}

void HTMLView::set_backdrop_filter_enabled(bool p_backdrop_filter_enabled) {
	if (backdrop_filter_enabled == p_backdrop_filter_enabled) {
		return;
	}
	backdrop_filter_enabled = p_backdrop_filter_enabled;
	surface->set_backdrop_filter_enabled(backdrop_filter_enabled);
	_update_backdrop_filter_canvas();
	queue_redraw();
}

bool HTMLView::is_backdrop_filter_enabled() const {
	return backdrop_filter_enabled;
}

Array HTMLView::get_backdrop_filter_regions() const {
	Array regions;
	for (const HTMLBackdropFilterRegion &region : surface->get_backdrop_filter_regions()) {
		regions.push_back(html_backdrop_filter_region_to_dictionary(region));
	}
	return regions;
}

Ref<Texture2D> HTMLView::get_texture() const {
	if (_should_defer_backend_activation()) {
		return Ref<Texture2D>();
	}
	return surface->get_texture();
}

Vector2 HTMLView::local_to_html_position(const Vector2 &p_position) const {
	return _local_to_html_position(p_position);
}

Error HTMLView::set_element_text(const StringName &p_id, const String &p_text) {
	Error err = surface->set_element_text(p_id, p_text);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::set_element_inner_html(const StringName &p_id, const String &p_html_fragment) {
	Error err = surface->set_element_inner_html(p_id, p_html_fragment);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::set_body_inner_html(const String &p_html_fragment) {
	Error err = surface->set_body_inner_html(p_html_fragment);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::set_element_attribute(const StringName &p_id, const StringName &p_name, const String &p_value) {
	Error err = surface->set_element_attribute(p_id, p_name, p_value);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::remove_element_attribute(const StringName &p_id, const StringName &p_name) {
	Error err = surface->remove_element_attribute(p_id, p_name);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::set_element_style(const StringName &p_id, const String &p_css_text) {
	Error err = surface->set_element_style(p_id, p_css_text);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::replace_stylesheet_text(const StringName &p_style_id, const String &p_css_text) {
	Error err = surface->replace_stylesheet_text(p_style_id, p_css_text);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::set_form_control_value(const StringName &p_id, const String &p_value) {
	Error err = surface->set_form_control_value(p_id, p_value);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::set_form_control_checked(const StringName &p_id, bool p_checked) {
	Error err = surface->set_form_control_checked(p_id, p_checked);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::focus_element(const StringName &p_id) {
	Error err = surface->focus_element(p_id);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::blur_focused_element() {
	Error err = surface->blur_focused_element();
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Error HTMLView::set_text_selection(const StringName &p_id, int p_start, int p_end) {
	Error err = surface->set_text_selection(p_id, p_start, p_end);
	if (err == OK) {
		_queue_frame_render();
	}
	return err;
}

Dictionary HTMLView::get_form_control_state(const StringName &p_id) {
	HTMLFormControlState state;
	if (!surface->get_form_control_state(p_id, state)) {
		return Dictionary();
	}
	return html_form_control_state_to_dictionary(state);
}

void HTMLView::bind_action(const StringName &p_action, const Callable &p_callable) {
	if (p_callable.is_valid()) {
		action_bindings[p_action] = p_callable;
	} else {
		action_bindings.erase(p_action);
	}
}

void HTMLView::unbind_action(const StringName &p_action) {
	action_bindings.erase(p_action);
}

bool HTMLView::has_action(const StringName &p_action) const {
	return action_bindings.has(p_action);
}

void HTMLView::gui_input(const Ref<InputEvent> &p_event) {
	if (!input_enabled || p_event.is_null()) {
		return;
	}

	Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid()) {
		const Vector2 html_position = _local_to_html_position(mm->get_position());
		if (surface->mouse_move(html_position, _modifiers_from_event(mm)) == OK) {
			_queue_frame_render();
		}
		accept_event();
		return;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		const Vector2 html_position = _local_to_html_position(mb->get_position());
		const MouseButton button_index = mb->get_button_index();

		Vector2 wheel_delta;
		const float wheel_step = 48.0f * mb->get_factor();
		switch (button_index) {
			case MouseButton::WHEEL_UP:
				wheel_delta.y = -wheel_step;
				break;
			case MouseButton::WHEEL_DOWN:
				wheel_delta.y = wheel_step;
				break;
			case MouseButton::WHEEL_LEFT:
				wheel_delta.x = -wheel_step;
				break;
			case MouseButton::WHEEL_RIGHT:
				wheel_delta.x = wheel_step;
				break;
			default:
				break;
		}

		if (!wheel_delta.is_zero_approx()) {
			if (mb->is_pressed()) {
				if (surface->wheel(html_position, wheel_delta) == OK) {
					_queue_frame_render();
					html_view_input_trace(vformat("wheel accepted local=%s html=%s delta=%s button=%d", mb->get_position(), html_position, wheel_delta, (int)button_index));
				} else {
					html_view_input_trace(vformat("wheel rejected local=%s html=%s delta=%s button=%d", mb->get_position(), html_position, wheel_delta, (int)button_index));
				}
				accept_event();
			}
			return;
		}

		const HTMLSurfaceMouseButton html_button = _to_html_mouse_button(button_index);
		if (html_button == HTML_SURFACE_MOUSE_BUTTON_NONE) {
			html_view_input_trace(vformat("mouse_button ignored local=%s html=%s button=%d pressed=%s reason=unsupported_button", mb->get_position(), html_position, (int)button_index, mb->is_pressed() ? "true" : "false"));
			return;
		}

		if (focus_on_click && mb->is_pressed() && button_index == MouseButton::LEFT) {
			grab_focus();
		}

		if (mb->is_pressed()) {
			const uint64_t trace_sequence = ++input_trace_sequence;
			const uint64_t input_start_usec = html_view_input_trace_enabled() && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
			pointer_press_active = false;
			pointer_press_button = button_index;
			const Error down_err = surface->mouse_down(html_position, html_button, _modifiers_from_event(mb, button_index, true), mb->is_double_click() ? 2 : 1);
			const bool has_press_hit = _hit_test(html_position, pointer_press_hit);
			if (has_press_hit) {
				pointer_press_active = true;
			}
			html_view_input_trace(vformat("seq=%d mouse_down accepted local=%s html=%s button=%d err=%d press_hit=%s element_id=%s elapsed_ms=%.3f",
					(int64_t)trace_sequence,
					mb->get_position(),
					html_position,
					(int)button_index,
					(int)down_err,
					has_press_hit ? "true" : "false",
					has_press_hit ? String(pointer_press_hit.element_id) : String(),
					html_view_elapsed_ms(input_start_usec)));
			_queue_frame_render();
			pending_input_trace_sequence = trace_sequence;
			html_view_input_trace(vformat("seq=%d queued_frame after=mouse_down frame_render_pending=%s", (int64_t)trace_sequence, frame_render_pending ? "true" : "false"));
			accept_event();
			return;
		}

		const uint64_t trace_sequence = ++input_trace_sequence;
		const uint64_t input_start_usec = html_view_input_trace_enabled() && OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
		const Error up_err = surface->mouse_up(html_position, html_button, _modifiers_from_event(mb, button_index, false), mb->is_double_click() ? 2 : 1);
		HTMLElementHit release_hit;
		const bool has_release_hit = _hit_test(html_position, release_hit);
		bool activation_emitted = false;
		if (button_index == MouseButton::LEFT && pointer_press_active && pointer_press_button == button_index && has_release_hit && _same_activation_target(pointer_press_hit, release_hit)) {
			_emit_activation(release_hit, html_position, button_index);
			activation_emitted = true;
		}
		html_view_input_trace(vformat("seq=%d mouse_up accepted local=%s html=%s button=%d err=%d release_hit=%s element_id=%s activation_emitted=%s elapsed_ms=%.3f",
				(int64_t)trace_sequence,
				mb->get_position(),
				html_position,
				(int)button_index,
				(int)up_err,
				has_release_hit ? "true" : "false",
				has_release_hit ? String(release_hit.element_id) : String(),
				activation_emitted ? "true" : "false",
				html_view_elapsed_ms(input_start_usec)));
		pointer_press_active = false;
		_queue_frame_render();
		pending_input_trace_sequence = trace_sequence;
		html_view_input_trace(vformat("seq=%d queued_frame after=mouse_up frame_render_pending=%s", (int64_t)trace_sequence, frame_render_pending ? "true" : "false"));
		accept_event();
		return;
	}

	if (_send_action_key_event(p_event)) {
		html_view_input_trace("key_action accepted");
		_queue_frame_render();
		accept_event();
		return;
	}

	Ref<InputEventKey> key_event = p_event;
	if (key_event.is_valid() && _send_key_event(key_event)) {
		_queue_frame_render();
		accept_event();
		return;
	}
}

Size2 HTMLView::get_minimum_size() const {
	if (use_document_minimum_size) {
		Ref<HTMLDocument> document = surface->get_document();
		if (document.is_valid()) {
			return document->get_default_size();
		}
	}
	return Size2();
}

HTMLView::HTMLView() {
	surface.instantiate();
	surface->set_changed_callback(callable_mp(this, &HTMLView::_surface_changed));
	surface->set_placeholder_background(Color(0.08, 0.09, 0.1, 1.0));
	surface->set_backend_preference(HTML_SURFACE_BACKEND_CPU);
	_ensure_backdrop_filter_canvas();
	set_focus_mode(FOCUS_CLICK);
	set_texture_filter(CanvasItem::TEXTURE_FILTER_NEAREST);
	set_notify_transform(true);
}

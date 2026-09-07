#include "hcsr_newest_backdrop.h"
#include "scene/resources/image_texture.h"

const HTMLGPUBackdropFrame &HCSRNewestBackdrop::update(hcsr_draw_packet_t packet, const Size2i &logical, const Size2i &physical) {
	hcsr_backdrop_view_t view = {};
	view.struct_size = sizeof(view);
	if (hcsr_draw_packet_get_backdrop_view(packet, &view) != HCSR_OK || view.effect_count == 0) {
		frame.clear(); previous.clear(); return frame;
	}
	// The existing canvas compositor has eight IDs and 64 ordered operations.
	if (view.effect_count > 8 || view.operation_count > 64) {
		WARN_PRINT_ONCE("HCSR backdrop frame exceeds Godot's eight-effect/64-operation compositor capacity.");
		frame.clear(); previous.clear(); return frame;
	}
	Vector<uint8_t> signature;
	auto append = [&](const void *data, size_t bytes) {
		int offset = signature.size(); signature.resize(offset + bytes);
		if (bytes) memcpy(signature.ptrw() + offset, data, bytes);
	};
	append(&logical, sizeof(logical));
	append(view.effects, view.effect_count * sizeof(hcsr_backdrop_effect_t));
	append(view.operations, view.operation_count * sizeof(hcsr_backdrop_operation_t));
	append(view.vertices, view.vertex_count * sizeof(hcsr_backdrop_vertex_t));
	if (signature != previous || physical != previous_size) {
		previous = signature; previous_size = physical; frame.clear();
		Ref<Image> image = Image::create_empty(physical.x, physical.y, false, Image::FORMAT_RGBA8);
		image->fill(Color(0, 0, 0, 1));
		Vector2 scale(float(physical.x) / logical.x, float(physical.y) / logical.y);
		for (size_t index = 0; index < view.effect_count; index++) {
			const auto &source = view.effects[index];
			HTMLGPUBackdropEffect effect;
			effect.id = index + 1;
			bool first = true;
			for (uint32_t op = 0; op < source.operation_count; op++) {
				const auto &operation = view.operations[source.first_operation + op];
				HTMLBackdropFilterOperation filter;
				filter.type = HTMLBackdropFilterOperationType(operation.kind); filter.amount = operation.amount;
				effect.filter_operations.push_back(filter);
				if (operation.kind == 0) effect.blur_radius_css_px = MAX(effect.blur_radius_css_px, operation.amount);
			}
			for (uint32_t vertex = 0; vertex + 2 < source.vertex_count; vertex += 3) {
				Vector2 p[3]; float coverage[3];
				for (int j = 0; j < 3; j++) {
					const auto &v = view.vertices[source.first_vertex + vertex + j];
					Vector2 logical_point(v.x, v.y);
					if (first) { effect.bounds.position = logical_point; first = false; }
					else effect.bounds.expand_to(logical_point);
					p[j] = logical_point * scale; coverage[j] = v.coverage;
				}
				float area = (p[1] - p[0]).cross(p[2] - p[0]);
				if (Math::abs(area) < 0.00001f) continue;
				int left = MAX(0, int(Math::floor(MIN(p[0].x, MIN(p[1].x, p[2].x)))));
				int top = MAX(0, int(Math::floor(MIN(p[0].y, MIN(p[1].y, p[2].y)))));
				int right = MIN(physical.x, int(Math::ceil(MAX(p[0].x, MAX(p[1].x, p[2].x)))));
				int bottom = MIN(physical.y, int(Math::ceil(MAX(p[0].y, MAX(p[1].y, p[2].y)))));
				for (int y = top; y < bottom; y++) for (int x = left; x < right; x++) {
					Vector2 point(x + .5f, y + .5f);
					float a = (p[1] - point).cross(p[2] - point) / area;
					float b = (p[2] - point).cross(p[0] - point) / area;
					float c = 1 - a - b;
					if (a < -0.00001f || b < -0.00001f || c < -0.00001f) continue;
					float amount = CLAMP(a * coverage[0] + b * coverage[1] + c * coverage[2], 0.0f, 1.0f);
					if (amount <= 0) continue;
					Color old = image->get_pixel(x, y);
					if (int(Math::round(old.r * 255)) == int(effect.id)) amount = MAX(amount, old.g);
					image->set_pixel(x, y, Color(effect.id / 255.0f, amount, 0, 1));
				}
			}
			frame.effects.push_back(effect);
		}
		frame.mask_texture = ImageTexture::create_from_image(image);
		frame.logical_size = logical; frame.physical_size = physical;
		frame.device_scale_factor = scale.x;
		frame.mask_encoding = HTML_GPU_BACKDROP_MASK_ENCODING_RGBA8_ID_COVERAGE;
		frame.max_effect_id = view.effect_count;
	}
	frame.frame_generation = frame.main_target_generation = frame.backdrop_mask_generation = view.generation;
	for (auto &effect : frame.effects) effect.generation = view.generation;
	return frame;
}

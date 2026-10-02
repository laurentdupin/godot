#include "hcsr_newest_backdrop.h"

#include "core/templates/hash_set.h"
#include "scene/resources/image_texture.h"
#include "servers/rendering/rendering_server.h"

const HTMLGPUBackdropFrame &HCSRNewestBackdrop::update(hcsr_draw_packet_t packet, const Size2i &logical, const Size2i &physical, HCSRNewestSceneRenderer &renderer) {
	hcsr_backdrop_view_t view = {};
	view.struct_size = sizeof(view);
	if (hcsr_draw_packet_get_backdrop_view(packet, &view) != HCSR_OK || view.effect_count == 0) {
		frame.clear();
		previous.clear();
		return frame;
	}
	hcsr_draw_packet_view_t packet_view = {};
	packet_view.struct_size = sizeof(packet_view);
	const bool local = (view.reserved & 1) != 0;
	if (local && hcsr_draw_packet_get_view(packet, &packet_view) != HCSR_OK) {
		frame.clear();
		return frame;
	}
	// The existing canvas compositor has eight IDs and 64 ordered operations.
	if (view.effect_count > 8 || view.operation_count > 64) {
		WARN_PRINT_ONCE("HCSR backdrop frame exceeds Godot's eight-effect/64-operation compositor capacity.");
		frame.clear();
		previous.clear();
		return frame;
	}
	Vector<uint8_t> signature;
	auto append = [&](const void *data, size_t bytes) {
		int offset = signature.size();
		signature.resize(offset + bytes);
		if (bytes) {
			memcpy(signature.ptrw() + offset, data, bytes);
		}
	};
	append(&logical, sizeof(logical));
    for(size_t i=0;i<view.effect_count;++i) {
        auto effect=view.effects[i];
        // Sampling root/order describe compositing, not coverage appearance.
        // Root/order changes preserve coverage; document-input membership
        // changes which effects this embedding mask actually contains.
        effect.sampling_root_id=0;effect.before_draw_index=0;effect.flags&=1;
        append(&effect,sizeof(effect));
    }
	append(view.operations, view.operation_count * sizeof(hcsr_backdrop_operation_t));
	append(view.vertices, view.vertex_count * sizeof(hcsr_backdrop_vertex_t));
    for(size_t i=0;i<view.surface_count;++i) {
        auto surface=view.surfaces[i];
        // Pinned addresses are transport lifetime, never appearance validity.
        surface.appearance.raster.pixels=nullptr;
        append(&surface,sizeof(surface));
        const auto entry=renderer.backdrop_entry(i);
        append(&entry.page,sizeof(entry.page));append(&entry.rect,sizeof(entry.rect));
    }
	if (local) {
		// Only actual mask dependencies affect validity. Ordinary scene motion
		// must not redraw an unrelated stationary filter mask.
		HashSet<uint32_t> states, clips;
		for (size_t i = 0; i < view.surface_count; ++i) {
			const uint32_t state = view.surfaces[i].gpu_state;
			if (states.has(state)) {
				continue;
			}
			states.insert(state);
			append(&packet_view.gpu.states[state], sizeof(hcsr_gpu_state_t));
			uint32_t clip = packet_view.gpu.states[state].clip;
			while (clip && !clips.has(clip)) {
				clips.insert(clip);
				const auto &record = packet_view.gpu.clips[clip];
				append(&record, sizeof(record));
				append(packet_view.gpu.planes + record.first_plane, record.plane_count * sizeof(hcsr_gpu_plane_t));
				clip = record.parent;
			}
		}
	}
	if (signature != previous || physical != previous_size) {
		if (local) {
			if (!draw_gpu(view, packet_view, logical, physical, renderer)) {
				frame.clear();
				previous.clear();
				return frame;
			}
			previous = signature;
			previous_size = physical;
		} else {
			previous = signature;
			previous_size = physical;
			frame.clear();
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
					filter.type = HTMLBackdropFilterOperationType(operation.kind);
					filter.amount = operation.amount;
					effect.filter_operations.push_back(filter);
					if (operation.kind == 0) {
						effect.blur_radius_css_px = MAX(effect.blur_radius_css_px, operation.amount);
					}
				}
				for (uint32_t vertex = 0; vertex + 2 < source.vertex_count; vertex += 3) {
					Vector2 p[3];
					float coverage[3];
					for (int j = 0; j < 3; j++) {
						const auto &v = view.vertices[source.first_vertex + vertex + j];
						Vector2 logical_point(v.x, v.y);
						if (first) {
							effect.bounds.position = logical_point;
							first = false;
						} else {
							effect.bounds.expand_to(logical_point);
						}
						p[j] = logical_point * scale;
						coverage[j] = v.coverage;
					}
					float area = (p[1] - p[0]).cross(p[2] - p[0]);
					if (Math::abs(area) < 0.00001f) {
						continue;
					}
					int left = MAX(0, int(Math::floor(MIN(p[0].x, MIN(p[1].x, p[2].x)))));
					int top = MAX(0, int(Math::floor(MIN(p[0].y, MIN(p[1].y, p[2].y)))));
					int right = MIN(physical.x, int(Math::ceil(MAX(p[0].x, MAX(p[1].x, p[2].x)))));
					int bottom = MIN(physical.y, int(Math::ceil(MAX(p[0].y, MAX(p[1].y, p[2].y)))));
					for (int y = top; y < bottom; y++) {
						for (int x = left; x < right; x++) {
							Vector2 point(x + .5f, y + .5f);
							float a = (p[1] - point).cross(p[2] - point) / area;
							float b = (p[2] - point).cross(p[0] - point) / area;
							float c = 1 - a - b;
							if (a < -0.00001f || b < -0.00001f || c < -0.00001f) {
								continue;
							}
							float amount = CLAMP(a * coverage[0] + b * coverage[1] + c * coverage[2], 0.0f, 1.0f);
							if (amount <= 0) {
								continue;
							}
							Color old = image->get_pixel(x, y);
							if (int(Math::round(old.r * 255)) == int(effect.id)) {
								amount = MAX(amount, old.g);
							}
							image->set_pixel(x, y, Color(effect.id / 255.0f, amount, 0, 1));
						}
					}
				}
				frame.effects.push_back(effect);
			}
			frame.mask_texture = ImageTexture::create_from_image(image);
			frame.logical_size = logical;
			frame.physical_size = physical;
			frame.device_scale_factor = scale.x;
			frame.mask_encoding = HTML_GPU_BACKDROP_MASK_ENCODING_RGBA8_ID_COVERAGE;
			frame.max_effect_id = view.effect_count;
		}
	}
	frame.frame_generation = frame.main_target_generation = frame.backdrop_mask_generation = view.generation;
	for (auto &effect : frame.effects) {
		effect.generation = view.generation;
	}
	return frame;
}


HCSRNewestBackdrop::~HCSRNewestBackdrop() {
	release_gpu();
}

void HCSRNewestBackdrop::release_gpu() {
	auto server = RenderingServer::get_singleton();
	auto device = server ? server->get_rendering_device() : nullptr;
	if (gpu_texture.is_valid()) {
		gpu_texture->set_texture_rd_rid(RID());
	}
    if(device && target.is_valid())device->free_rid(target);
    target=RID();

}

bool HCSRNewestBackdrop::draw_gpu(const hcsr_backdrop_view_t &view, const hcsr_draw_packet_view_t &packet, const Size2i &logical, const Size2i &physical, HCSRNewestSceneRenderer &renderer) {
	using RD = RenderingDevice;
	auto server = RenderingServer::get_singleton();
	auto device = server ? server->get_rendering_device() : nullptr;
	if (!device || !packet.gpu.state_count || !packet.gpu.clip_count) {
		return false;
	}
	if (!target.is_valid() || physical != previous_size) {
		if (gpu_texture.is_valid()) {
			gpu_texture->set_texture_rd_rid(RID());
		}
		if (target.is_valid()) {
			device->free_rid(target);
		}
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		format.width = physical.x;
		format.height = physical.y;
		format.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		target = device->texture_create(format, RD::TextureView());
		if (!target.is_valid()) {
			return false;
		}
		if (gpu_texture.is_null()) {
			gpu_texture.instantiate();
		}
		gpu_texture->_set_texture_rd_rid(target);
	}
    if(!renderer.draw_backdrop_mask(device,target,view,logical,physical))return false;
    surface_instances=view.surface_count;
	frame.clear();
	for (size_t i = 0; i < view.effect_count; ++i) {
		const auto &source = view.effects[i];
		HTMLGPUBackdropEffect effect;
		effect.id = i + 1;
		for (uint32_t op = 0; op < source.operation_count; ++op) {
			const auto &operation = view.operations[source.first_operation + op];
			HTMLBackdropFilterOperation filter;
			filter.type = HTMLBackdropFilterOperationType(operation.kind);
			filter.amount = operation.amount;
			effect.filter_operations.push_back(filter);
			if (operation.kind == 0) {
				effect.blur_radius_css_px = MAX(effect.blur_radius_css_px, operation.amount);
			}
		}
		bool first = true;
        for(uint32_t j=0;j<source.surface_count;++j) {
            const auto &surface=view.surfaces[source.first_surface+j];
            const auto &r=surface.appearance.raster.local_rect;
            const auto &m=packet.gpu.states[surface.gpu_state].transform;
            const Vector2 corners[]={{r.x,r.y},{r.x+r.width,r.y},{r.x+r.width,r.y+r.height},{r.x,r.y+r.height}};
            for(const auto &local:corners) {
                float w=m[3]*local.x+m[7]*local.y+m[15];
                if(Math::abs(w)<.000001f)w=1;
                Vector2 point((m[0]*local.x+m[4]*local.y+m[12])/w,(m[1]*local.x+m[5]*local.y+m[13])/w);
                if(first) {effect.bounds.position=point;first=false;}else effect.bounds.expand_to(point);
            }
        }
		effect.bounds = effect.bounds.intersection(Rect2(Vector2(), logical));
		frame.effects.push_back(effect);
	}
	++redraws;
	frame.mask_texture = gpu_texture;
	frame.logical_size = logical;
	frame.physical_size = physical;
	frame.device_scale_factor = float(physical.x) / logical.x;
	frame.mask_encoding = HTML_GPU_BACKDROP_MASK_ENCODING_RGBA8_ID_COVERAGE;
	frame.max_effect_id = view.effect_count;
	return true;
}

Dictionary HCSRNewestBackdrop::get_statistics() const {
	Dictionary result;
	result["backdrop_surface_instances"] = surface_instances;
	result["backdrop_gpu_redraws"] = int64_t(redraws);
	return result;
}

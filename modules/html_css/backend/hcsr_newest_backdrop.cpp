#include "hcsr_newest_backdrop.h"

#include "core/templates/hash_set.h"
#include "scene/resources/image_texture.h"
#include "servers/rendering/rendering_server.h"

const HTMLGPUBackdropFrame &HCSRNewestBackdrop::update(hcsr_draw_packet_t packet, const Size2i &logical, const Size2i &physical) {
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
	append(view.effects, view.effect_count * sizeof(hcsr_backdrop_effect_t));
	append(view.operations, view.operation_count * sizeof(hcsr_backdrop_operation_t));
	append(view.vertices, view.vertex_count * sizeof(hcsr_backdrop_vertex_t));
	if (local) {
		// Only actual mask dependencies affect validity. Ordinary scene motion
		// must not redraw an unrelated stationary filter mask.
		HashSet<uint32_t> states, clips;
		for (size_t i = 0; i < view.vertex_count; ++i) {
			const uint32_t state = view.vertices[i].gpu_state;
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
			if (!draw_gpu(view, packet_view, logical, physical)) {
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
#include "hcsr_shader_sources.h"

HCSRNewestBackdrop::~HCSRNewestBackdrop() {
	release_gpu();
}

void HCSRNewestBackdrop::release_gpu() {
	auto server = RenderingServer::get_singleton();
	auto device = server ? server->get_rendering_device() : nullptr;
	if (gpu_texture.is_valid()) {
		gpu_texture->set_texture_rd_rid(RID());
	}
	if (device) {
		for (RID rid : { uniform, pipeline, shader, target }) {
			if (rid.is_valid()) {
				device->free_rid(rid);
			}
		}
		for (RID rid : buffers) {
			if (rid.is_valid()) {
				device->free_rid(rid);
			}
		}
	}
	uniform = pipeline = shader = target = RID();
	for (int i = 0; i < 4; ++i) {
		buffers[i] = RID();
		capacities[i] = 0;
	}
	geometry_signature.clear();
}

bool HCSRNewestBackdrop::draw_gpu(const hcsr_backdrop_view_t &view, const hcsr_draw_packet_view_t &packet, const Size2i &logical, const Size2i &physical) {
	using RD = RenderingDevice;
	auto server = RenderingServer::get_singleton();
	auto device = server ? server->get_rendering_device() : nullptr;
	if (!device || !packet.gpu.state_count || !packet.gpu.clip_count) {
		return false;
	}
	if (!shader.is_valid()) {
		const char *sources[] = { hcsr::shaders::godot_backdrop_vertex, hcsr::shaders::godot_backdrop_fragment };
		Vector<RD::ShaderStageSPIRVData> stages;
		for (int i = 0; i < 2; ++i) {
			RD::ShaderStageSPIRVData stage;
			stage.shader_stage = i == 0 ? RD::SHADER_STAGE_VERTEX : RD::SHADER_STAGE_FRAGMENT;
			String error;
			stage.spirv = device->shader_compile_spirv_from_source(stage.shader_stage, sources[i], RD::SHADER_LANGUAGE_GLSL, &error);
			if (stage.spirv.is_empty()) {
				ERR_PRINT(error);
				return false;
			}
			stages.push_back(stage);
		}
		shader = device->shader_create_from_spirv(stages, "HCSR backdrop local coverage");
		if (!shader.is_valid()) {
			return false;
		}
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
	Vector<uint8_t> geometry;
	geometry.resize(view.vertex_count * sizeof(hcsr_backdrop_vertex_t));
	if (!geometry.is_empty()) {
		memcpy(geometry.ptrw(), view.vertices, geometry.size());
	}
	bool geometry_changed = geometry != geometry_signature;
	const void *data[] = { view.vertices, packet.gpu.states, packet.gpu.clips, packet.gpu.planes };
	const uint32_t sizes[] = { uint32_t(geometry.size()), uint32_t(packet.gpu.state_count * sizeof(hcsr_gpu_state_t)), uint32_t(packet.gpu.clip_count * sizeof(hcsr_gpu_clip_t)), uint32_t(packet.gpu.plane_count * sizeof(hcsr_gpu_plane_t)) };
	for (int i = 0; i < 4; ++i) {
		uint32_t bytes = MAX(sizes[i], 16u);
		if (!buffers[i].is_valid() || capacities[i] < bytes) {
			if (uniform.is_valid()) {
				device->free_rid(uniform);
			}
			uniform = RID();
			if (buffers[i].is_valid()) {
				device->free_rid(buffers[i]);
			}
			capacities[i] = MAX(bytes, 4096u);
			buffers[i] = device->storage_buffer_create(capacities[i]);
			if (i == 0) {
				geometry_changed = true;
			}
		}
		if (!buffers[i].is_valid()) {
			return false;
		}
		if (sizes[i] && (i != 0 || geometry_changed) && device->buffer_update(buffers[i], 0, sizes[i], static_cast<const uint8_t *>(data[i])) != OK) {
			return false;
		}
	}
	if (geometry_changed) {
		geometry_uploaded_bytes += sizes[0];
	}
	geometry_signature = geometry;
	if (!uniform.is_valid()) {
		Vector<RD::Uniform> uniforms;
		for (int i = 0; i < 4; ++i) {
			RD::Uniform entry;
			entry.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			entry.binding = i;
			entry.append_id(buffers[i]);
			uniforms.push_back(entry);
		}
		uniform = device->uniform_set_create(VectorView(uniforms.ptr(), uniforms.size()), shader, 0);
		if (!uniform.is_valid()) {
			return false;
		}
	}
	Vector<RID> attachments;
	attachments.push_back(target);
	RID framebuffer = device->framebuffer_create(attachments);
	if (!framebuffer.is_valid()) {
		return false;
	}
	if (!pipeline.is_valid()) {
		RD::PipelineColorBlendState blend;
		blend.attachments.push_back(RD::PipelineColorBlendState::Attachment());
		pipeline = device->render_pipeline_create(shader, device->framebuffer_get_format(framebuffer), RD::INVALID_ID, RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), blend);
	}
	if (!pipeline.is_valid()) {
		device->free_rid(framebuffer);
		return false;
	}
	const Color clear(0, 0, 0, 1);
	auto list = device->draw_list_begin(framebuffer, RD::DRAW_CLEAR_COLOR_ALL, VectorView(&clear, 1));
	device->draw_list_bind_render_pipeline(list, pipeline);
	device->draw_list_bind_uniform_set(list, uniform, 0);
	frame.clear();
	for (size_t i = 0; i < view.effect_count; ++i) {
		const auto &source = view.effects[i];
		const struct {
			uint32_t first, id;
			float width, height, target_width, target_height, padding[2];
		} push{ source.first_vertex, uint32_t(i + 1), float(logical.x), float(logical.y), float(physical.x), float(physical.y), {} };
		device->draw_list_set_push_constant(list, &push, sizeof(push));
		if (source.vertex_count) {
			device->draw_list_draw(list, false, 1, source.vertex_count);
		}
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
		for (uint32_t j = 0; j < source.vertex_count; ++j) {
			const auto &vertex = view.vertices[source.first_vertex + j];
			const auto &m = packet.gpu.states[vertex.gpu_state].transform;
			float w = m[3] * vertex.x + m[7] * vertex.y + m[15];
			Vector2 point((m[0] * vertex.x + m[4] * vertex.y + m[12]) / w, (m[1] * vertex.x + m[5] * vertex.y + m[13]) / w);
			if (first) {
				effect.bounds.position = point;
				first = false;
			} else {
				effect.bounds.expand_to(point);
			}
		}
		effect.bounds = effect.bounds.intersection(Rect2(Vector2(), logical));
		frame.effects.push_back(effect);
	}
	device->draw_list_end();
	device->free_rid(framebuffer);
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
	result["backdrop_geometry_uploaded_bytes"] = int64_t(geometry_uploaded_bytes);
	result["backdrop_gpu_redraws"] = int64_t(redraws);
	return result;
}

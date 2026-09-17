#include "hcsr_gpu_packet.h"
#include "hcsr_prepared_drawing.h"
#include "hcsr_shader_sources.h"
#include "hcsr_compositing.h"
#include "hcsr_newest_scene_renderer.h"
#include "hcsr_image_codec.h"
#include "core/os/os.h"
#include <cmath>

bool HCSRNewestSceneRenderer::prepare(const hcsr_draw_packet_view_t &packet, const Ref<HTMLDocument> &document, float output_scale) {
    const bool had_pending = resources.has_pending_glyphs();
    const bool next_gpu = packet.format == HCSR_DRAW_PACKET_FORMAT_GPU;
    if (!hcsr::render::validate_gpu_packet(packet)) return false;
    auto copy_table = [](auto &destination, const auto *source, size_t count) {
        if(destination.resize(int(count))!=OK) return false;
        if (count) memcpy(destination.ptrw(),source,count*sizeof(*source));
        return true;
    };
    if(next_gpu) {
        if(!copy_table(gpu_states,packet.gpu.states,packet.gpu.state_count)
            || !copy_table(gpu_clips,packet.gpu.clips,packet.gpu.clip_count)
            || !copy_table(gpu_planes,packet.gpu.planes,packet.gpu.plane_count)) return false;
    } else { gpu_states.clear(); gpu_clips.clear(); gpu_planes.clear(); }
    logical_width=packet.viewport_width; logical_height=packet.viewport_height;
    if(next_gpu && gpu_geometry && geometry_generation==packet.gpu.geometry_generation && prepared_scale==output_scale
        && !had_pending) {
        uploaded=false; geometry_dirty=false;
        update_visible_instances(packet);
        return true;
    }
    const Vector<Vertex> previous_vertices = vertices;
    const Vector<PreparedMesh> previous_meshes = prepared_meshes;
    prepared_meshes.clear();
    if (next_gpu && prepared_meshes.resize(int(packet.draw_item_count)) != OK) return false;
    gpu_geometry=next_gpu; geometry_generation=next_gpu ? packet.gpu.geometry_generation : 0;
    geometry_dirty=true; prepared_scale=output_scale; primitives.clear();
    resources.advance_rasterization();
	vertices.clear();
	batches.clear();
	hcsr::render::compositing_plan group_plan;
    std::string group_error;
    if (!hcsr::render::plan_compositing(packet, group_plan, group_error)) return false;
    HashSet<uint64_t> live_surfaces;
    for (size_t i=0; i<packet.draw_item_count; ++i) {
        const auto &material=packet.materials[packet.draw_items[i].material_index];
        if (material.kind!=HCSR_MATERIAL_RASTER && material.kind!=HCSR_MATERIAL_RASTER_SLICE) continue;
        const size_t expected = material.kind==HCSR_MATERIAL_RASTER_SLICE ? sizeof(hcsr_raster_slice_material_t) : sizeof(hcsr_raster_material_t);
        if (material.payload_size!=expected || material.payload_offset>packet.material_payload_size
            || material.payload_size>packet.material_payload_size-material.payload_offset) return false;
        hcsr_raster_material_t raster;
        memcpy(&raster,packet.material_payload+material.payload_offset,sizeof(raster));
        live_surfaces.insert(raster.identity);
    }
    resources.retain_surfaces(live_surfaces);
    group_depth = group_plan.depth;
	uploaded = false;
    // Draws may reuse index ranges, so size by emitted indices rather than the
    // packet index buffer. Acquire the writable pointer once; per-vertex push_back
    // repeats CowData resize/write checks across the entire mesh on every update.
    uint64_t emitted_count = 0;
    for (size_t i = 0; i < packet.draw_item_count; i++) {
        emitted_count += packet.draw_items[i].index_count;
        if (group_plan.events[i].kind == HCSR_GROUP_END) emitted_count += 6;
        if (emitted_count > INT32_MAX/sizeof(Vertex)) return false;
    }
    if (vertices.resize(int(emitted_count)) != OK) return false;
    Vertex *vertex_data = vertices.ptrw();
    uint32_t written = 0;
	for (size_t i = 0; i < packet.draw_item_count; i++) {
		const hcsr_draw_item_t &draw = packet.draw_items[i];
		const hcsr_material_t &material = packet.materials[draw.material_index];
        const auto group = group_plan.events[i];
        if (group.kind) {
            batches.push_back({0,gpu_geometry ? uint32_t(primitives.size()) : written,group.kind==HCSR_GROUP_END ? (gpu_geometry ? 1u : 6u) : 0u,group.kind,group.depth,group.opacity,
                Rect2(group.bounds.x/packet.viewport_width,group.bounds.y/packet.viewport_height,group.bounds.width/packet.viewport_width,group.bounds.height/packet.viewport_height)});
            if (group.kind==HCSR_GROUP_END) {
                if(gpu_geometry) { primitives.push_back({written,2,UINT32_MAX}); }
                const Vector2 corners[] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
                for (const auto &corner: corners) {
                    Vertex v={}; v.position_uv[0]=corner.x*2-1; v.position_uv[1]=corner.y*2-1;
                    v.position_uv[2]=corner.x; v.position_uv[3]=corner.y;
                    v.tint[3]=group.opacity; v.bounds[0]=-3; vertex_data[written++]=v;
                }
            }
            continue;
        }
		if (material.payload_size < sizeof(hcsr_area_grayscale_material_t) || material.payload_offset > packet.material_payload_size || material.payload_size > packet.material_payload_size - material.payload_offset) {
			return false;
		}
		hcsr_area_grayscale_material_t area;
		memcpy(&area, packet.material_payload + material.payload_offset, sizeof(area));
		Entry entry;
		Rect2 destination;
        if (material.kind == HCSR_MATERIAL_RASTER || material.kind == HCSR_MATERIAL_RASTER_SLICE) {
            const bool sliced = material.kind == HCSR_MATERIAL_RASTER_SLICE;
            if (material.payload_size != (sliced ? sizeof(hcsr_raster_slice_material_t) : sizeof(hcsr_raster_material_t))) return false;
            hcsr_raster_material_t raster;
            memcpy(&raster, packet.material_payload + material.payload_offset, sizeof(raster));
            destination = Rect2(raster.local_rect.x, raster.local_rect.y, raster.local_rect.width, raster.local_rect.height);
            float transform_scale = 1;
            if (gpu_geometry && draw.index_count) {
                const auto &m = packet.gpu.states[packet.gpu.vertex_states[packet.indices[draw.first_index]]].transform;
                transform_scale = MAX(Vector2(m[0],m[1]).length(),Vector2(m[4],m[5]).length());
            } else if (draw.index_count) {
                const auto &a = packet.vertices[packet.indices[draw.first_index]];
                for (uint32_t j=1; j<draw.index_count; ++j) {
                    const auto &b = packet.vertices[packet.indices[draw.first_index+j]];
                    const float local = Vector2(b.local_x-a.local_x,b.local_y-a.local_y).length();
                    if (local > .001f) transform_scale = MAX(transform_scale,Vector2(b.screen_x-a.screen_x,b.screen_y-a.screen_y).length()/local);
                }
            }
            // Nine-slice cuts share one pixel grid. Retain the authored raster
            // level instead of choosing a different mip from each patch's size.
            entry = resources.resolve_raster(raster,sliced ? Vector2(raster.width,raster.height) : destination.size * (output_scale * transform_scale));
            if (entry.page < 0) return false;
            if (sliced) {
                hcsr_raster_slice_material_t slice;
                memcpy(&slice, packet.material_payload + material.payload_offset, sizeof(slice));
                const auto &r = slice.source_rect;
                if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.width) || !std::isfinite(r.height)
                    || r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0
                    || r.x+r.width > raster.width || r.y+r.height > raster.height) return false;
                if (r.x != std::floor(r.x) || r.y != std::floor(r.y)
                    || r.width != std::floor(r.width) || r.height != std::floor(r.height)
                    || entry.rect.size != Size2i(raster.width,raster.height)) return false;
                entry.rect = Rect2i(entry.rect.position + Point2i(r.x,r.y), Size2i(r.width,r.height));
            }
        }
		if (material.kind == HCSR_MATERIAL_IMAGE && material.payload_size >= sizeof(hcsr_image_material_t)) {
			hcsr_image_material_t image;
			memcpy(&image, packet.material_payload + material.payload_offset, sizeof(image));
			if (image.source_length <= material.payload_size - sizeof(image)) {
				const String source = String::utf8((const char *)(packet.material_payload + material.payload_offset + sizeof(image)), image.source_length);
				const Size2i natural = resources.resolve_size(document, source);
				destination = Rect2(image.local_rect.x, image.local_rect.y, image.local_rect.width, image.local_rect.height);
				if (natural.x > 0 && natural.y > 0 && image.object_fit != 0) {
                    hcsr_rect_t fitted;
                    if (hcsr_image_fit_rect(&image.local_rect, natural.x, natural.y,
                                image.object_fit, image.position_x, image.position_y, &fitted) == HCSR_OK) {
                        destination = Rect2(fitted.x, fitted.y, fitted.width, fitted.height);
                    }
				}
                float transform_scale = 1;
                for (uint32_t j = 1; j < draw.index_count; j++) {
                    const auto &a = packet.vertices[packet.indices[draw.first_index]];
                    const auto &b = packet.vertices[packet.indices[draw.first_index + j]];
                    const float local = Vector2(b.local_x - a.local_x, b.local_y - a.local_y).length();
                    if (local > .001f) transform_scale = MAX(transform_scale, Vector2(b.screen_x - a.screen_x, b.screen_y - a.screen_y).length() / local);
                }
                if(gpu_geometry && draw.index_count) {
                    const auto &m=packet.gpu.states[packet.gpu.vertex_states[packet.indices[draw.first_index]]].transform;
                    transform_scale=MAX(Vector2(m[0],m[1]).length(),Vector2(m[4],m[5]).length());
                }
                entry = resources.resolve_image(document, source, natural, destination.size * (output_scale * transform_scale));
			}
		}
        hcsr_glyph_material_t glyph = {};
        const bool is_glyph = material.kind == HCSR_MATERIAL_GLYPH && material.payload_size >= sizeof(glyph);
        if (is_glyph) {
            memcpy(&glyph, packet.material_payload + material.payload_offset, sizeof(glyph));
            float transform_scale = 1;
            for (uint32_t j = 1; j < draw.index_count; j++) {
                const auto &a = packet.vertices[packet.indices[draw.first_index]];
                const auto &b = packet.vertices[packet.indices[draw.first_index + j]];
                float local = Vector2(b.local_x - a.local_x, b.local_y - a.local_y).length();
                if (local > .001f) transform_scale = MAX(transform_scale, Vector2(b.screen_x - a.screen_x, b.screen_y - a.screen_y).length() / local);
            }
            if(gpu_geometry && draw.index_count) {
                const auto &m=packet.gpu.states[packet.gpu.vertex_states[packet.indices[draw.first_index]]].transform;
                transform_scale=MAX(Vector2(m[0],m[1]).length(),Vector2(m[4],m[5]).length());
            }
            entry = resources.resolve_glyph(glyph, output_scale * transform_scale);
            if (entry.page >= 0) {
                float factor = glyph.font_size / entry.raster_size;
                destination = Rect2(Vector2(glyph.baseline_x, glyph.baseline_y) + entry.glyph_offset * factor, entry.glyph_size * factor);
            }
        }
		// Solid grayscale draws do not sample the atlas and can stay in the current batch.
        const int page = entry.page >= 0 ? entry.page : (batches.is_empty() ? 0 : batches[batches.size() - 1].page);
		if (batches.is_empty() || batches[batches.size() - 1].page != page || batches[batches.size() - 1].kind) {
			batches.push_back({ page, gpu_geometry ? uint32_t(primitives.size()) : written, 0 });
		}
		const bool quad = hcsr::render::scene_quad(packet,draw);
        batches.write[batches.size() - 1].count += gpu_geometry ? (quad ? 1 : (draw.index_count+5)/6) : draw.index_count;
        if(gpu_geometry) {
            hcsr::render::append_scene_primitives(written,draw.index_count,quad,uint32_t(i),
                [&](Primitive primitive) { primitives.push_back(primitive); });
        }
        hcsr_atlas_glyph_material_t resolved = {};
        const bool textured = entry.page >= 0 && destination.size.x > 0 && destination.size.y > 0;
        if (textured) {
            resolved.local_rect = { destination.position.x, destination.position.y, destination.size.x, destination.size.y };
            resolved.atlas_rect = { float(entry.rect.position.x), float(entry.rect.position.y), float(entry.rect.size.x), float(entry.rect.size.y) };
            resolved.red = is_glyph && !entry.color_glyph ? glyph.red : 1;
            resolved.green = is_glyph && !entry.color_glyph ? glyph.green : 1;
            resolved.blue = is_glyph && !entry.color_glyph ? glyph.blue : 1;
            resolved.alpha = area.opacity * (is_glyph ? glyph.alpha : 1);
        }
        // Direct-color meshes have no atlas-dependent preparation. Compare their
        // explicit immutable inputs and bulk-copy unchanged encoded vertices.
        if (gpu_geometry && material.kind == HCSR_MATERIAL_VERTEX_COLOR && draw.index_count && !quad) {
            const uint32_t first = packet.indices[draw.first_index];
            bool contiguous = draw.index_count <= packet.vertex_count - first;
            for (uint32_t j = 0; contiguous && j < draw.index_count; ++j)
                contiguous = packet.indices[draw.first_index+j] == first+j;
            if (contiguous) {
                prepared_meshes.write[i] = {first, written, draw.index_count};
                if (i < size_t(previous_meshes.size())) {
                    const auto &old = previous_meshes[i];
                    if (old.count == draw.index_count
                        && old.source_first + old.count <= uint32_t(prepared_source.size())
                        && old.source_first + old.count <= uint32_t(prepared_states.size())
                        && old.prepared_first + old.count <= uint32_t(previous_vertices.size())
                        && memcmp(prepared_source.ptr()+old.source_first, packet.vertices+first, old.count*sizeof(hcsr_paint_vertex_t)) == 0
                        && memcmp(prepared_states.ptr()+old.source_first, packet.gpu.vertex_states+first, old.count*sizeof(uint32_t)) == 0) {
                        memcpy(vertex_data+written, previous_vertices.ptr()+old.prepared_first, old.count*sizeof(Vertex));
                        written += old.count;
                        continue;
                    }
                }
            }
        }
        for (uint32_t j = 0; j < (quad ? 4u : draw.index_count); j++) {
            vertex_data[written++] = hcsr::render::prepare_scene_vertex(packet,draw,j,area,
                material.kind == HCSR_MATERIAL_VERTEX_COLOR,textured ? &resolved : nullptr);
        }
	}
    vertices.resize(written);
    if (gpu_geometry) {
        if (!copy_table(prepared_source, packet.vertices, packet.vertex_count)
            || !copy_table(prepared_states, packet.gpu.vertex_states, packet.vertex_count)) return false;
    } else { prepared_source.clear(); prepared_states.clear(); }
    // Empty and solid-only scenes use the same submission path. A bound
    // placeholder satisfies the shader interface without inventing a second
    // renderer for clears or for missing image resources.
    resources.ensure_sampling_page();
    if(gpu_geometry) { all_primitives=primitives; all_batches=batches; update_visible_instances(packet); }
	return true;
}

void HCSRNewestSceneRenderer::update_visible_instances(const hcsr_draw_packet_view_t &packet) {
    // Coarse CPU visibility only. Precise element clipping stays in the shader.
    hcsr::render::compositing_plan plan; std::string error;
    if(group_depth && !hcsr::render::plan_compositing(packet,plan,error)) return;
    primitives.clear(); batches.clear();
    size_t event=0;
    for(const auto &original:all_batches) {
        Batch batch=original; batch.first=primitives.size(); batch.count=0;
        if(batch.kind) {
            while(event<plan.events.size() && !plan.events[event].kind) ++event;
            if(event<plan.events.size()) {
                const auto &b=plan.events[event++].bounds;
                batch.bounds=Rect2(b.x/logical_width,b.y/logical_height,b.width/logical_width,b.height/logical_height);
            }
        }
        for(uint32_t j=0;j<original.count;j++) {
            const auto &primitive=all_primitives[original.first+j];
            if(primitive.draw_index!=UINT32_MAX) {
                if (!hcsr::render::scene_draw_visible(packet,primitive.draw_index)) continue;
            }
            primitives.push_back(primitive); ++batch.count;
        }
        if(batch.kind || batch.count) batches.push_back(batch);
    }
}

bool HCSRNewestSceneRenderer::upload(RenderingDevice *device) {
	using RD = RenderingDevice;
	if (!shader.is_valid()) {
        const char *sources[] = { hcsr::shaders::godot_vertex, hcsr::shaders::godot_fragment };
		Vector<RD::ShaderStageSPIRVData> stages;
		for (int i = 0; i < 2; i++) {
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
		shader = device->shader_create_from_spirv(stages, "HCSR grayscale and image atlas");
		RD::SamplerState sampling;
		sampling.min_filter = sampling.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler = device->sampler_create(sampling);
		if (!shader.is_valid() || !sampler.is_valid()) {
			return false;
		}
	}
    auto ensure_buffer = [&](RID &rid,uint32_t &capacity,uint32_t bytes) {
        bytes=MAX(bytes,16u);
        if(bytes<=capacity) return;
        for(GpuPage &page:gpu_pages) { if(page.uniform.is_valid()) device->free_rid(page.uniform); page.uniform=RID(); }
        release_groups(device);
        if(rid.is_valid()) device->free_rid(rid);
        capacity=MAX(bytes,65536u); rid=device->storage_buffer_create(capacity);
        if (&rid == &buffer) { geometry_dirty=true; uploaded_vertices.clear(); }
    };
    const uint32_t bytes=vertices.size()*sizeof(Vertex);
    ensure_buffer(buffer,buffer_capacity,bytes);
    ensure_buffer(state_buffer,state_capacity,gpu_states.size()*sizeof(hcsr_gpu_state_t));
    ensure_buffer(clip_buffer,clip_capacity,gpu_clips.size()*sizeof(hcsr_gpu_clip_t));
    ensure_buffer(plane_buffer,plane_capacity,gpu_planes.size()*sizeof(hcsr_gpu_plane_t));
    ensure_buffer(primitive_buffer,primitive_capacity,primitives.size()*sizeof(Primitive));
    auto update = [&](RID rid,uint32_t count,const void *data,bool geometry) {
        if(!count) return true;
        if(device->buffer_update(rid,0,count,data)!=OK) return false;
        if(geometry) geometry_uploaded_bytes+=count; else state_uploaded_bytes+=count;
        return true;
    };
    if (geometry_dirty) {
        // Local drawing changes usually touch a few boxes. Keep unchanged GPU
        // storage, including offscreen geometry, instead of uploading the scene.
        if (gpu_geometry && uploaded_vertices.size() == vertices.size()) {
            constexpr uint32_t block = 1024 * sizeof(Vertex);
            const auto *next = reinterpret_cast<const uint8_t *>(vertices.ptr());
            const auto *previous = reinterpret_cast<const uint8_t *>(uploaded_vertices.ptr());
            for (uint32_t offset = 0; offset < bytes; offset += block) {
                const uint32_t count = MIN(block, bytes - offset);
                if (memcmp(next + offset, previous + offset, count) == 0) continue;
                if (device->buffer_update(buffer, offset, count, next + offset) != OK) return false;
                geometry_uploaded_bytes += count;
            }
        } else if (!update(buffer, bytes, vertices.ptr(), true)) return false;
        uploaded_vertices = vertices;
    }
    const uint32_t instance_bytes=primitives.size()*sizeof(Primitive);
    if(instance_bytes && device->buffer_update(primitive_buffer,0,instance_bytes,primitives.ptr())!=OK) return false;
    instance_uploaded_bytes+=instance_bytes;
    if(!update(state_buffer,gpu_states.size()*sizeof(hcsr_gpu_state_t),gpu_states.ptr(),false)
        || !update(clip_buffer,gpu_clips.size()*sizeof(hcsr_gpu_clip_t),gpu_clips.ptr(),false)
        || !update(plane_buffer,gpu_planes.size()*sizeof(hcsr_gpu_plane_t),gpu_planes.ptr(),false)) return false;
    geometry_dirty=false;
    auto rgba_upload = [&](int page, const Rect2i &rect) {
        Vector<uint8_t> pixels = resources.page_pixels(page, rect);
        for (int i = 0; i < pixels.size(); i += 4) SWAP(pixels.write[i], pixels.write[i + 2]);
        return pixels;
    };
    if (gpu_pages.resize(resources.page_count()) != OK) return false;
    for (int index = 0; index < gpu_pages.size(); ++index) {
        GpuPage &page = gpu_pages.write[index];
        const Size2i size = resources.page_size(index);
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		format.width = size.x;
		format.height = size.y;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		if (!page.texture.is_valid()) {
			Vector<Vector<uint8_t>> data;
			data.push_back(rgba_upload(index,Rect2i(Point2i(),size)));
			page.texture = device->texture_create(format, RD::TextureView(), data);
			uploaded_bytes += uint64_t(format.width) * format.height * 4;
		} else {
			// Upload only new allocations. Existing atlas coordinates never move.
			for (const Rect2i &rect : resources.page_updates(index)) {
				format.width = rect.size.x;
				format.height = rect.size.y;
				format.usage_bits = RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
				Vector<Vector<uint8_t>> data;
				data.push_back(rgba_upload(index,rect));
				RID patch = device->texture_create(format, RD::TextureView(), data);
				if (!patch.is_valid()) {
					return false;
				}
				Error error = device->texture_copy(patch, page.texture, Vector3(), Vector3(rect.position.x, rect.position.y, 0), Vector3(rect.size.x, rect.size.y, 1), 0, 0, 0, 0);
				device->free_rid(patch);
				if (error != OK) {
					return false;
				}
				uploaded_bytes += uint64_t(rect.size.x) * rect.size.y * 4;
			}
		}
		resources.acknowledge_upload(index);
		if (!page.texture.is_valid()) {
			return false;
		}
		if (!page.uniform.is_valid()) {
			Vector<RD::Uniform> uniforms;
			RD::Uniform texture;
			texture.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
			texture.binding = 0;
			texture.append_id(sampler);
			texture.append_id(page.texture);
			uniforms.push_back(texture);
			RD::Uniform storage;
			storage.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			storage.binding = 1;
			storage.append_id(buffer);
			uniforms.push_back(storage);
            const RID extra[]={state_buffer,clip_buffer,plane_buffer,primitive_buffer};
            for(int i=0;i<4;i++) { RD::Uniform u; u.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER; u.binding=2+i; u.append_id(extra[i]); uniforms.push_back(u); }
			page.uniform = device->uniform_set_create(VectorView(uniforms.ptr(), uniforms.size()), shader, 0);
			if (!page.uniform.is_valid()) {
				return false;
			}
		}
	}
	return true;
}

bool HCSRNewestSceneRenderer::draw(RenderingDevice *device, RID target, const Color &background) {
	using RD = RenderingDevice;
    // Optional timestamps: no profiling queries in normal application runs.
    const bool profile_gpu=OS::get_singleton()->get_environment("HCSR_GPU_PROFILE")=="1";
    const String label="HCSR/"+String::num_uint64(target.get_id());
    if(profile_gpu) {
        uint64_t start=0;
        for(uint32_t i=0;i<device->get_captured_timestamps_count();i++) {
            const String name=device->get_captured_timestamp_name(i);
            if(name==label+"/begin") start=device->get_captured_timestamp_gpu_time(i);
            if(name==label+"/end" && start) { last_gpu_ms=double(device->get_captured_timestamp_gpu_time(i)-start)/1000000.0; last_gpu_frame=device->get_captured_timestamps_frame(); }
        }
        device->capture_timestamp(label+"/begin");
    }
    struct TimestampEnd { RenderingDevice *device; String label; bool active;
        ~TimestampEnd() { if(active) device->capture_timestamp(label+"/end"); }
    } timestamp_end{device,label,profile_gpu};
	if (!uploaded && !upload(device)) {
		return false;
	}
	uploaded = true;
	Vector<RID> attachments;
	attachments.push_back(target);
	RID framebuffer = device->framebuffer_create(attachments);
	if (!framebuffer.is_valid()) {
		return false;
	}
	if (!pipeline.is_valid()) {
		auto blend = RD::PipelineColorBlendState::create_blend();
		blend.attachments.write[0].src_alpha_blend_factor = RD::BLEND_FACTOR_ONE;
		pipeline = device->render_pipeline_create(shader, device->framebuffer_get_format(framebuffer), RD::INVALID_ID,
				RD::RENDER_PRIMITIVE_TRIANGLES, RD::PipelineRasterizationState(), RD::PipelineMultisampleState(), RD::PipelineDepthStencilState(), blend);
	}
	if (!pipeline.is_valid()) {
		device->free_rid(framebuffer);
		return false;
	}
    const auto format = device->texture_get_format(target);
    const Size2i size(format.width,format.height);
    int pool_index=-1;
    for(int i=group_pools.size()-1;i>=0;--i) {
        if(!device->texture_is_valid(group_pools[i].output)) {
            for(const auto &group:group_pools[i].targets)
                for(RID rid:{group.uniform,group.framebuffer,group.texture}) if(rid.is_valid()) device->free_rid(rid);
            group_pools.remove_at(i);
        }
    }
    for(int i=0;i<group_pools.size();++i) if(group_pools[i].output==target) pool_index=i;
    if(pool_index<0) { pool_index=group_pools.size(); GroupPool pool; pool.output=target; group_pools.push_back(pool); }
    auto &group_targets=group_pools.write[pool_index].targets;
    while (group_targets.size()<int(group_depth)) {
        GroupTarget group;
        auto layer_format=format;
        layer_format.usage_bits=RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
        group.texture=device->texture_create(layer_format,RD::TextureView()); group.size=size;
        if (!group.texture.is_valid()) { device->free_rid(framebuffer); return false; }
        Vector<RID> layers; layers.push_back(group.texture);
        group.framebuffer=device->framebuffer_create(layers);
        Vector<RD::Uniform> uniforms;
        RD::Uniform image; image.uniform_type=RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE; image.binding=0;
        image.append_id(sampler); image.append_id(group.texture); uniforms.push_back(image);
        RD::Uniform storage; storage.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER; storage.binding=1;
        storage.append_id(buffer); uniforms.push_back(storage);
        const RID extra[]={state_buffer,clip_buffer,plane_buffer,primitive_buffer};
        for(int i=0;i<4;i++) { RD::Uniform u; u.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER; u.binding=2+i; u.append_id(extra[i]); uniforms.push_back(u); }
        group.uniform=device->uniform_set_create(VectorView(uniforms.ptr(),uniforms.size()),shader,0);
        group_targets.push_back(group);
        ++group_allocations;
        if (!group.framebuffer.is_valid() || !group.uniform.is_valid()) { release_groups(device); device->free_rid(framebuffer); return false; }
    }
    auto region_for = [&](const Batch &batch) {
        const auto area=hcsr::render::group_region({batch.bounds.position.x,batch.bounds.position.y,batch.bounds.size.x,batch.bounds.size.y},1,1,size.x,size.y);
        return Rect2(area.x,area.y,area.width,area.height);
    };
    auto emit = [&](RD::DrawListID list, const Batch &batch) {
        if(batch.kind==HCSR_GROUP_END) device->draw_list_enable_scissor(list,region_for(batch));
        else device->draw_list_disable_scissor(list);
        device->draw_list_bind_uniform_set(list,batch.kind==HCSR_GROUP_END ? group_targets[batch.depth-1].uniform : gpu_pages[batch.page].uniform,0);
        const struct { uint32_t first,gpu_geometry; float width,height,target_width,target_height; } push{batch.first,
            gpu_geometry ? 1u : 0u,logical_width,logical_height,float(size.x),float(size.y)};
        device->draw_list_set_push_constant(list,&push,sizeof(push));
        device->draw_list_draw(list,false,gpu_geometry ? batch.count : 1,gpu_geometry ? 6 : batch.count);
        ++last_draw_calls;
    };
    std::vector<std::vector<size_t>> passes;
    // Process-wide diagnostic override for paired profiling and pixel checks.
    static const bool sequential_groups = OS::get_singleton()->get_environment("HCSR_SEQUENTIAL_OPACITY_GROUPS") == "1";
    last_disjoint_groups=!sequential_groups && group_depth && hcsr::render::schedule_disjoint_groups(batches.size(),[&](size_t i) {
        const auto &b=batches[i];
        return hcsr::render::group_event{b.kind,b.depth,b.opacity,{b.bounds.position.x,b.bounds.position.y,b.bounds.size.x,b.bounds.size.y}};
    },group_depth,1,1,size.x,size.y,passes);
    last_render_passes=0; last_draw_calls=0;
    if(last_disjoint_groups) {
        for(int depth=int(group_depth);depth>=0;--depth) {
            const Color clear=depth ? Color(0,0,0,0) : background;
            Rect2 region;
            bool first=true;
            if(depth) for(const auto &batch:batches) if(batch.kind==HCSR_GROUP_BEGIN && batch.depth==uint32_t(depth)) {
                const Rect2 next=region_for(batch);
                region=first ? next : region.merge(next); first=false;
            }
            auto list=device->draw_list_begin(depth ? group_targets[depth-1].framebuffer : framebuffer,
                RD::DRAW_CLEAR_COLOR_ALL,VectorView(&clear,1),1,0,region);
            device->draw_list_set_viewport(list,Rect2i(Vector2i(),size));
            device->draw_list_bind_render_pipeline(list,pipeline);
            for(size_t i:passes[depth]) emit(list,batches[i]);
            device->draw_list_end();
            ++last_render_passes;
        }
        device->free_rid(framebuffer);
        return true;
    }
	RD::DrawListID list = device->draw_list_begin(framebuffer, RD::DRAW_CLEAR_COLOR_ALL, VectorView(&background, 1));
    ++last_render_passes;
	device->draw_list_bind_render_pipeline(list, pipeline);
	for (const Batch &batch : batches) {
        if (batch.kind) {
            device->draw_list_end();
            const Color transparent(0,0,0,0);
            const Rect2 region=region_for(batch);
            RID destination = batch.kind==HCSR_GROUP_BEGIN ? group_targets[batch.depth-1].framebuffer
                    : batch.depth==1 ? framebuffer : group_targets[batch.depth-2].framebuffer;
            list=device->draw_list_begin(destination,batch.kind==HCSR_GROUP_BEGIN ? RD::DRAW_CLEAR_COLOR_ALL : 0,VectorView(&transparent,1),1,0,
                batch.kind==HCSR_GROUP_BEGIN ? region : Rect2());
            ++last_render_passes;
            device->draw_list_set_viewport(list,Rect2i(Vector2i(),size));
            device->draw_list_bind_render_pipeline(list,pipeline);
            if (batch.kind==HCSR_GROUP_BEGIN) continue;
        }
        emit(list,batch);
	}
	device->draw_list_end();
	device->free_rid(framebuffer);
	return true;
}

void HCSRNewestSceneRenderer::release_groups(RenderingDevice *device) {
    if (device) for(const auto &pool:group_pools) for (const auto &group : pool.targets)
        for (RID rid : {group.uniform,group.framebuffer,group.texture}) if (rid.is_valid()) device->free_rid(rid);
    group_pools.clear();
}

void HCSRNewestSceneRenderer::release(RenderingDevice *device) {
    release_groups(device);
	if (device) {
		for (const GpuPage &page : gpu_pages) {
			if (page.uniform.is_valid()) {
				device->free_rid(page.uniform);
			}
			if (page.texture.is_valid()) {
				device->free_rid(page.texture);
			}
		}
		for (RID rid : { pipeline, buffer, sampler, shader, state_buffer,clip_buffer,plane_buffer,primitive_buffer }) {
			if (rid.is_valid()) {
				device->free_rid(rid);
			}
		}
	}
	gpu_pages.clear();
	vertices.clear();
	batches.clear();
	pipeline = buffer = sampler = shader = RID();
	buffer_capacity = 0;
    state_buffer=clip_buffer=plane_buffer=primitive_buffer=RID();
    state_capacity=clip_capacity=plane_capacity=primitive_capacity=0;
    geometry_generation=0; gpu_geometry=false; primitives.clear();
    uploaded_vertices.clear();
    prepared_meshes.clear(); prepared_source.clear(); prepared_states.clear();
}

// Compile the same paint program used by the GPU adapters for the CPU path.
namespace hcsr_cpu_paint {
static Vector4 read_atlas(const Ref<Image> &atlas, const Vector2i &p) {
    const Color c = atlas->get_pixel(p.x, p.y);
    return Vector4(c.r, c.g, c.b, c.a);
}
#define HCSR_F2 Vector2
#define HCSR_F4 Vector4
#define HCSR_I2 Vector2i
#define HCSR_INLINE inline
#define HCSR_CONTEXT const Ref<Image> &atlas,
#define HCSR_ARGS atlas,
#define HCSR_CLAMP CLAMP
#define HCSR_MIN MIN
#define HCSR_MAX MAX
#define HCSR_FLOOR(p) (p).floor()
#define HCSR_MIX(a,b,t) (a).lerp((b),(t))
#define HCSR_FETCH(p) read_atlas(atlas,p)
#define HCSR_GROUP_FETCH(uv,tint) read_atlas(atlas,Vector2i((uv)*Vector2(atlas->get_size())))
#define HCSR_DISCARD return Vector4()
#include "hcsr_paint_shader.inc"
#undef HCSR_F2
#undef HCSR_F4
#undef HCSR_I2
#undef HCSR_INLINE
#undef HCSR_CONTEXT
#undef HCSR_ARGS
#undef HCSR_CLAMP
#undef HCSR_MIN
#undef HCSR_MAX
#undef HCSR_FLOOR
#undef HCSR_MIX
#undef HCSR_FETCH
#undef HCSR_GROUP_FETCH
#undef HCSR_DISCARD
}

void HCSRNewestSceneRenderer::draw_cpu(Ref<Image> target, const Color &background) {
	target->fill(background);
	const Vector2 size = target->get_size();
    Vector<Ref<Image>> parents;
    Vector<Ref<Image>> reference_pages; reference_pages.resize(resources.page_count());
	for (const Batch &batch : batches) {
        if (batch.kind==HCSR_GROUP_BEGIN) {
            parents.push_back(target);
            target=Image::create_empty(int(size.x),int(size.y),false,Image::FORMAT_RGBA8);
            target->fill(Color(0,0,0,0));
            continue;
        }
        if (batch.kind==HCSR_GROUP_END) {
            Ref<Image> parent=parents[parents.size()-1]; parents.resize(parents.size()-1);
            for (int y=0;y<int(size.y);++y) for(int x=0;x<int(size.x);++x) {
                Color color=target->get_pixel(x,y)*batch.opacity;
                Color under=parent->get_pixel(x,y);
                parent->set_pixel(x,y,color+under*(1-color.a));
            }
            target=parent;
            continue;
        }
        Ref<Image> &atlas_image = reference_pages.write[batch.page];
        if (atlas_image.is_null()) {
            // Temporary reference pixels, shared by all batches on this page.
            const Size2i atlas_size = resources.page_size(batch.page);
            Vector<uint8_t> atlas_pixels = resources.page_pixels(batch.page, Rect2i(Point2i(), atlas_size));
            for (int offset = 0; offset < atlas_pixels.size(); offset += 4) {
                SWAP(atlas_pixels.write[offset], atlas_pixels.write[offset + 2]);
            }
            atlas_image = Image::create_from_data(atlas_size.x, atlas_size.y, false, Image::FORMAT_RGBA8, atlas_pixels);
        }
		for (uint32_t i = batch.first; i < batch.first + batch.count; i += 3) {
			const Vertex &a = vertices[i], &b = vertices[i + 1], &c = vertices[i + 2];
			auto position = [&](const Vertex &v) { return (Vector2(v.position_uv[0], v.position_uv[1]) + Vector2(1, 1)) * .5f * size; };
			const Vector2 p = position(a), q = position(b), r = position(c);
            // Quantize once to a common subpixel grid. Exact edge equations make
            // adjacent triangles agree on coverage, including reversed winding.
            struct Point { int64_t x,y; };
            auto fixed=[](Vector2 v) { return Point{int64_t(Math::round(v.x*256)),int64_t(Math::round(v.y*256))}; };
            const Point fp=fixed(p),fq=fixed(q),fr=fixed(r);
            auto edge=[](Point a,Point b,Point c) { return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x); };
            const int64_t denominator=edge(fp,fq,fr);
            if(!denominator) continue;
            const int64_t sign=denominator>0 ? 1 : -1;
            const double inverse=1.0/double(denominator*sign);
            const Vector2 ab=q-p, ac=r-p;
            const Vector2 uv_ab(b.position_uv[2]-a.position_uv[2],b.position_uv[3]-a.position_uv[3]);
            const Vector2 uv_ac(c.position_uv[2]-a.position_uv[2],c.position_uv[3]-a.position_uv[3]);
            const float uv_area=ab.cross(ac);
            const Vector2 footprint=((uv_ab*ac.y-uv_ac*ab.y)/uv_area).abs()+((uv_ac*ab.x-uv_ab*ac.x)/uv_area).abs();
            auto owns_edge=[&](Point a,Point b) {
                const int64_t dx=(b.x-a.x)*sign,dy=(b.y-a.y)*sign;
                return dy<0 || (dy==0 && dx>0);
            };
            const bool owns_a=owns_edge(fq,fr),owns_b=owns_edge(fr,fp),owns_c=owns_edge(fp,fq);
			const int left = MAX(0, (int)Math::floor(MIN(p.x, MIN(q.x, r.x))));
			const int top = MAX(0, (int)Math::floor(MIN(p.y, MIN(q.y, r.y))));
			const int right = MIN(target->get_width(), (int)Math::ceil(MAX(p.x, MAX(q.x, r.x))));
			const int bottom = MIN(target->get_height(), (int)Math::ceil(MAX(p.y, MAX(q.y, r.y))));
			for (int y = top; y < bottom; y++) {
				for (int x = left; x < right; x++) {
                    const Point sample{int64_t(x)*256+128,int64_t(y)*256+128};
                    const int64_t ea=edge(fq,fr,sample)*sign;
                    const int64_t eb=edge(fr,fp,sample)*sign;
                    const int64_t ec=edge(fp,fq,sample)*sign;
                    if(ea<0 || eb<0 || ec<0 || (!ea && !owns_a) || (!eb && !owns_b) || (!ec && !owns_c)) continue;
                    const float wa=float(ea*inverse),wb=float(eb*inverse),wc=float(ec*inverse);
                    const Vector4 tint = Vector4(a.tint[0],a.tint[1],a.tint[2],a.tint[3])*wa
                            + Vector4(b.tint[0],b.tint[1],b.tint[2],b.tint[3])*wb
                            + Vector4(c.tint[0],c.tint[1],c.tint[2],c.tint[3])*wc;
                    const Vector2 uv(a.position_uv[2]*wa+b.position_uv[2]*wb+c.position_uv[2]*wc,
                            a.position_uv[3]*wa+b.position_uv[3]*wb+c.position_uv[3]*wc);
                    const Vector4 shaded = hcsr_cpu_paint::hcsr_shade(atlas_image, uv, tint,
                            Vector4(a.bounds[0],a.bounds[1],a.bounds[2],a.bounds[3]),footprint);
                    const Color color(shaded.x,shaded.y,shaded.z,shaded.w);
					const Color under = target->get_pixel(x, y);
					target->set_pixel(x, y, Color(color.r * color.a + under.r * (1 - color.a), color.g * color.a + under.g * (1 - color.a), color.b * color.a + under.b * (1 - color.a), color.a + under.a * (1 - color.a)));
				}
			}
		}
	}
}

Dictionary HCSRNewestSceneRenderer::get_statistics() const {
	Dictionary result = resources.get_statistics();
    result["vertices"] = vertices.size();
    result["gpu_geometry"] = gpu_geometry;
    result["instances"] = primitives.size();
    result["draw_calls"] = last_draw_calls;
    result["gpu_ms"] = last_gpu_ms;
    result["gpu_timestamp_frame"] = last_gpu_frame;
    result["geometry_uploaded_bytes"] = geometry_uploaded_bytes;
    result["instance_uploaded_bytes"] = instance_uploaded_bytes;
    result["state_uploaded_bytes"] = state_uploaded_bytes;
    result["geometry_generation"] = geometry_generation;
    result["color_patch_updates"] = 0; // Compatibility diagnostic; appearances replace color-patch updates.
	result["uploaded_bytes"] = uploaded_bytes;
	result["draw_batches"] = batches.size();
    result["opacity_group_depth"] = group_depth;
    int groups=0; for(const auto &batch:batches) if(batch.kind==HCSR_GROUP_BEGIN) ++groups;
    result["opacity_groups"] = groups;
    uint64_t bytes=0; for(const auto &pool:group_pools) for(const auto &group:pool.targets) bytes+=uint64_t(group.size.x)*group.size.y*4;
    result["opacity_target_bytes"] = bytes;
    result["opacity_target_allocations"] = group_allocations;
    result["render_passes"] = last_render_passes;
    result["disjoint_opacity_groups"] = last_disjoint_groups;
	return result;
}

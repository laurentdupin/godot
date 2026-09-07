#include "hcsr_newest_image_atlas.h"

#include "../bridge/html_asset_provider.h"

#include "core/crypto/crypto_core.h"

#include <cmath>
#include "servers/text/text_server.h"

HCSRNewestImageAtlas::Entry HCSRNewestImageAtlas::resolve(const Ref<HTMLDocument> &document, const String &source) {
	const String key = (document.is_valid() ? document->get_html_file() + "|" + document->get_resource_root() : String()) + "\n" + source;
	if (const Entry *existing = entries.getptr(key)) {
		return *existing;
	}
	Entry entry;
	Vector<uint8_t> bytes;
	String mime;
	if (source.begins_with("data:")) {
		const int comma = source.find(",");
		if (comma >= 0) {
			mime = source.substr(5, comma - 5).to_lower();
			const String data = source.substr(comma + 1);
			if (mime.ends_with(";base64")) {
				const CharString encoded = data.ascii();
				bytes.resize(encoded.length());
				size_t length = 0;
				if (CryptoCore::b64_decode(bytes.ptrw(), bytes.size(), &length,
							(const uint8_t *)encoded.get_data(), encoded.length()) == OK) {
					bytes.resize(length);
				} else {
					bytes.clear();
				}
			} else {
				const CharString decoded = data.uri_decode().utf8();
				bytes.resize(decoded.length());
				if (!bytes.is_empty()) {
					memcpy(bytes.ptrw(), decoded.get_data(), bytes.size());
				}
			}
		}
	} else {
		HTMLAssetResource asset;
		if (HTMLGodotAssetProvider::load_asset(document, source, asset) == OK) {
			bytes = asset.bytes;
			mime = asset.mime_type;
		}
	}
	Ref<Image> image;
	image.instantiate();
	Error error = ERR_FILE_UNRECOGNIZED;
	if (!bytes.is_empty()) {
		if (mime.begins_with("image/svg+xml")) {
			error = image->load_svg_from_buffer(bytes);
		} else if (mime.begins_with("image/png")) {
			error = image->load_png_from_buffer(bytes);
		} else if (mime.begins_with("image/jpeg")) {
			error = image->load_jpg_from_buffer(bytes);
		} else if (mime.begins_with("image/webp")) {
			error = image->load_webp_from_buffer(bytes);
		} else if (mime.begins_with("image/bmp")) {
			error = image->load_bmp_from_buffer(bytes);
		}
	}
	if (error == OK && !image->is_empty()) {
		decoded_images++;
		entry.natural_size = image->get_size();
		image->convert(Image::FORMAT_RGBA8); // Straight alpha; native BGRA codec contract is unchanged.
		const float scale = MIN(1.0f, float(PAGE_SIZE - 2) / MAX(image->get_width(), image->get_height()));
		if (scale < 1) {
			image->resize(MAX(1, int(image->get_width() * scale)), MAX(1, int(image->get_height() * scale)));
		}
		const int width = image->get_width() + 2, height = image->get_height() + 2;
		for (int i = 0; i <= pages.size() && i < MAX_PAGES; i++) {
			if (i == pages.size()) {
                int image_pages = 0; for (const Page &existing : pages) if (!existing.glyphs) image_pages++;
                if (image_pages >= 4) break;
				Page page;
				page.pixels = Image::create_empty(PAGE_SIZE, PAGE_SIZE, false, Image::FORMAT_RGBA8);
				pages.push_back(page);
			}
			Page &page = pages.write[i];
            if (page.glyphs) continue;
			int x = page.x, y = page.y, row = page.row_height;
			if (x + width > PAGE_SIZE) {
				x = 0;
				y += row;
				row = 0;
			}
			if (y + height > PAGE_SIZE) {
				continue;
			}
			entry.page = i;
			entry.rect = Rect2i(x + 1, y + 1, width - 2, height - 2);
			page.pixels->blit_rect(image, Rect2i(Point2i(), image->get_size()), entry.rect.position);
			// Extrude one texel on every side so linear sampling never bleeds adjacent entries.
			for (int px = -1; px <= image->get_width(); px++) {
				page.pixels->set_pixel(x + px + 1, y, image->get_pixel(CLAMP(px, 0, image->get_width() - 1), 0));
				page.pixels->set_pixel(x + px + 1, y + height - 1, image->get_pixel(CLAMP(px, 0, image->get_width() - 1), image->get_height() - 1));
			}
			for (int py = 0; py < image->get_height(); py++) {
				page.pixels->set_pixel(x, y + py + 1, image->get_pixel(0, py));
				page.pixels->set_pixel(x + width - 1, y + py + 1, image->get_pixel(image->get_width() - 1, py));
			}
			page.dirty.push_back(Rect2i(x, y, width, height));
			page.x = x + width;
			page.y = y;
			page.row_height = MAX(row, height);
			break;
		}
	}
	if (entry.page < 0) {
		WARN_PRINT("hcsr_newest image unavailable or atlas capacity exceeded: " + source.left(100));
	}
	entries.insert(key, entry); // Cache failures too; never retry I/O every animation frame.
	return entry;
}

HCSRNewestImageAtlas::Entry HCSRNewestImageAtlas::resolve_glyph(const hcsr_glyph_material_t &glyph, float scale) {
	uint32_t size_bits;
    memcpy(&size_bits, &glyph.font_size, sizeof(size_bits));
    const GlyphKey identity{ glyph.face, glyph.glyph, size_bits };
	const float required = CLAMP(glyph.font_size * scale, 1.0f, 768.0f);
	const int *existing_level = glyph_levels.getptr(identity);
    int level = existing_level ? *existing_level : 0;
	// Keep 20% headroom and hysteresis: hover scaling never follows fractional raster sizes.
	if (level == 0 || required > level || required < level * .45f) {
		level = 12;
		while (level < required * 1.2f && level < 768) level = (level * 3 + 1) / 2;
		level = MIN(level, 768);
		glyph_levels.insert(identity, level);
	}
	const GlyphKey key{ glyph.face, glyph.glyph, uint32_t(level) };
	const GlyphKey face_glyph{ glyph.face, glyph.glyph, 0 };
    if (const Entry *existing = glyph_entries.getptr(key)) {
        if (existing->page >= 0) return *existing;
        // A full atlas must not replace a valid lower-resolution glyph with a missing entry.
        if (const Entry *fallback = last_glyphs.getptr(face_glyph)) return *fallback;
        return *existing;
    }
    if (const Entry *fallback = last_glyphs.getptr(face_glyph)) {
        if (!queued_glyphs.has(key)) { pending_glyphs.push_back({ glyph, level, key }); queued_glyphs.insert(key, true); }
        return *fallback;
    }
    return rasterize_glyph(glyph, level);
}

HCSRNewestImageAtlas::Entry HCSRNewestImageAtlas::rasterize_glyph(const hcsr_glyph_material_t &glyph, int level) {
    const GlyphKey key{ glyph.face, glyph.glyph, uint32_t(level) };
    const GlyphKey face_glyph{ glyph.face, glyph.glyph, 0 };
	Entry entry;
	entry.raster_size = level;
	TextServer *ts = TextServerManager::get_singleton()->get_primary_interface().ptr();
	const RID face = RID::from_uint64(glyph.face);
	const Vector2i size(level, 0);
	ts->font_render_glyph(face, size, glyph.glyph);
	const int texture = ts->font_get_glyph_texture_idx(face, size, glyph.glyph);
	if (texture < 0) { glyph_entries.insert(key, entry); return entry; }
	Ref<Image> source = ts->font_get_texture_image(face, size, texture);
	const Rect2i uv = ts->font_get_glyph_uv_rect(face, size, glyph.glyph);
	if (source.is_null() || !uv.has_area()) { glyph_entries.insert(key, entry); return entry; }
	entry.color_glyph = source->get_format() == Image::FORMAT_RGBA8;
	Ref<Image> image = source->get_region(uv);
	image->convert(Image::FORMAT_RGBA8);
	entry.glyph_offset = ts->font_get_glyph_offset(face, size, glyph.glyph);
    entry.glyph_size = ts->font_get_glyph_size(face, size, glyph.glyph);
	entry.natural_size = image->get_size();
	const int width = image->get_width() + 2, height = image->get_height() + 2;
	for (int i = 0; i <= pages.size() && i < MAX_PAGES; i++) {
		if (i == pages.size()) {
            int glyph_pages = 0; for (const Page &existing : pages) if (existing.glyphs) glyph_pages++;
            if (glyph_pages >= 4) break;
			Page page;
			page.glyphs = true;
			page.pixels = Image::create_empty(PAGE_SIZE, PAGE_SIZE, false, Image::FORMAT_RGBA8);
			pages.push_back(page);
		}
		Page &page = pages.write[i];
		if (!page.glyphs) continue;
		int x = page.x, y = page.y, row = page.row_height;
		if (x + width > PAGE_SIZE) { x = 0; y += row; row = 0; }
		if (y + height > PAGE_SIZE || width > PAGE_SIZE) continue;
		entry.page = i;
		entry.rect = Rect2i(x + 1, y + 1, width - 2, height - 2);
		page.pixels->blit_rect(image, Rect2i(Point2i(), image->get_size()), entry.rect.position);
		// Glyph padding stays transparent, unlike the extruded borders of image entries.
		page.dirty.push_back(Rect2i(x, y, width, height));
		page.x = x + width; page.y = y; page.row_height = MAX(row, height);
		rasterized_glyphs++;
		break;
	}
	if (entry.page < 0) WARN_PRINT("HCSR glyph atlas capacity exceeded");
	glyph_entries.insert(key, entry);
    if (entry.page >= 0) last_glyphs.insert(face_glyph, entry);
	return entry;
}

bool HCSRNewestImageAtlas::prepare(const hcsr_draw_packet_view_t &packet, const Ref<HTMLDocument> &document, float output_scale) {
    // Upgrade previously seen glyphs incrementally; the old level remains visible meanwhile.
    for (int i = 0; i < 32 && !pending_glyphs.is_empty(); i++) {
        PendingGlyph pending = pending_glyphs[pending_glyphs.size() - 1];
        pending_glyphs.resize(pending_glyphs.size() - 1);
        queued_glyphs.erase(pending.key);
        rasterize_glyph(pending.glyph, pending.level);
    }
	vertices.clear();
	batches.clear();
	uploaded = false;
	bool has_images = false;
	bool references_images = false;
	for (size_t i = 0; i < packet.material_count; i++) {
		references_images |= (packet.materials[i].kind == HCSR_MATERIAL_IMAGE || packet.materials[i].kind == HCSR_MATERIAL_GLYPH);
	}
	if (!references_images) {
		return false;
	}
	for (size_t i = 0; i < packet.draw_item_count; i++) {
		const hcsr_draw_item_t &draw = packet.draw_items[i];
		const hcsr_material_t &material = packet.materials[draw.material_index];
		if (material.payload_size < sizeof(hcsr_area_grayscale_material_t) || material.payload_offset > packet.material_payload_size || material.payload_size > packet.material_payload_size - material.payload_offset) {
			return false;
		}
		hcsr_area_grayscale_material_t area;
		memcpy(&area, packet.material_payload + material.payload_offset, sizeof(area));
		Entry entry;
		Rect2 destination;
		if (material.kind == HCSR_MATERIAL_IMAGE && material.payload_size >= sizeof(hcsr_image_material_t)) {
			hcsr_image_material_t image;
			memcpy(&image, packet.material_payload + material.payload_offset, sizeof(image));
			if (image.source_length <= material.payload_size - sizeof(image)) {
				const String source = String::utf8((const char *)(packet.material_payload + material.payload_offset + sizeof(image)), image.source_length);
				entry = resolve(document, source);
				destination = Rect2(image.local_rect.x, image.local_rect.y, image.local_rect.width, image.local_rect.height);
				if (entry.page >= 0 && image.object_fit != 0) {
                    hcsr_rect_t fitted;
                    if (hcsr_image_fit_rect(&image.local_rect, entry.natural_size.x, entry.natural_size.y,
                                image.object_fit, image.position_x, image.position_y, &fitted) == HCSR_OK) {
                        destination = Rect2(fitted.x, fitted.y, fitted.width, fitted.height);
                    }
				}
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
            entry = resolve_glyph(glyph, output_scale * transform_scale);
            if (entry.page >= 0) {
                float factor = glyph.font_size / entry.raster_size;
                destination = Rect2(Vector2(glyph.baseline_x, glyph.baseline_y) + entry.glyph_offset * factor, entry.glyph_size * factor);
            }
        }
		has_images |= entry.page >= 0;
		// Solid grayscale draws do not sample the atlas and can stay in the current batch.
        const int page = entry.page >= 0 ? entry.page : (batches.is_empty() ? 0 : batches[batches.size() - 1].page);
		if (batches.is_empty() || batches[batches.size() - 1].page != page) {
			batches.push_back({ page, (uint32_t)vertices.size(), 0 });
		}
		batches.write[batches.size() - 1].count += draw.index_count;
		for (uint32_t j = 0; j < draw.index_count; j++) {
			const auto &v = packet.vertices[packet.indices[draw.first_index + j]];
			Vertex vertex = {};
			vertex.position_uv[0] = v.screen_x / packet.viewport_width * 2 - 1;
			vertex.position_uv[1] = v.screen_y / packet.viewport_height * 2 - 1;
			vertex.tint[0] = vertex.tint[1] = vertex.tint[2] = area.luminance;
			vertex.tint[3] = area.opacity;
			if (entry.page >= 0 && destination.size.x > 0 && destination.size.y > 0) {
				const Vector2 uv = (Vector2(v.local_x, v.local_y) - destination.position) / destination.size;
				vertex.position_uv[2] = (entry.rect.position.x + uv.x * entry.rect.size.x) / PAGE_SIZE;
				vertex.position_uv[3] = (entry.rect.position.y + uv.y * entry.rect.size.y) / PAGE_SIZE;
				vertex.tint[0] = is_glyph && !entry.color_glyph ? -2 - glyph.red : -1;
                if (is_glyph) { vertex.tint[1] = glyph.green; vertex.tint[2] = glyph.blue; vertex.tint[3] *= glyph.alpha; }
				vertex.bounds[0] = float(entry.rect.position.x) / PAGE_SIZE;
				vertex.bounds[1] = float(entry.rect.position.y) / PAGE_SIZE;
				vertex.bounds[2] = float(entry.rect.get_end().x) / PAGE_SIZE;
				vertex.bounds[3] = float(entry.rect.get_end().y) / PAGE_SIZE;
			}
			vertices.push_back(vertex);
		}
	}
	return has_images;
}

bool HCSRNewestImageAtlas::upload(RenderingDevice *device) {
	using RD = RenderingDevice;
	if (!shader.is_valid()) {
		const char *sources[] = {
			R"(#version 450
layout(set=0,binding=1,std430) readonly buffer Vertices { vec4 data[]; };
layout(push_constant,std430) uniform Params { uint first; uint p1; uint p2; uint p3; } params;
layout(location=0) out vec2 uv;
layout(location=1) out vec4 tint;
layout(location=2) out vec4 bounds;
void main() {
    uint i = (uint(gl_VertexIndex) + params.first) * 3u;
    vec4 vertex = data[i];
    gl_Position = vec4(vertex.xy, 0.0, 1.0);
    uv = vertex.zw; tint = data[i+1u]; bounds = data[i+2u];
})",
			R"(#version 450
layout(set=0,binding=0) uniform sampler2D atlas;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 tint;
layout(location=2) in vec4 bounds;
layout(location=0) out vec4 color;
void main() {
    if (tint.r < 0.0) {
        if (any(lessThan(uv,bounds.xy)) || any(greaterThan(uv,bounds.zw))) discard;
        color = texture(atlas, uv); color.a *= tint.a;
        if (tint.r <= -2.0) color.rgb *= vec3(-tint.r - 2.0, tint.g, tint.b);
    } else color = tint;
})"
		};
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
	const uint32_t bytes = vertices.size() * sizeof(Vertex);
	if (bytes > buffer_capacity) {
		for (Page &page : pages) {
			if (page.uniform.is_valid()) {
				device->free_rid(page.uniform);
			}
			page.uniform = RID();
		}
		if (buffer.is_valid()) {
			device->free_rid(buffer);
		}
		buffer_capacity = MAX(bytes, 65536u);
		buffer = device->storage_buffer_create(buffer_capacity);
	}
	if (!buffer.is_valid() || device->buffer_update(buffer, 0, bytes, vertices.ptr()) != OK) {
		return false;
	}
	for (Page &page : pages) {
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		format.width = PAGE_SIZE;
		format.height = PAGE_SIZE;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		if (!page.texture.is_valid()) {
			Vector<Vector<uint8_t>> data;
			data.push_back(page.pixels->get_data());
			page.texture = device->texture_create(format, RD::TextureView(), data);
			uploaded_bytes += uint64_t(PAGE_SIZE) * PAGE_SIZE * 4;
		} else {
			// Upload only new allocations. Existing atlas coordinates never move.
			for (const Rect2i &rect : page.dirty) {
				format.width = rect.size.x;
				format.height = rect.size.y;
				format.usage_bits = RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
				Vector<Vector<uint8_t>> data;
				data.push_back(page.pixels->get_region(rect)->get_data());
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
		page.dirty.clear();
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
			page.uniform = device->uniform_set_create(VectorView(uniforms.ptr(), uniforms.size()), shader, 0);
			if (!page.uniform.is_valid()) {
				return false;
			}
		}
	}
	return true;
}

bool HCSRNewestImageAtlas::draw(RenderingDevice *device, RID target, const Color &background) {
	using RD = RenderingDevice;
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
	const RD::DrawListID list = device->draw_list_begin(framebuffer, RD::DRAW_CLEAR_COLOR_ALL, VectorView(&background, 1));
	device->draw_list_bind_render_pipeline(list, pipeline);
	for (const Batch &batch : batches) {
		device->draw_list_bind_uniform_set(list, pages[batch.page].uniform, 0);
		const uint32_t push[] = { batch.first, 0, 0, 0 };
		device->draw_list_set_push_constant(list, push, sizeof(push));
		device->draw_list_draw(list, false, 1, batch.count);
	}
	device->draw_list_end();
	device->free_rid(framebuffer);
	return true;
}

void HCSRNewestImageAtlas::release(RenderingDevice *device) {
	if (device) {
		for (const Page &page : pages) {
			if (page.uniform.is_valid()) {
				device->free_rid(page.uniform);
			}
			if (page.texture.is_valid()) {
				device->free_rid(page.texture);
			}
		}
		for (RID rid : { pipeline, buffer, sampler, shader }) {
			if (rid.is_valid()) {
				device->free_rid(rid);
			}
		}
	}
	pages.clear();
	entries.clear();
    glyph_levels.clear();
    pending_glyphs.clear(); last_glyphs.clear(); queued_glyphs.clear(); glyph_entries.clear();
	vertices.clear();
	batches.clear();
	pipeline = buffer = sampler = shader = RID();
	buffer_capacity = 0;
}

void HCSRNewestImageAtlas::draw_cpu(Ref<Image> target, const Color &background) {
	target->fill(background);
	const Vector2 size = target->get_size();
	for (const Batch &batch : batches) {
		for (uint32_t i = batch.first; i < batch.first + batch.count; i += 3) {
			const Vertex &a = vertices[i], &b = vertices[i + 1], &c = vertices[i + 2];
			auto position = [&](const Vertex &v) { return (Vector2(v.position_uv[0], v.position_uv[1]) + Vector2(1, 1)) * .5f * size; };
			const Vector2 p = position(a), q = position(b), r = position(c);
			const float denominator = (q - p).cross(r - p);
			if (Math::is_zero_approx(denominator)) {
				continue;
			}
			const int left = MAX(0, (int)Math::floor(MIN(p.x, MIN(q.x, r.x))));
			const int top = MAX(0, (int)Math::floor(MIN(p.y, MIN(q.y, r.y))));
			const int right = MIN(target->get_width(), (int)Math::ceil(MAX(p.x, MAX(q.x, r.x))));
			const int bottom = MIN(target->get_height(), (int)Math::ceil(MAX(p.y, MAX(q.y, r.y))));
			for (int y = top; y < bottom; y++) {
				for (int x = left; x < right; x++) {
					const Vector2 sample(x + .5f, y + .5f);
					const float wb = (sample - p).cross(r - p) / denominator;
					const float wc = (q - p).cross(sample - p) / denominator;
					const float wa = 1 - wb - wc;
					if (wa < 0 || wb < 0 || wc < 0) {
						continue;
					}
					// The top-left rule assigns shared edges to exactly one triangle.
					auto owns_edge = [&](Vector2 from, Vector2 to) {
						Vector2 edge = denominator > 0 ? to - from : from - to;
						return edge.y < 0 || (edge.y == 0 && edge.x > 0);
					};
					if ((wa == 0 && !owns_edge(q, r)) || (wb == 0 && !owns_edge(r, p)) || (wc == 0 && !owns_edge(p, q))) {
						continue;
					}
					Color color(a.tint[0], a.tint[1], a.tint[2], a.tint[3]);
					if (a.tint[0] < 0) {
						const float u = a.position_uv[2] * wa + b.position_uv[2] * wb + c.position_uv[2] * wc;
						const float v = a.position_uv[3] * wa + b.position_uv[3] * wb + c.position_uv[3] * wc;
						if (u < a.bounds[0] || v < a.bounds[1] || u > a.bounds[2] || v > a.bounds[3]) {
							continue;
						}
						color = pages[batch.page].pixels->get_pixel(CLAMP(int(u * PAGE_SIZE), 0, PAGE_SIZE - 1), CLAMP(int(v * PAGE_SIZE), 0, PAGE_SIZE - 1));
						color.a *= a.tint[3];
                        if (a.tint[0] <= -2) { color.r *= -a.tint[0] - 2; color.g *= a.tint[1]; color.b *= a.tint[2]; }
					}
					const Color under = target->get_pixel(x, y);
					target->set_pixel(x, y, Color(color.r * color.a + under.r * (1 - color.a), color.g * color.a + under.g * (1 - color.a), color.b * color.a + under.b * (1 - color.a), color.a + under.a * (1 - color.a)));
				}
			}
		}
	}
}

Dictionary HCSRNewestImageAtlas::get_statistics() const {
	Dictionary result;
	result["pages"] = pages.size();
	result["sources"] = entries.size() + glyph_entries.size();
	result["decoded_images"] = decoded_images;
    result["rasterized_glyphs"] = rasterized_glyphs;
    result["pending_glyphs"] = pending_glyphs.size();
    int glyph_pages = 0; for (const Page &page : pages) if (page.glyphs) glyph_pages++;
    result["glyph_pages"] = glyph_pages;
    result["vertices"] = vertices.size();
	result["uploaded_bytes"] = uploaded_bytes;
	result["page_size"] = PAGE_SIZE;
	result["draw_batches"] = batches.size();
	return result;
}

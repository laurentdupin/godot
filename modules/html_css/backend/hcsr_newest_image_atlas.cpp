#include "hcsr_gpu_packet.h"
#include "hcsr_prepared_drawing.h"
#include "hcsr_shader_sources.h"
#include "hcsr_compositing.h"
#include "hcsr_newest_image_atlas.h"
#include "hcsr_image_codec.h"

#include "../bridge/html_asset_provider.h"

#include "core/crypto/crypto_core.h"
#include "core/io/xml_parser.h"
#include "core/os/os.h"

#include <cmath>
#include "servers/text/text_server.h"

static bool svg_has_filters(const Vector<uint8_t> &bytes) {
    XMLParser parser;
    if (parser.open_buffer(bytes) != OK) return false;
    while (parser.read() == OK) {
        if (parser.get_node_type() == XMLParser::NODE_ELEMENT && parser.get_node_name().get_slice(":", parser.get_node_name().contains(":") ? 1 : 0) == "filter") return true;
    }
    return false;
}

Ref<Image> HCSRNewestImageAtlas::load_image(const Ref<HTMLDocument> &document, const String &source) {
    // Caller holds image_mutex. Metadata does not allocate atlas/GPU resources.
    const String key = (document.is_valid() ? document->get_html_file() + "|" + document->get_resource_root() : String()) + "\n" + source;
    if (const Ref<Image> *cached = decoded_sources.getptr(key)) return *cached;
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
			// Share LunaSVG with Interactive/hcsr_old where supported. LunaSVG skips
            // SVG filters, so filtered assets use Godot's opacity-corrected ThorVG.
            if (svg_has_filters(bytes)) {
                error = image->load_svg_from_buffer(bytes);
            } else {
            hcsr_decoded_image decoded = {};
            if (hcsr_svg_decode_bgra32(bytes.ptr(), bytes.size(), 0, 0, &decoded)) {
                Vector<uint8_t> rgba;
                rgba.resize(decoded.width * decoded.height * 4);
                for (int y = 0; y < decoded.height; ++y) {
                    const uint8_t *src = decoded.pixels + y * decoded.stride;
                    uint8_t *dst = rgba.ptrw() + y * decoded.width * 4;
                    for (int x = 0; x < decoded.width; ++x) {
                        dst[x*4] = src[x*4+2]; dst[x*4+1] = src[x*4+1];
                        dst[x*4+2] = src[x*4]; dst[x*4+3] = src[x*4+3];
                    }
                }
                image->set_data(decoded.width, decoded.height, false, Image::FORMAT_RGBA8, rgba);
                hcsr_image_free(&decoded);
                error = OK;
            }
            }
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
    if (error != OK || image->is_empty()) image.unref();
    else decoded_images++;
    decoded_sources.insert(key, image);
    source_sizes.insert(key, image.is_valid() ? image->get_size() : Size2i());
    return image;
}

Size2i HCSRNewestImageAtlas::resolve_size(const Ref<HTMLDocument> &document, const String &source) {
    MutexLock lock(image_mutex);
    const String key = (document.is_valid() ? document->get_html_file() + "|" + document->get_resource_root() : String()) + "\n" + source;
    if (const Size2i *cached = source_sizes.getptr(key)) return *cached;
    Ref<Image> image = load_image(document, source);
    return image.is_valid() ? image->get_size() : Size2i();
}

HCSRNewestImageAtlas::Entry HCSRNewestImageAtlas::resolve(const Ref<HTMLDocument> &document, const String &source, const Size2i &natural, const Vector2 &physical_size) {
	// Cache power-of-two reductions, not a new raster for every animated size.
	const float ratio = MIN(natural.x / MAX(1.0f, physical_size.x), natural.y / MAX(1.0f, physical_size.y));
	const int level = ratio >= 2 ? MIN(12, int(std::floor(std::log2(ratio)))) : 0;
	const String source_key = (document.is_valid() ? document->get_html_file() + "|" + document->get_resource_root() : String()) + "\n" + source;
	const String key = source_key + "\nminification:" + itos(level);
	if (const Entry *existing = entries.getptr(key)) {
		return *existing;
	}
	Entry entry;
	Ref<Image> image;
	{
		MutexLock lock(image_mutex);
		image = load_image(document, source);
		decoded_sources.erase(source_key); // Atlas owns pixels after packing; retain only metadata.
	}
	if (image.is_valid()) {
		entry.natural_size = image->get_size();
		image->convert(Image::FORMAT_RGBA8); // Straight alpha; native BGRA codec contract is unchanged.
        if (level > 0) {
            // Filter each image independently in premultiplied alpha. Whole-atlas
            // mipmaps would mix neighboring entries and produce edge fringes.
            image->premultiply_alpha();
            image->generate_mipmaps();
            image = image->get_image_from_mipmap(MIN(level, image->get_mipmap_count()));
            Vector<uint8_t> pixels = image->get_data();
            uint8_t *data = pixels.ptrw();
            for (int64_t pixel = 0; pixel < int64_t(image->get_width()) * image->get_height(); pixel++) {
                uint8_t *rgba = data + pixel * 4;
                for (int channel = 0; channel < 3; channel++) {
                    rgba[channel] = rgba[3] ? MIN(255, (int(rgba[channel]) * 255 + rgba[3] / 2) / rgba[3]) : 0;
                }
            }
            image->set_data(image->get_width(), image->get_height(), false, Image::FORMAT_RGBA8, pixels);
        }
		const float scale = MIN(1.0f, float(PAGE_SIZE - 2) / MAX(image->get_width(), image->get_height()));
		if (scale < 1) {
			image->resize(MAX(1, int(image->get_width() * scale)), MAX(1, int(image->get_height() * scale)));
		}
		const int width = image->get_width() + 2, height = image->get_height() + 2;
		for (int i = 0; i <= pages.size() && i < MAX_PAGES; i++) {
			if (i == pages.size()) {
                int image_pages = 0; for (const Page &existing : pages) if (!existing.glyphs && existing.pixels->get_width() == PAGE_SIZE) image_pages++;
                if (image_pages >= 4) break;
				Page page;
				page.pixels = Image::create_empty(PAGE_SIZE, PAGE_SIZE, false, Image::FORMAT_RGBA8);
				pages.push_back(page);
			}
			Page &page = pages.write[i];
            if (page.glyphs || page.pixels->get_width() != PAGE_SIZE) continue;
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
		if (!page.glyphs || page.pixels->get_width() != PAGE_SIZE) continue;
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
    const bool had_pending = !pending_glyphs.is_empty();
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
    clipping_enabled = !next_gpu || packet.gpu.clipping_enabled != 0;
    logical_width=packet.viewport_width; logical_height=packet.viewport_height;
    if(next_gpu && gpu_geometry && geometry_generation==packet.gpu.geometry_generation && prepared_scale==output_scale
        && !had_pending) {
        uploaded=false; geometry_dirty=false;
        update_visible_instances(packet);
        return true;
    }
    if (next_gpu && gpu_geometry && prepared_scale == output_scale && !had_pending
        && packet.gpu.color_base_geometry_generation == geometry_generation
        && apply_color_patches(packet)) {
        geometry_generation = packet.gpu.geometry_generation;
        geometry_dirty = true;
        uploaded = false;
        ++color_patch_updates;
        update_visible_instances(packet);
        return true;
    }
    const Vector<Vertex> previous_vertices = vertices;
    const Vector<PreparedMesh> previous_meshes = prepared_meshes;
    prepared_meshes.clear();
    if (next_gpu && prepared_meshes.resize(int(packet.draw_item_count)) != OK) return false;
    gpu_geometry=next_gpu; geometry_generation=next_gpu ? packet.gpu.geometry_generation : 0;
    geometry_dirty=true; prepared_scale=output_scale; primitives.clear();
    // Upgrade previously seen glyphs incrementally; the old level remains visible meanwhile.
    for (int i = 0; i < 32 && !pending_glyphs.is_empty(); i++) {
        PendingGlyph pending = pending_glyphs[pending_glyphs.size() - 1];
        pending_glyphs.resize(pending_glyphs.size() - 1);
        queued_glyphs.erase(pending.key);
        rasterize_glyph(pending.glyph, pending.level);
    }
	vertices.clear();
	batches.clear();
	hcsr::render::compositing_plan group_plan;
    std::string group_error;
    if (!hcsr::render::plan_compositing(packet, group_plan, group_error)) return false;
    group_depth = group_plan.depth;
	uploaded = false;
	bool has_images = group_depth > 0 || gpu_geometry;
	bool references_images = group_depth > 0 || gpu_geometry;
	for (size_t i = 0; i < packet.material_count; i++) {
		references_images |= (packet.materials[i].kind == HCSR_MATERIAL_IMAGE || packet.materials[i].kind == HCSR_MATERIAL_GLYPH || packet.materials[i].kind == HCSR_MATERIAL_VERTEX_COLOR);
	}
	if (!references_images) {
		return false;
	}
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
		if (material.kind == HCSR_MATERIAL_IMAGE && material.payload_size >= sizeof(hcsr_image_material_t)) {
			hcsr_image_material_t image;
			memcpy(&image, packet.material_payload + material.payload_offset, sizeof(image));
			if (image.source_length <= material.payload_size - sizeof(image)) {
				const String source = String::utf8((const char *)(packet.material_payload + material.payload_offset + sizeof(image)), image.source_length);
				const Size2i natural = resolve_size(document, source);
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
                entry = resolve(document, source, natural, destination.size * (output_scale * transform_scale));
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
            entry = resolve_glyph(glyph, output_scale * transform_scale);
            if (entry.page >= 0) {
                float factor = glyph.font_size / entry.raster_size;
                destination = Rect2(Vector2(glyph.baseline_x, glyph.baseline_y) + entry.glyph_offset * factor, entry.glyph_size * factor);
            }
        }
		has_images |= entry.page >= 0 || material.kind == HCSR_MATERIAL_VERTEX_COLOR;
		// Solid grayscale draws do not sample the atlas and can stay in the current batch.
        const int page = entry.page >= 0 ? entry.page : (batches.is_empty() ? 0 : batches[batches.size() - 1].page);
		if (batches.is_empty() || batches[batches.size() - 1].page != page || batches[batches.size() - 1].kind) {
			batches.push_back({ page, gpu_geometry ? uint32_t(primitives.size()) : written, 0 });
		}
		const bool quad = gpu_geometry && (draw.flags & HCSR_DRAW_AXIS_ALIGNED_RECTANGLE) && draw.index_count==6;
        batches.write[batches.size() - 1].count += gpu_geometry ? (quad ? 1 : (draw.index_count+5)/6) : draw.index_count;
        if(gpu_geometry) {
            if(quad) primitives.push_back({written,1,uint32_t(i)});
            else for(uint32_t j=0;j<draw.index_count;j+=6) primitives.push_back({written+j,draw.index_count-j>=6 ? 2u : 0u,uint32_t(i)});
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
            const auto source_index=packet.indices[draw.first_index + (quad && j==3 ? 5 : j)];
            const auto &v = packet.vertices[source_index];
            const auto prepared = hcsr::render::prepare_vertex(v, packet.viewport_width, packet.viewport_height,
                    area, material.kind == HCSR_MATERIAL_VERTEX_COLOR, textured ? &resolved : nullptr);
            Vertex vertex = {};
            vertex.position_uv[0] = prepared.x;
            vertex.position_uv[1] = -prepared.y; // Godot render-target convention.
            if(gpu_geometry) {
                vertex.position_uv[0]=v.screen_x; vertex.position_uv[1]=v.screen_y;
                vertex.state=packet.gpu.vertex_states[source_index];
            }
            vertex.position_uv[2] = prepared.u;
            vertex.position_uv[3] = prepared.v;
            vertex.tint[0] = prepared.red; vertex.tint[1] = prepared.green;
            vertex.tint[2] = prepared.blue; vertex.tint[3] = prepared.alpha;
            vertex.bounds[0] = prepared.left;
            vertex.bounds[1] = prepared.top;
            vertex.bounds[2] = prepared.right;
            vertex.bounds[3] = prepared.bottom;
            vertex_data[written++] = vertex;
        }
	}
    vertices.resize(written);
    if (gpu_geometry) {
        if (!copy_table(prepared_source, packet.vertices, packet.vertex_count)
            || !copy_table(prepared_states, packet.gpu.vertex_states, packet.vertex_count)) return false;
    } else { prepared_source.clear(); prepared_states.clear(); }
    if (has_images && pages.is_empty()) {
        Page placeholder;
        placeholder.pixels = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
        pages.push_back(placeholder);
    }
    if(gpu_geometry) { all_primitives=primitives; all_batches=batches; update_visible_instances(packet); }
	return has_images;
}

bool HCSRNewestImageAtlas::apply_color_patches(const hcsr_draw_packet_view_t &packet) {
    if (!packet.gpu.color_patch_count) return false;
    Vector<PreparedMesh> targets;
    for (size_t i = 0; i < packet.gpu.color_patch_count; ++i) {
        const auto &patch = packet.gpu.color_patches[i];
        bool found = false;
        for (const auto &mesh : prepared_meshes) {
            if (mesh.source_first != patch.first || mesh.count != patch.count) continue;
            if (uint64_t(mesh.source_first)+mesh.count > uint64_t(prepared_source.size())
                || uint64_t(mesh.source_first)+mesh.count > uint64_t(prepared_states.size())
                || uint64_t(mesh.prepared_first)+mesh.count > uint64_t(vertices.size())) return false;
            for (uint32_t j = 0; j < mesh.count; ++j) {
                const auto &old = prepared_source[mesh.source_first+j];
                const auto &next = packet.vertices[mesh.source_first+j];
                if (old.screen_x != next.screen_x || old.screen_y != next.screen_y
                    || prepared_states[mesh.source_first+j] != packet.gpu.vertex_states[mesh.source_first+j]) return false;
            }
            targets.push_back(mesh);
            found = true;
            break;
        }
        if (!found) return false;
    }
    // Validate all ranges before changing any cached product. A missed base or
    // unsupported mesh shape takes the complete-packet path instead.
    auto *destination = vertices.ptrw();
    auto *source = prepared_source.ptrw();
    const hcsr_area_grayscale_material_t area = {};
    for (const auto &mesh : targets) {
        for (uint32_t j = 0; j < mesh.count; ++j) {
            const auto &next = packet.vertices[mesh.source_first+j];
            const auto color = hcsr::render::prepare_vertex(next, packet.viewport_width, packet.viewport_height, area, true);
            auto &vertex = destination[mesh.prepared_first+j];
            vertex.tint[0] = color.red; vertex.tint[1] = color.green;
            vertex.tint[2] = color.blue; vertex.tint[3] = color.alpha;
            source[mesh.source_first+j] = next;
        }
    }
    return true;
}

void HCSRNewestImageAtlas::update_visible_instances(const hcsr_draw_packet_view_t &packet) {
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
                const auto &b=packet.draw_items[primitive.draw_index].bounds;
                if(b.x+b.width<0 || b.y+b.height<0 || b.x>logical_width || b.y>logical_height) continue;
            }
            primitives.push_back(primitive); ++batch.count;
        }
        if(batch.kind || batch.count) batches.push_back(batch);
    }
}

bool HCSRNewestImageAtlas::upload(RenderingDevice *device) {
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
        for(Page &page:pages) { if(page.uniform.is_valid()) device->free_rid(page.uniform); page.uniform=RID(); }
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
	for (Page &page : pages) {
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		format.width = page.pixels->get_width();
		format.height = page.pixels->get_height();
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		if (!page.texture.is_valid()) {
			Vector<Vector<uint8_t>> data;
			data.push_back(page.pixels->get_data());
			page.texture = device->texture_create(format, RD::TextureView(), data);
			uploaded_bytes += uint64_t(format.width) * format.height * 4;
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

bool HCSRNewestImageAtlas::draw(RenderingDevice *device, RID target, const Color &background) {
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
        device->draw_list_bind_uniform_set(list,batch.kind==HCSR_GROUP_END ? group_targets[batch.depth-1].uniform : pages[batch.page].uniform,0);
        const struct { uint32_t first,flags; float width,height,target_width,target_height; } push{batch.first,
            (gpu_geometry ? 2u : 0u) | (clipping_enabled ? 1u : 0u),logical_width,logical_height,float(size.x),float(size.y)};
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

void HCSRNewestImageAtlas::release_groups(RenderingDevice *device) {
    if (device) for(const auto &pool:group_pools) for (const auto &group : pool.targets)
        for (RID rid : {group.uniform,group.framebuffer,group.texture}) if (rid.is_valid()) device->free_rid(rid);
    group_pools.clear();
}

void HCSRNewestImageAtlas::release(RenderingDevice *device) {
    release_groups(device);
	if (device) {
		for (const Page &page : pages) {
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
	pages.clear();
	entries.clear();
    glyph_levels.clear();
    pending_glyphs.clear(); last_glyphs.clear(); queued_glyphs.clear(); glyph_entries.clear();
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
#undef HCSR_FLOOR
#undef HCSR_MIX
#undef HCSR_FETCH
#undef HCSR_GROUP_FETCH
#undef HCSR_DISCARD
}

void HCSRNewestImageAtlas::draw_cpu(Ref<Image> target, const Color &background) {
	target->fill(background);
	const Vector2 size = target->get_size();
    Vector<Ref<Image>> parents;
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
                    const Vector4 shaded = hcsr_cpu_paint::hcsr_shade(pages[batch.page].pixels, uv, tint,
                            Vector4(a.bounds[0],a.bounds[1],a.bounds[2],a.bounds[3]));
                    const Color color(shaded.x,shaded.y,shaded.z,shaded.w);
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
    result["gpu_geometry"] = gpu_geometry;
    result["clipping_enabled"] = clipping_enabled;
    result["instances"] = primitives.size();
    result["draw_calls"] = last_draw_calls;
    result["gpu_ms"] = last_gpu_ms;
    result["gpu_timestamp_frame"] = last_gpu_frame;
    result["geometry_uploaded_bytes"] = geometry_uploaded_bytes;
    result["instance_uploaded_bytes"] = instance_uploaded_bytes;
    result["state_uploaded_bytes"] = state_uploaded_bytes;
    result["geometry_generation"] = geometry_generation;
    result["color_patch_updates"] = color_patch_updates;
	result["uploaded_bytes"] = uploaded_bytes;
	result["page_size"] = PAGE_SIZE;
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

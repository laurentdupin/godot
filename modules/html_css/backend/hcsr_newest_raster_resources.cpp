#include "hcsr_newest_raster_resources.h"
#include "hcsr_image_codec.h"
#include "../bridge/html_asset_provider.h"
#include "core/crypto/crypto_core.h"
#include "core/io/xml_parser.h"
#include "servers/text/text_server.h"
#include <cmath>

static bool svg_has_filters(const Vector<uint8_t> &bytes) {
    XMLParser parser;
    if (parser.open_buffer(bytes) != OK) return false;
    while (parser.read() == OK) {
        if (parser.get_node_type() == XMLParser::NODE_ELEMENT && parser.get_node_name().get_slice(":", parser.get_node_name().contains(":") ? 1 : 0) == "filter") return true;
    }
    return false;
}

Ref<Image> HCSRNewestRasterResources::load_image(const Ref<HTMLDocument> &document, const String &source) {
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

Size2i HCSRNewestRasterResources::resolve_size(const Ref<HTMLDocument> &document, const String &source) {
    MutexLock lock(image_mutex);
    const String key = (document.is_valid() ? document->get_html_file() + "|" + document->get_resource_root() : String()) + "\n" + source;
    if (const Size2i *cached = source_sizes.getptr(key)) return *cached;
    Ref<Image> image = load_image(document, source);
    return image.is_valid() ? image->get_size() : Size2i();
}

HCSRNewestRasterResources::Entry HCSRNewestRasterResources::resolve_image(const Ref<HTMLDocument> &document, const String &source, const Size2i &natural, const Vector2 &physical_size) {
	// Cache power-of-two reductions, not a new raster for every animated size.
	const float ratio = MIN(natural.x / MAX(1.0f, physical_size.x), natural.y / MAX(1.0f, physical_size.y));
	const int level = ratio >= 2 ? MIN(12, int(std::floor(std::log2(ratio)))) : 0;
	const String source_key = (document.is_valid() ? document->get_html_file() + "|" + document->get_resource_root() : String()) + "\n" + source;
	const String key = source_key + "\nminification:" + itos(level);
	if (const Entry *existing = entries.getptr(key)) {
		return *existing;
	}
	Ref<Image> image;
	{
		MutexLock lock(image_mutex);
		image = load_image(document, source);
		decoded_sources.erase(source_key); // Atlas owns pixels after packing; retain only metadata.
	}
    Entry entry = pack_image(image, level);
    if (entry.page < 0) WARN_PRINT("hcsr_newest image unavailable or atlas capacity exceeded: " + source.left(100));
    entries.insert(key, entry);
    return entry;
}

void HCSRNewestRasterResources::retain_surfaces(const HashSet<uint64_t> &live) {
    Vector<SurfaceKey> retired;
    for (const auto &item : surface_entries) if (!live.has(item.key.identity)) retired.push_back(item.key);
    for (const auto &key : retired) {
        const Entry entry = surface_entries[key];
        if (entry.page >= 0) release_image_slot(pages.write[entry.page],Rect2i(entry.rect.position-Point2i(1,1),entry.rect.size+Size2i(2,2)));
        surface_entries.erase(key);
    }
}

void HCSRNewestRasterResources::release_image_slot(Page &page, Rect2i slot) {
    for (int i=0; i<page.free_slots.size();) {
        const Rect2i other=page.free_slots[i];
        const bool horizontal=slot.position.y==other.position.y && slot.size.y==other.size.y
            && (slot.get_end().x==other.position.x || other.get_end().x==slot.position.x);
        const bool vertical=slot.position.x==other.position.x && slot.size.x==other.size.x
            && (slot.get_end().y==other.position.y || other.get_end().y==slot.position.y);
        if (horizontal || vertical) { slot=slot.merge(other); page.free_slots.remove_at(i); i=0; }
        else ++i;
    }
    page.free_slots.push_back(slot);
}

bool HCSRNewestRasterResources::reserve_image_slot(Page &page, int width, int height, Point2i &position) {
    int selected=-1, waste=INT32_MAX;
    for (int i=0; i<page.free_slots.size(); ++i) {
        const Rect2i slot=page.free_slots[i];
        const int remaining=slot.size.x*slot.size.y-width*height;
        if (slot.size.x>=width && slot.size.y>=height && remaining<waste) { selected=i; waste=remaining; }
    }
    if (selected>=0) {
        const Rect2i slot=page.free_slots[selected]; page.free_slots.remove_at(selected);
        position=slot.position;
        if (slot.size.x>width) page.free_slots.push_back(Rect2i(position+Point2i(width,0),Size2i(slot.size.x-width,height)));
        if (slot.size.y>height) page.free_slots.push_back(Rect2i(position+Point2i(0,height),Size2i(slot.size.x,slot.size.y-height)));
        ++reused_surface_slots;
        return true;
    }
    int x=page.x, y=page.y, row=page.row_height;
    if (x+width>PAGE_SIZE) { x=0; y+=row; row=0; }
    if (width>PAGE_SIZE || y+height>PAGE_SIZE) return false;
    position=Point2i(x,y); page.x=x+width; page.y=y; page.row_height=MAX(row,height);
    return true;
}

HCSRNewestRasterResources::Entry HCSRNewestRasterResources::resolve_raster(const hcsr_raster_material_t &raster, const Vector2 &physical_size) {
    Entry entry;
    if (!raster.identity || !raster.pixels || !raster.width || !raster.height
        || raster.width > INT32_MAX / 4 || raster.stride != raster.width * 4
        || uint64_t(raster.stride) * raster.height > INT32_MAX) return entry;
    const float ratio = MIN(raster.width / MAX(1.0f, physical_size.x), raster.height / MAX(1.0f, physical_size.y));
    const int level = ratio >= 2 ? MIN(12, int(std::floor(std::log2(ratio)))) : 0;
    const SurfaceKey key{ raster.identity, level };
    if (const Entry *cached = surface_entries.getptr(key)) return *cached;
    Vector<uint8_t> pixels;
    if (pixels.resize(int(raster.stride * raster.height)) != OK) return entry;
    uint8_t *dst = pixels.ptrw();
    for (uint64_t i = 0; i < uint64_t(raster.width) * raster.height; ++i) {
        dst[i*4] = raster.pixels[i*4+2]; dst[i*4+1] = raster.pixels[i*4+1];
        dst[i*4+2] = raster.pixels[i*4]; dst[i*4+3] = raster.pixels[i*4+3];
    }
    Ref<Image> image = Image::create_from_data(raster.width, raster.height, false, Image::FORMAT_RGBA8, pixels);
    entry = pack_image(image, level);
    surface_entries.insert(key, entry);
    return entry;
}

HCSRNewestRasterResources::Entry HCSRNewestRasterResources::pack_image(Ref<Image> image, int level) {
    Entry entry;
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
            Point2i position;
            if (!reserve_image_slot(page,width,height,position)) continue;
            const int x=position.x, y=position.y;
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
			break;
		}
	}
	return entry;
}

HCSRNewestRasterResources::Entry HCSRNewestRasterResources::resolve_glyph(const hcsr_glyph_material_t &glyph, float scale) {
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

HCSRNewestRasterResources::Entry HCSRNewestRasterResources::rasterize_glyph(const hcsr_glyph_material_t &glyph, int level) {
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

void HCSRNewestRasterResources::advance_rasterization() {
    // Upgrade previously seen glyphs incrementally; the old level remains visible meanwhile.
    for (int i = 0; i < 32 && !pending_glyphs.is_empty(); i++) {
        PendingGlyph pending = pending_glyphs[pending_glyphs.size() - 1];
        pending_glyphs.resize(pending_glyphs.size() - 1);
        queued_glyphs.erase(pending.key);
        rasterize_glyph(pending.glyph, pending.level);
    }
}

void HCSRNewestRasterResources::ensure_sampling_page() {
    if (pages.is_empty()) {
        Page placeholder;
        placeholder.pixels = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
        pages.push_back(placeholder);
    }
}

Dictionary HCSRNewestRasterResources::get_statistics() const {
    Dictionary result;
	result["pages"] = pages.size();
	result["sources"] = entries.size() + glyph_entries.size() + surface_entries.size();
	result["decoded_images"] = decoded_images;
    result["reused_surface_slots"] = reused_surface_slots;
    result["raster_surfaces"] = surface_entries.size();
    result["rasterized_glyphs"] = rasterized_glyphs;
    result["pending_glyphs"] = pending_glyphs.size();
    int glyph_pages = 0; for (const Page &page : pages) if (page.glyphs) glyph_pages++;
    result["glyph_pages"] = glyph_pages;
    result["page_size"] = PAGE_SIZE;
    return result;
}

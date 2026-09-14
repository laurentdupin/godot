#include "hcsr_newest_raster_resources.h"
#include "hcsr_image_codec.h"
#include "../bridge/html_asset_provider.h"
#include "core/crypto/crypto_core.h"
#include "core/io/xml_parser.h"
#include "servers/text/text_server.h"
#include <cmath>

HCSRNewestRasterResources::HCSRNewestRasterResources() { ERR_FAIL_COND(hcsr_atlas_create(&atlas)!=HCSR_OK); }
HCSRNewestRasterResources::~HCSRNewestRasterResources() { if(atlas) hcsr_atlas_destroy(atlas); }

Vector<uint8_t> HCSRNewestRasterResources::page_pixels(int page,const Rect2i &region) const {
    Vector<uint8_t> pixels;
    ERR_FAIL_COND_V(pixels.resize(region.size.x*region.size.y*4)!=OK,pixels);
    if(page==0) { memset(pixels.ptrw(),0,pixels.size());return pixels; }
    const hcsr_atlas_region_t r{region.position.x,region.position.y,region.size.x,region.size.y};
    ERR_FAIL_COND_V(hcsr_atlas_copy_region(atlas,page-1,&r,reinterpret_cast<uint32_t *>(pixels.ptrw()),pixels.size()/4)!=HCSR_OK,Vector<uint8_t>());
    return pixels;
}
Vector<Rect2i> HCSRNewestRasterResources::page_updates(int page) const {
    Vector<Rect2i> result;
    if(page==0) return result;
    const int count=hcsr_atlas_update_count(atlas,page-1);
    for(int i=0;i<count;++i) { hcsr_atlas_region_t r{};
        if(hcsr_atlas_get_update(atlas,page-1,i,&r)==HCSR_OK) result.push_back(Rect2i(r.x,r.y,r.width,r.height));
    }
    return result;
}

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
	const int level = hcsr_atlas_image_level(natural.x,natural.y,physical_size.x,physical_size.y);
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
        if (entry.page>0) {
            const hcsr_atlas_entry_t allocation{entry.page-1,{entry.rect.position.x,entry.rect.position.y,entry.rect.size.x,entry.rect.size.y}};
            hcsr_atlas_release(atlas,&allocation);
        }
        surface_entries.erase(key);
    }
}

HCSRNewestRasterResources::Entry HCSRNewestRasterResources::resolve_raster(const hcsr_raster_material_t &raster, const Vector2 &physical_size) {
    Entry entry;
    if (!raster.identity || !raster.pixels || !raster.width || !raster.height
        || raster.width > INT32_MAX / 4 || raster.stride != raster.width * 4
        || uint64_t(raster.stride) * raster.height > INT32_MAX) return entry;
    const int level = hcsr_atlas_image_level(raster.width,raster.height,physical_size.x,physical_size.y);
    const SurfaceKey key{ raster.identity, level };
    if (const Entry *cached = surface_entries.getptr(key)) return *cached;
    hcsr_atlas_entry_t allocation{};
    if(hcsr_atlas_pack(atlas,reinterpret_cast<const uint32_t *>(raster.pixels),raster.width,raster.height,level,0,&allocation)==HCSR_OK) {
        entry.page=allocation.page+1;entry.rect=Rect2i(allocation.rect.x,allocation.rect.y,allocation.rect.width,allocation.rect.height);
        entry.natural_size=Size2i(raster.width,raster.height);
    }
    surface_entries.insert(key, entry);
    return entry;
}

HCSRNewestRasterResources::Entry HCSRNewestRasterResources::pack_image(Ref<Image> image,int level,bool glyph) {
    Entry entry;
    if(image.is_null()) return entry;
    entry.natural_size=image->get_size();image->convert(Image::FORMAT_RGBA8);
    const Vector<uint8_t> pixels=image->get_data();hcsr_atlas_entry_t allocation{};
    if(hcsr_atlas_pack(atlas,reinterpret_cast<const uint32_t *>(pixels.ptr()),image->get_width(),image->get_height(),level,2u|(glyph?1u:0u),&allocation)==HCSR_OK) {
        entry.page=allocation.page+1;entry.rect=Rect2i(allocation.rect.x,allocation.rect.y,allocation.rect.width,allocation.rect.height);
    }
    return entry;
}

HCSRNewestRasterResources::Entry HCSRNewestRasterResources::resolve_glyph(const hcsr_glyph_material_t &glyph, float scale) {
	uint32_t size_bits;
    memcpy(&size_bits, &glyph.font_size, sizeof(size_bits));
    const GlyphKey identity{ glyph.face, glyph.glyph, size_bits };
    const int *previous=glyph_levels.getptr(identity);
    const int level=hcsr_atlas_glyph_level(glyph.font_size,scale,previous?*previous:0);
    glyph_levels.insert(identity,level);
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
    const Entry allocation = pack_image(image, 0, true);
    entry.page = allocation.page;
    entry.rect = allocation.rect;
    if (entry.page >= 0) ++rasterized_glyphs;
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

void HCSRNewestRasterResources::ensure_sampling_page() {} // Page zero is the permanent empty sampler.

Dictionary HCSRNewestRasterResources::get_statistics() const {
    Dictionary result;
	result["pages"] = page_count();
	result["sources"] = entries.size() + glyph_entries.size() + surface_entries.size();
	result["decoded_images"] = decoded_images;
    result["reused_surface_slots"] = hcsr_atlas_reused_slots(atlas);
    result["raster_surfaces"] = surface_entries.size();
    result["rasterized_glyphs"] = rasterized_glyphs;
    result["pending_glyphs"] = pending_glyphs.size();
    HashSet<int> glyph_page_ids; for(const auto &item:glyph_entries) if(item.value.page>0) glyph_page_ids.insert(item.value.page);
    const int glyph_pages=glyph_page_ids.size();
    result["glyph_pages"] = glyph_pages;
    result["page_size"] = PAGE_SIZE;
    return result;
}

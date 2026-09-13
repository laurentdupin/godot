#pragma once

#include "../html_document.h"
#include "hcsr_scene.h"
#include "core/io/image.h"
#include "core/os/mutex.h"

// Owns decoded/rasterized pixels, atlas allocation and resolution policy.
// It has no scene geometry, drawing pipeline or RenderingDevice resources.
// The view owns this cache independently of its scene renderer.
// Atlas mutation and upload acknowledgement run on the render thread; only
// resolve_size is also called during layout and guards source metadata with a mutex.
// One renderer consumes updates and shares its uploaded pages across outputs.
class HCSRNewestRasterResources {
public:
	static constexpr int PAGE_SIZE = 4096;
	static constexpr int MAX_PAGES = 9;
	struct Entry {
		int page = -1;
		Rect2i rect;
		Size2i natural_size;
        Vector2 glyph_offset, glyph_size;
        int raster_size = 0;
        bool color_glyph = false;
	};
private:
	struct Page {
        bool glyphs = false;
		Ref<Image> pixels;
		Vector<Rect2i> dirty;
		int x = 0, y = 0, row_height = 0;
	};
    struct GlyphKey {
        uint64_t face = 0;
        uint32_t glyph = 0, size = 0;
        bool operator==(const GlyphKey &other) const { return face == other.face && glyph == other.glyph && size == other.size; }
    };
    struct GlyphHasher {
        static uint32_t hash(const GlyphKey &key) {
            return hash_fmix32(hash_murmur3_one_32(key.size, hash_murmur3_one_32(key.glyph, hash_murmur3_one_64(key.face))));
        }
    };
    HashMap<GlyphKey, Entry, GlyphHasher> glyph_entries;
	HashMap<String, Entry> entries;
	Mutex image_mutex;
	HashMap<String, Ref<Image>> decoded_sources;
	HashMap<String, Size2i> source_sizes;
	Ref<Image> load_image(const Ref<HTMLDocument> &document, const String &source);
	Vector<Page> pages;
public:
	Entry resolve_image(const Ref<HTMLDocument> &document, const String &source, const Size2i &natural, const Vector2 &physical_size);
    Entry resolve_glyph(const hcsr_glyph_material_t &glyph, float scale);
private:
    Entry rasterize_glyph(const hcsr_glyph_material_t &glyph, int level);
    struct PendingGlyph { hcsr_glyph_material_t glyph; int level; GlyphKey key; };
    Vector<PendingGlyph> pending_glyphs;
    HashMap<GlyphKey, Entry, GlyphHasher> last_glyphs;
    HashMap<GlyphKey, bool, GlyphHasher> queued_glyphs;
    uint64_t rasterized_glyphs = 0;
    HashMap<GlyphKey, int, GlyphHasher> glyph_levels;
    uint64_t decoded_images = 0;
public:
    Size2i resolve_size(const Ref<HTMLDocument> &document, const String &source);
    bool has_pending_glyphs() const { return !pending_glyphs.is_empty(); }
    void advance_rasterization();
    void ensure_sampling_page();
    int page_count() const { return pages.size(); }
    const Ref<Image> &page_image(int page) const { return pages[page].pixels; }
    const Vector<Rect2i> &page_updates(int page) const { return pages[page].dirty; }
    void acknowledge_upload(int page) { pages.write[page].dirty.clear(); }
    Dictionary get_statistics() const;
};

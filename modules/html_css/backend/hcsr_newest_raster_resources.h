#pragma once

#include "../html_document.h"
#include "hcsr_scene.h"
#include "hcsr_atlas.h"
#include "core/io/image.h"
#include "core/os/mutex.h"
#include "core/templates/hash_set.h"

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
        bool atlas_failed = false;
	};
private:
    uint64_t atlas = 0;
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
    struct SurfaceKey {
        uint64_t identity;
        int level;
        bool operator==(const SurfaceKey &other) const { return identity == other.identity && level == other.level; }
    };
    struct SurfaceHasher {
        static uint32_t hash(const SurfaceKey &key) { return hash_fmix32(hash_murmur3_one_32(key.level, hash_murmur3_one_64(key.identity))); }
    };
    HashMap<SurfaceKey, Entry, SurfaceHasher> surface_entries;
    Entry pack_image(Ref<Image> image, int level, bool glyph = false);

    struct ImageKey {
        String source;
        int level;
        bool operator==(const ImageKey &other) const { return source == other.source && level == other.level; }
    };
    struct ImageHasher {
        static uint32_t hash(const ImageKey &key) { return hash_fmix32(hash_murmur3_one_32(key.level,key.source.hash())); }
    };
    HashMap<GlyphKey, Entry, GlyphHasher> glyph_entries;
    HashMap<ImageKey, Entry, ImageHasher> entries;
    HashSet<String> live_images;
    HashSet<GlyphKey, GlyphHasher> live_glyphs;
    HashSet<ImageKey, ImageHasher> requested_images;
    HashSet<GlyphKey, GlyphHasher> requested_glyphs;
    HashSet<GlyphKey, GlyphHasher> requested_glyph_identities;
    HashSet<SurfaceKey, SurfaceHasher> requested_surfaces;
    bool resolution_epoch = false;
    uint64_t retired_resolution_variants = 0;
    Vector<ImageKey> retired_images;
    Vector<GlyphKey> retired_glyphs;
    String retention_prefix;
    int failed_allocation_count = 0;
    bool release_entry(const Entry &entry);
    void retry_failed_allocations();
	mutable Mutex image_mutex;
	HashMap<String, Ref<Image>> decoded_sources;
	HashMap<String, Size2i> source_sizes;
	Ref<Image> load_image(const Ref<HTMLDocument> &document, const String &source, int width = 0, int height = 0, float content_width = 0, float content_height = 0);

public:
    bool copy_tile_pixels(const Ref<HTMLDocument> &document,const String &source,int width,int height,float content_width,float content_height,Vector<uint8_t> &pixels,hcsr_image_pixels_t &output);
    HCSRNewestRasterResources();
    ~HCSRNewestRasterResources();
    HCSRNewestRasterResources(const HCSRNewestRasterResources &) = delete;
    HCSRNewestRasterResources &operator=(const HCSRNewestRasterResources &) = delete;
	Entry resolve_image(const Ref<HTMLDocument> &document, const String &source, const Size2i &natural, const Vector2 &physical_size);
    void retain_surfaces(const HashSet<uint64_t> &live);
    void begin_asset_retention(const Ref<HTMLDocument> &document);
    void retain_image_source(const String &source);
    void retain_glyph(const hcsr_glyph_material_t &glyph);
    void end_asset_retention();
    void begin_resolution_requests();
    void end_resolution_requests();
    Entry resolve_raster(const hcsr_raster_material_t &raster, const Vector2 &physical_size);
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
    bool copy_source_pixels(const Ref<HTMLDocument> &document, const String &source, int width, int height, Vector<uint8_t> &pixels, hcsr_image_pixels_t &output);
    Size2i resolve_size(const Ref<HTMLDocument> &document, const String &source);
    bool has_pending_glyphs() const { return !pending_glyphs.is_empty(); }
    void advance_rasterization();
    void ensure_sampling_page();
    int page_count() const { return MAX(0,hcsr_atlas_page_count(atlas))+1; }
    int page_live_allocations(int page) const { return page==0 ? 1 : hcsr_atlas_live_allocations(atlas,page-1); }
    uint64_t atlas_revision() const { return hcsr_atlas_revision(atlas); }
    Size2i page_size(int page) const { return page==0 ? Size2i(1,1) : Size2i(PAGE_SIZE,PAGE_SIZE); }
    Vector<uint8_t> page_pixels(int page,const Rect2i &region) const;
    Vector<Rect2i> page_updates(int page) const;
    void acknowledge_upload(int page) { if(page>0) hcsr_atlas_acknowledge(atlas,page-1); }
    Dictionary get_statistics() const;
};

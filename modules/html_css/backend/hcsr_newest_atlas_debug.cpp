#include "hcsr_newest_raster_resources.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "servers/text/text_server.h"

static const String &atlas_dump_directory() {
    static const String directory = OS::get_singleton()->get_environment("HCSR_DUMP_ATLAS_DIR");
    return directory;
}

bool HCSRNewestRasterResources::atlas_dump_enabled() {
    return !atlas_dump_directory().is_empty();
}

Array HCSRNewestRasterResources::debug_atlas_entries(hcsr_scene_t scene, const hcsr_draw_packet_view_t &packet) const {
    // The owner holds State::mutex and executes on the render thread, just like
    // atlas allocation/upload. image_mutex additionally protects source metadata.
    MutexLock lock(image_mutex);
    HashMap<uint64_t, Array> owners;
    HashMap<GlyphKey, Array, GlyphHasher> glyph_sizes;
    for (size_t i = 0; i < packet.material_count; ++i) {
        const auto &material = packet.materials[i];
        if (material.kind != HCSR_MATERIAL_GLYPH || material.payload_size != sizeof(hcsr_glyph_material_t)
                || uint64_t(material.payload_offset) + material.payload_size > packet.material_payload_size) continue;
        hcsr_glyph_material_t glyph;
        memcpy(&glyph, packet.material_payload + material.payload_offset, sizeof(glyph));
        auto &sizes = glyph_sizes[{ glyph.face, glyph.glyph, 0 }];
        if (!sizes.has(glyph.font_size)) sizes.push_back(glyph.font_size);
    }
    for (size_t i = 0; i < packet.draw_item_count; ++i) {
        const auto &draw = packet.draw_items[i];
        if (draw.material_index >= packet.material_count) continue;
        const auto &material = packet.materials[draw.material_index];
        if (material.kind != HCSR_MATERIAL_RASTER && material.kind != HCSR_MATERIAL_RASTER_SLICE) continue;
        if (material.payload_size < sizeof(hcsr_raster_material_t)
                || uint64_t(material.payload_offset) + material.payload_size > packet.material_payload_size) continue;
        hcsr_raster_material_t raster;
        memcpy(&raster, packet.material_payload + material.payload_offset, sizeof(raster));
        Dictionary owner;
        owner["object_id"] = uitos(draw.object_id);
        owner["shape_type"] = material.kind == HCSR_MATERIAL_RASTER ? "raster" : "raster_slice";
        size_t bytes = 0;
        String element;
        if (hcsr_scene_copy_element_id(scene, draw.object_id, nullptr, 0, &bytes) == HCSR_BUFFER_TOO_SMALL && bytes > 0) {
            Vector<char> buffer;
            buffer.resize(bytes + 1);
            buffer.write[bytes] = 0;
            if (hcsr_scene_copy_element_id(scene, draw.object_id, buffer.ptrw(), bytes + 1, &bytes) == HCSR_OK)
                element = String::utf8(buffer.ptr());
        }
        owner["element"] = element;
        auto &list = owners[raster.identity];
        if (!list.has(owner)) list.push_back(owner);
    }
    Array result;
    auto append = [&](const Entry &entry, const String &kind, const Dictionary &identity) {
        if (entry.page <= 0 || !entry.rect.has_area()) return;
        Dictionary record;
        record["kind"] = kind;
        record["page"] = entry.page;
        record["x"] = entry.rect.position.x;
        record["y"] = entry.rect.position.y;
        record["w"] = entry.rect.size.x;
        record["h"] = entry.rect.size.y;
        record["identity"] = identity;
        result.push_back(record);
    };
    for (const auto &item : entries) {
        Dictionary identity;
        // Image keys prefix the source with the document/root context.
        identity["source"] = item.key.source.get_slice("\n", 1);
        identity["source_key"] = item.key.source;
        identity["level"] = item.key.level;
        append(item.value, "image", identity);
    }
    TextServer *ts = TextServerManager::get_singleton()->get_primary_interface().ptr();
    for (const auto &item : glyph_entries) {
        Dictionary identity;
        identity["font_face"] = uitos(item.key.face);
        identity["font_name"] = ts->font_get_name(RID::from_uint64(item.key.face));
        identity["glyph"] = int64_t(item.key.glyph);
        identity["raster_size"] = item.value.raster_size;
        identity["level"] = int64_t(item.key.size);
        const GlyphKey key{ item.key.face, item.key.glyph, 0 };
        identity["font_sizes"] = glyph_sizes.has(key) ? glyph_sizes[key] : Array();
        append(item.value, "glyph", identity);
    }
    for (const auto &item : surface_entries) {
        Dictionary identity;
        identity["surface_key"] = uitos(item.key.identity);
        identity["level"] = item.key.level;
        identity["owners"] = owners.has(item.key.identity) ? owners[item.key.identity] : Array();
        append(item.value, "surface", identity);
    }
    return result;
}

bool HCSRNewestRasterResources::dump_atlas_if_changed(const Ref<HTMLDocument> &document,
        uint64_t instance_id, hcsr_scene_t scene, const hcsr_draw_packet_view_t &packet) {
    const String &directory = atlas_dump_directory();
    if (directory.is_empty()) return false;
    const uint64_t revision = atlas_revision();
    if (revision == debug_dump_revision) return false;
    const uint64_t now = OS::get_singleton()->get_ticks_msec();
    if (debug_dump_revision != UINT64_MAX && now - debug_dump_msec < 500) return true;
    // A failed write must not produce a retry/error storm every rendered frame.
    debug_dump_revision = revision;
    debug_dump_msec = now;
    const Error mkdir_error = DirAccess::make_dir_recursive_absolute(directory);
    if (mkdir_error != OK && mkdir_error != ERR_ALREADY_EXISTS) {
        ERR_PRINT("HCSR atlas dump directory could not be created: " + directory);
        return false;
    }
    String stem = document.is_valid() ? document->get_html_file().get_file().get_basename() : String();
    if (stem.is_empty() && document.is_valid()) stem = document->get_path().get_file().get_basename();
    if (stem.is_empty()) stem = "inline";
    stem = stem.validate_filename();
    const String prefix = stem + "-" + itos(OS::get_singleton()->get_process_id()) + "-" + uitos(instance_id);
    const Array all_entries = debug_atlas_entries(scene, packet);
    for (int page = 1; page < page_count(); ++page) {
        Array page_entries;
        int rows = 0;
        int64_t area = 0;
        for (int i = 0; i < all_entries.size(); ++i) {
            const Dictionary entry = all_entries[i];
            if (int(entry["page"]) != page) continue;
            page_entries.push_back(entry);
            rows = MAX(rows, int(entry["y"]) + int(entry["h"]));
            area += int64_t(entry["w"]) * int64_t(entry["h"]);
        }
        const int image_rows = MAX(1, rows);
        Vector<uint8_t> pixels = page_pixels(page, Rect2i(0, 0, PAGE_SIZE, image_rows));
        if (pixels.size() != PAGE_SIZE * image_rows * 4) continue;
        uint8_t *data = pixels.ptrw();
        for (int y = 0; y < image_rows; ++y) for (int x = 0; x < PAGE_SIZE; ++x) {
            const int offset = (y * PAGE_SIZE + x) * 4;
            const int background = ((x / 16 + y / 16) % 2) ? 144 : 192;
            const int alpha = data[offset + 3];
            const uint8_t red = data[offset + 2], green = data[offset + 1], blue = data[offset];
            data[offset] = (red * alpha + background * (255 - alpha) + 127) / 255;
            data[offset + 1] = (green * alpha + background * (255 - alpha) + 127) / 255;
            data[offset + 2] = (blue * alpha + background * (255 - alpha) + 127) / 255;
            data[offset + 3] = 255;
        }
        Ref<Image> image = Image::create_from_data(PAGE_SIZE, image_rows, false, Image::FORMAT_RGBA8, pixels);
        Ref<Image> overlay = image->duplicate();
        Array tallest = page_entries.duplicate();
        // Debug-only insertion sort keeps ties deterministic by insertion order.
        for (int i = 1; i < tallest.size(); ++i) {
            const Dictionary entry = tallest[i];
            int j = i;
            while (j > 0 && int(Dictionary(tallest[j - 1])["h"]) < int(entry["h"])) {
                tallest[j] = tallest[j - 1];
                --j;
            }
            tallest[j] = entry;
        }
        if (tallest.size() > 10) tallest.resize(10);
        for (int i = 0; i < page_entries.size(); ++i) {
            const Dictionary entry = page_entries[i];
            const String kind = entry["kind"];
            const Color color = kind == "glyph" ? Color(0, 1, 0) : kind == "surface" ? Color(1, 0.5, 0) : Color(0, 0.5, 1);
            const int x = entry["x"], y = entry["y"], w = entry["w"], h = entry["h"];
            for (int px = x; px < x + w; ++px) { overlay->set_pixel(px, y, color); overlay->set_pixel(px, y + h - 1, color); }
            for (int py = y; py < y + h; ++py) { overlay->set_pixel(x, py, color); overlay->set_pixel(x + w - 1, py, color); }
        }
        Dictionary metadata;
        metadata["document"] = document.is_valid() ? document->get_html_file() : String();
        metadata["surface_instance_id"] = uitos(instance_id);
        metadata["process_id"] = OS::get_singleton()->get_process_id();
        metadata["revision"] = uitos(revision);
        metadata["page"] = page;
        Array dimensions;
        dimensions.push_back(PAGE_SIZE);
        dimensions.push_back(PAGE_SIZE);
        metadata["page_size"] = dimensions;
        metadata["width"] = PAGE_SIZE;
        metadata["height"] = PAGE_SIZE;
        metadata["used_rows"] = rows;
        metadata["packed_area_pixels"] = area;
        metadata["efficiency"] = rows > 0 ? double(area) / (int64_t(rows) * PAGE_SIZE) : 0.0;
        metadata["entries"] = page_entries;
        metadata["ten_tallest_entries"] = tallest;
        metadata["shape_type_note"] = "Owners expose packet material kinds; original CSS painter shape types are not carried by this ABI. Empty owners may denote masks or retired appearances.";
        for (const String &suffix : { "-rev" + uitos(revision), String("-latest") }) {
            const String path = directory.path_join(prefix + "-page" + itos(page) + suffix);
            Error error = image->save_png(path + ".png");
            if (error == OK) error = overlay->save_png(path + "-overlay.png");
            Ref<FileAccess> file = FileAccess::open(path + ".json", FileAccess::WRITE);
            if (file.is_valid()) file->store_string(JSON::stringify(metadata, "  "));
            if (error != OK || file.is_null()) ERR_PRINT("HCSR atlas dump write failed: " + path);
        }
    }
    return false;
}

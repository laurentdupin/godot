#include "hcsr_newest_raster_resources.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include <unordered_map>
#ifdef GLES3_ENABLED
#include <platform_gl.h>
#endif

// One backing owner per rendering context. All mutation is serialized on the
// render thread. Per-view maps are leases/demand policy, never pixel copies.
struct HCSRNewestRasterResources::Pool {
    uint64_t atlas=0, next_lease=0, bindings=0, reclaimed=0, retired_pages=0, uploaded=UINT64_MAX;
    uint32_t views=0;
    Rect2i bounds[MAX_PAGES];
    bool bounds_dirty=false;
    struct Slot {Entry entry;uint32_t references=1;GlyphKey glyph;bool shared=false;};
    HashMap<uint64_t,Slot> slots;
    HashMap<GlyphKey,uint64_t,GlyphHasher> glyphs;
    struct Page {RID texture;Size2i size;};
    Vector<Page> pages;
    RenderingDevice *device=nullptr;
    uint32_t gl_texture=0;
    int gl_layers=0,gl_extent=0;
    Pool() {ERR_FAIL_COND(hcsr_atlas_create(&atlas)!=HCSR_OK);}
    ~Pool() {
        // Renderer descriptor sets are released before the last resource owner.
        if(device && RenderingServer::get_singleton() && RenderingServer::get_singleton()->get_rendering_device()==device)
            for(auto &page:pages)if(page.texture.is_valid())device->free_rid(page.texture);
#ifdef GLES3_ENABLED
        if(gl_texture)glDeleteTextures(1,&gl_texture);
#endif
        if(atlas)hcsr_atlas_destroy(atlas);
    }
};

HCSRNewestRasterResources::HCSRNewestRasterResources():pool(std::make_shared<Pool>()) {atlas=pool->atlas;++pool->views;}
HCSRNewestRasterResources::~HCSRNewestRasterResources() {
    for(const auto &item:entries)release_pool_entry(item.value);
    for(const auto &item:glyph_entries)release_pool_entry(item.value);
    for(const auto &item:surface_entries)release_pool_entry(item.value);
    --pool->views;
}
void HCSRNewestRasterResources::use_shared_pool(uint64_t context) {
    if(pool_context==context)return;
    // Attachment precedes the first packing operation. A backend/context change
    // creates a fresh view owner, rather than relocating live atlas coordinates.
    ERR_FAIL_COND(!pool->slots.is_empty());
    static std::unordered_map<uint64_t,std::weak_ptr<Pool>> pools;
    for(auto it=pools.begin();it!=pools.end();)it=it->second.expired()?pools.erase(it):std::next(it);
    auto shared=pools[context].lock();
    if(!shared){shared=std::make_shared<Pool>();pools[context]=shared;}
    --pool->views;pool=shared;++pool->views;atlas=pool->atlas;pool_context=context;
}
void HCSRNewestRasterResources::track_entry(Entry &entry) {
    entry.lease=++pool->next_lease;pool->slots.insert(entry.lease,{entry});
    const auto padded=entry.rect.grow(1);auto &bounds=pool->bounds[entry.page];
    bounds=bounds.has_area()?bounds.merge(padded):padded;
}
bool HCSRNewestRasterResources::release_pool_entry(const Entry &entry) {
    if(!entry.lease)return false;
    auto *slot=pool->slots.getptr(entry.lease);ERR_FAIL_NULL_V(slot,false);
    if(--slot->references)return false;
    if(slot->shared)pool->glyphs.erase(slot->glyph);
    const hcsr_atlas_entry_t allocation{entry.page-1,{entry.rect.position.x,entry.rect.position.y,entry.rect.size.x,entry.rect.size.y}};
    const bool released=hcsr_atlas_release(atlas,&allocation)==HCSR_OK;
    pool->slots.erase(entry.lease);pool->bounds_dirty=true;if(released)++pool->reclaimed;return released;
}
bool HCSRNewestRasterResources::refresh_pool_reclamation() {
    if(pool_reclamation_seen==pool->reclaimed)return false;
    pool_reclamation_seen=pool->reclaimed;
    if(!failed_allocation_count)return false;
    retry_failed_allocations();return true;
}
bool HCSRNewestRasterResources::acquire_shared_glyph(const GlyphKey &key,Entry &entry) {
    auto *lease=pool->glyphs.getptr(key);if(!lease)return false;
    auto &slot=pool->slots[*lease];++slot.references;entry=slot.entry;return true;
}
void HCSRNewestRasterResources::share_glyph(const GlyphKey &key,const Entry &entry) {
    if(!entry.lease)return;
    auto &slot=pool->slots[entry.lease];slot.entry=entry;slot.glyph=key;slot.shared=true;pool->glyphs.insert(key,entry.lease);
}
void HCSRNewestRasterResources::refresh_pool_bounds() const {
    if(!pool->bounds_dirty)return;
    for(auto &bounds:pool->bounds)bounds=Rect2i();
    for(const auto &item:pool->slots) {
        const auto &entry=item.value.entry;auto &bounds=pool->bounds[entry.page];
        const auto padded=entry.rect.grow(1);bounds=bounds.has_area()?bounds.merge(padded):padded;
    }
    pool->bounds_dirty=false;
}
Rect2i HCSRNewestRasterResources::page_live_bounds(int page) const {refresh_pool_bounds();return pool->bounds[page];}
Size2i HCSRNewestRasterResources::page_size(int page) const {
    if(!page)return Size2i(1,1);
    const auto bounds=page_live_bounds(page);int size=1024;
    while(size<MAX(bounds.get_end().x,bounds.get_end().y) && size<PAGE_SIZE)size*=2;
    return Size2i(size,size);
}
RID HCSRNewestRasterResources::pool_rd_texture(int page) const {return page<pool->pages.size()?pool->pages[page].texture:RID();}
uint64_t HCSRNewestRasterResources::pool_binding_revision() const {return pool->bindings;}
bool HCSRNewestRasterResources::prepare_pool_rd(RenderingDevice *device,uint64_t &bytes) {
    using RD=RenderingDevice;
    if(pool->device && pool->device!=device)return false;
    pool->device=device;
    const bool dirty=pool->uploaded!=atlas_revision();
    if(!dirty && !pool->pages.is_empty())return true;
    if(pool->pages.resize(page_count())!=OK)return false;
    for(int index=0;index<pool->pages.size();++index) {
        auto &page=pool->pages.write[index];
        if(page_live_allocations(index)==0) {
            if(page.texture.is_valid()){device->free_rid(page.texture);page.texture=RID();++pool->bindings;++pool->retired_pages;}
            acknowledge_upload(index);continue;
        }
        const auto size=page_size(index);
        RD::TextureFormat format;format.format=RD::DATA_FORMAT_R8G8B8A8_UNORM;format.width=size.x;format.height=size.y;
        format.usage_bits=RD::TEXTURE_USAGE_SAMPLING_BIT|RD::TEXTURE_USAGE_CAN_COPY_TO_BIT|RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
        const bool restore=!page.texture.is_valid();
        if(restore || page.size!=size) {
            auto replacement=device->texture_create(format,RD::TextureView());if(!replacement.is_valid())return false;
            if(device->texture_clear(replacement,Color(0,0,0,0),0,1,0,1)!=OK){device->free_rid(replacement);return false;}
            if(!restore && device->texture_copy(page.texture,replacement,Vector3(),Vector3(),Vector3(MIN(size.x,page.size.x),MIN(size.y,page.size.y),1),0,0,0,0)!=OK){device->free_rid(replacement);return false;}
            if(!restore){device->free_rid(page.texture);++pool->retired_pages;}
            page.texture=replacement;page.size=size;++pool->bindings;
        }
        auto updates=page_updates(index);
        if(restore){updates.clear();const auto bounds=page_live_bounds(index);if(bounds.has_area())updates.push_back(bounds);}
        for(const auto &rect:updates) {
            auto pixels=page_pixels(index,rect);for(int i=0;i<pixels.size();i+=4)SWAP(pixels.write[i],pixels.write[i+2]);
            format.width=rect.size.x;format.height=rect.size.y;format.usage_bits=RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
            Vector<Vector<uint8_t>> data;data.push_back(pixels);
            auto patch=device->texture_create(format,RD::TextureView(),data);if(!patch.is_valid())return false;
            auto error=device->texture_copy(patch,page.texture,Vector3(),Vector3(rect.position.x,rect.position.y,0),Vector3(rect.size.x,rect.size.y,1),0,0,0,0);
            device->free_rid(patch);if(error!=OK)return false;bytes+=pixels.size();
        }
        acknowledge_upload(index);
    }
    pool->uploaded=atlas_revision();return true;
}
uint32_t HCSRNewestRasterResources::pool_gl_texture() const {return pool->gl_texture;}
int HCSRNewestRasterResources::pool_gl_layers() const {return pool->gl_layers;}
int HCSRNewestRasterResources::pool_gl_extent() const {return pool->gl_extent;}
bool HCSRNewestRasterResources::prepare_pool_gl(uint64_t &bytes) {
#ifdef GLES3_ENABLED
    if(pool->uploaded==atlas_revision() && pool->gl_texture)return true;
    int layers=1,extent=1;
    for(int i=1;i<page_count();++i)if(page_live_allocations(i)>0){layers=MAX(layers,i);extent=MAX(extent,page_size(i).x);}
    const bool resized=!pool->gl_texture || layers!=pool->gl_layers || extent!=pool->gl_extent;
    if(!pool->gl_texture)glGenTextures(1,&pool->gl_texture);
    glActiveTexture(GL_TEXTURE9);glBindTexture(GL_TEXTURE_2D_ARRAY,pool->gl_texture);
    if(resized) {
        if(pool->gl_layers)pool->retired_pages+=pool->gl_layers;
        glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_SWIZZLE_R,GL_BLUE);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_SWIZZLE_B,GL_RED);
        glTexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_RGBA8,extent,extent,layers,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        GLuint fbo=0;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo);const GLfloat clear[4]={};
        for(int i=0;i<layers;++i){glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,pool->gl_texture,0,i);
            if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){glDeleteFramebuffers(1,&fbo);return false;}glClearBufferfv(GL_COLOR,0,clear);}
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);glDeleteFramebuffers(1,&fbo);
        pool->gl_layers=layers;pool->gl_extent=extent;++pool->bindings;
    }
    for(int i=1;i<page_count();++i) {
        auto updates=page_updates(i);
        if(resized){updates.clear();const auto bounds=page_live_bounds(i);if(bounds.has_area())updates.push_back(bounds);}
        for(const auto &rect:updates) {
            auto pixels=page_pixels(i,rect);glTexSubImage3D(GL_TEXTURE_2D_ARRAY,0,rect.position.x,rect.position.y,i-1,rect.size.x,rect.size.y,1,GL_RGBA,GL_UNSIGNED_BYTE,pixels.ptr());bytes+=pixels.size();
        }
        if(glGetError()!=GL_NO_ERROR)return false;
        acknowledge_upload(i);
    }
    pool->uploaded=atlas_revision();return true;
#else
    return false;
#endif
}
Dictionary HCSRNewestRasterResources::pool_statistics() const {
    Dictionary result;result["atlas_pool_id"]=int64_t(atlas);result["atlas_pool_views"]=pool->views;
    result["atlas_pool_shared"]=pool_context!=0;
    result["shared_atlas_retired_pages"]=int64_t(pool->retired_pages);
    result["gpu_atlas_retired_pages"]=int64_t(pool->retired_pages);
    result["shared_atlas_entries"]=pool->slots.size();result["shared_atlas_glyphs"]=pool->glyphs.size();
    uint64_t bytes=0;for(auto &page:pool->pages)if(page.texture.is_valid())bytes+=uint64_t(page.size.x)*page.size.y*4;
    if(pool->gl_texture)bytes+=uint64_t(pool->gl_extent)*pool->gl_extent*pool->gl_layers*4;
    result["shared_atlas_gpu_bytes"]=int64_t(bytes);return result;
}

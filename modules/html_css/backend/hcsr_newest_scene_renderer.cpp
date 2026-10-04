#include "hcsr_gpu_packet.h"
#include "hcsr_prepared_drawing.h"
#include "hcsr_shader_sources.h"
#include "hcsr_compositing.h"
#include "hcsr_document_input_shader.h"
#include "hcsr_newest_scene_renderer.h"
#include "hcsr_image_codec.h"
#include "core/os/os.h"
#include <cmath>

hcsr_gpu_state_t HCSRNewestSceneRenderer::reference_state(const hcsr_draw_packet_view_t &packet,uint32_t index) const {
    return hcsr::render::query_reference_state(packet,index,local_hierarchy?&hierarchy:nullptr);
}

bool HCSRNewestSceneRenderer::prepare_coverage(RenderingDevice *device,const Size2i &size) {
    using RD=RenderingDevice;
    if(coverage.program.groups.empty())return true;
    const char *sources[]={hcsr::shaders::coverage_glsl,hcsr::shaders::region_glsl,hcsr::shaders::region_vertex_glsl,hcsr::shaders::region_fragment_glsl};
    for(int i=0;i<3;i++)if(!coverage.shaders[i].is_valid()) {
        Vector<RD::ShaderStageSPIRVData> stages;
        for(int j=0;j<(i==2?2:1);j++) {
            RD::ShaderStageSPIRVData stage;String error;
            stage.shader_stage=i==2?(j==0?RD::SHADER_STAGE_VERTEX:RD::SHADER_STAGE_FRAGMENT):RD::SHADER_STAGE_COMPUTE;
            stage.spirv=device->shader_compile_spirv_from_source(stage.shader_stage,sources[i+j],RD::SHADER_LANGUAGE_GLSL,&error);
            if(stage.spirv.is_empty()){ERR_PRINT(error);return false;}
            stages.push_back(stage);
        }
        coverage.shaders[i]=device->shader_create_from_spirv(stages,"HCSR GPU compositing regions");
        if(!coverage.shaders[i].is_valid())return false;
        if(i<2) {
            coverage.pipelines[i]=device->compute_pipeline_create(coverage.shaders[i]);
            if(!coverage.pipelines[i].is_valid())return false;
        }
    }
    const uint32_t bytes[]={uint32_t(coverage.words.size()*4),uint32_t(coverage.program.groups.size()*16),uint32_t(coverage.program.groups.size()*48),uint32_t(coverage.program.groups.size()*12)};
    RID *buffers[]={&coverage.data,&coverage.output,&coverage.controls,&coverage.arguments};
    for(int i=0;i<4;i++)if(bytes[i]>coverage.capacities[i]) {
        for(RID rid:{coverage.uniform,coverage.region_uniform,coverage.clear_uniform})if(rid.is_valid())device->free_rid(rid);
        coverage.uniform=coverage.region_uniform=coverage.clear_uniform=RID();coverage.dirty=true;coverage.revision=0;coverage.target=Size2i();
        release_groups(device);
        if(buffers[i]->is_valid())device->free_rid(*buffers[i]);
        coverage.capacities[i]=MAX(bytes[i],65536u);
        BitField<RD::StorageBufferUsage> usage;if(i==3)usage=RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT;
        *buffers[i]=device->storage_buffer_create(coverage.capacities[i],{},usage);
        if(!buffers[i]->is_valid())return false;
    }
    auto bindings=[&](RID &rid,int shader_index,const Vector<RID> &resources) {
        if(rid.is_valid())return true;
        Vector<RD::Uniform> uniforms;
        for(int i=0;i<resources.size();i++){RD::Uniform u;u.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER;u.binding=i;u.append_id(resources[i]);uniforms.push_back(u);}
        rid=device->uniform_set_create(uniforms,coverage.shaders[shader_index],0);return rid.is_valid();
    };
    Vector<RID> inputs;inputs.push_back(coverage.data);inputs.push_back(state_buffer);inputs.push_back(coverage.output);
    if(!bindings(coverage.uniform,0,inputs))return false;
    inputs.clear();inputs.push_back(coverage.output);inputs.push_back(coverage.controls);inputs.push_back(coverage.arguments);
    if(!bindings(coverage.region_uniform,1,inputs))return false;
    inputs.clear();inputs.push_back(coverage.controls);
    if(!bindings(coverage.clear_uniform,2,inputs))return false;
    const Vector2 logical(logical_width,logical_height);
    const bool evaluate=coverage.dirty || coverage.logical!=logical || !local_hierarchy || coverage.revision!=hierarchy.revision;
    if(coverage.dirty) {
        if(device->buffer_update(coverage.data,0,bytes[0],coverage.words.data())!=OK)return false;
        coverage.uploaded_bytes+=bytes[0];
    }
    if(evaluate) {
        auto list=device->compute_list_begin();device->compute_list_bind_compute_pipeline(list,coverage.pipelines[0]);
        device->compute_list_bind_uniform_set(list,coverage.uniform,0);
        for(const auto &level:coverage.program.levels) {
            const struct {uint32_t first,count,offset,reserved;float width,height,padding[2];} push{level.first,level.count,0,0,logical_width,logical_height,{0,0}};
            device->compute_list_set_push_constant(list,&push,sizeof(push));device->compute_list_dispatch(list,level.count,1,1);
            device->compute_list_add_barrier(list);
        }
        device->compute_list_end();coverage.revision=hierarchy.revision;coverage.logical=logical;++coverage.evaluations;
    }
    if(evaluate || coverage.target!=size) {
        const struct {uint32_t count;float width,height;uint32_t target_width,target_height,padding[3];} push{uint32_t(coverage.program.groups.size()),logical_width,logical_height,uint32_t(size.x),uint32_t(size.y),{0,0,0}};
        auto list=device->compute_list_begin();device->compute_list_bind_compute_pipeline(list,coverage.pipelines[1]);
        device->compute_list_bind_uniform_set(list,coverage.region_uniform,0);device->compute_list_set_push_constant(list,&push,sizeof(push));
        device->compute_list_dispatch(list,(coverage.program.groups.size()+63)/64,1,1);device->compute_list_end();
        coverage.target=size;
    }
    coverage.dirty=false;return true;
}

bool HCSRNewestSceneRenderer::copy_group_region(RenderingDevice *device,GroupPool &pool,RID source,size_t event,uint32_t depth) {
    using RD=RenderingDevice;
    if(!blend_copy_shader.is_valid()) {
        RD::ShaderStageSPIRVData stage;stage.shader_stage=RD::SHADER_STAGE_COMPUTE;String error;
        stage.spirv=device->shader_compile_spirv_from_source(stage.shader_stage,hcsr::shaders::region_copy_glsl,RD::SHADER_LANGUAGE_GLSL,&error);
        if(stage.spirv.is_empty()){ERR_PRINT(error);return false;}
        Vector<RD::ShaderStageSPIRVData> stages;stages.push_back(stage);
        blend_copy_shader=device->shader_create_from_spirv(stages,"HCSR GPU blend region copy");
        if(!blend_copy_shader.is_valid())return false;
        blend_copy_pipeline=device->compute_pipeline_create(blend_copy_shader);
    }
    if(!blend_copy_pipeline.is_valid() || !depth)return false;
    if(pool.blend_copies.size()<int(depth) && pool.blend_copies.resize(depth)!=OK)return false;
    auto &copy=pool.blend_copies.write[depth-1];
    // RenderingDevice invalidates dependent uniform sets when an output/source
    // texture is destroyed. A nonzero RID alone does not prove it is still live.
    if(copy.uniform.is_valid() && !device->uniform_set_is_valid(copy.uniform))copy.uniform=RID();
    // Filter/underlay swaps can change a depth's source texture. These are GPU
    // descriptors owned by the output, not a scene/transform validity cache.
    if(copy.source!=source) {
        if(copy.uniform.is_valid())device->free_rid(copy.uniform);
        copy.uniform=RID();copy.source=source;
    }
    if(!copy.uniform.is_valid()) {
        Vector<RD::Uniform> bindings;
        RD::Uniform controls;controls.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER;controls.binding=0;controls.append_id(coverage.controls);bindings.push_back(controls);
        RD::Uniform destination;destination.uniform_type=RD::UNIFORM_TYPE_IMAGE;destination.binding=1;destination.append_id(pool.blend_texture);bindings.push_back(destination);
        RD::Uniform texture;texture.uniform_type=RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;texture.binding=2;texture.append_id(sampler);texture.append_id(source);bindings.push_back(texture);
        copy.uniform=device->uniform_set_create(VectorView(bindings.ptr(),bindings.size()),blend_copy_shader,0);
        if(!copy.uniform.is_valid())return false;
    }
    const uint32_t index=uint32_t(bounds_program.group_index(event));
    const uint32_t push[]={index,0,0,0};
    auto list=device->compute_list_begin();
    device->compute_list_bind_compute_pipeline(list,blend_copy_pipeline);
    device->compute_list_bind_uniform_set(list,copy.uniform,0);
    device->compute_list_set_push_constant(list,push,sizeof(push));
    device->compute_list_dispatch_indirect(list,coverage.arguments,index*12);
    device->compute_list_end();++gpu_blend_region_copies;
    return true;
}

bool HCSRNewestSceneRenderer::clear_group(RenderingDevice *device,RenderingDevice::DrawListID list,RID framebuffer,const Size2i &size,size_t event) {
    using RD=RenderingDevice;
    if(!coverage.pipelines[2].is_valid()) {
        RD::PipelineColorBlendState blend;blend.attachments.resize(1);
        coverage.pipelines[2]=device->render_pipeline_create(coverage.shaders[2],device->framebuffer_get_format(framebuffer),RD::INVALID_ID,RD::RENDER_PRIMITIVE_TRIANGLES,RD::PipelineRasterizationState(),RD::PipelineMultisampleState(),RD::PipelineDepthStencilState(),blend);
        if(!coverage.pipelines[2].is_valid())return false;
    }
    const struct {uint32_t region;float width,height;uint32_t reserved;} push{uint32_t(bounds_program.group_index(event)),float(size.x),float(size.y),0};
    device->draw_list_bind_render_pipeline(list,coverage.pipelines[2]);
    device->draw_list_bind_uniform_set(list,coverage.clear_uniform,0);
    device->draw_list_set_push_constant(list,&push,sizeof(push));
    device->draw_list_draw(list,false,1,6);++coverage.clears;++last_draw_calls;return true;
}

bool HCSRNewestSceneRenderer::prepare(const hcsr_draw_packet_view_t &submitted, const Ref<HTMLDocument> &document, float output_scale, const hcsr_backdrop_view_t &backdrop,const hcsr_hierarchy_view_t &local,const hcsr_raster_demand_view_t &raster_demand) {
    const auto &packet=submitted;
    const bool had_pending = resources.has_pending_glyphs();
    const bool next_gpu = hcsr::render::gpu_drawing(packet.format);
    if(next_gpu && (raster_demand.count!=packet.gpu.state_count || (raster_demand.count && !raster_demand.scales)))return false;
    const auto appearance_scale = [&](const hcsr_draw_item_t &draw) {
        const auto index=hcsr::render::drawing_state(packet,hcsr::render::draw_vertex_index(packet,draw,0));
        return raster_demand.scales[index];
    };
    const bool next_hierarchy=next_gpu && local.revision!=0;
    const bool retained_surfaces=packet.struct_size>=sizeof(packet) && packet.format==HCSR_DRAW_PACKET_FORMAT_SURFACES
        && gpu_geometry && geometry_generation==packet.gpu.geometry_generation;
    if (!hcsr::render::validate_gpu_packet(packet,next_hierarchy,!retained_surfaces)) return false;
    if(next_hierarchy && hierarchy.geometry!=local.geometry_generation)hierarchy_uploaded_revision=0;
    if(next_hierarchy && !hierarchy.prepare(local,packet.gpu.geometry_generation,packet.gpu.state_count,packet.gpu.clip_count))return false;
    if(!next_hierarchy) {hierarchy.clear();hierarchy_uploaded_revision=0;}
    local_hierarchy=next_hierarchy;gpu_state_count=next_gpu?packet.gpu.state_count:0;
    auto copy_table = [](auto &destination, const auto *source, size_t count) {
        if(destination.resize(int(count))!=OK) return false;
        if (count) memcpy(destination.ptrw(),source,count*sizeof(*source));
        return true;
    };
    if(next_gpu) {
        if(local_hierarchy)gpu_states.clear();
        else if(!copy_table(gpu_states,packet.gpu.states,packet.gpu.state_count)) return false;
        if(!gpu_geometry || geometry_generation!=packet.gpu.geometry_generation) {
            if(!copy_table(gpu_clips,packet.gpu.clips,packet.gpu.clip_count)
                || !copy_table(gpu_planes,packet.gpu.planes,packet.gpu.plane_count)) return false;
        }
    } else { gpu_states.clear(); gpu_clips.clear(); gpu_planes.clear(); }
    logical_width=packet.viewport_width; logical_height=packet.viewport_height;
    Vector<uint64_t> next_backdrop_identities;
    if(backdrop.surface_count && (!next_gpu || !backdrop.surfaces))return false;
    if(next_backdrop_identities.resize(int(backdrop.surface_count))!=OK)return false;
    for(size_t i=0;i<backdrop.surface_count;++i) {
        const auto &surface=backdrop.surfaces[i];
        if(surface.gpu_state>=packet.gpu.state_count || !surface.appearance.raster.identity)return false;
        next_backdrop_identities.write[i]=surface.appearance.raster.identity;
    }
    if(next_gpu && gpu_geometry && geometry_generation==packet.gpu.geometry_generation && prepared_scale==output_scale
        && !had_pending && next_backdrop_identities==backdrop_identities) {
        uploaded=false; geometry_dirty=false;
        update_compositing_bounds(packet);
        return true;
    }
    gpu_geometry=next_gpu; geometry_generation=next_gpu ? packet.gpu.geometry_generation : 0;
    geometry_dirty=true; prepared_scale=output_scale; primitives.clear();
	vertices.clear();
	batches.clear();
    document_regions.clear();document_regions_dirty=true;
	hcsr::render::compositing_plan group_plan;
    std::string group_error;
    // GPU pass topology needs no projected coordinates. The coverage program
    // supplies them once below for the remaining host scheduling consumers.
    if (!hcsr::render::plan_compositing(packet, group_plan, group_error,next_hierarchy,next_hierarchy?&hierarchy:nullptr,!gpu_geometry,gpu_geometry)) return false;
    std::vector<hcsr::render::ordered_backdrop> backdrop_plan;
    if(gpu_geometry && !hcsr::render::plan_ordered_backdrops(packet,group_plan,backdrop,backdrop_plan,group_error))return false;
    ordered_backdrops=gpu_geometry && !backdrop_plan.empty();
    document_backdrops=gpu_geometry && std::any_of(backdrop_plan.begin(),backdrop_plan.end(),[](const auto &op){return op.document_input;});
    const bool prefix_snapshots=std::any_of(backdrop_plan.begin(),backdrop_plan.end(),[](const auto &op){return !op.prefix_events.empty();});
    snapshot_target=document_backdrops || prefix_snapshots;
    bounds_program.build(packet,group_plan,gpu_geometry?&backdrop:nullptr);
    if(gpu_geometry) {
        coverage.program.build(packet,bounds_program);coverage.words=coverage.program.words();coverage.dirty=true;coverage.target=Size2i();
    } else {coverage.program={};coverage.words.clear();}
    HashSet<uint64_t> live_surfaces;
    Vector<String> image_sources;
    resources.begin_asset_retention(document);
    for (size_t i=0; i<packet.draw_item_count; ++i) {
        const auto &draw=packet.draw_items[i];
        const auto &material=packet.materials[draw.material_index];
        if(group_plan.events[i].mask.kind==1) {hcsr_raster_material_t mask;memcpy(&mask,group_plan.events[i].mask.data,sizeof(mask));live_surfaces.insert(mask.identity);}
        if(draw.index_count && (material.kind==HCSR_MATERIAL_IMAGE || material.kind==HCSR_MATERIAL_GLYPH)) {
            if(material.payload_offset>packet.material_payload_size || material.payload_size>packet.material_payload_size-material.payload_offset)return false;
            if(material.kind==HCSR_MATERIAL_IMAGE) {
                if(material.payload_size<sizeof(hcsr_image_material_t))return false;
                hcsr_image_material_t image;memcpy(&image,packet.material_payload+material.payload_offset,sizeof(image));
                if(image.source_length>material.payload_size-sizeof(image))return false;
                const String source=String::utf8((const char *)(packet.material_payload+material.payload_offset+sizeof(image)),image.source_length);
                image_sources.push_back(source);resources.retain_image_source(source);
            } else {
                if(material.payload_size<sizeof(hcsr_glyph_material_t))return false;
                hcsr_glyph_material_t glyph;memcpy(&glyph,packet.material_payload+material.payload_offset,sizeof(glyph));
                resources.retain_glyph(glyph);
            }
        }
        if (material.kind!=HCSR_MATERIAL_RASTER && material.kind!=HCSR_MATERIAL_RASTER_SLICE) continue;
        const size_t expected = material.kind==HCSR_MATERIAL_RASTER_SLICE ? sizeof(hcsr_raster_slice_material_t) : sizeof(hcsr_raster_material_t);
        if (material.payload_size!=expected || material.payload_offset>packet.material_payload_size
            || material.payload_size>packet.material_payload_size-material.payload_offset) return false;
        hcsr_raster_material_t raster;
        memcpy(&raster,packet.material_payload+material.payload_offset,sizeof(raster));
        live_surfaces.insert(raster.identity);
    }
    for(uint64_t identity:next_backdrop_identities)live_surfaces.insert(identity);
    resources.end_asset_retention();
    resources.retain_surfaces(live_surfaces);
    resources.advance_rasterization();
    int next_image_source=0;
    if(backdrop_entries.resize(int(backdrop.surface_count))!=OK)return false;
    for(size_t i=0;i<backdrop.surface_count;++i) {
        const auto &raster=backdrop.surfaces[i].appearance.raster;
        auto entry=resources.resolve_raster(raster,Vector2(raster.width,raster.height));
        if(entry.page<0 || entry.rect.size!=Size2i(raster.width,raster.height))return false;
        backdrop_entries.write[i]=entry;
    }
    backdrop_identities=next_backdrop_identities;
    group_depth = group_plan.depth;
	uploaded = false;
    // Reserve by emitted records in 16-byte words, including compositor support.
    // Compact surfaces need four words, regardless of their six shader vertices.
    // Acquire writable storage once rather than repeating CowData checks.
    uint64_t emitted_count = backdrop.surface_count*4;
    for (size_t i = 0; i < packet.draw_item_count; i++) {
        const auto &draw=packet.draw_items[i];
        emitted_count += packet.format==HCSR_DRAW_PACKET_FORMAT_SURFACES ? (draw.index_count?4:0)
            : uint64_t(hcsr::render::scene_quad(packet,draw)?4:draw.index_count)*4;
        if (group_plan.events[i].kind == HCSR_GROUP_END) emitted_count += 4*(6 + 14 * group_plan.events[i].filters.size() + ((group_plan.events[i].mask.kind || group_plan.events[i].blend_mode)?8:0));
        if (emitted_count > INT32_MAX/sizeof(hcsr::render::scene_word)) return false;
    }
    for(const auto &op:backdrop_plan)if(gpu_geometry) {const auto &effect=backdrop.effects[op.effect];
        emitted_count+=24*(op.prefix_events.size()+(op.prefix_events.empty() || op.document_input?0:1));
        emitted_count+=4*(uint64_t(effect.surface_count)*(14+effect.operation_count)+uint64_t(effect.operation_count)*20+6);
        // One immutable support descriptor per effect, shared by its blur
        // passes and document snapshot, with each patch placement binding.
        emitted_count+=4*(2+2*uint64_t(effect.surface_count));
        if(emitted_count>INT32_MAX/sizeof(hcsr::render::scene_word))return false;
    }
    if (vertices.resize(int(emitted_count)) != OK) return false;
    auto *vertex_data = vertices.ptrw();
    uint32_t written = 0;
    auto append_vertex=[&](const Vertex &v){memcpy(vertex_data+written,&v,sizeof(v));written+=4;};
    auto append_surface=[&](const hcsr::render::scene_surface &v){memcpy(vertex_data+written,&v,sizeof(v));written+=4;};
    std::vector<hcsr::render::filter_pass> filter_passes;
    size_t backdrop_cursor=0;
    std::vector<bool> underlay(group_plan.depth+1,false);
    auto emit_backdrop=[&](const hcsr::render::ordered_backdrop &op) {
        if(!gpu_geometry)return;
        const auto &effect=backdrop.effects[op.effect];
        if(!effect.surface_count)return;
        underlay[op.destination_depth]=true;
        std::vector<hcsr_filter_operation_t> operations;
        for(uint32_t i=0;i<effect.operation_count;++i){const auto &src=backdrop.operations[effect.first_operation+i];hcsr_filter_operation_t value{};value.kind=src.kind;value.amount=src.amount;operations.push_back(value);}
        hcsr::render::filter_program program{reinterpret_cast<const uint8_t *>(operations.data()),uint32_t(operations.size())};
        hcsr::render::plan_filter_passes(program,filter_passes);
        const bool spatial=std::any_of(filter_passes.begin(),filter_passes.end(),[](const auto &pass){return pass.axis!=0;});
        size_t intermediates=spatial?filter_passes.size():0;
        if(intermediates && filter_passes.back().axis==0)--intermediates;
        const bool snapshot=op.document_input || !op.prefix_events.empty();
        // Local appearance bounds are immutable inputs. Projection and the
        // physical-pixel blur halo are evaluated by the vertex shader.
        const uint32_t blur_state=backdrop.surfaces[effect.first_surface].gpu_state;
        float blur_halo_x=0,blur_halo_y=0,blur_rounding=0;
        for(const auto &pass:filter_passes) {
            if(pass.axis==1)blur_halo_x+=3*pass.sigma;
            if(pass.axis==2)blur_halo_y+=3*pass.sigma;
            if(pass.axis)++blur_rounding;
        }
        const uint32_t effect_region=written;
        auto region_parameter=[&](float x,float y,float z,float w){Vertex v={};v.state=UINT32_MAX;v.position_uv[0]=x;v.position_uv[1]=y;v.position_uv[2]=z;v.position_uv[3]=w;append_vertex(v);};
        region_parameter(effect.surface_count,blur_state+1,blur_halo_x,blur_halo_y);
        region_parameter(blur_rounding,0,0,0);
        for(uint32_t i=0;i<effect.surface_count;++i) {
            const auto &patch=backdrop.surfaces[effect.first_surface+i];
            const auto &r=patch.appearance.raster.local_rect;
            region_parameter(r.x,r.y,r.width,r.height);
            region_parameter(patch.gpu_state+1,0,0,0);
        }
        const uint32_t document_region=document_regions.size();
        if(op.document_input)document_regions.push_back(effect_region);
        uint32_t source=snapshot?group_plan.depth+2:op.source_depth-1;
        bool document_source=op.document_input;
        auto prefix=[&](uint32_t layer,size_t event,bool clear) {
            Batch batch{0,uint32_t(primitives.size()),1,6,op.destination_depth,1,Rect2(0,0,1,1),event};
            batch.backdrop=true;batch.backdrop_prefix=true;batch.backdrop_first=clear;
            batch.document_source=document_source;batch.document_region=document_region;batch.backdrop_source=layer;
            batch.backdrop_source_event=event;batch.backdrop_destination=group_plan.depth+2;
            batches.push_back(batch);primitives.push_back({written,2,UINT32_MAX});
            constexpr float corners[][2]={{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
            for(const auto &c:corners){Vertex v={};v.state=UINT32_MAX;v.position_uv[0]=c[0]*2-1;v.position_uv[1]=c[1]*2-1;v.position_uv[2]=c[0];v.position_uv[3]=c[1];v.tint[3]=1;v.bounds[0]=-3;append_vertex(v);}
            document_source=false;
        };
        if(!op.prefix_events.empty()) {
            if(!op.document_input)prefix(op.source_depth-1,op.source_event,true);
            for(const size_t event:op.prefix_events)prefix(group_plan.events[event].depth-1,event,false);
        }
        size_t mask_batch_begin=SIZE_MAX;
        auto emit=[&](hcsr::render::filter_program colors,float sigma,uint32_t axis,const hcsr_backdrop_surface_t *surface,uint32_t destination) {
            const uint32_t parameters=written;
            auto parameter=[&](float x,float y,float z,float w){Vertex v={};v.state=UINT32_MAX;v.position_uv[0]=x;v.position_uv[1]=y;v.position_uv[2]=z;v.position_uv[3]=w;append_vertex(v);};
            if(axis)parameter(float(backdrop.surfaces[effect.first_surface].gpu_state+1),sigma,axis==1,axis==2);
            for(const auto &color:colors){Vertex v={};v.state=UINT32_MAX;memcpy(v.position_uv,&color,sizeof(color));append_vertex(v);}
            uint32_t mask_parameters=0;int mask_page=0;
            if(surface) {
                const size_t surface_index=size_t(surface-backdrop.surfaces);const auto &entry=backdrop_entries[surface_index];
                const auto &r=surface->appearance.raster.local_rect,&crop=surface->appearance.source_rect;
                mask_page=entry.page;mask_parameters=written;
                parameter(parameters,colors.size(),4,0);
                parameter(r.x,r.y,r.width,r.height);
                parameter(entry.rect.position.x+crop.x,entry.rect.position.y+crop.y,crop.width,crop.height);
                parameter(surface->gpu_state+1,logical_width,logical_height,0);
                for(int row=0;row<4;++row)parameter(0,0,0,0);
            }
            const uint32_t blur_parameters=axis?effect_region:0;
            const uint32_t primitive=uint32_t(primitives.size());primitives.push_back({written,2,UINT32_MAX});
            constexpr float corners[][2]={{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
            for(const auto &c:corners){Vertex v={};v.state=UINT32_MAX;v.position_uv[0]=c[0]*2-1;v.position_uv[1]=c[1]*2-1;v.position_uv[2]=c[0];v.position_uv[3]=c[1];v.tint[3]=1;
                v.bounds[0]=surface?-6.f:axis?-5.f:colors.empty()?-3.f:-4.f;v.bounds[1]=surface?mask_parameters:parameters;v.bounds[2]=axis?blur_parameters:colors.size();v.bounds[3]=4;append_vertex(v);}
            Batch batch{0,primitive,1,6,op.destination_depth,1,Rect2(0,0,1,1),effect.before_draw_index};
            batch.mask_page=mask_page;batch.backdrop=true;batch.backdrop_mask=surface!=nullptr;
            batch.backdrop_first=surface && surface==&backdrop.surfaces[effect.first_surface];
            batch.document_source=document_source;batch.document_region=document_region;
            batch.backdrop_source=source;batch.backdrop_destination=destination;batch.backdrop_source_event=source<group_plan.depth?op.source_event:SIZE_MAX;
            // Only combine adjacent mask slices within this effect. The first
            // batch retains its clear/snapshot flags; atlas changes still split.
            if(surface && size_t(batches.size())>mask_batch_begin && batches[batches.size()-1].mask_page==batch.mask_page &&
                    batches[batches.size()-1].first+batches.write[batches.size()-1].count==primitive) {
                ++batches.write[batches.size()-1].count;
            } else batches.push_back(batch);
        };
        for(size_t i=0;i<intermediates;++i){const auto &pass=filter_passes[i];const uint32_t destination=group_plan.depth+uint32_t(i%2);
            emit(pass.colors,pass.sigma,pass.axis,nullptr,destination);source=destination;document_source=false;}
        const auto colors=spatial?(intermediates<filter_passes.size()?filter_passes.back().colors:hcsr::render::filter_program{}):program;
        mask_batch_begin=batches.size();
        for(uint32_t i=0;i<effect.surface_count;++i){emit(colors,0,0,&backdrop.surfaces[effect.first_surface+i],op.destination_depth-1);document_source=false;}
    };
	for (size_t i = 0; i < packet.draw_item_count; i++) {
        while(backdrop_cursor<backdrop_plan.size() && backdrop_plan[backdrop_cursor].before_draw==i)emit_backdrop(backdrop_plan[backdrop_cursor++]);
		const hcsr_draw_item_t &draw = packet.draw_items[i];
		const hcsr_material_t &material = packet.materials[draw.material_index];
        const auto group = group_plan.events[i];
        if (group.kind) {
            const Rect2 region(group.bounds.x/packet.viewport_width,group.bounds.y/packet.viewport_height,group.bounds.width/packet.viewport_width,group.bounds.height/packet.viewport_height);
            if(group.kind==HCSR_GROUP_BEGIN) {
                underlay[group.depth]=false;
                batches.push_back({0,gpu_geometry?uint32_t(primitives.size()):written,0,group.kind,group.depth,group.opacity,region,i});
                batches.write[batches.size()-1].opacity_state_plus_one=group.opacity_state_plus_one;
                continue;
            }
            // Descendant sampling sees the root's foreground, excluding its
            // own filtered backdrop. Merge that underlay only when closing it.
            if(underlay[group.depth]) {
                Batch merge{0,uint32_t(primitives.size()),1,6,group.depth,1,Rect2(0,0,1,1),i};
                merge.backdrop=true;merge.backdrop_merge=true;
                merge.backdrop_source=merge.backdrop_destination=group.depth-1;merge.backdrop_source_event=i;
                batches.push_back(merge);primitives.push_back({written,2,UINT32_MAX});
                constexpr float corners[][2]={{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
                for(const auto &c:corners){Vertex v={};v.state=UINT32_MAX;v.position_uv[0]=c[0]*2-1;v.position_uv[1]=c[1]*2-1;v.position_uv[2]=c[0];v.position_uv[3]=c[1];v.tint[3]=1;v.bounds[0]=-3;append_vertex(v);}
            }
            Entry mask_entry;hcsr_raster_material_t mask={};
            if(group.mask.kind) {
                if(group.mask.kind!=1)return false;
                memcpy(&mask,group.mask.data,sizeof(mask));
                mask_entry=resources.resolve_raster(mask,Vector2(mask.width,mask.height));
                if(mask_entry.page<0)return false;
            }
            hcsr::render::plan_filter_passes(group.filters,filter_passes);
            const bool spatial=std::any_of(filter_passes.begin(),filter_passes.end(),[](const auto &pass){return pass.axis!=0;});
            size_t intermediates=spatial?filter_passes.size():0;
            if(intermediates && filter_passes.back().axis==0)--intermediates;
            auto emit_group=[&](uint32_t kind,hcsr::render::filter_program colors,float sigma,uint32_t axis,float opacity,const hcsr_filter_operation_t &shadow) {
                batches.push_back({0,gpu_geometry?uint32_t(primitives.size()):written,gpu_geometry?1u:6u,kind,group.depth,opacity,region,i});
                batches.write[batches.size()-1].source_scratch=kind==hcsr::render::group_shadow_apply;
                const uint32_t parameters_first=written;
                if(axis) {
                    Vertex v={};v.state=UINT32_MAX;v.position_uv[1]=sigma;v.position_uv[2]=axis==1?1:0;v.position_uv[3]=axis==2?1:0;
                    if(kind==hcsr::render::group_shadow_apply) {memcpy(v.tint,shadow.color,sizeof(shadow.color));v.bounds[0]=shadow.reserved[0];v.bounds[1]=shadow.reserved[1];}
                    append_vertex(v);
                }
                for(const auto &op:colors) {
                    Vertex v={};v.state=UINT32_MAX;memcpy(v.position_uv,&op,sizeof(op));
                    append_vertex(v);
                }
                const bool masked=kind==HCSR_GROUP_END && (group.mask.kind || group.blend_mode);
                uint32_t mask_first=0;
                if(masked) {
                    batches.write[batches.size()-1].mask_page=group.mask.kind?mask_entry.page:0;
                    batches.write[batches.size()-1].blend_mode=group.blend_mode;
                    mask_first=written;
                    auto parameter=[&](float x,float y,float z,float w) {Vertex v={};v.state=UINT32_MAX;v.position_uv[0]=x;v.position_uv[1]=y;v.position_uv[2]=z;v.position_uv[3]=w;append_vertex(v);};
                    parameter(float(parameters_first),float(colors.size()),4,0);
                    parameter(mask.local_rect.x,mask.local_rect.y,mask.local_rect.width,mask.local_rect.height);
                    parameter(mask_entry.rect.position.x,mask_entry.rect.position.y,mask_entry.rect.size.x,mask_entry.rect.size.y);
                    parameter(group.mask.state_plus_one,logical_width,logical_height,group.blend_mode);
                    float matrix[16]={};if(group.mask.kind)memcpy(matrix,group.mask.data+sizeof(mask),64);
                    for(int row=0;row<4;++row)parameter(matrix[row*4],matrix[row*4+1],matrix[row*4+2],matrix[row*4+3]);
                }
                if(!gpu_geometry)batches.write[batches.size()-1].first=written;
                if(gpu_geometry) { primitives.push_back({written,2,UINT32_MAX}); }
                const Vector2 corners[] = {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
                for (const auto &corner: corners) {
                    Vertex v={}; v.position_uv[0]=corner.x*2-1; v.position_uv[1]=corner.y*2-1;
                    v.position_uv[2]=corner.x; v.position_uv[3]=corner.y;
                    v.tint[3]=kind==HCSR_GROUP_END && group.opacity_state_plus_one ? -float(group.opacity_state_plus_one) : opacity; v.state=UINT32_MAX;
                    if(kind==HCSR_GROUP_END && group.opacity_state_plus_one)memcpy(&v.pad[0],&opacity,4);
                    v.bounds[0]=masked?-6.0f:kind==hcsr::render::group_shadow_apply?-7.0f:axis?-5.0f:colors.empty()?-3.0f:-4.0f;
                    v.bounds[1]=float(masked?mask_first:parameters_first);v.bounds[2]=float(colors.size());v.bounds[3]=4;
                    append_vertex(v);
                }
            };
            for(size_t step=0;step<intermediates;++step) {
                const auto &pass=filter_passes[step];const uint32_t kind=pass.shadow.kind==9?(pass.axis==1?hcsr::render::group_shadow_blur:hcsr::render::group_shadow_apply):hcsr::render::group_filter_pass;
                emit_group(kind,pass.colors,pass.sigma,pass.axis,1,pass.shadow);
            }
            emit_group(HCSR_GROUP_END,spatial?(intermediates<filter_passes.size()?filter_passes.back().colors:hcsr::render::filter_program{}):group.filters,0,0,group.opacity,{});
            continue;
        }
        if(!draw.index_count)continue;
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
                transform_scale = appearance_scale(draw);
            } else if (draw.index_count) {
                const auto a = hcsr::render::drawing_vertex(packet,hcsr::render::draw_vertex_index(packet,draw,0));
                for (uint32_t j=1; j<draw.index_count; ++j) {
                    const auto b = hcsr::render::drawing_vertex(packet,hcsr::render::draw_vertex_index(packet,draw,j));
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
                const String &source=image_sources[next_image_source++];
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
                for (uint32_t j = 1; !gpu_geometry && j < draw.index_count; j++) {
                    const auto a = hcsr::render::drawing_vertex(packet,hcsr::render::draw_vertex_index(packet,draw,0));
                    const auto b = hcsr::render::drawing_vertex(packet,hcsr::render::draw_vertex_index(packet,draw,j));
                    const float local = Vector2(b.local_x - a.local_x, b.local_y - a.local_y).length();
                    if (local > .001f) transform_scale = MAX(transform_scale, Vector2(b.screen_x - a.screen_x, b.screen_y - a.screen_y).length() / local);
                }
                if(gpu_geometry && draw.index_count) {
                    transform_scale = appearance_scale(draw);
                }
                entry = resources.resolve_image(document, source, natural, destination.size * (output_scale * transform_scale));
			}
		}
        hcsr_glyph_material_t glyph = {};
        const bool is_glyph = material.kind == HCSR_MATERIAL_GLYPH && material.payload_size >= sizeof(glyph);
        if (is_glyph) {
            memcpy(&glyph, packet.material_payload + material.payload_offset, sizeof(glyph));
            float transform_scale = 1;
            for (uint32_t j = 1; !gpu_geometry && j < draw.index_count; j++) {
                const auto a = hcsr::render::drawing_vertex(packet,hcsr::render::draw_vertex_index(packet,draw,0));
                const auto b = hcsr::render::drawing_vertex(packet,hcsr::render::draw_vertex_index(packet,draw,j));
                float local = Vector2(b.local_x - a.local_x, b.local_y - a.local_y).length();
                if (local > .001f) transform_scale = MAX(transform_scale, Vector2(b.screen_x - a.screen_x, b.screen_y - a.screen_y).length() / local);
            }
            if(gpu_geometry && draw.index_count) {
                transform_scale = appearance_scale(draw);
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
            if(packet.format==HCSR_DRAW_PACKET_FORMAT_SURFACES)primitives.push_back({written,3,uint32_t(i),packet.gpu.surfaces[draw.first_index/6].state});
            else hcsr::render::append_scene_primitives(written,draw.index_count,quad,uint32_t(i),
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
        if(packet.format==HCSR_DRAW_PACKET_FORMAT_SURFACES) {
            if(material.kind==HCSR_MATERIAL_VERTEX_COLOR)return false;
            const bool geometry_coverage=material.kind==HCSR_MATERIAL_RASTER || material.kind==HCSR_MATERIAL_RASTER_SLICE
                || (material.kind==HCSR_MATERIAL_ATLAS_GLYPH && (material.flags&HCSR_ATLAS_GEOMETRY_COVERAGE));
            append_surface(hcsr::render::prepare_scene_surface(packet.gpu.surfaces[draw.first_index/6],area,textured?&resolved:nullptr,geometry_coverage));
        } else for (uint32_t j = 0; j < (quad ? 4u : draw.index_count); j++) {
            append_vertex(hcsr::render::prepare_scene_vertex(packet,draw,j,area,
                material.kind == HCSR_MATERIAL_VERTEX_COLOR,textured ? &resolved : nullptr));
        }
	}
    while(backdrop_cursor<backdrop_plan.size())emit_backdrop(backdrop_plan[backdrop_cursor++]);
    // Backdrop quads belong to the same prepared scene geometry. The host
    // mask pass borrows these buffers, rather than owning another placement
    // table or rebuilding mask vertices when transforms change.
    backdrop_first_primitive=primitives.size();
    for(size_t i=0;i<backdrop.surface_count;++i) {
        const auto &surface=backdrop.surfaces[i];const auto entry=backdrop_entries[i];
        const auto &source=surface.appearance.source_rect;const auto &r=surface.appearance.raster.local_rect;
        if(source.x<0 || source.y<0 || source.width<=0 || source.height<=0
            || source.x+source.width>entry.rect.size.x || source.y+source.height>entry.rect.size.y)return false;
        hcsr_atlas_glyph_material_t mask{};
        mask.local_rect=r;
        mask.atlas_rect={float(entry.rect.position.x)+source.x,float(entry.rect.position.y)+source.y,source.width,source.height};
        mask.red=mask.green=mask.blue=1;mask.alpha=surface.appearance.raster.opacity;
        primitives.push_back({written,3,UINT32_MAX,surface.gpu_state});
        append_surface(hcsr::render::prepare_scene_surface({r,surface.gpu_state},{1,mask.alpha},&mask,false));
    }
    vertices.resize(written);
    // Empty and solid-only scenes use the same submission path. A bound
    // placeholder satisfies the shader interface without inventing a second
    // renderer for clears or for missing image resources.
    resources.ensure_sampling_page();
    const bool ordinary_groups = !ordered_backdrops
        && !std::any_of(batches.begin(),batches.end(),[](const Batch &batch){return batch.blend_mode!=0 || batch.kind>=hcsr::render::group_filter_pass;});
    nested_group_passes.clear();
    if(gpu_geometry && ordinary_groups && group_depth)
        hcsr::render::schedule_nested_groups(batches.size(),[&](size_t i) {
            const auto &b=batches[i];
            return hcsr::render::group_event{b.kind,b.depth};
        },group_depth,nested_group_passes);
    // Copies, foreground passes and ordered prefixes consume GPU coverage.
    // Only ordinary disjoint-group scheduling still needs host bounds;
    // reference geometry retains its CPU coverage path.
    host_coverage_required = !gpu_geometry || (ordinary_groups && nested_group_passes.empty());
    if(gpu_geometry) update_compositing_bounds(packet);
	return true;
}

void HCSRNewestSceneRenderer::update_compositing_bounds(const hcsr_draw_packet_view_t &packet) {
    if (!group_depth || !host_coverage_required) return;
    ++cpu_compositing_bounds_evaluations;
    bounds_program.update(packet,local_hierarchy?&hierarchy:nullptr);
    for (auto &batch:batches) {
        if (batch.kind && (!batch.backdrop || (batch.backdrop_prefix && !batch.backdrop_first))) {
            const auto &b=bounds_program.bounds(batch.event_index);
            batch.bounds=Rect2(b.x/logical_width,b.y/logical_height,b.width/logical_width,b.height/logical_height);
        }
    }
}

bool HCSRNewestSceneRenderer::draw_backdrop_mask(RenderingDevice *device,RID target,const hcsr_backdrop_view_t &view,const Size2i &logical,const Size2i &physical) {
    using RD=RenderingDevice;
    if(!prepare_gpu_resources(device))return false;
    Vector<RID> attachments;attachments.push_back(target);
    RID framebuffer=device->framebuffer_create(attachments);
    if(!framebuffer.is_valid())return false;
    if(!pipeline.is_valid()) {auto blend=RD::PipelineColorBlendState::create_blend();blend.attachments.write[0].src_alpha_blend_factor=RD::BLEND_FACTOR_ONE;
        pipeline=device->render_pipeline_create(shader,device->framebuffer_get_format(framebuffer),RD::INVALID_ID,RD::RENDER_PRIMITIVE_TRIANGLES,RD::PipelineRasterizationState(),RD::PipelineMultisampleState(),RD::PipelineDepthStencilState(),blend);}
    if(!pipeline.is_valid()){device->free_rid(framebuffer);return false;}
    const Color clear(0,0,0,1);
    auto list=device->draw_list_begin(framebuffer,RD::DRAW_CLEAR_COLOR_ALL,VectorView(&clear,1));
    device->draw_list_bind_render_pipeline(list,pipeline);
    for(size_t i=0;i<view.effect_count;++i) {
        const auto &effect=view.effects[i];if(!(effect.flags&1))continue;uint32_t first=effect.first_surface;
        while(first<effect.first_surface+effect.surface_count) {
            const int page=backdrop_entries[first].page;uint32_t count=1;
            while(first+count<effect.first_surface+effect.surface_count && backdrop_entries[first+count].page==page)++count;
            device->draw_list_bind_uniform_set(list,gpu_pages[page].uniform,0);
            device->draw_list_bind_uniform_set(list,gpu_pages[page].mask_uniform,1);
            device->draw_list_bind_uniform_set(list,gpu_pages[page].blend_uniform,2);
            const struct {
                uint32_t first,mode;
                float width,height,target_width,target_height;uint32_t region;float mask_mode,group_bounds[4];
            } push{backdrop_first_primitive+first,2,float(logical.x),float(logical.y),float(physical.x),float(physical.y),uint32_t(i+1),0,{}};
            device->draw_list_set_push_constant(list,&push,sizeof(push));
            device->draw_list_draw(list,false,count,6);
            first+=count;
        }
    }
    device->draw_list_end();device->free_rid(framebuffer);return true;
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
        if(hierarchy_uniform.is_valid())device->free_rid(hierarchy_uniform);
        hierarchy_uniform=RID();hierarchy_uploaded_revision=0;
        for(RID uniform:{coverage.uniform,coverage.region_uniform,coverage.clear_uniform})if(uniform.is_valid())device->free_rid(uniform);
        coverage.uniform=coverage.region_uniform=coverage.clear_uniform=RID();coverage.revision=0;coverage.target=Size2i();
        for(GpuPage &page:gpu_pages) { if(page.uniform.is_valid()) device->free_rid(page.uniform); page.uniform=RID(); }
        release_groups(device);
        if(rid.is_valid()) device->free_rid(rid);
        capacity=MAX(bytes,65536u); rid=device->storage_buffer_create(capacity);
        if (&rid == &buffer) { geometry_dirty=true; uploaded_vertices.clear(); }
    };
    const uint32_t bytes=vertices.size()*sizeof(hcsr::render::scene_word);
    ensure_buffer(buffer,buffer_capacity,bytes);
    ensure_buffer(state_buffer,state_capacity,gpu_state_count*sizeof(hcsr_gpu_state_t));
    if(local_hierarchy)ensure_buffer(hierarchy_buffer,hierarchy_capacity,hierarchy.nodes.size()*sizeof(hcsr_hierarchy_node_t));
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
    if(geometry_dirty && instance_bytes) {
        if(device->buffer_update(primitive_buffer,0,instance_bytes,primitives.ptr())!=OK) return false;
        instance_uploaded_bytes+=instance_bytes;
    }
    if(local_hierarchy) {
        if(!hierarchy_shader.is_valid()) {
            RD::ShaderStageSPIRVData stage;stage.shader_stage=RD::SHADER_STAGE_COMPUTE;
            String error;stage.spirv=device->shader_compile_spirv_from_source(stage.shader_stage,hcsr::shaders::hierarchy_glsl,RD::SHADER_LANGUAGE_GLSL,&error);
            if(stage.spirv.is_empty()){ERR_PRINT(error);return false;}
            Vector<RD::ShaderStageSPIRVData> stages;stages.push_back(stage);
            hierarchy_shader=device->shader_create_from_spirv(stages,"HCSR local scene hierarchy");
            if(!hierarchy_shader.is_valid())return false;
            hierarchy_pipeline=device->compute_pipeline_create(hierarchy_shader);
        }
        if(!hierarchy_pipeline.is_valid())return false;
        if(!hierarchy_uniform.is_valid()) {
            Vector<RD::Uniform> uniforms;
            for(int i=0;i<2;++i) {RD::Uniform uniform;uniform.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER;uniform.binding=i;
                uniform.append_id(i?state_buffer:hierarchy_buffer);uniforms.push_back(uniform);}
            hierarchy_uniform=device->uniform_set_create(uniforms,hierarchy_shader,0);
        }
        if(!hierarchy_uniform.is_valid())return false;
        if(hierarchy_uploaded_revision!=hierarchy.revision) {
            auto upload_nodes=[&](size_t first,size_t count) {
                const auto bytes=count*sizeof(hcsr_hierarchy_node_t);
                if(bytes && device->buffer_update(hierarchy_buffer,first*sizeof(hcsr_hierarchy_node_t),bytes,hierarchy.nodes.data()+first)!=OK)return false;
                hierarchy_uploaded_bytes+=bytes;state_uploaded_bytes+=bytes;return true;
            };
            if(hierarchy.full_update || hierarchy_uploaded_revision==0 || hierarchy_uploaded_revision!=hierarchy.base_revision) {
                if(!upload_nodes(0,hierarchy.nodes.size()))return false;
            } else {
                for(size_t i=0;i<hierarchy.changed.size();) {
                    size_t first=hierarchy.changed[i],last=first;
                    while(++i<hierarchy.changed.size() && hierarchy.changed[i]==last+1)last=hierarchy.changed[i];
                    if(!upload_nodes(first,last-first+1))return false;
                }
            }
            const uint32_t push[4]={uint32_t(hierarchy.nodes.size()),0,0,0};
            auto list=device->compute_list_begin();
            device->compute_list_bind_compute_pipeline(list,hierarchy_pipeline);
            device->compute_list_bind_uniform_set(list,hierarchy_uniform,0);
            device->compute_list_set_push_constant(list,push,sizeof(push));
            device->compute_list_dispatch(list,(hierarchy.nodes.size()+63)/64,1,1);
            device->compute_list_end();
            hierarchy_uploaded_revision=hierarchy.revision;++hierarchy_evaluations;
        }
    } else {
        if(!update(state_buffer,gpu_states.size()*sizeof(hcsr_gpu_state_t),gpu_states.ptr(),false))return false;
        resolved_state_uploaded_bytes+=gpu_states.size()*sizeof(hcsr_gpu_state_t);
    }
    if(geometry_dirty) {
        if(!update(clip_buffer,gpu_clips.size()*sizeof(hcsr_gpu_clip_t),gpu_clips.ptr(),false)
            || !update(plane_buffer,gpu_planes.size()*sizeof(hcsr_gpu_plane_t),gpu_planes.ptr(),false)) return false;
        clip_definition_uploaded_bytes+=gpu_clips.size()*sizeof(hcsr_gpu_clip_t)+gpu_planes.size()*sizeof(hcsr_gpu_plane_t);
    }
    geometry_dirty=false;
    auto rgba_upload = [&](int page, const Rect2i &rect) {
        Vector<uint8_t> pixels = resources.page_pixels(page, rect);
        for (int i = 0; i < pixels.size(); i += 4) SWAP(pixels.write[i], pixels.write[i + 2]);
        return pixels;
    };
    const uint64_t atlas_revision=resources.atlas_revision();
    const bool atlas_dirty=uploaded_atlas_revision!=atlas_revision;
    const int page_count=atlas_dirty ? resources.page_count() : gpu_pages.size();
    for(int index=page_count;index<gpu_pages.size();++index) release_page(device,gpu_pages.write[index]);
    if (gpu_pages.resize(page_count) != OK) return false;
    for (int index = 0; index < gpu_pages.size(); ++index) {
        GpuPage &page = gpu_pages.write[index];
        const int live=atlas_dirty ? resources.page_live_allocations(index) : (page.texture.is_valid()?1:0);
        if(live<0) return false;
        if(live==0) {
            if(page.texture.is_valid()) ++retired_atlas_pages;
            // RenderingDevice defers actual destruction until this frame slot
            // is safe. Keep the stable page index; reuse creates fresh bindings.
            release_page(device,page);
            if(atlas_dirty) resources.acknowledge_upload(index);
            continue;
        }
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
		} else if(atlas_dirty) {
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
		if(atlas_dirty) resources.acknowledge_upload(index);
		if (!page.texture.is_valid()) {
			return false;
		}
        if(!page.blend_uniform.is_valid()) {
            RD::Uniform backdrop;backdrop.uniform_type=RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;backdrop.binding=0;backdrop.append_id(sampler);backdrop.append_id(page.texture);
            page.blend_uniform=device->uniform_set_create(VectorView(&backdrop,1),shader,2);
            if(!page.blend_uniform.is_valid())return false;
        }
        if(!page.mask_uniform.is_valid()) {
            RD::Uniform mask;mask.uniform_type=RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;mask.binding=0;mask.append_id(sampler);mask.append_id(page.texture);
            page.mask_uniform=device->uniform_set_create(VectorView(&mask,1),shader,1);
            if(!page.mask_uniform.is_valid())return false;
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
            // Ordinary draws never read regions; the state buffer satisfies the
            // common shader interface without allocating another placeholder.
            RD::Uniform region;region.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER;region.binding=6;region.append_id(state_buffer);uniforms.push_back(region);
			page.uniform = device->uniform_set_create(VectorView(uniforms.ptr(), uniforms.size()), shader, 0);
		if (!page.uniform.is_valid()) {
				return false;
			}
		}
	}
    uploaded_atlas_revision=atlas_revision;
	return true;
}

bool HCSRNewestSceneRenderer::prepare_gpu_resources(RenderingDevice *device) {
    if (!uploaded && !upload(device)) return false;
    uploaded = true;
    return true;
}

bool HCSRNewestSceneRenderer::prepare_document_regions(RenderingDevice *device,const Size2i &size) {
    using RD=RenderingDevice;
    if(document_regions.is_empty())return true;
    if(!document_region_shader.is_valid()) {
        RD::ShaderStageSPIRVData stage;stage.shader_stage=RD::SHADER_STAGE_COMPUTE;String error;
        stage.spirv=device->shader_compile_spirv_from_source(stage.shader_stage,hcsr::shaders::document_region_glsl,RD::SHADER_LANGUAGE_GLSL,&error);
        if(stage.spirv.is_empty()){ERR_PRINT(error);return false;}
        Vector<RD::ShaderStageSPIRVData> stages;stages.push_back(stage);
        document_region_shader=device->shader_create_from_spirv(stages,"HCSR document snapshot region");
        if(!document_region_shader.is_valid())return false;
        document_region_pipeline=device->compute_pipeline_create(document_region_shader);
    }
    if(!document_region_pipeline.is_valid())return false;
    // Output submission scratch, not an appearance or placement cache. Resolve
    // every document effect together against current GPU states once per draw.
    const uint32_t count=document_regions.size();
    if(count>document_region_capacity) {
        if(document_region_uniform.is_valid() && device->uniform_set_is_valid(document_region_uniform))device->free_rid(document_region_uniform);
        document_region_uniform=RID();
        for(RID rid:{document_region_buffer,document_arguments,document_descriptors})if(rid.is_valid())device->free_rid(rid);
        document_region_buffer=device->storage_buffer_create(count*16);
        document_arguments=device->storage_buffer_create(count*12,{},RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
        document_descriptors=device->storage_buffer_create(count*4);
        document_region_capacity=count;document_regions_dirty=true;
    }
    if(document_regions_dirty) {
        if(device->buffer_update(document_descriptors,0,count*4,document_regions.ptr())!=OK)return false;
        document_regions_dirty=false;
    }
    if(!document_region_buffer.is_valid() || !document_arguments.is_valid() || !document_descriptors.is_valid())return false;
    // RenderingDevice owns descriptor dependency invalidation. Buffer growth
    // invalidates this set; placement revisions do not change its resources.
    if(document_region_uniform.is_valid() && !device->uniform_set_is_valid(document_region_uniform))document_region_uniform=RID();
    if(!document_region_uniform.is_valid()) {
        Vector<RD::Uniform> region_uniforms;
        const RID region_resources[]={buffer,state_buffer,document_region_buffer,document_arguments,document_descriptors};
        for(uint32_t i=0;i<5;++i){RD::Uniform u;u.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER;u.binding=i;u.append_id(region_resources[i]);region_uniforms.push_back(u);}
        document_region_uniform=device->uniform_set_create(region_uniforms,document_region_shader,0);
    }
    if(!document_region_uniform.is_valid())return false;
    const struct {uint32_t count,stride;float width,height,target_width,target_height;uint32_t padding[2];} region_push{count,4,logical_width,logical_height,float(size.x),float(size.y),{0,0}};
    auto list=device->compute_list_begin();
    device->compute_list_bind_compute_pipeline(list,document_region_pipeline);
    device->compute_list_bind_uniform_set(list,document_region_uniform,0);
    device->compute_list_set_push_constant(list,&region_push,sizeof(region_push));
    device->compute_list_dispatch(list,count,1,1);
    device->compute_list_end();
    return true;
}

bool HCSRNewestSceneRenderer::snapshot_document(RenderingDevice *device,GroupPool &pool, RID prefix, RID snapshot, uint32_t region, const CanvasRenderTargetPreparation::Input *input) {
    using RD=RenderingDevice;
    if (!document_shader.is_valid()) {
        RD::ShaderStageSPIRVData stage;stage.shader_stage=RD::SHADER_STAGE_COMPUTE;
        String error;stage.spirv=device->shader_compile_spirv_from_source(stage.shader_stage,hcsr::shaders::document_input_compute,RD::SHADER_LANGUAGE_GLSL,&error);
        if(stage.spirv.is_empty()){ERR_PRINT(error);return false;}
        Vector<RD::ShaderStageSPIRVData> stages;stages.push_back(stage);
        document_shader=device->shader_create_from_spirv(stages,"HCSR ordered document input");
        if(!document_shader.is_valid())return false;
        document_pipeline=device->compute_pipeline_create(document_shader);
    }
    if(!document_pipeline.is_valid())return false;
    const RID host=input?input->color_texture:prefix;
    auto &entry=pool.document_bindings;
    if(entry.uniform.is_valid() && !device->uniform_set_is_valid(entry.uniform))entry.uniform=RID();
    if(entry.prefix!=prefix || entry.host!=host || entry.snapshot!=snapshot) {
        if(entry.uniform.is_valid())device->free_rid(entry.uniform);
        entry=DocumentBindings{prefix,host,snapshot,RID()};
    }
    RID bindings=entry.uniform;
    if(!bindings.is_valid()) {
        Vector<RD::Uniform> uniforms;
        for(uint32_t i=0;i<2;++i){RD::Uniform u;u.uniform_type=RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;u.binding=i;u.append_id(sampler);u.append_id(i?host:prefix);uniforms.push_back(u);}
        RD::Uniform destination;destination.uniform_type=RD::UNIFORM_TYPE_IMAGE;destination.binding=2;destination.append_id(snapshot);uniforms.push_back(destination);
        RD::Uniform rectangle;rectangle.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER;rectangle.binding=3;rectangle.append_id(document_region_buffer);uniforms.push_back(rectangle);
        bindings=device->uniform_set_create(uniforms,document_shader,0);
        if(!bindings.is_valid())return false;
        entry=DocumentBindings{prefix,host,snapshot,bindings};
    }
    const auto size=device->texture_get_format(snapshot);
    struct {float row_x[4],row_y[4],sizes[4],flags[4];} push{};
    push.sizes[0]=size.width;push.sizes[1]=size.height;
    if(input) {
        const auto &t=input->item_transform;const auto &r=input->item_rect;
        const Vector2 origin=t.xform(r.position);
        push.row_x[0]=t[0].x*r.size.x/size.width;push.row_x[1]=t[1].x*r.size.y/size.height;push.row_x[2]=origin.x;
        push.row_y[0]=t[0].y*r.size.x/size.width;push.row_y[1]=t[1].y*r.size.y/size.height;push.row_y[2]=origin.y;
        push.sizes[2]=input->size.x;push.sizes[3]=input->size.y;push.flags[0]=1;push.flags[1]=input->linear_colors;
    }
    push.flags[2]=float(region);
    auto list=device->compute_list_begin();
    device->compute_list_bind_compute_pipeline(list,document_pipeline);
    device->compute_list_bind_uniform_set(list,bindings,0);
    device->compute_list_set_push_constant(list,&push,sizeof(push));
    device->compute_list_dispatch_indirect(list,document_arguments,region*12);
    device->compute_list_end();++gpu_document_snapshots;
    return true;
}

bool HCSRNewestSceneRenderer::draw(RenderingDevice *device, RID target, const Color &background, const CanvasRenderTargetPreparation::Input *input) {
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
    if(!prepare_coverage(device,size) || !prepare_document_regions(device,size)){device->free_rid(framebuffer);return false;}
    int pool_index=-1;
    for(int i=group_pools.size()-1;i>=0;--i) {
        if(!device->texture_is_valid(group_pools[i].output)) {
            for(const auto &copy:group_pools[i].blend_copies)if(copy.uniform.is_valid() && device->uniform_set_is_valid(copy.uniform))device->free_rid(copy.uniform);
            for(RID rid:{group_pools[i].blend_uniform,group_pools[i].blend_texture})if(rid.is_valid())device->free_rid(rid);
            for(const auto &group:group_pools[i].targets)
                for(RID rid:{group.uniform,group.framebuffer,group.texture}) if(rid.is_valid()) device->free_rid(rid);
            for(const auto &group:group_pools[i].underlays)
                for(RID rid:{group.uniform,group.framebuffer,group.texture}) if(rid.is_valid()) device->free_rid(rid);
            group_pools.remove_at(i);
        }
    }
    for(int i=0;i<group_pools.size();++i) if(group_pools[i].output==target) pool_index=i;
    if(pool_index<0) { pool_index=group_pools.size(); GroupPool pool; pool.output=target; group_pools.push_back(pool); }
    auto &pool=group_pools.write[pool_index];
    auto &group_targets=pool.targets;
    const bool blending=std::any_of(batches.begin(),batches.end(),[](const Batch &batch){return batch.blend_mode!=0;});
    if(blending && !pool.blend_texture.is_valid()) {
        auto copy_format=format;copy_format.usage_bits=RD::TEXTURE_USAGE_SAMPLING_BIT|RD::TEXTURE_USAGE_CAN_COPY_TO_BIT|RD::TEXTURE_USAGE_STORAGE_BIT;
        pool.blend_texture=device->texture_create(copy_format,RD::TextureView());
        if(!pool.blend_texture.is_valid()){device->free_rid(framebuffer);return false;}
        RD::Uniform backdrop;backdrop.uniform_type=RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;backdrop.binding=0;backdrop.append_id(sampler);backdrop.append_id(pool.blend_texture);
        pool.blend_uniform=device->uniform_set_create(VectorView(&backdrop,1),shader,2);
        if(!pool.blend_uniform.is_valid()){device->free_rid(framebuffer);return false;}
    }
    const bool spatial_filters=std::any_of(batches.begin(),batches.end(),[](const Batch &batch){return batch.kind>=hcsr::render::group_filter_pass;});
    const bool shadows=std::any_of(batches.begin(),batches.end(),[](const Batch &batch){return batch.kind==hcsr::render::group_shadow_apply;});
    if(shadows && !shadow_pipeline.is_valid()) {
        auto blend=RD::PipelineColorBlendState::create_blend();
        blend.attachments.write[0].src_color_blend_factor=RD::BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        blend.attachments.write[0].dst_color_blend_factor=RD::BLEND_FACTOR_ONE;
        blend.attachments.write[0].src_alpha_blend_factor=RD::BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        blend.attachments.write[0].dst_alpha_blend_factor=RD::BLEND_FACTOR_ONE;
        shadow_pipeline=device->render_pipeline_create(shader,device->framebuffer_get_format(framebuffer),RD::INVALID_ID,RD::RENDER_PRIMITIVE_TRIANGLES,RD::PipelineRasterizationState(),RD::PipelineMultisampleState(),RD::PipelineDepthStencilState(),blend);
        if(!shadow_pipeline.is_valid()){device->free_rid(framebuffer);return false;}
    }
    // A single scratch target is shared across all depths in the ordered path.
    auto ensure_group_target=[&](GroupTarget &group) {
        if(group.texture.is_valid())return true;
        auto layer_format=format;
        layer_format.usage_bits=RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
        group.texture=device->texture_create(layer_format,RD::TextureView());group.size=size;
        if(!group.texture.is_valid())return false;
        // Bounded compute clears preserve pixels outside the current group.
        // New targets must start defined, including filter sampling beyond it.
        if(device->texture_clear(group.texture,Color(0,0,0,0),0,1,0,1)!=OK)return false;
        Vector<RID> layers;layers.push_back(group.texture);group.framebuffer=device->framebuffer_create(layers);
        Vector<RD::Uniform> uniforms;
        RD::Uniform image;image.uniform_type=RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;image.binding=0;image.append_id(sampler);image.append_id(group.texture);uniforms.push_back(image);
        RD::Uniform storage;storage.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER;storage.binding=1;storage.append_id(buffer);uniforms.push_back(storage);
        const RID extra[]={state_buffer,clip_buffer,plane_buffer,primitive_buffer};
        for(int i=0;i<4;++i){RD::Uniform u;u.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER;u.binding=2+i;u.append_id(extra[i]);uniforms.push_back(u);}
        RD::Uniform region;region.uniform_type=RD::UNIFORM_TYPE_STORAGE_BUFFER;region.binding=6;region.append_id(gpu_geometry && coverage.controls.is_valid()?coverage.controls:state_buffer);uniforms.push_back(region);
        group.uniform=device->uniform_set_create(VectorView(uniforms.ptr(),uniforms.size()),shader,0);++group_allocations;
        return group.framebuffer.is_valid() && group.uniform.is_valid();
    };
    while(group_targets.size()<int(group_depth)+(ordered_backdrops?(snapshot_target?3:2):spatial_filters?1:0)) {
        group_targets.push_back({});
        if(!ensure_group_target(group_targets.write[group_targets.size()-1])){release_groups(device);device->free_rid(framebuffer);return false;}
    }
    if(ordered_backdrops) {
        if(pool.underlays.size()<int(group_depth) && pool.underlays.resize(group_depth)!=OK){device->free_rid(framebuffer);return false;}
        for(const auto &batch:batches)if(batch.backdrop_mask || batch.backdrop_merge)
            if(!ensure_group_target(pool.underlays.write[batch.backdrop_destination])){release_groups(device);device->free_rid(framebuffer);return false;}
    }
    hcsr::render::transparent_group_submission visibility;
    auto opacity=[&](uint32_t index) {return local_hierarchy?hierarchy.reference_opacity(index):hcsr::render::decode_opacity(gpu_states[index].reserved[0]);};
    for(auto &batch:batches)batch.hidden=visibility.skip(batch.kind,batch.depth,batch.opacity,batch.opacity_state_plus_one,opacity);
    auto region_for = [&](const Batch &batch) {
        const auto area=hcsr::render::group_region({batch.bounds.position.x,batch.bounds.position.y,batch.bounds.size.x,batch.bounds.size.y},1,1,size.x,size.y);
        return Rect2(area.x,area.y,area.width,area.height);
    };
    auto emit = [&](RD::DrawListID list, const Batch &batch) {
        if(batch.hidden)return;
        // Prefix copies need bounded placement as well as bounded sampling.
        // Their first pass still clears the complete snapshot below; later
        // prefixes preserve pixels outside this GPU-selected source region.
        const bool gpu_prefix=gpu_geometry && batch.backdrop_prefix && batch.backdrop_source_event!=SIZE_MAX;
        const bool gpu_region=gpu_geometry && batch.kind && (!batch.backdrop || gpu_prefix);
        if(batch.kind && !gpu_region) device->draw_list_enable_scissor(list,region_for(batch));
        else device->draw_list_disable_scissor(list);
        device->draw_list_bind_uniform_set(list,batch.kind ? group_targets[batch.backdrop?batch.backdrop_source:batch.source_scratch?group_depth:batch.depth-1].uniform : gpu_pages[batch.page].uniform,0);
        device->draw_list_bind_uniform_set(list,gpu_pages[batch.mask_page].mask_uniform,1);
        device->draw_list_bind_uniform_set(list,blending?pool.blend_uniform:gpu_pages[0].blend_uniform,2);
        Rect2 region;
        uint32_t source_region=0;
        if(gpu_region)source_region=uint32_t(bounds_program.group_index(gpu_prefix?batch.backdrop_source_event:batch.event_index))+1;
        else if(gpu_geometry && batch.backdrop && batch.backdrop_source_event!=SIZE_MAX)
            source_region=0x80000000u|(uint32_t(bounds_program.group_index(batch.backdrop_source_event))+1);
        else region=region_for(batch);
        if(!gpu_geometry && batch.backdrop && batch.backdrop_source_event!=SIZE_MAX) {const auto &b=bounds_program.bounds(batch.backdrop_source_event);
            const auto area=hcsr::render::group_region(b,logical_width,logical_height,size.x,size.y);region=Rect2(area.x,area.y,area.width,area.height);
        }
        const struct { uint32_t first,gpu_geometry; float width,height,target_width,target_height;uint32_t source_region;float mask_mode,region[4]; } push{batch.first,
            gpu_geometry ? 1u : 0u,logical_width,logical_height,float(size.x),float(size.y),source_region,batch.backdrop_mask?1.f:batch.backdrop?2.f:0.f,
            {float(region.position.x),float(region.position.y),float(region.get_end().x),float(region.get_end().y)}};
        device->draw_list_set_push_constant(list,&push,sizeof(push));
        device->draw_list_draw(list,false,gpu_geometry ? batch.count : 1,gpu_geometry ? 6 : batch.count);
        if(gpu_prefix)++gpu_backdrop_prefix_draws;
        ++last_draw_calls;
    };
    std::vector<std::vector<size_t>> dynamic_passes;
    // Process-wide diagnostic override for paired profiling and pixel checks.
    static const bool sequential_groups = OS::get_singleton()->get_environment("HCSR_SEQUENTIAL_OPACITY_GROUPS") == "1";
    const bool nested_groups=!sequential_groups && !nested_group_passes.empty();
    last_disjoint_groups=nested_groups || (!ordered_backdrops && !sequential_groups && !blending && !spatial_filters && group_depth && host_coverage_required && hcsr::render::schedule_disjoint_groups(batches.size(),[&](size_t i) {
        const auto &b=batches[i];
        return hcsr::render::group_event{b.kind,b.depth,b.opacity,{b.bounds.position.x,b.bounds.position.y,b.bounds.size.x,b.bounds.size.y}};
    },group_depth,1,1,size.x,size.y,dynamic_passes));
    const auto &passes=nested_groups?nested_group_passes:dynamic_passes;
    last_render_passes=0; last_draw_calls=0;
    if(last_disjoint_groups) {
        for(int depth=int(group_depth);depth>=0;--depth) {
            if(depth && std::none_of(passes[depth].begin(),passes[depth].end(),[&](size_t i){return !batches[i].hidden;}))continue;
            const Color clear=depth ? Color(0,0,0,0) : background;
            Rect2 region;
            bool first=true;
            if(depth && !nested_groups) for(const auto &batch:batches) if(!batch.hidden && batch.kind==HCSR_GROUP_BEGIN && batch.depth==uint32_t(depth)) {
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
    uint32_t resume_foreground_depth=0;
	for (const Batch &batch : batches) {
        if(batch.hidden)continue;
        if (!batch.kind && resume_foreground_depth) {
            // Backdrop pixels belong to the underlay. Ordinary node painting
            // must resume in the foreground sampled by descendant roots.
            device->draw_list_end();
            list=device->draw_list_begin(group_targets[resume_foreground_depth-1].framebuffer);
            ++last_render_passes;
            device->draw_list_set_viewport(list,Rect2i(Vector2i(),size));
            device->draw_list_bind_render_pipeline(list,pipeline);
            resume_foreground_depth=0;
        }
        if (batch.kind) {
            resume_foreground_depth=0;
            device->draw_list_end();
            if(batch.document_source && !snapshot_document(device,pool,target,group_targets[group_depth+2].texture,batch.document_region,input)){device->free_rid(framebuffer);return false;}
            const Color transparent(0,0,0,0);
            const Rect2 region=region_for(batch);
            if(batch.blend_mode) {
                const RID source=batch.depth==1?target:group_targets[batch.depth-2].texture;
                if(gpu_geometry) {
                    if(!copy_group_region(device,pool,source,batch.event_index,batch.depth)){device->free_rid(framebuffer);return false;}
                } else if(device->texture_copy(source,pool.blend_texture,Vector3(region.position.x,region.position.y,0),Vector3(region.position.x,region.position.y,0),Vector3(region.size.x,region.size.y,1),0,0,0,0)!=OK) {device->free_rid(framebuffer);return false;}
            }
            RID destination = batch.backdrop?(batch.backdrop_mask || batch.backdrop_merge?pool.underlays[batch.backdrop_destination].framebuffer:group_targets[batch.backdrop_destination].framebuffer) : (batch.kind==hcsr::render::group_filter_pass || batch.kind==hcsr::render::group_shadow_blur) ? group_targets[group_depth].framebuffer
                    : (batch.kind==HCSR_GROUP_BEGIN || batch.kind==hcsr::render::group_shadow_apply) ? group_targets[batch.depth-1].framebuffer
                    : batch.depth==1 ? framebuffer : group_targets[batch.depth-2].framebuffer;
            const bool clear=batch.backdrop?(batch.backdrop_prefix?batch.backdrop_first:batch.backdrop_first || (!batch.backdrop_mask && !batch.backdrop_merge)):batch.kind!=HCSR_GROUP_END && batch.kind!=hcsr::render::group_shadow_apply;
            const bool gpu_clear=clear && gpu_geometry && !batch.backdrop;
            list=device->draw_list_begin(destination,clear && !gpu_clear ? RD::DRAW_CLEAR_COLOR_ALL : 0,VectorView(&transparent,1),1,0,clear && !gpu_clear ? region : Rect2());
            ++last_render_passes;
            device->draw_list_set_viewport(list,Rect2i(Vector2i(),size));
            if(gpu_clear && !clear_group(device,list,destination,size,batch.event_index)) {
                device->draw_list_end();device->free_rid(framebuffer);return false;
            }
            device->draw_list_bind_render_pipeline(list,batch.kind==hcsr::render::group_shadow_apply?shadow_pipeline:pipeline);
            if (batch.kind==HCSR_GROUP_BEGIN) continue;
        }
        emit(list,batch);
        if(batch.backdrop_mask)resume_foreground_depth=batch.depth;
        if(batch.backdrop_merge)SWAP(group_targets.write[batch.backdrop_destination],pool.underlays.write[batch.backdrop_destination]);
        if(!batch.backdrop && batch.kind==hcsr::render::group_filter_pass) SWAP(group_targets.write[batch.depth-1],group_targets.write[group_depth]);
	}
	device->draw_list_end();
	device->free_rid(framebuffer);
	return true;
}

void HCSRNewestSceneRenderer::release_groups(RenderingDevice *device) {
    if(device) for(const auto &pool:group_pools) {
        if(pool.document_bindings.uniform.is_valid() && device->uniform_set_is_valid(pool.document_bindings.uniform))device->free_rid(pool.document_bindings.uniform);
        for(const auto &copy:pool.blend_copies)if(copy.uniform.is_valid() && device->uniform_set_is_valid(copy.uniform))device->free_rid(copy.uniform);
        for(RID rid:{pool.blend_uniform,pool.blend_texture})if(rid.is_valid())device->free_rid(rid);
        for(const auto &group:pool.targets)for(RID rid:{group.uniform,group.framebuffer,group.texture})if(rid.is_valid())device->free_rid(rid);
        for(const auto &group:pool.underlays)for(RID rid:{group.uniform,group.framebuffer,group.texture})if(rid.is_valid())device->free_rid(rid);
    }
    group_pools.clear();
}

void HCSRNewestSceneRenderer::release_page(RenderingDevice *device,GpuPage &page) {
    if(device) for(RID rid:{page.uniform,page.mask_uniform,page.blend_uniform,page.texture})
        if(rid.is_valid()) device->free_rid(rid);
    page=GpuPage();
}

void HCSRNewestSceneRenderer::release(RenderingDevice *device) {
    release_groups(device);
	if (device) {
        for(RID rid:{coverage.uniform,coverage.region_uniform,coverage.clear_uniform,coverage.data,coverage.output,coverage.controls,coverage.arguments})
            if(rid.is_valid())device->free_rid(rid);
        for(int i=0;i<3;i++) {
            if(coverage.pipelines[i].is_valid())device->free_rid(coverage.pipelines[i]);
            if(coverage.shaders[i].is_valid())device->free_rid(coverage.shaders[i]);
        }
        for(GpuPage &page:gpu_pages) release_page(device,page);
        if(document_region_uniform.is_valid() && device->uniform_set_is_valid(document_region_uniform))device->free_rid(document_region_uniform);
		for (RID rid : { hierarchy_uniform,hierarchy_pipeline,hierarchy_shader,hierarchy_buffer,pipeline, shadow_pipeline, document_pipeline,document_region_pipeline,document_region_shader,document_region_buffer,document_arguments,document_descriptors, blend_copy_pipeline,blend_copy_shader,buffer, sampler, shader, document_shader, state_buffer,clip_buffer,plane_buffer,primitive_buffer }) {
			if (rid.is_valid()) {
				device->free_rid(rid);
			}
		}
	}
	coverage=CoverageGpu();
	gpu_pages.clear();
    uploaded_atlas_revision=UINT64_MAX;
	vertices.clear();
	batches.clear();
	pipeline = shadow_pipeline = buffer = sampler = shader = document_shader = document_pipeline = RID();
    blend_copy_shader=blend_copy_pipeline=RID();
    document_region_pipeline=document_region_shader=document_region_buffer=document_arguments=document_region_uniform=document_descriptors=RID();
    document_region_capacity=0;document_regions.clear();document_regions_dirty=true;
	buffer_capacity = 0;
    state_buffer=clip_buffer=plane_buffer=primitive_buffer=RID();
    state_capacity=clip_capacity=plane_capacity=primitive_capacity=0;
    hierarchy_uniform=hierarchy_pipeline=hierarchy_shader=hierarchy_buffer=RID();
    hierarchy_capacity=gpu_state_count=0;hierarchy_uploaded_revision=0;local_hierarchy=false;hierarchy.clear();
    geometry_generation=0; gpu_geometry=false; primitives.clear();
    uploaded_vertices.clear();
}

// Compile the same paint program used by the GPU adapters for the CPU path.
namespace hcsr_cpu_paint {
static Vector4 read_atlas(const Ref<Image> &atlas, const Vector2i &p) {
    const Color c = atlas->get_pixel(p.x, p.y);
    return Vector4(c.r, c.g, c.b, c.a);
}
#define HCSR_F3 Vector3
#define HCSR_F2 Vector2
#define HCSR_F4 Vector4
#define HCSR_I2 Vector2i
#define HCSR_INLINE inline
#define HCSR_CONTEXT const Ref<Image> &atlas, const hcsr::render::scene_word *effects, const Ref<Image> &mask_atlas, const hcsr_gpu_state_t *states,const Ref<Image> &blend_atlas,
#define HCSR_ARGS atlas,effects,mask_atlas,states,blend_atlas,
#define HCSR_CLAMP CLAMP
#define HCSR_MIN MIN
#define HCSR_MAX MAX
#define HCSR_FLOOR(p) (p).floor()
#define HCSR_MIX(a,b,t) (a).lerp((b),(t))
#define HCSR_FETCH(p) read_atlas(atlas,p)
#define HCSR_GROUP_FETCH(uv,tint) read_atlas(atlas,Vector2i((uv)*Vector2(atlas->get_size())))
#define HCSR_EFFECT(i) read_effect(effects,i)
#define HCSR_MASK_FETCH(p) read_atlas(mask_atlas,p)
#define HCSR_BLEND_FETCH(uv,tint) read_atlas(blend_atlas,Vector2i((uv)*Vector2(blend_atlas->get_size())))
#define HCSR_MASK_TRANSFORM(i,p) mask_transform(states,i,p)
static Vector4 mask_transform(const hcsr_gpu_state_t *states,int index,Vector4 p) {const float *m=states[index].transform;return Vector4(m[0],m[1],m[2],m[3])*p.x+Vector4(m[4],m[5],m[6],m[7])*p.y+Vector4(m[8],m[9],m[10],m[11])*p.z+Vector4(m[12],m[13],m[14],m[15])*p.w;}
#define HCSR_BRANCH
#define HCSR_LOOP
#define HCSR_GROUP_SIZE(tint) Vector2(atlas->get_size())
#define HCSR_BLUR_SCALE(operation) ((operation).z>0?tint.x:tint.y)
#define HCSR_BLUR_DIRECTION(operation) Vector2((operation).z,(operation).w)
#define HCSR_LENGTH(v) ((v).length())
#define HCSR_OFFSET_SCALE(offset) ((offset)*Vector2(tint.x,tint.y))
#define HCSR_DISCARD return Vector4()
static Vector4 read_effect(const hcsr::render::scene_word *effects,int index) {
    const float *value=reinterpret_cast<const float *>(effects)+index*4;
    return Vector4(value[0],value[1],value[2],value[3]);
}
#include "hcsr_group_effects_shader.inc"
#include "hcsr_paint_shader.inc"
#undef HCSR_F3
#undef HCSR_BLEND_FETCH
#undef HCSR_F2
#undef HCSR_F4
#undef HCSR_I2
#undef HCSR_INLINE
#undef HCSR_CONTEXT
#undef HCSR_ARGS
#undef HCSR_GROUP_SIZE
#undef HCSR_BLUR_SCALE
#undef HCSR_BLUR_DIRECTION
#undef HCSR_LENGTH
#undef HCSR_OFFSET_SCALE
#undef HCSR_CLAMP
#undef HCSR_MIN
#undef HCSR_MAX
#undef HCSR_FLOOR
#undef HCSR_MIX
#undef HCSR_FETCH
#undef HCSR_GROUP_FETCH
#undef HCSR_DISCARD
#undef HCSR_MASK_FETCH
#undef HCSR_MASK_TRANSFORM
#undef HCSR_EFFECT
#undef HCSR_BRANCH
#undef HCSR_LOOP
}

void HCSRNewestSceneRenderer::draw_cpu(Ref<Image> target, const Color &background) {
	target->fill(background);
	const Vector2 size = target->get_size();
    Vector<Ref<Image>> parents;
    Ref<Image> scratch;
    Vector<Ref<Image>> reference_pages; reference_pages.resize(resources.page_count());
	for (const Batch &batch : batches) {
        if (batch.kind==HCSR_GROUP_BEGIN) {
            parents.push_back(target);
            target=Image::create_empty(int(size.x),int(size.y),false,Image::FORMAT_RGBA8);
            target->fill(Color(0,0,0,0));
            continue;
        }
        if (batch.kind>=HCSR_GROUP_END) {
            Ref<Image> parent;
            Ref<Image> source=target;
            if(batch.kind==HCSR_GROUP_END) {parent=parents[parents.size()-1];parents.resize(parents.size()-1);}
            else if(batch.kind==hcsr::render::group_shadow_apply){parent=target;source=batch.source_scratch?scratch:Ref<Image>(target->duplicate());}
            else {parent=Image::create_empty(int(size.x),int(size.y),false,Image::FORMAT_RGBA8);parent->fill(Color(0,0,0,0));}
            for (int y=0;y<int(size.y);++y) for(int x=0;x<int(size.x);++x) {
                Color color=source->get_pixel(x,y)*batch.opacity;
                Vertex parameters;memcpy(&parameters,vertices.ptr()+batch.first,sizeof(parameters));
                if(parameters.bounds[0]<=-4) {
                    const Vector2 uv((x+.5f)/size.x,(y+.5f)/size.y);
                    const Vector4 tint(size.x/logical_width,size.y/logical_height,0,batch.opacity);
                    const Vector4 bounds(parameters.bounds[0],parameters.bounds[1],parameters.bounds[2],parameters.bounds[3]);
                    Ref<Image> &mask_image=reference_pages.write[batch.mask_page];
                    if(parameters.bounds[0]==-6 && mask_image.is_null()) {
                        const auto atlas_size=resources.page_size(batch.mask_page);auto pixels=resources.page_pixels(batch.mask_page,Rect2i(Point2i(),atlas_size));
                        for(int offset=0;offset<pixels.size();offset+=4)SWAP(pixels.write[offset],pixels.write[offset+2]);
                        mask_image=Image::create_from_data(atlas_size.x,atlas_size.y,false,Image::FORMAT_RGBA8,pixels);
                    }
                    const Vector4 filtered=parameters.bounds[0]==-7?hcsr_cpu_paint::hcsr_shadow_group(source,vertices.ptr(),mask_image,gpu_states.ptr(),parent,uv,tint,bounds)
                        :parameters.bounds[0]==-6 ? hcsr_cpu_paint::hcsr_mask_group(source,vertices.ptr(),mask_image,gpu_states.ptr(),parent,uv,tint,bounds)
                        :parameters.bounds[0]==-5 ? hcsr_cpu_paint::hcsr_blur_group(source,vertices.ptr(),mask_image,gpu_states.ptr(),parent,uv,tint,bounds)
                        :hcsr_cpu_paint::hcsr_filter_group(source,vertices.ptr(),mask_image,gpu_states.ptr(),parent,uv,tint,
                        Vector4(parameters.bounds[0],parameters.bounds[1],parameters.bounds[2],parameters.bounds[3]));
                    color=parameters.bounds[0]==-7?Color(filtered.x,filtered.y,filtered.z,filtered.w):Color(filtered.x*filtered.w,filtered.y*filtered.w,filtered.z*filtered.w,filtered.w);
                }
                Color under=parent->get_pixel(x,y);
                parent->set_pixel(x,y,batch.kind==hcsr::render::group_shadow_apply?under+color*(1-under.a):color+under*(1-color.a));
            }
            if(batch.kind==hcsr::render::group_shadow_blur)scratch=parent;else target=parent;
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
		for (uint32_t i = batch.first; i < batch.first + batch.count*4; i += 12) {
			Vertex a,b,c;memcpy(&a,vertices.ptr()+i,sizeof(a));memcpy(&b,vertices.ptr()+i+4,sizeof(b));memcpy(&c,vertices.ptr()+i+8,sizeof(c));
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
                    const Vector4 shaded = hcsr_cpu_paint::hcsr_shade(atlas_image, vertices.ptr(),atlas_image,gpu_states.ptr(),atlas_image, uv, tint,
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
    uint64_t atlas_bytes=0;int atlas_pages=0;
    for(int index=0;index<gpu_pages.size();++index) if(gpu_pages[index].texture.is_valid()) {
        const Size2i size=resources.page_size(index);
        atlas_bytes+=uint64_t(size.x)*size.y*4;++atlas_pages;
    }
    result["gpu_atlas_pages"]=atlas_pages;
    result["gpu_atlas_bytes"]=atlas_bytes;
    result["gpu_atlas_retired_pages"]=retired_atlas_pages;
    result["geometry_words"] = vertices.size();
    result["prepared_geometry_bytes"] = vertices.size()*sizeof(hcsr::render::scene_word);
    result["gpu_geometry"] = gpu_geometry;
    result["instances"] = primitives.size();
    result["draw_calls"] = last_draw_calls;
    result["gpu_ms"] = last_gpu_ms;
    result["gpu_timestamp_frame"] = last_gpu_frame;
    result["geometry_uploaded_bytes"] = geometry_uploaded_bytes;
    result["instance_uploaded_bytes"] = instance_uploaded_bytes;
    result["state_uploaded_bytes"] = state_uploaded_bytes;
    result["local_hierarchy"] = local_hierarchy;
    result["hierarchy_nodes"] = hierarchy.nodes.size();
    result["coverage_groups"] = coverage.program.groups.size();
    result["coverage_evaluations"] = coverage.evaluations;
    result["coverage_uploaded_bytes"] = coverage.uploaded_bytes;
    result["coverage_buffer_bytes"] = uint64_t(coverage.capacities[0])+coverage.capacities[1]+coverage.capacities[2]+coverage.capacities[3];
    result["gpu_group_clears"] = coverage.clears;
    result["hierarchy_uploaded_bytes"] = hierarchy_uploaded_bytes;
    result["hierarchy_evaluations"] = hierarchy_evaluations;
    result["resolved_state_uploaded_bytes"] = resolved_state_uploaded_bytes;
    result["clip_definition_uploaded_bytes"] = clip_definition_uploaded_bytes;
    result["geometry_generation"] = geometry_generation;
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
    result["cpu_compositing_bounds_evaluations"] = cpu_compositing_bounds_evaluations;
    result["host_compositing_bounds_required"] = host_coverage_required;
    result["nested_group_passes"] = int64_t(nested_group_passes.size());
    result["gpu_blend_region_copies"] = gpu_blend_region_copies;
    result["gpu_backdrop_prefix_draws"] = gpu_backdrop_prefix_draws;
    result["gpu_document_snapshots"] = gpu_document_snapshots;
	return result;
}

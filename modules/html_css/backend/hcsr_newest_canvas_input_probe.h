#pragma once

#include "core/os/mutex.h"
#include "servers/rendering/canvas_render_target_preparation.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

// Opt-in validation of the native handoff. GPU readback is never enabled in
// normal rendering. The adapter survives queued canvas items; render-thread
// destruction disconnects it before releasing its owned probe resources.
class HCSRCanvasInputProbe : public CanvasRenderTargetPreparation {
	Mutex mutex;
	Dictionary diagnostics;
	bool connected = true;
	uint64_t calls = 0;
	RID shader, pipeline, sampler, output;
	bool sample(RenderingDevice *device, RID input, Color &origin, Color &prefix) {
		using RD = RenderingDevice;
		if (!shader.is_valid()) {
			const char *source = R"(#version 450
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
layout(set=0,binding=0) uniform sampler2D input_texture;
layout(set=0,binding=1,std430) buffer Samples { vec4 colors[2]; } samples;
void main() {
    uint i=gl_GlobalInvocationID.x;
    ivec2 p=i==0?ivec2(0):min(ivec2(100,80),textureSize(input_texture,0)-ivec2(1));
    samples.colors[i]=texelFetch(input_texture,p,0);
})";
			RD::ShaderStageSPIRVData stage;
			stage.shader_stage = RD::SHADER_STAGE_COMPUTE;
			String error;
			stage.spirv = device->shader_compile_spirv_from_source(stage.shader_stage, source, RD::SHADER_LANGUAGE_GLSL, &error);
			if (stage.spirv.is_empty()) return false;
			Vector<RD::ShaderStageSPIRVData> stages;
			stages.push_back(stage);
			shader = device->shader_create_from_spirv(stages);
			if (!shader.is_valid()) return false;
			pipeline = device->compute_pipeline_create(shader);
			sampler = device->sampler_create(RD::SamplerState());
			output = device->storage_buffer_create(32);
		}
		if (!pipeline.is_valid() || !sampler.is_valid() || !output.is_valid()) return false;
		RD::Uniform texture;
		texture.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
		texture.binding = 0;
		texture.append_id(sampler);
		texture.append_id(input);
		RD::Uniform buffer;
		buffer.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		buffer.binding = 1;
		buffer.append_id(output);
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(texture);
		uniforms.push_back(buffer);
		RID bindings = device->uniform_set_create(uniforms, shader, 0);
		if (!bindings.is_valid()) return false;
		auto list = device->compute_list_begin();
		device->compute_list_bind_compute_pipeline(list, pipeline);
		device->compute_list_bind_uniform_set(list, bindings, 0);
		device->compute_list_dispatch(list, 2, 1, 1);
		device->compute_list_end();
		const auto bytes = device->buffer_get_data(output);
		device->free_rid(bindings);
		if (bytes.size() != 32) return false;
		float colors[8];
		memcpy(colors, bytes.ptr(), sizeof(colors));
		origin = Color(colors[0], colors[1], colors[2], colors[3]);
		prefix = Color(colors[4], colors[5], colors[6], colors[7]);
		return true;
	}

public:
	Dictionary get_diagnostics() { MutexLock lock(mutex); return diagnostics; }
	void disconnect() {
		MutexLock lock(mutex);
		connected = false;
		auto server = RenderingServer::get_singleton();
		auto device = server != nullptr ? server->get_rendering_device() : nullptr;
		if (device) for (RID rid : { pipeline, shader, sampler, output }) if (rid.is_valid()) device->free_rid(rid);
		pipeline = shader = sampler = output = RID();
	}
	void prepare(const Input &p_input) override {
		MutexLock lock(mutex);
		if (!connected) return;
		auto device = RenderingServer::get_singleton()->get_rendering_device();
		if (device == nullptr) return;
		const auto format = device->texture_get_format(p_input.color_texture);
		Dictionary next;
		next["calls"] = int64_t(++calls);
		next["size"] = p_input.size;
		next["item_transform"] = p_input.item_transform;
		next["item_rect"] = p_input.item_rect;
		next["clip_rect"] = p_input.clip_rect;
		next["clipped"] = p_input.clipped;
		next["linear_colors"] = p_input.linear_colors;
		next["canvas_group"] = p_input.canvas_group;
		next["format"] = int(format.format);
		Color origin, prefix;
		if (sample(device, p_input.color_texture, origin, prefix)) {
			next["origin"] = origin;
			next["prefix"] = prefix;
		}
		diagnostics = next;
	}
};


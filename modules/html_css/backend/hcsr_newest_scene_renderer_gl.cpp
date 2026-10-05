#include "hcsr_newest_scene_renderer.h"

#ifdef GLES3_ENABLED
#include "hcsr_shader_sources.h"

#include "drivers/gles3/rasterizer_util_gles3.h"
#include "drivers/gles3/storage/texture_storage.h"
#include "servers/rendering/rendering_server.h"

#include <platform_gl.h>

namespace {
// Render-thread adapter: restore the engine's bindings rather than invalidating
// its state assumptions. No glFinish, readback, world-matrix mirror or paint cache.
struct GLStateGuard {
	GLint program, vao, draw_fbo, read_fbo, viewport[4], scissor[4], active, unpack, pack, unpack_buffer, pack_buffer;
	GLint src_rgb, dst_rgb, src_alpha, dst_alpha, equation_rgb, equation_alpha;
	GLint unpack_row, unpack_skip_rows, unpack_skip_pixels;
	GLint textures[8], samplers[8];
	GLboolean mask[4];
	bool blend, depth, cull, scissor_on, srgb = false;
	GLStateGuard() {
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
		glGetIntegerv(GL_VIEWPORT, viewport);
		glGetIntegerv(GL_SCISSOR_BOX, scissor);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
		glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row);
		glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
		glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);
		glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
		glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
		glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
		glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
		glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation_rgb);
		glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &equation_alpha);
		glGetBooleanv(GL_COLOR_WRITEMASK, mask);
		blend = glIsEnabled(GL_BLEND);
		depth = glIsEnabled(GL_DEPTH_TEST);
		cull = glIsEnabled(GL_CULL_FACE);
		scissor_on = glIsEnabled(GL_SCISSOR_TEST);
		if (RasterizerUtilGLES3::is_gles_over_gl()) {
			srgb = glIsEnabled(0x8DB9);
			glDisable(0x8DB9);
		}
		for (int i = 0; i < 8; i++) {
			glActiveTexture(GL_TEXTURE0 + i);
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i]);
			glGetIntegerv(GL_SAMPLER_BINDING, &samplers[i]);
			glBindSampler(i, 0);
		}
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_SCISSOR_TEST);
		glColorMask(true, true, true, true);
	}
	static void enable(GLenum name, bool value) {
		if (value) {
			glEnable(name);
		} else {
			glDisable(name);
		}
	}
	~GLStateGuard() {
		glUseProgram(program);
		glBindVertexArray(vao);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
		glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
		glBlendEquationSeparate(equation_rgb, equation_alpha);
		enable(GL_BLEND, blend);
		enable(GL_DEPTH_TEST, depth);
		enable(GL_CULL_FACE, cull);
		enable(GL_SCISSOR_TEST, scissor_on);
		if (RasterizerUtilGLES3::is_gles_over_gl()) {
			enable(0x8DB9, srgb);
		}
		glColorMask(mask[0], mask[1], mask[2], mask[3]);
		for (int i = 0; i < 8; i++) {
			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(GL_TEXTURE_2D, textures[i]);
			glBindSampler(i, samplers[i]);
		}
		glActiveTexture(active);
		glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
		glPixelStorei(GL_PACK_ALIGNMENT, pack);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
	}
};
GLuint compile_program(const char *vertex, const char *fragment) {
	CharString adapted[2];
	const char *sources[] = { vertex, fragment };
	if (RasterizerUtilGLES3::is_gles_over_gl()) {
		for (int i = 0; i < 2; i++) {
			adapted[i] = String(sources[i]).replace("#version 300 es", "#version 330 core").replace("precision highp float;", "").replace("precision highp int;", "").replace("precision highp usampler2D;", "").utf8();
			sources[i] = adapted[i].get_data();
		}
	}
	GLuint shaders[2]{};
	bool valid = true;
	for (int i = 0; i < 2; i++) {
		shaders[i] = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
		glShaderSource(shaders[i], 1, &sources[i], nullptr);
		glCompileShader(shaders[i]);
		GLint success = 0;
		glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &success);
		if (!success) {
			char log[8192]{};
			glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
			ERR_PRINT(String("HCSR OpenGL shader: ") + log);
			valid = false;
		}
	}
	GLuint program = 0;
	if (valid) {
		program = glCreateProgram();
		for (auto shader : shaders) {
			glAttachShader(program, shader);
		}
		glLinkProgram(program);
		GLint success = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &success);
		if (!success) {
			char log[8192]{};
			glGetProgramInfoLog(program, sizeof(log), nullptr, log);
			ERR_PRINT(String("HCSR OpenGL link: ") + log);
			glDeleteProgram(program);
			program = 0;
		}
	}
	for (auto shader : shaders) {
		glDeleteShader(shader);
	}
	return program;
}
void bind_texture(int unit, GLuint texture) {
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, texture);
}
void sampling(GLint filter = GL_NEAREST) {
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
} //namespace

bool HCSRNewestSceneRenderer::draw_gl(RID target, const Size2i &size, const Color &background, bool mipmaps) {
	// Explicit first-pass boundary: a host/document backdrop must never quietly
	// become an unfiltered ordinary surface or sample yesterday's canvas.
	if (ordered_backdrops) {
		WARN_PRINT_ONCE("HCSR OpenGL first pass does not yet support ordered backdrop capture.");
		return false;
	}
	if (!gpu_geometry || !local_hierarchy) {
		return false;
	}
	GLStateGuard guard;
	if (!gl.program) {
		gl.program = compile_program(hcsr::shaders::opengl_vertex, hcsr::shaders::opengl_fragment);
	}
	if (!gl.hierarchy_program) {
		gl.hierarchy_program = compile_program(hcsr::shaders::opengl_fullscreen, hcsr::shaders::opengl_hierarchy);
	}
	if (!gl.program || !gl.hierarchy_program) {
		return false;
	}
	if (!gl.vao) {
		glGenVertexArrays(1, &gl.vao);
	}
	if (!gl.framebuffer) {
		glGenFramebuffers(1, &gl.framebuffer);
	}
	glBindVertexArray(gl.vao);
	GLint limit = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
	auto ensure_data = [&](GLData &data, size_t words) {
		int width = MIN(limit, 1024), height = int((MAX(words, size_t(1)) + width - 1) / width);
		if (height > limit) {
			return false;
		}
		if (data.texture && data.width == width && data.height >= height) {
			return true;
		}
		if (data.texture) {
			glDeleteTextures(1, &data.texture);
		}
		glGenTextures(1, &data.texture);
		bind_texture(3, data.texture);
		sampling();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, width, height, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);
		data.width = width;
		data.height = height;
		return true;
	};
	auto write_data = [&](GLData &data, size_t first, size_t count, const void *bytes) {
		bind_texture(3, data.texture);
		const auto *source = static_cast<const uint32_t *>(bytes);
		while (count) {
			size_t x = first % data.width, y = first / data.width, n = MIN(count, size_t(data.width) - x);
			glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, n, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, source);
			geometry_uploaded_bytes += &data == &gl.geometry ? n * 16 : 0;
			state_uploaded_bytes += &data == &gl.nodes ? n * 16 : 0;
			first += n;
			count -= n;
			source += n * 4;
		}
	};
	if (geometry_dirty || !gl.geometry.texture) {
		if (!ensure_data(gl.geometry, vertices.size()) || !ensure_data(gl.primitives, primitives.size()) || !ensure_data(gl.clips, gpu_clips.size()) || !ensure_data(gl.planes, gpu_planes.size())) {
			return false;
		}
		write_data(gl.geometry, 0, vertices.size(), vertices.ptr());
		write_data(gl.primitives, 0, primitives.size(), primitives.ptr());
		write_data(gl.clips, 0, gpu_clips.size(), gpu_clips.ptr());
		write_data(gl.planes, 0, gpu_planes.size(), gpu_planes.ptr());
		instance_uploaded_bytes += primitives.size() * 16;
		clip_definition_uploaded_bytes += gpu_clips.size() * 16 + gpu_planes.size() * 16;
		geometry_dirty = false;
	}
	if (gl.hierarchy_geometry != hierarchy.geometry || gl.hierarchy_revision != hierarchy.revision) {
		bool full = gl.hierarchy_geometry != hierarchy.geometry || gl.hierarchy_revision != hierarchy.base_revision || hierarchy.full_update;
		if (!ensure_data(gl.nodes, hierarchy.nodes.size() * 5) || !ensure_data(gl.states, hierarchy.state_count * 5) || !ensure_data(gl.outputs, (hierarchy.output_nodes.size() + 3) / 4)) {
			return false;
		}
		if (full) {
			write_data(gl.nodes, 0, hierarchy.nodes.size() * 5, hierarchy.nodes.data());
		} else {
			for (uint32_t index : hierarchy.changed) {
				write_data(gl.nodes, index * 5, 5, &hierarchy.nodes[index]);
			}
		}
		if (gl.hierarchy_geometry != hierarchy.geometry) {
			std::vector<uint32_t> map = hierarchy.output_nodes;
			map.resize((map.size() + 3) / 4 * 4);
			write_data(gl.outputs, 0, map.size() / 4, map.data());
		}
		glUseProgram(gl.hierarchy_program);
		bind_texture(3, gl.nodes.texture);
		bind_texture(4, gl.outputs.texture);
		glUniform1i(glGetUniformLocation(gl.hierarchy_program, "nodes"), 3);
		glUniform1i(glGetUniformLocation(gl.hierarchy_program, "output_nodes"), 4);
		glUniform1ui(glGetUniformLocation(gl.hierarchy_program, "node_count"), hierarchy.nodes.size());
		glUniform1ui(glGetUniformLocation(gl.hierarchy_program, "state_count"), hierarchy.state_count);
		glUniform1ui(glGetUniformLocation(gl.hierarchy_program, "output_width"), gl.states.width);
		glBindFramebuffer(GL_FRAMEBUFFER, gl.framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.states.texture, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			return false;
		}
		glViewport(0, 0, gl.states.width, gl.states.height);
		glDisable(GL_BLEND);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		gl.hierarchy_geometry = hierarchy.geometry;
		gl.hierarchy_revision = hierarchy.revision;
		++hierarchy_evaluations;
	}
	const bool atlas_dirty = uploaded_atlas_revision != resources.atlas_revision();
	if (atlas_dirty || gl.pages.is_empty()) {
		while (gl.pages.size() > resources.page_count()) {
			auto texture = gl.pages[gl.pages.size() - 1];
			if (texture) {
				glDeleteTextures(1, &texture);
			}
			gl.pages.resize(gl.pages.size() - 1);
		}
		gl.pages.resize_initialized(resources.page_count());
		for (int i = 0; i < gl.pages.size(); i++) {
			auto &texture = gl.pages.write[i];
			if (!resources.page_live_allocations(i)) {
				if (texture) {
					glDeleteTextures(1, &texture);
					texture = 0;
					++retired_atlas_pages;
				}
				resources.acknowledge_upload(i);
				continue;
			}
			if (!texture) {
				glGenTextures(1, &texture);
				bind_texture(0, texture);
				sampling(GL_LINEAR);
				auto s = resources.page_size(i);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s.x, s.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
				// Native atlas pixels are straight-alpha BGRA on every backend.
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
				auto pixels = resources.page_pixels(i, Rect2i(Point2i(), s));
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.x, s.y, GL_RGBA, GL_UNSIGNED_BYTE, pixels.ptr());
				uploaded_bytes += pixels.size();
			} else {
				bind_texture(0, texture);
				for (auto region : resources.page_updates(i)) {
					auto pixels = resources.page_pixels(i, region);
					glTexSubImage2D(GL_TEXTURE_2D, 0, region.position.x, region.position.y, region.size.x, region.size.y, GL_RGBA, GL_UNSIGNED_BYTE, pixels.ptr());
					uploaded_bytes += pixels.size();
				}
			}
			resources.acknowledge_upload(i);
		}
		uploaded_atlas_revision = resources.atlas_revision();
	}
	int pool_index = -1;
	for (int i = 0; i < gl.pools.size(); i++) {
		if (gl.pools[i].output == target) {
			pool_index = i;
		}
	}
	if (pool_index < 0) {
		pool_index = gl.pools.size();
		GLPool p;
		p.output = target;
		gl.pools.push_back(p);
	}
	auto &pool = gl.pools.write[pool_index];
	auto release_target = [](GLTarget &t) {if(t.texture){glDeleteTextures(1,&t.texture);
}if(t.framebuffer){glDeleteFramebuffers(1,&t.framebuffer);
}t={}; };
	if (pool.size != size) {
		for (auto &t : pool.groups) {
			release_target(t);
		}
		release_target(pool.blend);
		pool.size = size;
	}
	auto ensure_target = [&](GLTarget &t) {
		if (t.texture) {
			return true;
		}
		glGenTextures(1, &t.texture);
		bind_texture(0, t.texture);
		sampling(GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glGenFramebuffers(1, &t.framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, t.framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0);
		++group_allocations;
		return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	};
	const bool spatial = std::any_of(batches.begin(), batches.end(), [](const Batch &b) { return b.kind >= hcsr::render::group_filter_pass; });
	int targets = int(group_depth) + (spatial ? 1 : 0);
	while (pool.groups.size() > targets) {
		release_target(pool.groups.write[pool.groups.size() - 1]);
		pool.groups.resize(pool.groups.size() - 1);
	}
	pool.groups.resize(targets);
	for (auto &t : pool.groups) {
		if (!ensure_target(t)) {
			return false;
		}
	}
	bool blending = std::any_of(batches.begin(), batches.end(), [](const Batch &b) { return b.blend_mode != 0; });
	if (blending && !ensure_target(pool.blend)) {
		return false;
	}
	if (!blending) {
		release_target(pool.blend);
	}
	const GLuint output = RenderingServer::get_singleton()->texture_get_native_handle(target);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		return false;
	}
	glViewport(0, 0, size.x, size.y);
	const float clear[] = { background.r, background.g, background.b, background.a };
	glClearBufferfv(GL_COLOR, 0, clear);
	glUseProgram(gl.program);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	// Shared paint functions return straight color; fixed-function blending
	// stores premultiplied pixels, exactly like RenderingDevice::create_blend.
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	const char *names[] = { "atlas", "mask_atlas", "blend_atlas", "geometry_data", "state_data", "clip_data", "plane_data", "primitive_data" };
	for (int i = 0; i < 8; i++) {
		glUniform1i(glGetUniformLocation(gl.program, names[i]), i);
	}
	const GLuint data[] = { gl.geometry.texture, gl.states.texture, gl.clips.texture, gl.planes.texture, gl.primitives.texture };
	for (int i = 0; i < 5; i++) {
		bind_texture(i + 3, data[i]);
	}
	glUniform1ui(glGetUniformLocation(gl.program, "params.gpu_geometry"), 1);
	glUniform1f(glGetUniformLocation(gl.program, "params.width"), logical_width);
	glUniform1f(glGetUniformLocation(gl.program, "params.height"), logical_height);
	glUniform1f(glGetUniformLocation(gl.program, "params.target_width"), size.x);
	glUniform1f(glGetUniformLocation(gl.program, "params.target_height"), size.y);
	glUniform1f(glGetUniformLocation(gl.program, "params.mask_mode"), 0);
	glUniform4f(glGetUniformLocation(gl.program, "params.group_bounds"), 0, 0, size.x, size.y);
	last_draw_calls = 0;
	last_render_passes = 1;
	last_disjoint_groups = false;
	hcsr::render::transparent_group_submission visibility;
	for (auto &batch : batches) {
		batch.hidden = visibility.skip(batch.kind, batch.depth, batch.opacity, batch.opacity_state_plus_one, [&](uint32_t index) { return hierarchy.reference_opacity(index); });
	}
	for (const auto &batch : batches) {
		if (batch.hidden) {
			continue;
		}
		GLuint source = 0;
		if (batch.kind) {
			source = pool.groups[batch.source_scratch ? group_depth : batch.depth - 1].texture;
			GLuint parent = batch.depth == 1 ? gl.framebuffer : pool.groups[batch.depth - 2].framebuffer;
			if (batch.blend_mode) {
				glBindFramebuffer(GL_READ_FRAMEBUFFER, parent);
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pool.blend.framebuffer);
				glBlitFramebuffer(0, 0, size.x, size.y, 0, 0, size.x, size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
			GLuint destination = (batch.kind == hcsr::render::group_filter_pass || batch.kind == hcsr::render::group_shadow_blur) ? pool.groups[group_depth].framebuffer
					: (batch.kind == HCSR_GROUP_BEGIN || batch.kind == hcsr::render::group_shadow_apply)						  ? pool.groups[batch.depth - 1].framebuffer
																																  : parent;
			glBindFramebuffer(GL_FRAMEBUFFER, destination);
			++last_render_passes;
			if (batch.kind != HCSR_GROUP_END && batch.kind != hcsr::render::group_shadow_apply) {
				const float zero[4]{};
				glClearBufferfv(GL_COLOR, 0, zero);
			}
			if (batch.kind == HCSR_GROUP_BEGIN) {
				continue;
			}
		} else {
			source = gl.pages[batch.page];
		}
		bind_texture(0, source);
		bind_texture(1, gl.pages[batch.mask_page]);
		bind_texture(2, blending ? pool.blend.texture : gl.pages[0]);
		if (batch.kind == hcsr::render::group_shadow_apply) {
			glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE);
		} else {
			glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		}
		glUniform1ui(glGetUniformLocation(gl.program, "params.first"), batch.first);
		glDrawArraysInstanced(GL_TRIANGLES, 0, 6, batch.count);
		++last_draw_calls;
		if (batch.kind == hcsr::render::group_filter_pass) {
			SWAP(pool.groups.write[batch.depth - 1], pool.groups.write[group_depth]);
		}
	}
	if (mipmaps) {
		bind_texture(0, output);
		glGenerateMipmap(GL_TEXTURE_2D);
	}
#ifdef TOOLS_ENABLED
	if (auto texture = GLES3::TextureStorage::get_singleton()->get_texture(target)) {
		texture->image_cache_2d.unref();
	}
#endif
	uploaded = true;
	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		ERR_PRINT(vformat("HCSR OpenGL submission error %d", error));
		return false;
	}
	return true;
}

void HCSRNewestSceneRenderer::release_gl() {
	GLStateGuard guard;
	for (auto &p : gl.pools) {
		for (auto t : p.groups) {
			glDeleteTextures(1, &t.texture);
			glDeleteFramebuffers(1, &t.framebuffer);
		}
		glDeleteTextures(1, &p.blend.texture);
		glDeleteFramebuffers(1, &p.blend.framebuffer);
	}
	for (auto texture : gl.pages) {
		if (texture) {
			glDeleteTextures(1, &texture);
		}
	}
	for (auto data : { gl.geometry, gl.states, gl.clips, gl.planes, gl.primitives, gl.nodes, gl.outputs }) {
		if (data.texture) {
			glDeleteTextures(1, &data.texture);
		}
	}
	if (gl.program) {
		glDeleteProgram(gl.program);
	}
	if (gl.hierarchy_program) {
		glDeleteProgram(gl.hierarchy_program);
	}
	if (gl.vao) {
		glDeleteVertexArrays(1, &gl.vao);
	}
	if (gl.framebuffer) {
		glDeleteFramebuffers(1, &gl.framebuffer);
	}
	gl = GLResources();
}
void HCSRNewestSceneRenderer::release_gl_output(RID target) {
	for (int i = 0; i < gl.pools.size(); i++) {
		if (gl.pools[i].output == target) {
			GLStateGuard guard;
			for (auto t : gl.pools[i].groups) {
				glDeleteTextures(1, &t.texture);
				glDeleteFramebuffers(1, &t.framebuffer);
			}
			auto t = gl.pools[i].blend;
			glDeleteTextures(1, &t.texture);
			glDeleteFramebuffers(1, &t.framebuffer);
			gl.pools.remove_at(i);
			return;
		}
	}
}
#else
bool HCSRNewestSceneRenderer::draw_gl(RID, const Size2i &, const Color &, bool) {
	return false;
}
void HCSRNewestSceneRenderer::release_gl_output(RID) {}
#endif

/*
* Descent 3: Piccu Engine
* Copyright (C) 2024 Parallax Software
* Copyright (C) 2024 SaladBadger
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "gl_local.h"
#include "game.h"

#include <cstring>
#include <vector>

//The number of vertex attributes the legacy code used.
constexpr int NUM_LEGACY_VERTEX_ATTRIBS = 7;
//The count of vertices that each buffer will store
constexpr int NUM_VERTS_PER_BUFFER = 640000;

static bool GL4DrawTargetIsFramebuffer(GLuint framebuffer)
{
	GLint current_draw = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_draw);
	return (GLuint)current_draw == framebuffer;
}

static void GL4UseSceneDrawBuffersWithoutAOClass(bool include_motion_vectors)
{
	const GLenum draw_buffers[4] =
	{
		GL_COLOR_ATTACHMENT0,
		static_cast<GLenum>(include_motion_vectors ? GL_COLOR_ATTACHMENT1 : GL_NONE),
		GL_COLOR_ATTACHMENT2,
		GL_NONE
	};
	glDrawBuffers(4, draw_buffers);
	GL_ConfigurePostMaskBlend();
}

static void GL4UseSceneDrawBuffersForCurrentDraw(bool include_motion_vectors, bool include_ao_class)
{
	const GLenum draw_buffers[4] =
	{
		GL_COLOR_ATTACHMENT0,
		static_cast<GLenum>(include_motion_vectors ? GL_COLOR_ATTACHMENT1 : GL_NONE),
		GL_COLOR_ATTACHMENT2,
		static_cast<GLenum>(include_ao_class ? GL_COLOR_ATTACHMENT3 : GL_NONE)
	};
	glDrawBuffers(4, draw_buffers);
	GL_ConfigurePostMaskBlend();
}

static void GL4BuildOrtho(float* mat, float left, float right, float bottom, float top, float znear, float zfar)
{
	memset(mat, 0, sizeof(float[16]));
	mat[0] = 2 / (right - left);
	mat[5] = 2 / (top - bottom);
	mat[10] = -2 / (zfar - znear);
	mat[12] = -((right + left) / (right - left));
	mat[13] = -((top + bottom) / (top - bottom));
	mat[14] = -((zfar + znear) / (zfar - znear));
	mat[15] = 1;
}

static float GL4FontBatchIdentity[16] =
{
	1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 1, 0,
	0, 0, 0, 1
};

static void GL4WriteMotionVectorDebugPixel(GLuint framebuffer, int x, int y, const float value[4])
{
	GLint old_draw = 0;
	GLint old_draw_buffer = GL_COLOR_ATTACHMENT0;
	GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);
	GLint old_scissor[4];
	GLboolean color_mask[4];
	GLboolean blend_enabled = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
	glGetIntegerv(GL_DRAW_BUFFER, &old_draw_buffer);
	glGetIntegerv(GL_SCISSOR_BOX, old_scissor);
	glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
	GLenum draw_buffer = GL_COLOR_ATTACHMENT1;
	glDrawBuffers(1, &draw_buffer);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable(GL_BLEND);
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, y, 1, 1);
	glClearBufferfv(GL_COLOR, 0, value);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
	glDrawBuffer(old_draw_buffer);
	glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
	if (blend_enabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	if (scissor_was_enabled)
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
	}
	else
	{
		glDisable(GL_SCISSOR_TEST);
	}
}

static int GL4FontBatchIndexForAlpha(int alpha_type)
{
	return alpha_type == AT_SATURATE_TEXTURE ? 1 : 0;
}

void GL4Renderer::UseDrawVAO()
{
	glBindVertexArray(drawvao);
}

void GL4Renderer::RestoreLegacy()
{
	glBindVertexArray(drawvao);
}

bool GL4Renderer::MotionVectorTargetEnabled() const
{
	return OpenGL_preferred_state.motion_vector_mode != RENDERER_MOTION_VECTOR_OFF &&
		motion_vectors.velocity_texture != 0;
}

bool GL4Renderer::PixelMotionVectorModeEnabled() const
{
	return OpenGL_preferred_state.motion_vector_mode == RENDERER_MOTION_VECTOR_PIXEL &&
		motion_vectors.velocity_texture != 0;
}

bool GL4Renderer::MotionVectorWritesEnabled() const
{
	return PixelMotionVectorModeEnabled() && !motion_vectors_capture_locked;
}

bool GL4Renderer::CurrentDrawWritesPixelMotionVectors() const
{
	if (!MotionVectorWritesEnabled() || OpenGL_state.cur_zbuffer_state == 0)
		return false;

	switch (OpenGL_state.cur_alpha_type)
	{
	case AT_ALWAYS:
	case AT_TEXTURE:
		return true;
	case AT_CONSTANT_TEXTURE:
		return OpenGL_state.cur_alpha == 255 && ao_class_draw_value == RENDERER_AO_CLASS_TERRAIN;
	default:
		return false;
	}
}

void GL4Renderer::DrawMotionVectorDebugPreview(int supersampling_factor)
{
	renderer_motion_vector_debug_sample debug_sample = {};
	debug_sample.mode = OpenGL_preferred_state.motion_vector_mode;

	if (!OpenGL_preferred_state.motion_vector_debug_preview || Game_mode == GM_NONE ||
		!MotionVectorTargetEnabled() || motionvectordebugshader.Handle() == 0 ||
		post_present_framebuffer.Handle() == 0)
	{
		rend_SetMotionVectorDebugSample(nullptr);
		return;
	}

	GLuint velocity_texture = motion_vectors.TextureForRead(framebuffers[framebuffer_current_draw].Handle());
	if (velocity_texture == 0 || motion_vectors.width == 0 || motion_vectors.height == 0)
	{
		rend_SetMotionVectorDebugSample(&debug_sample);
		return;
	}

	if (supersampling_factor < 1)
		supersampling_factor = 1;
	const int framebuffer_logical_bottom_offset =
		framebuffer_logical_height - OpenGL_state.screen_height - framebuffer_logical_offset_y;
	const float source_width = (float)motion_vectors.width;
	const float source_height = (float)motion_vectors.height;
	const float uv_origin_x = (float)(framebuffer_logical_offset_x * supersampling_factor) / source_width;
	const float uv_origin_y = (float)(framebuffer_logical_bottom_offset * supersampling_factor) / source_height;
	const float uv_scale_x = (float)(OpenGL_state.screen_width * supersampling_factor) / source_width;
	const float uv_scale_y = (float)(OpenGL_state.screen_height * supersampling_factor) / source_height;
	const int center_x = std::max(0, std::min((int)motion_vectors.width - 1,
		(int)((uv_origin_x + uv_scale_x * 0.5f) * source_width)));
	const int center_y = std::max(0, std::min((int)motion_vectors.height - 1,
		(int)((uv_origin_y + uv_scale_y * 0.5f) * source_height)));

	GLint old_read = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
	GLint old_read_buffer = GL_COLOR_ATTACHMENT0;
	glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, motion_vectors.resolve_framebuffer);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	float center_velocity[2] = {};
	glReadPixels(center_x, center_y, 1, 1, GL_RG, GL_FLOAT, center_velocity);
	float max_velocity[2] = {};
	float max_mag_squared = 0.0f;
	int max_x = center_x;
	int max_y = center_y;
	const int sample_columns = 9;
	const int sample_rows = 7;
	for (int row = 0; row < sample_rows; row++)
	{
		for (int column = 0; column < sample_columns; column++)
		{
			const float u = ((float)column + 0.5f) / (float)sample_columns;
			const float v = ((float)row + 0.5f) / (float)sample_rows;
			const int sample_x = std::max(0, std::min((int)motion_vectors.width - 1,
				(int)((uv_origin_x + uv_scale_x * u) * source_width)));
			const int sample_y = std::max(0, std::min((int)motion_vectors.height - 1,
				(int)((uv_origin_y + uv_scale_y * v) * source_height)));
			float velocity[2] = {};
			glReadPixels(sample_x, sample_y, 1, 1, GL_RG, GL_FLOAT, velocity);
			const float mag_squared = velocity[0] * velocity[0] + velocity[1] * velocity[1];
			if (mag_squared > max_mag_squared)
			{
				max_mag_squared = mag_squared;
				max_velocity[0] = velocity[0];
				max_velocity[1] = velocity[1];
				max_x = sample_x;
				max_y = sample_y;
			}
		}
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
	glReadBuffer(old_read_buffer);
	debug_sample.valid = true;
	debug_sample.x = center_x;
	debug_sample.y = center_y;
	debug_sample.width = (int)motion_vectors.width;
	debug_sample.height = (int)motion_vectors.height;
	debug_sample.vx = center_velocity[0];
	debug_sample.vy = center_velocity[1];
	debug_sample.max_x = max_x;
	debug_sample.max_y = max_y;
	debug_sample.max_vx = max_velocity[0];
	debug_sample.max_vy = max_velocity[1];
	debug_sample.max_mag = max_mag_squared;

	const float probe_value[4] = { 0.03125f, -0.0625f, 0.0f, 0.0f };
	const float restore_value[4] = { center_velocity[0], center_velocity[1], 0.0f, 0.0f };
	GL4WriteMotionVectorDebugPixel(framebuffers[framebuffer_current_draw].Handle(), center_x, center_y, probe_value);
	motion_vectors.TextureForRead(framebuffers[framebuffer_current_draw].Handle());
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
	glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, motion_vectors.resolve_framebuffer);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	float probe_velocity[2] = {};
	glReadPixels(center_x, center_y, 1, 1, GL_RG, GL_FLOAT, probe_velocity);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
	glReadBuffer(old_read_buffer);
	debug_sample.probe_valid = true;
	debug_sample.probe_vx = probe_velocity[0];
	debug_sample.probe_vy = probe_velocity[1];
	GL4WriteMotionVectorDebugPixel(framebuffers[framebuffer_current_draw].Handle(), center_x, center_y, restore_value);
	velocity_texture = motion_vectors.TextureForRead(framebuffers[framebuffer_current_draw].Handle());
	rend_SetMotionVectorDebugSample(&debug_sample);

	motionvectordebugshader.Use();
	if (motionvectordebug_velocity_source != -1)
		glUniform1i(motionvectordebug_velocity_source, 0);
	if (motionvectordebug_uv_origin != -1)
		glUniform2f(motionvectordebug_uv_origin, uv_origin_x, uv_origin_y);
	if (motionvectordebug_uv_scale != -1)
		glUniform2f(motionvectordebug_uv_scale, uv_scale_x, uv_scale_y);
	if (motionvectordebug_screen_size != -1)
		glUniform2f(motionvectordebug_screen_size, (float)OpenGL_state.screen_width, (float)OpenGL_state.screen_height);

	rend_ClearBoundTextures();
	GL_BindFramebufferTexture(velocity_texture, 0, GL_NEAREST);

	const GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
	const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean cull_was_enabled = glIsEnabled(GL_CULL_FACE);
	const GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean color_mask[4];
	GLboolean depth_mask;
	glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
	GLint old_src_rgb = GL_ONE;
	GLint old_dst_rgb = GL_ZERO;
	GLint old_src_alpha = GL_ONE;
	GLint old_dst_alpha = GL_ZERO;
	glGetIntegerv(GL_BLEND_SRC_RGB, &old_src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &old_dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &old_src_alpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &old_dst_alpha);
	GLint old_viewport[4];
	glGetIntegerv(GL_VIEWPORT, old_viewport);
	GLint old_draw = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);

	glBindVertexArray(GL_GetFramebufferVAO());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, post_present_framebuffer.Handle());
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glViewport(0, 0, post_present_framebuffer.Width(), post_present_framebuffer.Height());
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_FALSE);
	rend_RecordDrawCall(RENDERER_DRAW_CALL_POSTPROCESS);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
	glReadBuffer(old_read_buffer);
	glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
	glBlendFuncSeparate(old_src_rgb, old_dst_rgb, old_src_alpha, old_dst_alpha);
	if (blend_was_enabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	if (depth_was_enabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (cull_was_enabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
	if (scissor_was_enabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
	glDepthMask(depth_mask);
	rend_ClearBoundTextures();
	RestoreLegacy();
}

void GL4Renderer::UseSceneDrawBuffers()
{
	post_protection_mask.UseSceneDrawBuffers(framebuffers[framebuffer_current_draw].Handle(),
		MotionVectorWritesEnabled());
}

void GL4Renderer::InitPersistentDrawBuffer(size_t size)
{
	//Due to names becoming immutable when using buffer storage,
	//need to recycle buffers by explicitly deleting the old one. OpenGL maintains its lifetime until it is done
	if (drawbuffer != 0)
	{
		glBindBuffer(GL_ARRAY_BUFFER, drawbuffer);
		glUnmapBuffer(GL_ARRAY_BUFFER);
		glDeleteBuffers(1, &drawbuffer);
	}

	glGenBuffers(1, &drawbuffer);
	glBindBuffer(GL_ARRAY_BUFFER, drawbuffer);

	glBufferStorage(GL_ARRAY_BUFFER, size, nullptr, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif

	//of course glVertexAttribBinding has to only be core in 4.3+
	// because OpenGL makes only the best API design decisions at every step.

	//attrib 0: position
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), 0);

	//attrib 1: color
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, color));

	//attrib 2: uv
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, tex_coord));

	//attrib 3: uv 2
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, tex_coord2));

	//attrib 4: per-pixel lighting normal
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, normal));

	//attrib 5: motion vector
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, motion_velocity_x));

	//attrib 6: perspective-correct world position for pixel motion vectors
	glEnableVertexAttribArray(6);
	glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, motion_world_position));
#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif

	drawbuffermap = glMapBufferRange(GL_ARRAY_BUFFER, 0, size, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif
}

void GL4Renderer::DestroyPersistentDrawBuffer()
{
	if (OpenGL_buffer_storage_enabled && drawbuffer)
	{
		glBindBuffer(GL_ARRAY_BUFFER, drawbuffer);
		glUnmapBuffer(GL_ARRAY_BUFFER);
		glDeleteBuffers(1, &drawbuffer);
		drawbuffer = 0;
	}
}

void GL4Renderer::InitMotionVectorDraw()
{
	if (motionvector_vao != 0)
		return;

	glGenVertexArrays(1, &motionvector_vao);
	glGenBuffers(1, &motionvector_vbo);

	glBindVertexArray(motionvector_vao);
	glBindBuffer(GL_ARRAY_BUFFER, motionvector_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(gl_motion_vertex) * 100, nullptr, GL_STREAM_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(gl_motion_vertex), 0);

	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(gl_motion_vertex),
		(const void*)offsetof(gl_motion_vertex, velocity_x));

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(drawvao);
}

void GL4Renderer::DestroyMotionVectorDraw()
{
	glDeleteBuffers(1, &motionvector_vbo);
	glDeleteVertexArrays(1, &motionvector_vao);
	motionvector_vbo = 0;
	motionvector_vao = 0;
}

void GL4Renderer::DrawMotionVectorPolygon(int nv, g3Point** p)
{
	if (!motion_object_active || motionvectorshader.Handle() == 0 ||
		OpenGL_preferred_state.motion_vector_mode != RENDERER_MOTION_VECTOR_VERTEX ||
		motion_vectors_capture_locked || motionvector_vao == 0 ||
		motion_vectors.velocity_texture == 0 || nv <= 0)
	{
		return;
	}

	gl_motion_vertex motion_vertices[100];
	for (int i = 0; i < nv; i++)
	{
		if (!p[i]->p3_motion_valid)
			return;

		motion_vertices[i].x = GL_vertices[i].vert.x;
		motion_vertices[i].y = GL_vertices[i].vert.y;
		motion_vertices[i].z = GL_vertices[i].vert.z;
		motion_vertices[i].velocity_x = p[i]->p3_sx - p[i]->p3_prev_sx;
		motion_vertices[i].velocity_y = p[i]->p3_sy - p[i]->p3_prev_sy;
	}

	GLboolean color_mask[4];
	GLboolean depth_mask;
	GLboolean blend_enabled = glIsEnabled(GL_BLEND);
	GLint old_draw = 0;
	GLint old_draw_buffer = 0;
	glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
	glGetIntegerv(GL_DRAW_BUFFER, &old_draw_buffer);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[framebuffer_current_draw].Handle());
	GLenum draw_buffer = GL_COLOR_ATTACHMENT1;
	glDrawBuffers(1, &draw_buffer);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);

	motionvectorshader.Use();
	if (motionvector_screen_size != -1)
		glUniform2f(motionvector_screen_size,
			(float)(OpenGL_state.clip_x2 - OpenGL_state.clip_x1),
			(float)(OpenGL_state.clip_y2 - OpenGL_state.clip_y1));

	glBindVertexArray(motionvector_vao);
	glBindBuffer(GL_ARRAY_BUFFER, motionvector_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(gl_motion_vertex) * nv, motion_vertices, GL_STREAM_DRAW);
	rend_RecordDrawCall(RENDERER_DRAW_CALL_MOTION_VECTOR);
	glDrawArrays(GL_TRIANGLE_FAN, 0, nv);
	motion_vectors_dirty = true;

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
	glDrawBuffer(old_draw_buffer);
	if ((GLuint)old_draw == framebuffers[framebuffer_current_draw].Handle())
		UseSceneDrawBuffers();
	glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
	glDepthMask(depth_mask);
	if (blend_enabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	UseDrawVAO();
}

void GL4Renderer::DrawMotionVectorTriangles(const gl_motion_vertex* vertices, int nv)
{
	if (!motion_object_active || motionvectorshader.Handle() == 0 ||
		OpenGL_preferred_state.motion_vector_mode != RENDERER_MOTION_VECTOR_VERTEX ||
		motion_vectors_capture_locked || motionvector_vao == 0 ||
		motion_vectors.velocity_texture == 0 || !vertices || nv <= 0)
	{
		return;
	}

	GLboolean color_mask[4];
	GLboolean depth_mask;
	GLboolean blend_enabled = glIsEnabled(GL_BLEND);
	GLint old_draw = 0;
	GLint old_draw_buffer = 0;
	glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
	glGetIntegerv(GL_DRAW_BUFFER, &old_draw_buffer);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[framebuffer_current_draw].Handle());
	GLenum draw_buffer = GL_COLOR_ATTACHMENT1;
	glDrawBuffers(1, &draw_buffer);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);

	motionvectorshader.Use();
	if (motionvector_screen_size != -1)
		glUniform2f(motionvector_screen_size,
			(float)(OpenGL_state.clip_x2 - OpenGL_state.clip_x1),
			(float)(OpenGL_state.clip_y2 - OpenGL_state.clip_y1));

	glBindVertexArray(motionvector_vao);
	glBindBuffer(GL_ARRAY_BUFFER, motionvector_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(gl_motion_vertex) * nv, vertices, GL_STREAM_DRAW);
	rend_RecordDrawCall(RENDERER_DRAW_CALL_MOTION_VECTOR);
	glDrawArrays(GL_TRIANGLES, 0, nv);
	motion_vectors_dirty = true;

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
	glDrawBuffer(old_draw_buffer);
	if ((GLuint)old_draw == framebuffers[framebuffer_current_draw].Handle())
		UseSceneDrawBuffers();
	glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
	glDepthMask(depth_mask);
	if (blend_enabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	UseDrawVAO();
}

int GL4Renderer::CopyVertices(int numvertices)
{
	return CopyVertices(GL_vertices, numvertices);
}

int GL4Renderer::CopyVertices(const gl_vertex* vertices, int numvertices)
{
	if (OpenGL_buffer_storage_enabled)
	{
		if (nextcommittedvertex + numvertices > NUM_VERTS_PER_BUFFER)
		{
			size_t buffersize = NUM_VERTS_PER_BUFFER * sizeof(gl_vertex);
			InitPersistentDrawBuffer(buffersize);
			nextcommittedvertex = 0;
		}

		glBindBuffer(GL_ARRAY_BUFFER, drawbuffer);
		int startoffset = nextcommittedvertex;
		void* dataptr = (void*)((uintptr_t)drawbuffermap + startoffset * sizeof(gl_vertex));
		memcpy(dataptr, vertices, numvertices * sizeof(gl_vertex));

		nextcommittedvertex += numvertices;

		return startoffset;
	}
	else
	{
		glBindBuffer(GL_ARRAY_BUFFER, drawbuffer);
		if (nextcommittedvertex + numvertices > NUM_VERTS_PER_BUFFER)
		{
			size_t buffersize = NUM_VERTS_PER_BUFFER * sizeof(gl_vertex);
			glBufferData(GL_ARRAY_BUFFER, buffersize, nullptr, GL_STREAM_DRAW);
			nextcommittedvertex = 0;
		}

		int startoffset = nextcommittedvertex;

		void* dataptr = glMapBufferRange(GL_ARRAY_BUFFER, startoffset * sizeof(gl_vertex), numvertices * sizeof(gl_vertex), GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
		memcpy(dataptr, vertices, numvertices * sizeof(gl_vertex));
		glUnmapBuffer(GL_ARRAY_BUFFER);

		nextcommittedvertex += numvertices;

		return startoffset;
	}
}

void GL4Renderer::BuildDrawVertex(gl_vertex& vert, const g3Point* pnt, float xscalar, float yscalar,
	ubyte fr, ubyte fg, ubyte fb)
{
	float alpha = Alpha_multiplier * OpenGL_Alpha_factor;
	if (OpenGL_state.cur_alpha_type & ATF_VERTEX)
		alpha = pnt->p3_a * Alpha_multiplier * OpenGL_Alpha_factor;

	if (OpenGL_state.cur_light_state != LS_NONE)
	{
		if (OpenGL_state.cur_light_state == LS_FLAT_GOURAUD)
		{
			vert.color.r = fr;
			vert.color.g = fg;
			vert.color.b = fb;
			vert.color.a = (ubyte)alpha;
		}
		else
		{
			if (OpenGL_state.cur_color_model == CM_MONO)
			{
				vert.color.r = pnt->p3_l * 255;
				vert.color.g = pnt->p3_l * 255;
				vert.color.b = pnt->p3_l * 255;
				vert.color.a = (ubyte)alpha;
			}
			else
			{
				vert.color.r = pnt->p3_r * 255;
				vert.color.g = pnt->p3_g * 255;
				vert.color.b = pnt->p3_b * 255;
				vert.color.a = (ubyte)alpha;
			}
		}
	}
	else
	{
		if (OpenGL_state.cur_texture_type != 0)
		{
			vert.color.r = 255;
			vert.color.g = 255;
			vert.color.b = 255;
			vert.color.a = (ubyte)alpha;
		}
		else
		{
			vert.color.r = fr;
			vert.color.g = fg;
			vert.color.b = fb;
			vert.color.a = (ubyte)alpha;
		}
	}

	if (OpenGL_state.cur_texture_type != 0)
	{
		float texw = 1.0 / (pnt->p3_z + Z_bias);
		vert.tex_coord.s = pnt->p3_u * texw;
		vert.tex_coord.t = pnt->p3_v * texw;
		vert.tex_coord.w = texw;

		if (Overlay_type != OT_NONE)
		{
			vert.tex_coord2.s = pnt->p3_u2 * xscalar * texw;
			vert.tex_coord2.t = pnt->p3_v2 * yscalar * texw;
			vert.tex_coord2.w = texw;
		}
	}

	if (OpenGL_state.cur_light_state == LS_PHONG || per_pixel_dynamic_light_count > 0)
	{
		float payloadw = 1.0f / (pnt->p3_z + Z_bias);
		vert.normal.x = pnt->p3_vecPreRot.x * payloadw;
		vert.normal.y = pnt->p3_vecPreRot.y * payloadw;
		vert.normal.z = pnt->p3_vecPreRot.z * payloadw;
		vert.normal.w = payloadw;
	}

	vert.vert.x = pnt->p3_sx;
	vert.vert.y = pnt->p3_sy;
	vert.motion_velocity_x = 0.0f;
	vert.motion_velocity_y = 0.0f;
	vert.motion_world_position.x = 0.0f;
	vert.motion_world_position.y = 0.0f;
	vert.motion_world_position.z = 0.0f;
	vert.motion_world_position.w = 0.0f;
	if (motion_object_active && OpenGL_preferred_state.motion_vector_mode == RENDERER_MOTION_VECTOR_PIXEL && pnt->p3_motion_valid)
	{
		const float screen_width = OpenGL_state.screen_width > 0 ? (float)OpenGL_state.screen_width : 1.0f;
		const float screen_height = OpenGL_state.screen_height > 0 ? (float)OpenGL_state.screen_height : 1.0f;
		vert.motion_velocity_x = (pnt->p3_sx - pnt->p3_prev_sx) / screen_width;
		vert.motion_velocity_y = -(pnt->p3_sy - pnt->p3_prev_sy) / screen_height;
	}
	if (CurrentDrawWritesPixelMotionVectors())
	{
		float payloadw = 1.0f / (pnt->p3_z + Z_bias);
		vert.motion_world_position.x = pnt->p3_vecPreRot.x * payloadw;
		vert.motion_world_position.y = pnt->p3_vecPreRot.y * payloadw;
		vert.motion_world_position.z = pnt->p3_vecPreRot.z * payloadw;
		vert.motion_world_position.w = payloadw;
	}

	float z = std::max(0.f, std::min(1.0f, 1.0f - (1.0f / (pnt->p3_z + Z_bias))));
	vert.vert.z = -z;
}

void GL4Renderer::SetFontBatchFullscreenDrawState(GLint old_viewport[4])
{
	glGetIntegerv(GL_VIEWPORT, old_viewport);

	float projection[16];
	GL4BuildOrtho(projection, 0, (float)OpenGL_state.screen_width, (float)OpenGL_state.screen_height, 0, 0, 1);
	UpdateLegacyBlock(projection, GL4FontBatchIdentity);

	if (GL4DrawTargetIsFramebuffer(post_present_framebuffer.Handle()) || !framebuffer_ok)
	{
		glViewport(0, 0, OpenGL_state.screen_width, OpenGL_state.screen_height);
	}
	else
	{
		glViewport(ScaledX(0), FramebufferHeight() - ScaledY(OpenGL_state.screen_height),
			ScaledW(OpenGL_state.screen_width), ScaledH(OpenGL_state.screen_height));
	}
}

void GL4Renderer::RestoreFontBatchDrawState(const GLint old_viewport[4])
{
	int clip_width = OpenGL_state.clip_x2 - OpenGL_state.clip_x1;
	int clip_height = OpenGL_state.clip_y2 - OpenGL_state.clip_y1;
	if (clip_width <= 0)
		clip_width = OpenGL_state.screen_width;
	if (clip_height <= 0)
		clip_height = OpenGL_state.screen_height;

	float projection[16];
	GL4BuildOrtho(projection, 0, (float)clip_width, (float)clip_height, 0, 0, 1);
	UpdateLegacyBlock(projection, GL4FontBatchIdentity);
	glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
}

bool GL4Renderer::FontBatchHasVertices() const
{
	return !font_batch_vertices[0].empty() || !font_batch_vertices[1].empty();
}

void GL4Renderer::ClearFontBatchVertices()
{
	font_batch_vertices[0].clear();
	font_batch_vertices[1].clear();
}

void GL4Renderer::FlushFontBatchVertices(int batch_index)
{
	if (batch_index < 0 || batch_index >= 2 || font_batch_vertices[batch_index].empty())
		return;

	if (batch_index == 1)
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	else
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	float old_ao_suppression = ao_suppression_draw_value;
	float old_bloom_suppression = bloom_suppression_draw_value;
	if (ao_suppression_draw_value != 1.0f || bloom_suppression_draw_value != 1.0f)
		legacy_draw_uniforms_dirty = true;
	ao_suppression_draw_value = 1.0f;
	bloom_suppression_draw_value = 1.0f;
	post_protection_mask_dirty = true;

	fontshader.Use();
	if (font_texture_array != 0)
	{
		if (UseMultitexture && Last_texel_unit_set != 0)
		{
			glActiveTexture(GL_TEXTURE0);
			Last_texel_unit_set = 0;
		}
		glBindTexture(GL_TEXTURE_2D_ARRAY, font_texture_array);
	}

	const int offset = CopyVertices(font_batch_vertices[batch_index].data(), (int)font_batch_vertices[batch_index].size());
	const bool suppress_ao_class_write = framebuffer_ok &&
		OpenGL_state.cur_zbuffer_state == 0 &&
		GL4DrawTargetIsFramebuffer(framebuffers[framebuffer_current_draw].Handle());
	if (suppress_ao_class_write)
		GL4UseSceneDrawBuffersWithoutAOClass(false);
	rend_RecordDrawCall(RENDERER_DRAW_CALL_FONT);
	glDrawArrays(GL_TRIANGLES, offset, (GLsizei)font_batch_vertices[batch_index].size());
	if (suppress_ao_class_write)
		UseSceneDrawBuffers();

	font_batch_vertices[batch_index].clear();

	if (ao_suppression_draw_value != old_ao_suppression || bloom_suppression_draw_value != old_bloom_suppression)
		legacy_draw_uniforms_dirty = true;
	ao_suppression_draw_value = old_ao_suppression;
	bloom_suppression_draw_value = old_bloom_suppression;
	ShaderProgram::ClearBinding();
}

void GL4Renderer::FlushFontBatch()
{
	if (!FontBatchHasVertices())
		return;

	GLint old_viewport[4] = {};
	GLboolean depth_test_was_enabled = glIsEnabled(GL_DEPTH_TEST);
	GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
	GLint blend_src_rgb = GL_ONE;
	GLint blend_dst_rgb = GL_ZERO;
	GLint blend_src_alpha = GL_ONE;
	GLint blend_dst_alpha = GL_ZERO;
	glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);

	SetFontBatchFullscreenDrawState(old_viewport);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);

	FlushFontBatchVertices(0);
	FlushFontBatchVertices(1);

	if (depth_test_was_enabled)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
	if (blend_was_enabled)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);
	glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha);
	RestoreFontBatchDrawState(old_viewport);

	CHECK_ERROR(10);
}

void GL4Renderer::UploadFontTextureLayer(int layer, int bm_handle)
{
	if (layer < 0 || bm_handle < 0)
		return;

	const int w = bm_w(bm_handle, 0);
	const int h = bm_h(bm_handle, 0);
	ushort* bm_ptr = bm_data(bm_handle, 0);
	if (w <= 0 || h <= 0 || bm_ptr == nullptr)
		return;

	SetUploadBufferSize(w, h);
	if (bm_format(bm_handle) == BITMAP_FORMAT_4444)
	{
		for (int i = 0; i < w * h; i++)
			opengl_Upload_data[i] = opengl_4444_translate_table[bm_ptr[i]];
	}
	else
	{
		for (int i = 0; i < w * h; i++)
			opengl_Upload_data[i] = opengl_Translate_table[bm_ptr[i]];
	}

	if (UseMultitexture && Last_texel_unit_set != 0)
	{
		glActiveTexture(GL_TEXTURE0);
		Last_texel_unit_set = 0;
	}
	glBindTexture(GL_TEXTURE_2D_ARRAY, font_texture_array);
	glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE, opengl_Upload_data);
	GameBitmaps[bm_handle].flags &= ~(BF_CHANGED | BF_BRAND_NEW);
	OpenGL_uploads++;
}

int GL4Renderer::GetFontTextureLayer(int bm_handle)
{
	if (bm_handle < 0)
		return -1;

	const int w = bm_w(bm_handle, 0);
	const int h = bm_h(bm_handle, 0);
	if (w <= 0 || h <= 0)
		return -1;

	if (font_texture_array != 0 &&
		(font_texture_array_width != w || font_texture_array_height != h) &&
		FontBatchHasVertices())
	{
		FlushFontBatch();
	}

	if (font_texture_array == 0 || font_texture_array_width != w || font_texture_array_height != h)
	{
		if (font_texture_array != 0)
			glDeleteTextures(1, &font_texture_array);

		GLint max_layers = 0;
		glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
		font_texture_array_layers = std::max(1, std::min(max_layers > 0 ? max_layers : 256, 256));
		font_texture_array_width = w;
		font_texture_array_height = h;
		font_texture_array_handles.assign(font_texture_array_layers, -1);

		glGenTextures(1, &font_texture_array);
		glBindTexture(GL_TEXTURE_2D_ARRAY, font_texture_array);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, w, h, font_texture_array_layers, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	}

	for (int layer = 0; layer < (int)font_texture_array_handles.size(); layer++)
	{
		if (font_texture_array_handles[layer] == bm_handle)
		{
			if (GameBitmaps[bm_handle].flags & (BF_CHANGED | BF_BRAND_NEW))
				UploadFontTextureLayer(layer, bm_handle);
			return layer;
		}
	}

	for (int layer = 0; layer < (int)font_texture_array_handles.size(); layer++)
	{
		if (font_texture_array_handles[layer] == -1)
		{
			font_texture_array_handles[layer] = bm_handle;
			UploadFontTextureLayer(layer, bm_handle);
			return layer;
		}
	}

	return -1;
}

void GL4Renderer::DestroyFontBatchResources()
{
	ClearFontBatchVertices();
	font_texture_array_handles.clear();
	if (font_texture_array != 0)
	{
		glDeleteTextures(1, &font_texture_array);
		font_texture_array = 0;
	}
	font_texture_array_width = 0;
	font_texture_array_height = 0;
	font_texture_array_layers = 0;
}

void GL4Renderer::SetDrawDefaults()
{
	//Init shaders
	CFILE* cf = cfopen("generic.vert", "rb");
	if (!cf)
		Error("opengl_SetDrawDefaults: Couldn't open shader source file generic.vert!");
	int len = cfilelength(cf);
	char* genericVertexBody = new char[len + 1];
	if (cf_ReadBytes((ubyte*)genericVertexBody, len, cf) != len)
		Error("opengl_SetDrawDefaults: Failure reading generic.vert!");
	genericVertexBody[len] = '\0';
	cfclose(cf);

	cf = cfopen("generic.frag", "rb");
	if (!cf)
		Error("opengl_SetDrawDefaults: Couldn't open shader source file generic.frag!");
	len = cfilelength(cf);
	char* genericFragBody = new char[len + 1];
	if (cf_ReadBytes((ubyte*)genericFragBody, len, cf) != len)
		Error("opengl_SetDrawDefaults: Failure reading generic.frag!");
	genericFragBody[len] = '\0';
	cfclose(cf);

	//Without fog
	//No texturing
	drawshaders[0].AttachSourcePreprocess(genericVertexBody, genericFragBody, false, false, false, false);
	//Textured
	drawshaders[1].AttachSourcePreprocess(genericVertexBody, genericFragBody, true, false, false, false);
	//Textured and lightmapped
	drawshaders[2].AttachSourcePreprocess(genericVertexBody, genericFragBody, true, true, false, false);
	//Specular.
	drawshaders[3].AttachSourcePreprocess(genericVertexBody, genericFragBody, true, false, true, false);

	//With fog
	//No texturing
	drawshaders[4].AttachSourcePreprocess(genericVertexBody, genericFragBody, false, false, false, true);
	//Textured
	drawshaders[5].AttachSourcePreprocess(genericVertexBody, genericFragBody, true, false, false, true);
	//Textured and lightmapped
	drawshaders[6].AttachSourcePreprocess(genericVertexBody, genericFragBody, true, true, false, true);
	//Specular.
	drawshaders[7].AttachSourcePreprocess(genericVertexBody, genericFragBody, true, false, true, true);

	for (int i = 0; i < 8; i++)
	{
		drawshader_phong_enabled_uniforms[i] = drawshaders[i].FindUniform("phong_enabled");
		drawshader_light_direction_uniforms[i] = drawshaders[i].FindUniform("phong_light_direction");
		drawshader_dynamic_count_uniforms[i] = drawshaders[i].FindUniform("dynamic_light_count");
		drawshader_dynamic_face_normal_uniforms[i] = drawshaders[i].FindUniform("dynamic_face_normal");
		drawshader_dynamic_positions_uniforms[i] = drawshaders[i].FindUniform("dynamic_light_positions[0]");
		drawshader_dynamic_colors_uniforms[i] = drawshaders[i].FindUniform("dynamic_light_colors[0]");
		drawshader_dynamic_radii_uniforms[i] = drawshaders[i].FindUniform("dynamic_light_radii[0]");
		drawshader_dynamic_directions_uniforms[i] = drawshaders[i].FindUniform("dynamic_light_directions[0]");
		drawshader_dynamic_dot_ranges_uniforms[i] = drawshaders[i].FindUniform("dynamic_light_dot_ranges[0]");
		drawshader_dynamic_directional_uniforms[i] = drawshaders[i].FindUniform("dynamic_light_directional[0]");
		drawshader_ao_suppression_uniforms[i] = drawshaders[i].FindUniform("ao_suppression");
		drawshader_bloom_suppression_uniforms[i] = drawshaders[i].FindUniform("bloom_suppression");
		drawshader_ao_class_uniforms[i] = drawshaders[i].FindUniform("ao_class_value");
		drawshader_ao_weight_uniforms[i] = drawshaders[i].FindUniform("ao_weight_value");
		drawshader_ao_capture_weight_mode_uniforms[i] = drawshaders[i].FindUniform("ao_capture_weight_mode");
		drawshader_post_mask_luminance_uniforms[i] = drawshaders[i].FindUniform("post_mask_use_luminance");
		drawshader_cockpit_backing_enabled_uniforms[i] = drawshaders[i].FindUniform("cockpit_backing_enabled");
		drawshader_cockpit_backing_alpha_uniforms[i] = drawshaders[i].FindUniform("cockpit_backing_alpha");
		drawshader_cockpit_backing_darkness_uniforms[i] = drawshaders[i].FindUniform("cockpit_backing_darkness");
		drawshader_cockpit_scanlines_enabled_uniforms[i] = drawshaders[i].FindUniform("cockpit_scanlines_enabled");
		drawshader_cockpit_scanline_strength_uniforms[i] = drawshaders[i].FindUniform("cockpit_scanline_strength");
		drawshader_cockpit_scanline_spacing_uniforms[i] = drawshaders[i].FindUniform("cockpit_scanline_spacing");
		drawshader_cockpit_scanline_thickness_uniforms[i] = drawshaders[i].FindUniform("cockpit_scanline_thickness");
		drawshader_cockpit_scanline_phase_uniforms[i] = drawshaders[i].FindUniform("cockpit_scanline_phase");
		drawshader_motion_vector_mode_uniforms[i] = drawshaders[i].FindUniform("motion_vector_mode");
		drawshader_motion_vector_current_view_projection_uniforms[i] = drawshaders[i].FindUniform("motion_vector_current_view_projection");
		drawshader_motion_vector_previous_view_projection_uniforms[i] = drawshaders[i].FindUniform("motion_vector_previous_view_projection");
		drawshader_motion_vector_screen_size_uniforms[i] = drawshaders[i].FindUniform("motion_vector_screen_size");
		drawshader_motion_vector_has_previous_uniforms[i] = drawshaders[i].FindUniform("motion_vector_has_previous");
	}

	lastdrawshader = -1;

	delete[] genericVertexBody;
	delete[] genericFragBody;

	//Init VAO and vertex state
	glGenVertexArrays(1, &drawvao);
	glBindVertexArray(drawvao);

	//Init draw buffers
	size_t buffersize = NUM_VERTS_PER_BUFFER * sizeof(gl_vertex);
	if (OpenGL_buffer_storage_enabled)
	{
		InitPersistentDrawBuffer(buffersize);
	}
	else
	{
		glGenBuffers(1, &drawbuffer);
		glBindBuffer(GL_ARRAY_BUFFER, drawbuffer);
		glBufferData(GL_ARRAY_BUFFER, buffersize, nullptr, GL_STREAM_DRAW);

		//attrib 0: position
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), 0);

		//attrib 1: color
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, color));

		//attrib 2: uv
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, tex_coord));

		//attrib 3: uv 2
		glEnableVertexAttribArray(3);
		glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, tex_coord2));

		//attrib 4: per-pixel lighting normal
		glEnableVertexAttribArray(4);
		glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, normal));

		//attrib 5: motion vector
		glEnableVertexAttribArray(5);
		glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, motion_velocity_x));

		//attrib 6: perspective-correct world position for pixel motion vectors
		glEnableVertexAttribArray(6);
		glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, sizeof(gl_vertex), (const void*)offsetof(gl_vertex, motion_world_position));
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	InitMotionVectorDraw();
}

void GL4Renderer::SelectDrawShader()
{
	int shader_index = 0;

	if (OpenGL_state.cur_fog_state)
	{
		post_protection_mask_dirty = true;
		if (OpenGL_state.cur_alpha_type == AT_SPECULAR)
			shader_index = 7;
		else if (OpenGL_state.cur_texture_quality == 0)
			shader_index = 4;
		else if (OpenGL_state.cur_texture_quality != 0)
		{
			if (Overlay_type != OT_NONE)
				shader_index = 6;
			else
				shader_index = 5;
		}
	}
	else
	{
		if (OpenGL_state.cur_alpha_type == AT_SPECULAR)
			shader_index = 3;
		else if (OpenGL_state.cur_texture_quality == 0)
			shader_index = 0;
		else if (OpenGL_state.cur_texture_quality != 0)
		{
			if (Overlay_type != OT_NONE)
				shader_index = 2;
			else
				shader_index = 1;
		}
	}

	const bool shader_changed = shader_index != lastdrawshader;
	drawshaders[shader_index].Use();
	if (!shader_changed && !legacy_draw_uniforms_dirty && !CurrentDrawWritesPixelMotionVectors())
		return;

	if (drawshader_ao_suppression_uniforms[shader_index] != -1)
		glUniform1f(drawshader_ao_suppression_uniforms[shader_index], ao_suppression_draw_value);
	if (drawshader_bloom_suppression_uniforms[shader_index] != -1)
		glUniform1f(drawshader_bloom_suppression_uniforms[shader_index], bloom_suppression_draw_value);
	if (drawshader_ao_class_uniforms[shader_index] != -1)
		glUniform1i(drawshader_ao_class_uniforms[shader_index], ao_class_draw_value);
	if (drawshader_ao_weight_uniforms[shader_index] != -1)
		glUniform1f(drawshader_ao_weight_uniforms[shader_index], ao_weight_draw_value);
	if (drawshader_ao_capture_weight_mode_uniforms[shader_index] != -1)
		glUniform1i(drawshader_ao_capture_weight_mode_uniforms[shader_index], 0);
	if (drawshader_post_mask_luminance_uniforms[shader_index] != -1)
	{
		bool use_luminance =
			OpenGL_state.cur_alpha_type == AT_SATURATE_TEXTURE ||
			OpenGL_state.cur_alpha_type == AT_SATURATE_VERTEX ||
			OpenGL_state.cur_alpha_type == AT_SATURATE_CONSTANT_VERTEX ||
			OpenGL_state.cur_alpha_type == AT_SATURATE_TEXTURE_VERTEX ||
			OpenGL_state.cur_alpha_type == AT_LIGHTMAP_BLEND_SATURATE;
		glUniform1i(drawshader_post_mask_luminance_uniforms[shader_index], use_luminance ? 1 : 0);
	}
	if (drawshader_cockpit_backing_enabled_uniforms[shader_index] != -1)
		glUniform1i(drawshader_cockpit_backing_enabled_uniforms[shader_index], cockpit_backing_effect.enabled);
	if (drawshader_cockpit_backing_alpha_uniforms[shader_index] != -1)
		glUniform1f(drawshader_cockpit_backing_alpha_uniforms[shader_index], cockpit_backing_effect.alpha);
	if (drawshader_cockpit_backing_darkness_uniforms[shader_index] != -1)
		glUniform1f(drawshader_cockpit_backing_darkness_uniforms[shader_index], cockpit_backing_effect.darkness);
	if (drawshader_cockpit_scanlines_enabled_uniforms[shader_index] != -1)
		glUniform1i(drawshader_cockpit_scanlines_enabled_uniforms[shader_index], cockpit_backing_effect.scanlines_enabled);
	if (drawshader_cockpit_scanline_strength_uniforms[shader_index] != -1)
		glUniform1f(drawshader_cockpit_scanline_strength_uniforms[shader_index], cockpit_backing_effect.scanline_strength);
	if (drawshader_cockpit_scanline_spacing_uniforms[shader_index] != -1)
		glUniform1f(drawshader_cockpit_scanline_spacing_uniforms[shader_index], cockpit_backing_effect.scanline_spacing);
	if (drawshader_cockpit_scanline_thickness_uniforms[shader_index] != -1)
		glUniform1f(drawshader_cockpit_scanline_thickness_uniforms[shader_index], cockpit_backing_effect.scanline_thickness);
	if (drawshader_cockpit_scanline_phase_uniforms[shader_index] != -1)
		glUniform1f(drawshader_cockpit_scanline_phase_uniforms[shader_index], cockpit_backing_effect.scanline_phase);
	if (drawshader_motion_vector_mode_uniforms[shader_index] != -1)
		glUniform1i(drawshader_motion_vector_mode_uniforms[shader_index],
			CurrentDrawWritesPixelMotionVectors() ? RENDERER_MOTION_VECTOR_PIXEL : RENDERER_MOTION_VECTOR_OFF);
	if (drawshader_motion_vector_current_view_projection_uniforms[shader_index] != -1)
		glUniformMatrix4fv(drawshader_motion_vector_current_view_projection_uniforms[shader_index], 1, GL_FALSE, current_view_projection);
	if (drawshader_motion_vector_previous_view_projection_uniforms[shader_index] != -1)
		glUniformMatrix4fv(drawshader_motion_vector_previous_view_projection_uniforms[shader_index], 1, GL_FALSE, previous_view_projection);
	if (drawshader_motion_vector_screen_size_uniforms[shader_index] != -1)
		glUniform2f(drawshader_motion_vector_screen_size_uniforms[shader_index],
			(float)FramebufferWidth(), (float)FramebufferHeight());
	if (drawshader_motion_vector_has_previous_uniforms[shader_index] != -1)
		glUniform1i(drawshader_motion_vector_has_previous_uniforms[shader_index],
			have_previous_view_projection ? 1 : 0);

	const bool phong_enabled = OpenGL_state.cur_light_state == LS_PHONG;
	if (drawshader_phong_enabled_uniforms[shader_index] != -1)
		glUniform1i(drawshader_phong_enabled_uniforms[shader_index], phong_enabled ? 1 : 0);
	if (phong_enabled && drawshader_light_direction_uniforms[shader_index] != -1)
		glUniform3f(drawshader_light_direction_uniforms[shader_index], per_pixel_light_direction.x,
			per_pixel_light_direction.y, per_pixel_light_direction.z);

	drawshaders[shader_index].ApplyDynamicLighting(per_pixel_dynamic_light_count,
		&per_pixel_dynamic_face_normal.x, &per_pixel_dynamic_positions[0][0],
		&per_pixel_dynamic_colors[0][0], per_pixel_dynamic_radii,
		&per_pixel_dynamic_directions[0][0], per_pixel_dynamic_dot_ranges,
		per_pixel_dynamic_directional);

	lastdrawshader = shader_index;
	legacy_draw_uniforms_dirty = false;
}

// Takes nv vertices and draws the 3D polygon defined by those vertices.
// Uses bitmap "handle" as a texture
void GL4Renderer::DrawPolygon3D(int handle, g3Point** p, int nv, int map_type)
{
	g3Point* pnt;
	int i;
	ubyte fr, fg, fb;

	ASSERT(nv < 100);

	float one_over_square_res = 1;
	float xscalar = 1;
	float yscalar = 1;

	SelectDrawShader();

	if (OpenGL_state.cur_light_state == LS_FLAT_GOURAUD || OpenGL_state.cur_texture_type == 0)
	{
		fr = GR_COLOR_RED(OpenGL_state.cur_color);
		fg = GR_COLOR_GREEN(OpenGL_state.cur_color);
		fb = GR_COLOR_BLUE(OpenGL_state.cur_color);
	}

	if (UseMultitexture)
	{
		SetMultitextureBlendMode(false);
	}

	if (OpenGL_state.cur_texture_quality != 0)
	{
		// make sure our bitmap is ready to be drawn
		MakeBitmapCurrent(handle, map_type, 0);
		MakeWrapTypeCurrent(handle, map_type, 0);
		MakeFilterTypeCurrent(handle, map_type, 0);

		if (Overlay_type != OT_NONE)
		{
			one_over_square_res = 1.0 / GameLightmaps[Overlay_map].square_res;
			xscalar = (float)GameLightmaps[Overlay_map].width * one_over_square_res;
			yscalar = (float)GameLightmaps[Overlay_map].height * one_over_square_res;
			// make sure our bitmap is ready to be drawn
			MakeBitmapCurrent(Overlay_map, MAP_TYPE_LIGHTMAP, 1);
			MakeWrapTypeCurrent(Overlay_map, MAP_TYPE_LIGHTMAP, 1);
			MakeFilterTypeCurrent(Overlay_map, MAP_TYPE_LIGHTMAP, 1);
		}
	}

	float alpha = Alpha_multiplier * OpenGL_Alpha_factor;

	gl_vertex* vertp = GL_vertices;

	// Specify our coordinates
	for (i = 0; i < nv; i++, vertp++)
	{
		pnt = p[i];

		if (OpenGL_state.cur_alpha_type & ATF_VERTEX)
		{
			alpha = pnt->p3_a * Alpha_multiplier * OpenGL_Alpha_factor;
		}

		// If we have a lighting model, apply the correct lighting!
		if (OpenGL_state.cur_light_state != LS_NONE)
		{
			if (OpenGL_state.cur_light_state == LS_FLAT_GOURAUD)
			{
				vertp->color.r = fr;
				vertp->color.g = fg;
				vertp->color.b = fb;
				vertp->color.a = (ubyte)alpha;
			}
			else
			{
				// Do lighting based on intesity (MONO) or colored (RGB)
				if (OpenGL_state.cur_color_model == CM_MONO)
				{
					vertp->color.r = pnt->p3_l * 255;
					vertp->color.g = pnt->p3_l * 255;
					vertp->color.b = pnt->p3_l * 255;
					vertp->color.a = (ubyte)alpha;
				}
				else
				{
					vertp->color.r = pnt->p3_r * 255;
					vertp->color.g = pnt->p3_g * 255;
					vertp->color.b = pnt->p3_b * 255;
					vertp->color.a = (ubyte)alpha;
				}
			}
		}
		else
		{
			if (OpenGL_state.cur_texture_type != 0)
			{
				vertp->color.r = 255;
				vertp->color.g = 255;
				vertp->color.b = 255;
				vertp->color.a = (ubyte)alpha;
			}
			else
			{
				vertp->color.r = fr;
				vertp->color.g = fg;
				vertp->color.b = fb;
				vertp->color.a = (ubyte)alpha;
			}
		}

		if (OpenGL_state.cur_texture_type != 0)
		{
			// Texture this polygon!
			float texw = 1.0 / (pnt->p3_z + Z_bias);
			vertp->tex_coord.s = pnt->p3_u * texw;
			vertp->tex_coord.t = pnt->p3_v * texw;
			vertp->tex_coord.w = texw;

			if (Overlay_type != OT_NONE)
			{
				vertp->tex_coord2.s = pnt->p3_u2 * xscalar * texw;
				vertp->tex_coord2.t = pnt->p3_v2 * yscalar * texw;
				vertp->tex_coord2.w = texw;
			}
		}

		if (OpenGL_state.cur_light_state == LS_PHONG || per_pixel_dynamic_light_count > 0)
		{
			float payloadw = 1.0f / (pnt->p3_z + Z_bias);
			vertp->normal.x = pnt->p3_vecPreRot.x * payloadw;
			vertp->normal.y = pnt->p3_vecPreRot.y * payloadw;
			vertp->normal.z = pnt->p3_vecPreRot.z * payloadw;
			vertp->normal.w = payloadw;
		}

		// Finally, specify a vertex
		vertp->vert.x = pnt->p3_sx;
		vertp->vert.y = pnt->p3_sy;
		vertp->motion_velocity_x = 0.0f;
		vertp->motion_velocity_y = 0.0f;
		vertp->motion_world_position.x = 0.0f;
		vertp->motion_world_position.y = 0.0f;
		vertp->motion_world_position.z = 0.0f;
		vertp->motion_world_position.w = 0.0f;
		if (motion_object_active && OpenGL_preferred_state.motion_vector_mode == RENDERER_MOTION_VECTOR_PIXEL && pnt->p3_motion_valid)
		{
			const float screen_width = OpenGL_state.screen_width > 0 ? (float)OpenGL_state.screen_width : 1.0f;
			const float screen_height = OpenGL_state.screen_height > 0 ? (float)OpenGL_state.screen_height : 1.0f;
			vertp->motion_velocity_x = (pnt->p3_sx - pnt->p3_prev_sx) / screen_width;
			vertp->motion_velocity_y = -(pnt->p3_sy - pnt->p3_prev_sy) / screen_height;
		}
		if (CurrentDrawWritesPixelMotionVectors())
		{
			float payloadw = 1.0f / (pnt->p3_z + Z_bias);
			vertp->motion_world_position.x = pnt->p3_vecPreRot.x * payloadw;
			vertp->motion_world_position.y = pnt->p3_vecPreRot.y * payloadw;
			vertp->motion_world_position.z = pnt->p3_vecPreRot.z * payloadw;
			vertp->motion_world_position.w = payloadw;
		}

		float z = std::max(0.f, std::min(1.0f, 1.0f - (1.0f / (pnt->p3_z + Z_bias))));
		vertp->vert.z = -z;
	}

	// And draw!
	int offset = CopyVertices(nv);
	const bool drawing_to_scene = framebuffer_ok &&
		GL4DrawTargetIsFramebuffer(framebuffers[framebuffer_current_draw].Handle());
	const bool include_motion_vectors = CurrentDrawWritesPixelMotionVectors();
	const bool include_ao_class = OpenGL_state.cur_zbuffer_state != 0;
	const bool override_draw_buffers = drawing_to_scene &&
		((MotionVectorWritesEnabled() && !include_motion_vectors) || !include_ao_class);
	if (override_draw_buffers)
		GL4UseSceneDrawBuffersForCurrentDraw(include_motion_vectors, include_ao_class);
	rend_RecordDrawCall(draw_call_category);
	glDrawArrays(GL_TRIANGLE_FAN, offset, nv);
	if (override_draw_buffers)
		UseSceneDrawBuffers();
	DrawMotionVectorPolygon(nv, p);
	OpenGL_polys_drawn++;
	OpenGL_verts_processed += nv;

	CHECK_ERROR(10);
}

void GL4Renderer::DrawPolygon3DBatch(int handle, const renderer_poly_batch_item *items, int count, int map_type)
{
	if (!items || count <= 0)
		return;

	ubyte fr = 0, fg = 0, fb = 0;
	float one_over_square_res = 1;
	float xscalar = 1;
	float yscalar = 1;

	SelectDrawShader();

	if (OpenGL_state.cur_light_state == LS_FLAT_GOURAUD || OpenGL_state.cur_texture_type == 0)
	{
		fr = GR_COLOR_RED(OpenGL_state.cur_color);
		fg = GR_COLOR_GREEN(OpenGL_state.cur_color);
		fb = GR_COLOR_BLUE(OpenGL_state.cur_color);
	}

	if (UseMultitexture)
	{
		SetMultitextureBlendMode(false);
	}

	if (OpenGL_state.cur_texture_quality != 0)
	{
		MakeBitmapCurrent(handle, map_type, 0);
		MakeWrapTypeCurrent(handle, map_type, 0);
		MakeFilterTypeCurrent(handle, map_type, 0);

		if (Overlay_type != OT_NONE)
		{
			one_over_square_res = 1.0f / GameLightmaps[Overlay_map].square_res;
			xscalar = (float)GameLightmaps[Overlay_map].width * one_over_square_res;
			yscalar = (float)GameLightmaps[Overlay_map].height * one_over_square_res;
			MakeBitmapCurrent(Overlay_map, MAP_TYPE_LIGHTMAP, 1);
			MakeWrapTypeCurrent(Overlay_map, MAP_TYPE_LIGHTMAP, 1);
			MakeFilterTypeCurrent(Overlay_map, MAP_TYPE_LIGHTMAP, 1);
		}
	}

	int triangle_vertices = 0;
	int original_vertices = 0;
	for (int i = 0; i < count; i++)
	{
		if (items[i].nv >= 3 && items[i].nv < 100)
		{
			triangle_vertices += (items[i].nv - 2) * 3;
			original_vertices += items[i].nv;
		}
	}

	if (triangle_vertices <= 0)
		return;

	std::vector<gl_vertex> vertices;
	vertices.reserve(triangle_vertices);

	std::vector<gl_motion_vertex> motion_vertices;
	const bool capture_motion = motion_object_active && motionvectorshader.Handle() != 0 &&
		OpenGL_preferred_state.motion_vector_mode == RENDERER_MOTION_VECTOR_VERTEX &&
		motionvector_vao != 0 && motion_vectors.velocity_texture != 0;
	if (capture_motion)
		motion_vertices.reserve(triangle_vertices);

	int polygons_drawn = 0;
	for (int i = 0; i < count; i++)
	{
		const renderer_poly_batch_item& item = items[i];
		if (item.nv < 3 || item.nv >= 100)
			continue;

		gl_vertex face_vertices[100];
		for (int v = 0; v < item.nv; v++)
			BuildDrawVertex(face_vertices[v], item.pointlist[v], xscalar, yscalar, fr, fg, fb);

		bool motion_valid = capture_motion;
		if (motion_valid)
		{
			for (int v = 0; v < item.nv; v++)
			{
				if (!item.pointlist[v]->p3_motion_valid)
				{
					motion_valid = false;
					break;
				}
			}
		}

		for (int v = 0; v < item.nv - 2; v++)
		{
			const int indices[3] = {0, v + 1, v + 2};
			for (int corner = 0; corner < 3; corner++)
				vertices.push_back(face_vertices[indices[corner]]);

			if (motion_valid)
			{
				for (int corner = 0; corner < 3; corner++)
				{
					const int index = indices[corner];
					gl_motion_vertex motion_vertex;
					motion_vertex.x = face_vertices[index].vert.x;
					motion_vertex.y = face_vertices[index].vert.y;
					motion_vertex.z = face_vertices[index].vert.z;
					motion_vertex.velocity_x = item.pointlist[index]->p3_sx - item.pointlist[index]->p3_prev_sx;
					motion_vertex.velocity_y = item.pointlist[index]->p3_sy - item.pointlist[index]->p3_prev_sy;
					motion_vertices.push_back(motion_vertex);
				}
			}
		}
		polygons_drawn++;
	}

	if (vertices.empty())
		return;

	int offset = CopyVertices(vertices.data(), (int)vertices.size());
	const bool drawing_to_scene = framebuffer_ok &&
		GL4DrawTargetIsFramebuffer(framebuffers[framebuffer_current_draw].Handle());
	const bool include_motion_vectors = CurrentDrawWritesPixelMotionVectors();
	const bool include_ao_class = OpenGL_state.cur_zbuffer_state != 0;
	const bool override_draw_buffers = drawing_to_scene &&
		((MotionVectorWritesEnabled() && !include_motion_vectors) || !include_ao_class);
	if (override_draw_buffers)
		GL4UseSceneDrawBuffersForCurrentDraw(include_motion_vectors, include_ao_class);
	rend_RecordDrawCall(draw_call_category);
	glDrawArrays(GL_TRIANGLES, offset, (GLsizei)vertices.size());
	if (override_draw_buffers)
		UseSceneDrawBuffers();
	DrawMotionVectorTriangles(motion_vertices.data(), (int)motion_vertices.size());
	OpenGL_polys_drawn += polygons_drawn;
	OpenGL_verts_processed += original_vertices;

	CHECK_ERROR(10);
}

// Takes nv vertices and draws the 2D polygon defined by those vertices.
// Uses bitmap "handle" as a texture
void GL4Renderer::DrawPolygon2D(int handle, g3Point** p, int nv)
{
	ASSERT(nv < 100);
	ASSERT(Overlay_type == OT_NONE);

	renderer_draw_call_category old_category = draw_call_category;
	if (draw_call_category == RENDERER_DRAW_CALL_3D)
		draw_call_category = RENDERER_DRAW_CALL_2D;
	DrawPolygon3D(handle, p, nv, MAP_TYPE_BITMAP);
	draw_call_category = old_category;
}

void GL4Renderer::BeginMotionObject(int object_handle, float screen_x, float screen_y)
{
	motion_object_active = object_handle >= 0 && framebuffer_ok &&
		!motion_vectors_capture_locked &&
		OpenGL_preferred_state.motion_vector_mode != RENDERER_MOTION_VECTOR_OFF &&
		motion_vectors.velocity_texture != 0;
}

void GL4Renderer::EndMotionObject()
{
	motion_object_active = false;
}

bool GL4Renderer::ProjectPreviousFramePoint(const vector *world_pos, float *screen_x, float *screen_y)
{
	if (!world_pos || !screen_x || !screen_y || !have_previous_view_projection)
		return false;

	float x = world_pos->x;
	float y = world_pos->y;
	float z = world_pos->z;
	float clip_x = previous_view_projection[0] * x + previous_view_projection[4] * y +
		previous_view_projection[8] * z + previous_view_projection[12];
	float clip_y = previous_view_projection[1] * x + previous_view_projection[5] * y +
		previous_view_projection[9] * z + previous_view_projection[13];
	float clip_w = previous_view_projection[3] * x + previous_view_projection[7] * y +
		previous_view_projection[11] * z + previous_view_projection[15];
	if (clip_w <= 0.00001f)
		return false;

	float ndc_x = clip_x / clip_w;
	float ndc_y = clip_y / clip_w;
	*screen_x = (ndc_x * 0.5f + 0.5f) * (float)OpenGL_state.screen_width;
	*screen_y = (0.5f - ndc_y * 0.5f) * (float)OpenGL_state.screen_height;
	return true;
}

// draws a scaled 2d bitmap to our buffer
void GL4Renderer::DrawScaledBitmap(int x1, int y1, int x2, int y2,
	int bm, float u0, float v0, float u1, float v1, int color, float* alphas)
{
	g3Point* ptr_pnts[4];
	g3Point pnts[4];
	float r, g, b;
	if (color != -1)
	{
		r = GR_COLOR_RED(color) / 255.0;
		g = GR_COLOR_GREEN(color) / 255.0;
		b = GR_COLOR_BLUE(color) / 255.0;
	}
	for (int i = 0; i < 4; i++)
	{
		if (color == -1)
			pnts[i].p3_l = 1.0;
		else
		{
			pnts[i].p3_r = r;
			pnts[i].p3_g = g;
			pnts[i].p3_b = b;
		}
		if (alphas)
		{
			pnts[i].p3_a = alphas[i];
		}

		pnts[i].p3_z = 1.0f;
		pnts[i].p3_flags = PF_PROJECTED;
		pnts[i].p3_motion_valid = 0;
		pnts[i].p3_vecPreRot.x = 0.0f;
		pnts[i].p3_vecPreRot.y = 0.0f;
		pnts[i].p3_vecPreRot.z = 0.0f;
	}

	pnts[0].p3_sx = x1;
	pnts[0].p3_sy = y1;
	pnts[0].p3_u = u0;
	pnts[0].p3_v = v0;
	pnts[1].p3_sx = x2;
	pnts[1].p3_sy = y1;
	pnts[1].p3_u = u1;
	pnts[1].p3_v = v0;
	pnts[2].p3_sx = x2;
	pnts[2].p3_sy = y2;
	pnts[2].p3_u = u1;
	pnts[2].p3_v = v1;
	pnts[3].p3_sx = x1;
	pnts[3].p3_sy = y2;
	pnts[3].p3_u = u0;
	pnts[3].p3_v = v1;
	ptr_pnts[0] = &pnts[0];
	ptr_pnts[1] = &pnts[1];
	ptr_pnts[2] = &pnts[2];
	ptr_pnts[3] = &pnts[3];
	SetTextureType(TT_LINEAR);
	DrawPolygon2D(bm, ptr_pnts, 4);
}

void GL4Renderer::DrawScaledBitmapWithZ(int x1, int y1, int x2, int y2,
	int bm, float u0, float v0, float u1, float v1, float zval, int color, float* alphas)
{
	g3Point* ptr_pnts[4];
	g3Point pnts[4];
	float r, g, b;

	if (color != -1)
	{
		r = GR_COLOR_RED(color) / 255.0;
		g = GR_COLOR_GREEN(color) / 255.0;
		b = GR_COLOR_BLUE(color) / 255.0;
	}

	for (int i = 0; i < 4; i++)
	{
		if (color == -1)
			pnts[i].p3_l = 1.0;
		else
		{
			pnts[i].p3_r = r;
			pnts[i].p3_g = g;
			pnts[i].p3_b = b;
		}

		if (alphas)
		{
			pnts[i].p3_a = alphas[i];
		}

		pnts[i].p3_z = zval;
		pnts[i].p3_flags = PF_PROJECTED;
		pnts[i].p3_motion_valid = 0;
		pnts[i].p3_vecPreRot.x = 0.0f;
		pnts[i].p3_vecPreRot.y = 0.0f;
		pnts[i].p3_vecPreRot.z = 0.0f;
	}



	pnts[0].p3_sx = x1;
	pnts[0].p3_sy = y1;
	pnts[0].p3_u = u0;
	pnts[0].p3_v = v0;

	pnts[1].p3_sx = x2;
	pnts[1].p3_sy = y1;
	pnts[1].p3_u = u1;
	pnts[1].p3_v = v0;

	pnts[2].p3_sx = x2;
	pnts[2].p3_sy = y2;
	pnts[2].p3_u = u1;
	pnts[2].p3_v = v1;

	pnts[3].p3_sx = x1;
	pnts[3].p3_sy = y2;
	pnts[3].p3_u = u0;
	pnts[3].p3_v = v1;

	ptr_pnts[0] = &pnts[0];
	ptr_pnts[1] = &pnts[1];
	ptr_pnts[2] = &pnts[2];
	ptr_pnts[3] = &pnts[3];

	SetTextureType(TT_LINEAR);
	DrawPolygon3D(bm, ptr_pnts, 4);
}

// Fills a rectangle on the display
void GL4Renderer::FillRect(ddgr_color color, int x1, int y1, int x2, int y2)
{
	int r = GR_COLOR_RED(color);
	int g = GR_COLOR_GREEN(color);
	int b = GR_COLOR_BLUE(color);

	int width = x2 - x1;
	int height = y2 - y1;

	x1 += OpenGL_state.clip_x1;
	y1 += OpenGL_state.clip_y1;

	glEnable(GL_SCISSOR_TEST);
	glScissor(ScaledX(x1), FramebufferHeight() - ScaledY(height + y1), ScaledW(width), ScaledH(height));
	glClearColor((float)r / 255.0, (float)g / 255.0, (float)b / 255.0, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	width = OpenGL_state.clip_x2 - OpenGL_state.clip_x1;
	height = OpenGL_state.clip_y2 - OpenGL_state.clip_y1;

	glScissor(ScaledX(OpenGL_state.clip_x1), FramebufferHeight() - ScaledY(OpenGL_state.clip_y1 + height), ScaledW(width), ScaledH(height));
	glDisable(GL_SCISSOR_TEST);
}

// Sets a pixel on the display
void GL4Renderer::SetPixel(ddgr_color color, int x, int y)
{
	ubyte r = (color >> 16 & 0xFF);
	ubyte g = (color >> 8 & 0xFF);
	ubyte b = (color & 0xFF);

	SelectDrawShader();

	GL_vertices[0].color.r = r;
	GL_vertices[0].color.g = g;
	GL_vertices[0].color.b = b;
	GL_vertices[0].color.a = 255;

	GL_vertices[0].vert.x = x;
	GL_vertices[0].vert.y = y;
	GL_vertices[0].vert.z = 0;
	GL_vertices[0].motion_velocity_x = 0.0f;
	GL_vertices[0].motion_velocity_y = 0.0f;
	GL_vertices[0].motion_world_position.x = 0.0f;
	GL_vertices[0].motion_world_position.y = 0.0f;
	GL_vertices[0].motion_world_position.z = 0.0f;
	GL_vertices[0].motion_world_position.w = 0.0f;

	//please do not call this function if you can avoid it.
	int offset = CopyVertices(1);
	GLfloat point_size = std::max(1.0f, std::min((GLfloat)SupersamplingFactor(), max_point_size));
	glPointSize(point_size);
	rend_RecordDrawCall(RENDERER_DRAW_CALL_PRIMITIVE);
	glDrawArrays(GL_POINTS, offset, 1);
	glPointSize(1.0f);
}

// Sets a pixel on the display
ddgr_color GL4Renderer::GetPixel(int x, int y)
{
	ddgr_color color[4];
	glReadPixels(ScaledX(x), FramebufferHeight() - 1 - ScaledY(y), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid*)color);
	return color[0];
}

void GL4Renderer::FillCircle(ddgr_color col, int x, int y, int rad)
{
}

void GL4Renderer::DrawCircle(int x, int y, int rad)
{
}

// Sets up a font character to draw.  We draw our fonts as pieces of textures
void GL4Renderer::DrawFontCharacter(int bm_handle, int x1, int y1, int x2, int y2, float u, float v, float w, float h)
{
	const int batch_index = GL4FontBatchIndexForAlpha(OpenGL_state.cur_alpha_type);

	const int texture_layer = GetFontTextureLayer(bm_handle);
	if (texture_layer < 0)
		return;
	if (font_batch_vertices[batch_index].size() + 6 > 60000)
		FlushFontBatch();

	gl_vertex quad[4] = {};
	const ubyte fr = GR_COLOR_RED(OpenGL_state.cur_color);
	const ubyte fg = GR_COLOR_GREEN(OpenGL_state.cur_color);
	const ubyte fb = GR_COLOR_BLUE(OpenGL_state.cur_color);
	const float alpha = Alpha_multiplier * OpenGL_Alpha_factor;
	const float z = -std::max(0.f, std::min(1.0f, 1.0f - (1.0f / (1.0f + Z_bias))));
	const float offset_x = (float)OpenGL_state.clip_x1;
	const float offset_y = (float)OpenGL_state.clip_y1;

	for (int i = 0; i < 4; i++)
	{
		quad[i].color.r = fr;
		quad[i].color.g = fg;
		quad[i].color.b = fb;
		quad[i].color.a = (ubyte)alpha;
		quad[i].tex_coord.w = 1.0f;
		quad[i].tex_coord2.s = (float)texture_layer;
		quad[i].vert.z = z;
	}

	quad[0].vert.x = offset_x + (float)x1;
	quad[0].vert.y = offset_y + (float)y1;
	quad[0].tex_coord.s = u;
	quad[0].tex_coord.t = v;
	quad[1].vert.x = offset_x + (float)x2;
	quad[1].vert.y = offset_y + (float)y1;
	quad[1].tex_coord.s = u + w;
	quad[1].tex_coord.t = v;
	quad[2].vert.x = offset_x + (float)x2;
	quad[2].vert.y = offset_y + (float)y2;
	quad[2].tex_coord.s = u + w;
	quad[2].tex_coord.t = v + h;
	quad[3].vert.x = offset_x + (float)x1;
	quad[3].vert.y = offset_y + (float)y2;
	quad[3].tex_coord.s = u;
	quad[3].tex_coord.t = v + h;

	font_batch_vertices[batch_index].push_back(quad[0]);
	font_batch_vertices[batch_index].push_back(quad[1]);
	font_batch_vertices[batch_index].push_back(quad[2]);
	font_batch_vertices[batch_index].push_back(quad[0]);
	font_batch_vertices[batch_index].push_back(quad[2]);
	font_batch_vertices[batch_index].push_back(quad[3]);
}

void GL4Renderer::FlushTextLayer()
{
	FlushFontBatch();
}

// Draws a line
void GL4Renderer::DrawLine(int x1, int y1, int x2, int y2)
{
	sbyte atype;
	light_state ltype;
	texture_type ttype;
	int color = OpenGL_state.cur_color;

	ubyte r = GR_COLOR_RED(color);
	ubyte g = GR_COLOR_GREEN(color);
	ubyte b = GR_COLOR_BLUE(color);

	atype = OpenGL_state.cur_alpha_type;
	ltype = OpenGL_state.cur_light_state;
	ttype = OpenGL_state.cur_texture_type;

	SetAlphaType(AT_ALWAYS);
	SetLighting(LS_NONE);
	SetTextureType(TT_FLAT);

	SelectDrawShader();

	GL_vertices[0].color.r = r;
	GL_vertices[0].color.g = g;
	GL_vertices[0].color.b = b;
	GL_vertices[0].color.a = 255;
	GL_vertices[1].color = GL_vertices[0].color;

	//hack to avoid line clipping but this isn't working correctly yet, causes one corner to vanish.
	GL_vertices[0].vert.x = x1 + 1.f;
	GL_vertices[0].vert.y = y1 + 1.f;
	GL_vertices[0].vert.z = 0;
	GL_vertices[1].vert.x = x2 + 1.f;
	GL_vertices[1].vert.y = y2 + 1.f;
	GL_vertices[1].vert.z = 0;
	GL_vertices[0].motion_velocity_x = 0.0f;
	GL_vertices[0].motion_velocity_y = 0.0f;
	GL_vertices[1].motion_velocity_x = 0.0f;
	GL_vertices[1].motion_velocity_y = 0.0f;
	GL_vertices[0].motion_world_position.x = 0.0f;
	GL_vertices[0].motion_world_position.y = 0.0f;
	GL_vertices[0].motion_world_position.z = 0.0f;
	GL_vertices[0].motion_world_position.w = 0.0f;
	GL_vertices[1].motion_world_position.x = 0.0f;
	GL_vertices[1].motion_world_position.y = 0.0f;
	GL_vertices[1].motion_world_position.z = 0.0f;
	GL_vertices[1].motion_world_position.w = 0.0f;

	int offset = CopyVertices(2);
	GLfloat line_width = std::max(1.0f, std::min((GLfloat)SupersamplingFactor(), max_line_width));
	glLineWidth(line_width);
	rend_RecordDrawCall(RENDERER_DRAW_CALL_PRIMITIVE);
	glDrawArrays(GL_LINES, offset, 2);
	glLineWidth(1.0f);

	SetAlphaType(atype);
	SetLighting(ltype);
	SetTextureType(ttype);
}


// Sets the argb characteristics of the font characters.  color1 is the upper left and proceeds clockwise
void GL4Renderer::SetCharacterParameters(ddgr_color color1, ddgr_color color2, ddgr_color color3, ddgr_color color4)
{
	rend_FontRed[0] = (float)(GR_COLOR_RED(color1) / 255.0f);
	rend_FontRed[1] = (float)(GR_COLOR_RED(color2) / 255.0f);
	rend_FontRed[2] = (float)(GR_COLOR_RED(color3) / 255.0f);
	rend_FontRed[3] = (float)(GR_COLOR_RED(color4) / 255.0f);
	rend_FontGreen[0] = (float)(GR_COLOR_GREEN(color1) / 255.0f);
	rend_FontGreen[1] = (float)(GR_COLOR_GREEN(color2) / 255.0f);
	rend_FontGreen[2] = (float)(GR_COLOR_GREEN(color3) / 255.0f);
	rend_FontGreen[3] = (float)(GR_COLOR_GREEN(color4) / 255.0f);
	rend_FontBlue[0] = (float)(GR_COLOR_BLUE(color1) / 255.0f);
	rend_FontBlue[1] = (float)(GR_COLOR_BLUE(color2) / 255.0f);
	rend_FontBlue[2] = (float)(GR_COLOR_BLUE(color3) / 255.0f);
	rend_FontBlue[3] = (float)(GR_COLOR_BLUE(color4) / 255.0f);
	rend_FontAlpha[0] = (color1 >> 24) / 255.0f;
	rend_FontAlpha[1] = (color2 >> 24) / 255.0f;
	rend_FontAlpha[2] = (color3 >> 24) / 255.0f;
	rend_FontAlpha[3] = (color4 >> 24) / 255.0f;
}

// Turns on/off multitexture blending
void GL4Renderer::SetMultitextureBlendMode(bool state)
{
	if (OpenGL_multitexture_state == state)
		return;
	OpenGL_multitexture_state = state;
	if (state)
	{
		Last_texel_unit_set = 0;
	}
	else
	{
		Last_texel_unit_set = 0;
	}
}

// Draws a line using the states of the renderer
void GL4Renderer::DrawSpecialLine(g3Point* p0, g3Point* p1)
{
	ubyte fr, fg, fb, alpha;
	int i;

	fr = GR_COLOR_RED(OpenGL_state.cur_color);
	fg = GR_COLOR_GREEN(OpenGL_state.cur_color);
	fb = GR_COLOR_BLUE(OpenGL_state.cur_color);

	alpha = Alpha_multiplier * OpenGL_Alpha_factor;

	gl_vertex* vertp = GL_vertices;

	// And draw!
	for (i = 0; i < 2; i++, vertp++)
	{
		color_array* colorp = &vertp->color;

		g3Point* pnt = p0;

		if (i == 1)
			pnt = p1;

		if (OpenGL_state.cur_alpha_type & ATF_VERTEX)
			alpha = (ubyte)(pnt->p3_a * Alpha_multiplier * OpenGL_Alpha_factor);

		// If we have a lighting model, apply the correct lighting!
		if (OpenGL_state.cur_light_state != LS_NONE)
		{
			if (OpenGL_state.cur_light_state == LS_FLAT_GOURAUD)
			{
				colorp->r = fr; colorp->g = fg, colorp->b = fb; colorp->a = (ubyte)alpha;
			}
			else
			{
				// Do lighting based on intesity (MONO) or colored (RGB)
				if (OpenGL_state.cur_color_model == CM_MONO)
				{
					colorp->r = pnt->p3_uvl.l * 255; colorp->g = pnt->p3_uvl.l * 255; colorp->b = pnt->p3_uvl.l * 255; colorp->a = (ubyte)alpha;
				}
				else
				{
					colorp->r = pnt->p3_uvl.r * 255; colorp->g = pnt->p3_uvl.g * 255; colorp->b = pnt->p3_uvl.r * 255; colorp->a = (ubyte)alpha;
				}
			}
		}
		else
		{
			colorp->r = fr; colorp->g = fg, colorp->b = fb; colorp->a = (ubyte)alpha;
		}

		// Finally, specify a vertex
		float z = std::max(0., std::min(1.0, 1.0 - (1.0 / (pnt->p3_z + Z_bias))));

		vertp->vert.x = pnt->p3_sx; vertp->vert.y = pnt->p3_sy; vertp->vert.z = -z;
		vertp->motion_velocity_x = 0.0f;
		vertp->motion_velocity_y = 0.0f;
		vertp->motion_world_position.x = 0.0f;
		vertp->motion_world_position.y = 0.0f;
		vertp->motion_world_position.z = 0.0f;
		vertp->motion_world_position.w = 0.0f;
		//glVertex3f(pnt->p3_sx + x_add, pnt->p3_sy + y_add, -z);
	}

	SelectDrawShader();
	int offset = CopyVertices(2);
	GLfloat line_width = std::max(1.0f, std::min((GLfloat)SupersamplingFactor(), max_line_width));
	glLineWidth(line_width);
	rend_RecordDrawCall(RENDERER_DRAW_CALL_PRIMITIVE);
	glDrawArrays(GL_LINES, offset, 2);
	glLineWidth(1.0f);
}

//	given a chunked bitmap, renders it.
void GL4Renderer::DrawChunkedBitmap(chunked_bitmap* chunk, int x, int y, ubyte alpha)
{
	int* bm_array = chunk->bm_array;
	int w = chunk->w;
	int h = chunk->h;
	int piece_w = bm_w(bm_array[0], 0);
	int piece_h = bm_h(bm_array[0], 0);
	int screen_w, screen_h;
	int i, t;
	SetZBufferState(0);
	GetProjectionParameters(&screen_w, &screen_h);
	for (i = 0; i < h; i++)
	{
		for (t = 0; t < w; t++)
		{
			int dx = x + (piece_w * t);
			int dy = y + (piece_h * i);
			int dw, dh;
			if ((dx + piece_w) > screen_w)
				dw = piece_w - ((dx + piece_w) - screen_w);
			else
				dw = piece_w;
			if ((dy + piece_h) > screen_h)
				dh = piece_h - ((dy + piece_h) - screen_h);
			else
				dh = piece_h;

			float u2 = (float)dw / (float)piece_w;
			float v2 = (float)dh / (float)piece_h;
			DrawSimpleBitmap(bm_array[i * w + t], dx, dy);
		}
	}
	SetZBufferState(1);
}

//	given a chunked bitmap, renders it.scaled
void GL4Renderer::DrawScaledChunkedBitmap(chunked_bitmap* chunk, int x, int y, int neww, int newh, ubyte alpha)
{
	int* bm_array = chunk->bm_array;
	int w = chunk->w;
	int h = chunk->h;
	int piece_w;
	int piece_h;
	int screen_w, screen_h;
	int i, t;

	float scalew, scaleh;

	scalew = ((float)neww) / ((float)chunk->pw);
	scaleh = ((float)newh) / ((float)chunk->ph);
	piece_w = scalew * ((float)bm_w(bm_array[0], 0));
	piece_h = scaleh * ((float)bm_h(bm_array[0], 0));
	GetProjectionParameters(&screen_w, &screen_h);
	SetOverlayType(OT_NONE);
	SetLighting(LS_NONE);
	SetColorModel(CM_MONO);
	SetZBufferState(0);
	SetAlphaType(AT_CONSTANT_TEXTURE);
	SetAlphaValue(alpha);
	SetWrapType(WT_WRAP);
	for (i = 0; i < h; i++)
	{
		for (t = 0; t < w; t++)
		{
			int dx = x + (piece_w * t);
			int dy = y + (piece_h * i);
			int dw, dh;
			if ((dx + piece_w) > screen_w)
				dw = piece_w - ((dx + piece_w) - screen_w);
			else
				dw = piece_w;
			if ((dy + piece_h) > screen_h)
				dh = piece_h - ((dy + piece_h) - screen_h);
			else
				dh = piece_h;

			float u2 = (float)dw / (float)piece_w;
			float v2 = (float)dh / (float)piece_h;
			DrawScaledBitmap(dx, dy, dx + dw, dy + dh, bm_array[i * w + t], 0, 0, u2, v2);

		}
	}
	SetZBufferState(1);
}

// Draws a simple bitmap at the specified x,y location
void GL4Renderer::DrawSimpleBitmap(int bm_handle, int x, int y)
{
	SetAlphaType(AT_CONSTANT_TEXTURE);
	SetAlphaValue(255);
	SetLighting(LS_NONE);
	SetColorModel(CM_MONO);
	SetOverlayType(OT_NONE);
	SetFiltering(0);
	DrawScaledBitmap(x, y, x + bm_w(bm_handle, 0), y + bm_h(bm_handle, 0), bm_handle, 0, 0, 1, 1);
	SetFiltering(1);
}

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

#include <vector>

//The number of vertex attributes the legacy code used.
constexpr int NUM_LEGACY_VERTEX_ATTRIBS = 5;
//The count of vertices that each buffer will store
constexpr int NUM_VERTS_PER_BUFFER = 640000;

void GL3Renderer::UseDrawVAO()
{
	glBindVertexArray(drawvao);
}

void GL3Renderer::RestoreLegacy()
{
	glBindVertexArray(drawvao);
}

void GL3Renderer::InitPersistentDrawBuffer(size_t size)
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

void GL3Renderer::DestroyPersistentDrawBuffer()
{
	if (OpenGL_buffer_storage_enabled && drawbuffer)
	{
		glBindBuffer(GL_ARRAY_BUFFER, drawbuffer);
		glUnmapBuffer(GL_ARRAY_BUFFER);
		glDeleteBuffers(1, &drawbuffer);
		drawbuffer = 0;
	}
}

void GL3Renderer::InitMotionVectorDraw()
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

void GL3Renderer::DestroyMotionVectorDraw()
{
	glDeleteBuffers(1, &motionvector_vbo);
	glDeleteVertexArrays(1, &motionvector_vao);
	motionvector_vbo = 0;
	motionvector_vao = 0;
}

void GL3Renderer::DrawMotionVectorPolygon(int nv, g3Point** p)
{
	if (!motion_object_active || motionvectorshader.Handle() == 0 ||
		motionvector_vao == 0 || motion_vectors.velocity_texture == 0 || nv <= 0)
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

	motionvectorshader.Use();
	if (motionvector_screen_size != -1)
		glUniform2f(motionvector_screen_size, (float)OpenGL_state.screen_width, (float)OpenGL_state.screen_height);

	glBindVertexArray(motionvector_vao);
	glBindBuffer(GL_ARRAY_BUFFER, motionvector_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(gl_motion_vertex) * nv, motion_vertices, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLE_FAN, 0, nv);
	motion_vectors_dirty = true;

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
	glDrawBuffer(old_draw_buffer);
	if ((GLuint)old_draw == framebuffers[framebuffer_current_draw].Handle())
		hbao_mask.UseSceneDrawBuffers(framebuffers[framebuffer_current_draw].Handle());
	glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
	glDepthMask(depth_mask);
	UseDrawVAO();
}

void GL3Renderer::DrawMotionVectorTriangles(const gl_motion_vertex* vertices, int nv)
{
	if (!motion_object_active || motionvectorshader.Handle() == 0 ||
		motionvector_vao == 0 || motion_vectors.velocity_texture == 0 || !vertices || nv <= 0)
	{
		return;
	}

	GLboolean color_mask[4];
	GLboolean depth_mask;
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

	motionvectorshader.Use();
	if (motionvector_screen_size != -1)
		glUniform2f(motionvector_screen_size, (float)OpenGL_state.screen_width, (float)OpenGL_state.screen_height);

	glBindVertexArray(motionvector_vao);
	glBindBuffer(GL_ARRAY_BUFFER, motionvector_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(gl_motion_vertex) * nv, vertices, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, nv);
	motion_vectors_dirty = true;

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
	glDrawBuffer(old_draw_buffer);
	if ((GLuint)old_draw == framebuffers[framebuffer_current_draw].Handle())
		hbao_mask.UseSceneDrawBuffers(framebuffers[framebuffer_current_draw].Handle());
	glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
	glDepthMask(depth_mask);
	UseDrawVAO();
}

int GL3Renderer::CopyVertices(int numvertices)
{
	return CopyVertices(GL_vertices, numvertices);
}

int GL3Renderer::CopyVertices(const gl_vertex* vertices, int numvertices)
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

void GL3Renderer::BuildDrawVertex(gl_vertex& vert, const g3Point* pnt, float xscalar, float yscalar,
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

	float z = std::max(0.f, std::min(1.0f, 1.0f - (1.0f / (pnt->p3_z + Z_bias))));
	vert.vert.z = -z;
}

void GL3Renderer::SetDrawDefaults()
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
		drawshader_hbao_suppression_uniforms[i] = drawshaders[i].FindUniform("hbao_suppression");
		drawshader_bloom_suppression_uniforms[i] = drawshaders[i].FindUniform("bloom_suppression");
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
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	InitMotionVectorDraw();
}

void GL3Renderer::SelectDrawShader()
{
	int shader_index = 0;

	if (OpenGL_state.cur_fog_state)
	{
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
	if (!shader_changed && !legacy_draw_uniforms_dirty)
		return;

	if (drawshader_hbao_suppression_uniforms[shader_index] != -1)
		glUniform1f(drawshader_hbao_suppression_uniforms[shader_index], hbao_suppression_draw_value);
	if (drawshader_bloom_suppression_uniforms[shader_index] != -1)
		glUniform1f(drawshader_bloom_suppression_uniforms[shader_index], bloom_suppression_draw_value);

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
void GL3Renderer::DrawPolygon3D(int handle, g3Point** p, int nv, int map_type)
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

		float z = std::max(0.f, std::min(1.0f, 1.0f - (1.0f / (pnt->p3_z + Z_bias))));
		vertp->vert.z = -z;
	}

	// And draw!
	int offset = CopyVertices(nv);
	glDrawArrays(GL_TRIANGLE_FAN, offset, nv);
	DrawMotionVectorPolygon(nv, p);
	OpenGL_polys_drawn++;
	OpenGL_verts_processed += nv;

	CHECK_ERROR(10);
}

void GL3Renderer::DrawPolygon3DBatch(int handle, const renderer_poly_batch_item *items, int count, int map_type)
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
	glDrawArrays(GL_TRIANGLES, offset, (GLsizei)vertices.size());
	DrawMotionVectorTriangles(motion_vertices.data(), (int)motion_vertices.size());
	OpenGL_polys_drawn += polygons_drawn;
	OpenGL_verts_processed += original_vertices;

	CHECK_ERROR(10);
}

// Takes nv vertices and draws the 2D polygon defined by those vertices.
// Uses bitmap "handle" as a texture
void GL3Renderer::DrawPolygon2D(int handle, g3Point** p, int nv)
{
	ASSERT(nv < 100);
	ASSERT(Overlay_type == OT_NONE);

	DrawPolygon3D(handle, p, nv, MAP_TYPE_BITMAP);
}

void GL3Renderer::BeginMotionObject(int object_handle, float screen_x, float screen_y)
{
	motion_object_active = object_handle >= 0 && framebuffer_ok && motion_vectors.velocity_texture != 0;
}

void GL3Renderer::EndMotionObject()
{
	motion_object_active = false;
}

bool GL3Renderer::ProjectPreviousFramePoint(const vector *world_pos, float *screen_x, float *screen_y)
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
void GL3Renderer::DrawScaledBitmap(int x1, int y1, int x2, int y2,
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

void GL3Renderer::DrawScaledBitmapWithZ(int x1, int y1, int x2, int y2,
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
void GL3Renderer::FillRect(ddgr_color color, int x1, int y1, int x2, int y2)
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
void GL3Renderer::SetPixel(ddgr_color color, int x, int y)
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

	//please do not call this function if you can avoid it.
	int offset = CopyVertices(1);
	GLfloat point_size = std::max(1.0f, std::min((GLfloat)SupersamplingFactor(), max_point_size));
	glPointSize(point_size);
	glDrawArrays(GL_POINTS, offset, 1);
	glPointSize(1.0f);
}

// Sets a pixel on the display
ddgr_color GL3Renderer::GetPixel(int x, int y)
{
	ddgr_color color[4];
	glReadPixels(ScaledX(x), FramebufferHeight() - 1 - ScaledY(y), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid*)color);
	return color[0];
}

void GL3Renderer::FillCircle(ddgr_color col, int x, int y, int rad)
{
}

void GL3Renderer::DrawCircle(int x, int y, int rad)
{
}

// Sets up a font character to draw.  We draw our fonts as pieces of textures
void GL3Renderer::DrawFontCharacter(int bm_handle, int x1, int y1, int x2, int y2, float u, float v, float w, float h)
{
	g3Point* ptr_pnts[4];
	g3Point pnts[4];
	for (int i = 0; i < 4; i++)
	{
		pnts[i].p3_z = 1;	// Make REALLY close!
		pnts[i].p3_flags = PF_PROJECTED;
		ptr_pnts[i] = &pnts[i];
	}
	pnts[0].p3_sx = x1;
	pnts[0].p3_sy = y1;
	pnts[0].p3_u = u;
	pnts[0].p3_v = v;
	pnts[1].p3_sx = x2;
	pnts[1].p3_sy = y1;
	pnts[1].p3_u = u + w;
	pnts[1].p3_v = v;
	pnts[2].p3_sx = x2;
	pnts[2].p3_sy = y2;
	pnts[2].p3_u = u + w;
	pnts[2].p3_v = v + h;
	pnts[3].p3_sx = x1;
	pnts[3].p3_sy = y2;
	pnts[3].p3_u = u;
	pnts[3].p3_v = v + h;
	float old_bloom_suppression = bloom_suppression_draw_value;
	SetBloomSuppression(1.0f);
	DrawPolygon2D(bm_handle, ptr_pnts, 4);
	SetBloomSuppression(old_bloom_suppression);
}

// Draws a line
void GL3Renderer::DrawLine(int x1, int y1, int x2, int y2)
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

	int offset = CopyVertices(2);
	GLfloat line_width = std::max(1.0f, std::min((GLfloat)SupersamplingFactor(), max_line_width));
	glLineWidth(line_width);
	glDrawArrays(GL_LINES, offset, 2);
	glLineWidth(1.0f);

	SetAlphaType(atype);
	SetLighting(ltype);
	SetTextureType(ttype);
}


// Sets the argb characteristics of the font characters.  color1 is the upper left and proceeds clockwise
void GL3Renderer::SetCharacterParameters(ddgr_color color1, ddgr_color color2, ddgr_color color3, ddgr_color color4)
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
void GL3Renderer::SetMultitextureBlendMode(bool state)
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
void GL3Renderer::DrawSpecialLine(g3Point* p0, g3Point* p1)
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
		//glVertex3f(pnt->p3_sx + x_add, pnt->p3_sy + y_add, -z);
	}

	SelectDrawShader();
	int offset = CopyVertices(2);
	GLfloat line_width = std::max(1.0f, std::min((GLfloat)SupersamplingFactor(), max_line_width));
	glLineWidth(line_width);
	glDrawArrays(GL_LINES, offset, 2);
	glLineWidth(1.0f);
}

//	given a chunked bitmap, renders it.
void GL3Renderer::DrawChunkedBitmap(chunked_bitmap* chunk, int x, int y, ubyte alpha)
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
void GL3Renderer::DrawScaledChunkedBitmap(chunked_bitmap* chunk, int x, int y, int neww, int newh, ubyte alpha)
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
void GL3Renderer::DrawSimpleBitmap(int bm_handle, int x, int y)
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

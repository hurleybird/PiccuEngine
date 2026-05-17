/*
* Piccu Engine
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
#include <string.h>
#include <string>
#include <vector>
#include "CFILE.H"
#include "pserror.h"
#include "renderer.h"
#include "gl_local.h"

constexpr int TERRAIN_FOG_COUNTER_MAX = 100;

GLuint commonbuffername;
GLuint legacycommonbuffername;
GLuint fogbuffername;
GLuint specularbuffername;
GLuint terrainfogbuffername;
int terrainfogcounter = 0;

ShaderProgram* lastshaderprog = nullptr;

constexpr int COMMON_BINDING = 0;
constexpr int LEGACY_BINDING = 1;
constexpr int SPECULAR_BINDING = 2;
constexpr int ROOM_BINDING = 3;
constexpr int TERRAIN_FOG_BINDING = 4;

static bool InvertMatrix4(const float m[16], float out[16])
{
	float inv[16];

	inv[0] = m[5] * m[10] * m[15] -
		m[5] * m[11] * m[14] -
		m[9] * m[6] * m[15] +
		m[9] * m[7] * m[14] +
		m[13] * m[6] * m[11] -
		m[13] * m[7] * m[10];

	inv[4] = -m[4] * m[10] * m[15] +
		m[4] * m[11] * m[14] +
		m[8] * m[6] * m[15] -
		m[8] * m[7] * m[14] -
		m[12] * m[6] * m[11] +
		m[12] * m[7] * m[10];

	inv[8] = m[4] * m[9] * m[15] -
		m[4] * m[11] * m[13] -
		m[8] * m[5] * m[15] +
		m[8] * m[7] * m[13] +
		m[12] * m[5] * m[11] -
		m[12] * m[7] * m[9];

	inv[12] = -m[4] * m[9] * m[14] +
		m[4] * m[10] * m[13] +
		m[8] * m[5] * m[14] -
		m[8] * m[6] * m[13] -
		m[12] * m[5] * m[10] +
		m[12] * m[6] * m[9];

	inv[1] = -m[1] * m[10] * m[15] +
		m[1] * m[11] * m[14] +
		m[9] * m[2] * m[15] -
		m[9] * m[3] * m[14] -
		m[13] * m[2] * m[11] +
		m[13] * m[3] * m[10];

	inv[5] = m[0] * m[10] * m[15] -
		m[0] * m[11] * m[14] -
		m[8] * m[2] * m[15] +
		m[8] * m[3] * m[14] +
		m[12] * m[2] * m[11] -
		m[12] * m[3] * m[10];

	inv[9] = -m[0] * m[9] * m[15] +
		m[0] * m[11] * m[13] +
		m[8] * m[1] * m[15] -
		m[8] * m[3] * m[13] -
		m[12] * m[1] * m[11] +
		m[12] * m[3] * m[9];

	inv[13] = m[0] * m[9] * m[14] -
		m[0] * m[10] * m[13] -
		m[8] * m[1] * m[14] +
		m[8] * m[2] * m[13] +
		m[12] * m[1] * m[10] -
		m[12] * m[2] * m[9];

	inv[2] = m[1] * m[6] * m[15] -
		m[1] * m[7] * m[14] -
		m[5] * m[2] * m[15] +
		m[5] * m[3] * m[14] +
		m[13] * m[2] * m[7] -
		m[13] * m[3] * m[6];

	inv[6] = -m[0] * m[6] * m[15] +
		m[0] * m[7] * m[14] +
		m[4] * m[2] * m[15] -
		m[4] * m[3] * m[14] -
		m[12] * m[2] * m[7] +
		m[12] * m[3] * m[6];

	inv[10] = m[0] * m[5] * m[15] -
		m[0] * m[7] * m[13] -
		m[4] * m[1] * m[15] +
		m[4] * m[3] * m[13] +
		m[12] * m[1] * m[7] -
		m[12] * m[3] * m[5];

	inv[14] = -m[0] * m[5] * m[14] +
		m[0] * m[6] * m[13] +
		m[4] * m[1] * m[14] -
		m[4] * m[2] * m[13] -
		m[12] * m[1] * m[6] +
		m[12] * m[2] * m[5];

	inv[3] = -m[1] * m[6] * m[11] +
		m[1] * m[7] * m[10] +
		m[5] * m[2] * m[11] -
		m[5] * m[3] * m[10] -
		m[9] * m[2] * m[7] +
		m[9] * m[3] * m[6];

	inv[7] = m[0] * m[6] * m[11] -
		m[0] * m[7] * m[10] -
		m[4] * m[2] * m[11] +
		m[4] * m[3] * m[10] +
		m[8] * m[2] * m[7] -
		m[8] * m[3] * m[6];

	inv[11] = -m[0] * m[5] * m[11] +
		m[0] * m[7] * m[9] +
		m[4] * m[1] * m[11] -
		m[4] * m[3] * m[9] -
		m[8] * m[1] * m[7] +
		m[8] * m[3] * m[5];

	inv[15] = m[0] * m[5] * m[10] -
		m[0] * m[6] * m[9] -
		m[4] * m[1] * m[10] +
		m[4] * m[2] * m[9] +
		m[8] * m[1] * m[6] -
		m[8] * m[2] * m[5];

	float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if (det == 0.0f)
		return false;

	det = 1.0f / det;
	for (int i = 0; i < 16; i++)
		out[i] = inv[i] * det;
	return true;
}

//Shader pipeline system.
//Contains a table of all shader definitions used by newrender. Renderer will request shader handles by name.
ShaderDefinition gl_shaderdefs[] =
{
	{"lightmap", SF_HASCOMMON, "lightmap.vert", "lightmap.frag"},
	{"lightmap_room", SF_HASCOMMON, "lightmap_room.vert", "lightmap_room.frag"},
	{"lightmapped_specular", SF_HASCOMMON | SF_HASSPECULAR, "lightmap_specular.vert", "lightmap_specular.frag"},
	{"lightmap_room_fog", SF_HASCOMMON | SF_HASROOM, "lightmap_room_fog.vert", "lightmap_room_fog.frag"},
	{"lightmap_room_specular_fog", SF_HASCOMMON | SF_HASROOM | SF_HASSPECULAR, "lightmap_room_specular_fog.vert", "lightmap_room_specular_fog.frag"},
	{"terrain_lightmap", SF_HASCOMMON, "terrain_lightmap.vert", "terrain_lightmap.frag"},
	{"terrain_lightmap_fog", SF_HASCOMMON, "terrain_lightmap_fog.vert", "terrain_lightmap_fog.frag"},
	{"terrain_legacy_compute", 0, "terrain_legacy_compute.vert", "terrain_legacy_compute.frag"},
	{"terrain_legacy_compute_fog", 0, "terrain_legacy_compute.vert", "terrain_legacy_compute_fog.frag"},
	{"unlit_room", SF_HASCOMMON, "unlit_room.vert", "unlit_room.frag"},
	{"unlit_room_fog", SF_HASCOMMON | SF_HASROOM, "unlit_room_fog.vert", "unlit_room_fog.frag"},
	{"fog_portal", SF_HASCOMMON | SF_HASROOM, "fog_portal.vert", "fog_portal.frag"},
};

#define NUM_SHADERDEFS sizeof(gl_shaderdefs) / sizeof(gl_shaderdefs[0])

ShaderProgram gl_shaderprogs[NUM_SHADERDEFS];

void GL4Renderer::InitShaders()
{
	lastshaderprog = nullptr;
	glGenBuffers(1, &commonbuffername);
	glBindBuffer(GL_COPY_WRITE_BUFFER, commonbuffername);
	glBufferData(GL_COPY_WRITE_BUFFER, sizeof(CommonBlock) * 35, nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, COMMON_BINDING, commonbuffername);

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif

	//The legacy common buffer uses the ortho matrix as a passthrough.
	glGenBuffers(1, &legacycommonbuffername);
	glBindBuffer(GL_COPY_WRITE_BUFFER, legacycommonbuffername);
	glBufferData(GL_COPY_WRITE_BUFFER, sizeof(CommonBlock), nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, LEGACY_BINDING, legacycommonbuffername);

#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif

	glGenBuffers(1, &specularbuffername);
	glBindBuffer(GL_COPY_WRITE_BUFFER, specularbuffername);
	glBufferData(GL_COPY_WRITE_BUFFER, sizeof(SpecularBlock), nullptr, GL_STREAM_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, SPECULAR_BINDING, specularbuffername);

#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif

	glGenBuffers(1, &fogbuffername);
	glBindBuffer(GL_COPY_WRITE_BUFFER, fogbuffername);
	glBufferData(GL_COPY_WRITE_BUFFER, sizeof(RoomBlock) * 100, nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, ROOM_BINDING, fogbuffername);

#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif

	glGenBuffers(1, &terrainfogbuffername);
	glBindBuffer(GL_COPY_WRITE_BUFFER, terrainfogbuffername);
	glBufferData(GL_COPY_WRITE_BUFFER, sizeof(TerrainFogBlock) * TERRAIN_FOG_COUNTER_MAX, nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, TERRAIN_FOG_BINDING, terrainfogbuffername);

#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif
	terrainfogcounter = 0;

	//Init shader pipelines
	for (int i = 0; i < NUM_SHADERDEFS; i++)
	{
		gl_shaderprogs[i].AttachSourceFromDefiniton(gl_shaderdefs[i]);
	}
}

uint32_t GL4Renderer::GetPipelineByName(const char* name)
{
	for (uint32_t i = 0; i < NUM_SHADERDEFS; i++)
	{
		if (!stricmp(gl_shaderdefs[i].name, name))
			return i;
	}
	return 0xFFFFFFFFu;
}

void GL4Renderer::BindPipeline(uint32_t handle)
{
	if (handle < NUM_SHADERDEFS)
	{
		if (strstr(gl_shaderdefs[handle].name, "fog") != nullptr)
			post_protection_mask_dirty = true;
		gl_shaderprogs[handle].Use();
		GLint ao_class_uniform = gl_shaderprogs[handle].FindUniform("ao_class_value");
		if (ao_class_uniform != -1)
			glUniform1i(ao_class_uniform, ao_class_draw_value);
		GLint ao_weight_uniform = gl_shaderprogs[handle].FindUniform("ao_weight_value");
		if (ao_weight_uniform != -1)
			glUniform1f(ao_weight_uniform, ao_weight_draw_value);
		GLint ao_capture_weight_mode_uniform = gl_shaderprogs[handle].FindUniform("ao_capture_weight_mode");
		if (ao_capture_weight_mode_uniform != -1)
			glUniform1i(ao_capture_weight_mode_uniform, 0);
	}
}

void GL4Renderer::UpdateCommon(float* projection, float* modelview, int depth)
{
	CommonBlock newblock;
	memcpy(newblock.projection, projection, sizeof(newblock.projection));
	memcpy(newblock.modelview, modelview, sizeof(newblock.modelview));

	//Cache the main-scene projection for GTAO. depth==0 is the regular world
	//pass; instance/portal draws use deeper slots which we don't care about
	//for ambient occlusion (it runs against the main framebuffer only).
	if (depth == 0)
	{
		memcpy(last_projection, projection, sizeof(last_projection));
		g3_Mat4Multiply(current_view_projection, projection, modelview);
		have_current_view_projection = true;
		have_current_inverse_view_projection = InvertMatrix4(current_view_projection, current_inverse_view_projection);
	}

	glBindBuffer(GL_COPY_WRITE_BUFFER, commonbuffername);
	glBufferSubData(GL_COPY_WRITE_BUFFER, sizeof(CommonBlock) * depth, sizeof(CommonBlock), &newblock);

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif

	SetCommonDepth(depth);
}

void GL4Renderer::SetCommonDepth(int depth)
{
	glBindBufferRange(GL_UNIFORM_BUFFER, COMMON_BINDING, commonbuffername, depth * sizeof(CommonBlock), sizeof(CommonBlock));
}

void GL4Renderer::UpdateSpecular(SpecularBlock* specularstate)
{
	glBindBuffer(GL_COPY_WRITE_BUFFER, specularbuffername);
	glBufferSubData(GL_COPY_WRITE_BUFFER, 0, 16 + (specularstate->num_speculars * 32), specularstate);

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif
}

void GL4Renderer::UpdateFogBrightness(RoomBlock* roomstate, int numrooms)
{
	glBindBuffer(GL_COPY_WRITE_BUFFER, fogbuffername);
	glBufferSubData(GL_COPY_WRITE_BUFFER, 0, sizeof(RoomBlock) * numrooms, roomstate);

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif
}

void GL4Renderer::SetCurrentRoomNum(int roomblocknum)
{
	glBindBufferRange(GL_UNIFORM_BUFFER, ROOM_BINDING, fogbuffername, roomblocknum * sizeof(RoomBlock), sizeof(RoomBlock));

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif
}

void GL4Renderer::UpdateTerrainFog(float color[4], float start, float end)
{
	TerrainFogBlock block;
	memcpy(block.color, color, sizeof(float) * 3);
	block.start_dist = start;
	block.end_dist = end;

	glBindBuffer(GL_COPY_WRITE_BUFFER, terrainfogbuffername);
	terrainfogcounter++;
	if (terrainfogcounter == TERRAIN_FOG_COUNTER_MAX)
	{
		glBufferData(GL_COPY_WRITE_BUFFER, sizeof(TerrainFogBlock) * TERRAIN_FOG_COUNTER_MAX, nullptr, GL_DYNAMIC_DRAW);
		terrainfogcounter = 0;
	}
	
	glBufferSubData(GL_COPY_WRITE_BUFFER, terrainfogcounter * sizeof(TerrainFogBlock), sizeof(TerrainFogBlock), &block);
	glBindBufferRange(GL_UNIFORM_BUFFER, TERRAIN_FOG_BINDING, terrainfogbuffername, terrainfogcounter * sizeof(TerrainFogBlock), sizeof(TerrainFogBlock));
#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif
}

void GL4Renderer::UpdateLegacyBlock(float* projection, float* modelview)
{
	CommonBlock newblock;
	memcpy(newblock.projection, projection, sizeof(newblock.projection));
	memcpy(newblock.modelview, modelview, sizeof(newblock.modelview));

	glBindBuffer(GL_COPY_WRITE_BUFFER, legacycommonbuffername);
	glBufferSubData(GL_COPY_WRITE_BUFFER, 0, sizeof(CommonBlock), &newblock);

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();
#endif
}

//ATM this is redundant, but it will support a preprocessor for #include if needed later. 
static GLuint CompileShaderFromFile(GLenum type, const char* filename)
{
	std::string str;

	CFILE* fp = cfopen(filename, "rb");
	if (!fp)
		Error("CompileShaderFromFile: Couldn't open source file %s!", filename);

	str.resize(cfilelength(fp));
	cf_ReadBytes((ubyte*)str.data(), str.size(), fp);
	cfclose(fp);

	GLuint name = glCreateShader(type);
	const char* strptr = str.c_str();
	glShaderSource(name, 1, &strptr, nullptr);
	glCompileShader(name);
	GLint status;
	glGetShaderiv(name, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE)
	{
		GLint length;
		glGetShaderiv(name, GL_INFO_LOG_LENGTH, &length);
		char* buf = new char[length];
		glGetShaderInfoLog(name, length, &length, buf);

		mprintf((1, "%s\n", buf));
		Error("CompileShaderFromFile: Failed to compile shader %s!\n%s", filename, buf);
	}

	return name;
}

static GLuint CompileShader(GLenum type, int numstrs, const char** src, GLint* lengths)
{
	GLuint name = glCreateShader(type);
	glShaderSource(name, numstrs, src, lengths);
	glCompileShader(name);
	GLint status;
	glGetShaderiv(name, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE)
	{
		GLint length;
		glGetShaderiv(name, GL_INFO_LOG_LENGTH, &length);
		char* buf = new char[length];
		glGetShaderInfoLog(name, length, &length, buf);

		mprintf((1, "%s\n", buf));
		Error("CompileShader: Failed to compile shader! This error message needs more context..\n%s", buf);
	}

	return name;
}

void ShaderProgram::CreateCommonBindings(int bindindex)
{
	Use();

	//Find colortexture
	GLint index = glGetUniformLocation(m_name, "colortexture");
	if (index != -1)
		glUniform1i(index, 0); //Set to GL_TEXTURE0

	//Find lightmaptexture
	index = glGetUniformLocation(m_name, "lightmaptexture");
	if (index != -1)
		glUniform1i(index, 1); //Set to GL_TEXTURE1

	//Find CommonBlock
	GLuint uboindex = glGetUniformBlockIndex(m_name, "CommonBlock");
	if (uboindex != GL_INVALID_INDEX)
	{
		//Bind to GL_UNIFORM_BUFFER bindindex. This is so that "legacy" shaders can have the passthrough matricies. 
		glUniformBlockBinding(m_name, uboindex, bindindex);
	}

	//Find SpecularBlock
	uboindex = glGetUniformBlockIndex(m_name, "SpecularBlock");
	if (uboindex != GL_INVALID_INDEX)
	{
		glUniformBlockBinding(m_name, uboindex, SPECULAR_BINDING);
	}
	
	//Find RoomBlock
	uboindex = glGetUniformBlockIndex(m_name, "RoomBlock");
	if (uboindex != GL_INVALID_INDEX)
	{
		glUniformBlockBinding(m_name, uboindex, ROOM_BINDING);
	}

	//Find RoomBlock
	uboindex = glGetUniformBlockIndex(m_name, "TerrainFogBlock");
	if (uboindex != GL_INVALID_INDEX)
	{
		glUniformBlockBinding(m_name, uboindex, TERRAIN_FOG_BINDING);
	}

	m_dynamic_light_count = glGetUniformLocation(m_name, "dynamic_light_count");
	m_dynamic_face_normal = glGetUniformLocation(m_name, "dynamic_face_normal");
	m_dynamic_light_positions = glGetUniformLocation(m_name, "dynamic_light_positions[0]");
	m_dynamic_light_colors = glGetUniformLocation(m_name, "dynamic_light_colors[0]");
	m_dynamic_light_radii = glGetUniformLocation(m_name, "dynamic_light_radii[0]");
	m_dynamic_light_directions = glGetUniformLocation(m_name, "dynamic_light_directions[0]");
	m_dynamic_light_dot_ranges = glGetUniformLocation(m_name, "dynamic_light_dot_ranges[0]");
	m_dynamic_light_directional = glGetUniformLocation(m_name, "dynamic_light_directional[0]");
	m_last_dynamic_light_count = -1;

	ClearBinding();
}

void ShaderProgram::AttachSource(const char* vertexsource, const char* fragsource)
{
	GLint vertexsourcelen = strlen(vertexsource);
	GLint fragsourcelen = strlen(fragsource);
	GLuint vertexprog = CompileShader(GL_VERTEX_SHADER, 1, &vertexsource, &vertexsourcelen);
	GLuint fragmentprog = CompileShader(GL_FRAGMENT_SHADER, 1, &fragsource, &fragsourcelen);

	m_name = glCreateProgram();
	glAttachShader(m_name, vertexprog);
	glAttachShader(m_name, fragmentprog);
	glLinkProgram(m_name);
	GLint status;
	glGetProgramiv(m_name, GL_LINK_STATUS, &status);
	if (status == GL_FALSE)
	{
		GLint length;
		glGetProgramiv(m_name, GL_INFO_LOG_LENGTH, &length);
		char* buf = new char[length];
		glGetProgramInfoLog(m_name, length, &length, buf);

		Error("ShaderProgram::AttachSource: Failed to link program! This error message needs more context..\n%s", buf);
	}

	glDeleteShader(vertexprog);
	glDeleteShader(fragmentprog);

	CreateCommonBindings(COMMON_BINDING);
}

void ShaderProgram::AttachSourceFromDefiniton(ShaderDefinition& def)
{
	GLuint vertexprog = CompileShaderFromFile(GL_VERTEX_SHADER, def.vertex_filename);
	GLuint fragmentprog = CompileShaderFromFile(GL_FRAGMENT_SHADER, def.fragment_filename);

	m_name = glCreateProgram();
	glAttachShader(m_name, vertexprog);
	glAttachShader(m_name, fragmentprog);
	glLinkProgram(m_name);
	GLint status;
	glGetProgramiv(m_name, GL_LINK_STATUS, &status);
	if (status == GL_FALSE)
	{
		GLint length;
		glGetProgramiv(m_name, GL_INFO_LOG_LENGTH, &length);
		char* buf = new char[length];
		glGetProgramInfoLog(m_name, length, &length, buf);

		Error("ShaderProgram::AttachSource: Failed to link program! This error message needs more context..\n%s", buf);
	}

	glDeleteShader(vertexprog);
	glDeleteShader(fragmentprog);

	CreateCommonBindings(COMMON_BINDING);
}

void ShaderProgram::AttachSourcePreprocess(const char* vertexsource, const char* fragsource, bool textured, bool lightmapped, bool speculared, bool fogged)
{
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
		Int3();

	const char* vertexstrs[3];
	GLint vertexlens[3];
	const char* fragstrs[3];
	GLint fraglens[3];

	vertexstrs[0] = fragstrs[0] = "#version 450 core\n";
	vertexlens[0] = fraglens[0] = strlen(vertexstrs[0]);

	std::string preprocessorstr;
	if (textured)
		preprocessorstr.append("#define USE_TEXTURING\n");
	if (lightmapped)
		preprocessorstr.append("#define USE_LIGHTMAP\n");
	if (speculared)
		preprocessorstr.append("#define USE_SPECULAR\n");
	if (fogged)
		preprocessorstr.append("#define USE_FOG\n");

	vertexstrs[1] = fragstrs[1] = preprocessorstr.c_str();
	vertexlens[1] = fraglens[1] = preprocessorstr.size();

	vertexstrs[2] = vertexsource; vertexlens[2] = strlen(vertexsource);
	fragstrs[2] = fragsource; fraglens[2] = strlen(fragsource);

	GLuint vertexprog = CompileShader(GL_VERTEX_SHADER, 3, vertexstrs, vertexlens);
	GLuint fragmentprog = CompileShader(GL_FRAGMENT_SHADER, 3, fragstrs, fraglens);

	m_name = glCreateProgram();
	glAttachShader(m_name, vertexprog);
	glAttachShader(m_name, fragmentprog);
	glLinkProgram(m_name);
	GLint status;
	glGetProgramiv(m_name, GL_LINK_STATUS, &status);
	if (status == GL_FALSE)
	{
		GLint length;
		glGetProgramiv(m_name, GL_INFO_LOG_LENGTH, &length);
		char* buf = new char[length];
		glGetProgramInfoLog(m_name, length, &length, buf);

		Error("ShaderProgram::AttachSource: Failed to link program! This error message needs more context..\n%s", buf);
	}

	glDeleteShader(vertexprog);
	glDeleteShader(fragmentprog);

	//Always use the legacy block with these preprocessed shaders, for now.
	CreateCommonBindings(LEGACY_BINDING);
}

GLint ShaderProgram::FindUniform(const char* uniform)
{
	return glGetUniformLocation(m_name, uniform);
}

void ShaderProgram::Destroy()
{
	ClearBinding();
	glDeleteProgram(m_name);
	m_name = 0;
	m_last_dynamic_light_count = -1;
}

void ShaderProgram::Use()
{
	if (lastshaderprog != this)
	{
		lastshaderprog = this;
		glUseProgram(m_name);
	}
}

void ShaderProgram::ApplyDynamicLighting(int count, const float* face_normal, const GLfloat* positions,
	const GLfloat* colors, const GLfloat* radii, const GLfloat* directions,
	const GLfloat* dot_ranges, const GLint* directional)
{
	if (m_dynamic_light_count == -1)
		return;

	if (count <= 0)
	{
		if (m_last_dynamic_light_count == 0)
			return;

		glUniform1i(m_dynamic_light_count, 0);
		m_last_dynamic_light_count = 0;
		return;
	}

	glUniform1i(m_dynamic_light_count, count);
	if (m_dynamic_face_normal != -1)
		glUniform3fv(m_dynamic_face_normal, 1, face_normal);
	if (m_dynamic_light_positions != -1)
		glUniform3fv(m_dynamic_light_positions, count, positions);
	if (m_dynamic_light_colors != -1)
		glUniform3fv(m_dynamic_light_colors, count, colors);
	if (m_dynamic_light_radii != -1)
		glUniform1fv(m_dynamic_light_radii, count, radii);
	if (m_dynamic_light_directions != -1)
		glUniform3fv(m_dynamic_light_directions, count, directions);
	if (m_dynamic_light_dot_ranges != -1)
		glUniform1fv(m_dynamic_light_dot_ranges, count, dot_ranges);
	if (m_dynamic_light_directional != -1)
		glUniform1iv(m_dynamic_light_directional, count, directional);

	m_last_dynamic_light_count = count;
}

void ShaderProgram::ClearBinding()
{
	lastshaderprog = nullptr;
	glUseProgram(0);
}

ShaderProgram* ShaderProgram::Current()
{
	return lastshaderprog;
}

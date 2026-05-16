/*
* Descent 3
* Copyright (C) 2024 Parallax Software
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

#include <algorithm>
#include <stddef.h>
#include <vector>

#ifdef NEWEDITOR
#include "neweditor\globals.h"
void RenderMine(int viewer_roomnum, int flag_automap, int called_from_terrain, bool render_all, bool outline, bool flat, prim* prim);
#endif
#include "terrain.h"
#include "grdefs.h"
#include "3d.h"
#include "pstypes.h"
#include "pserror.h"
#include "renderer.h"
#include "gametexture.h"
#include "descent.h"
#include "render.h"
#include "game.h"
#include "texture.h"
#include "bitmap.h"
#include "lightmap.h"
#include "ddio.h"
#include "polymodel.h"
#include "lighting.h"
#include "vecmat.h"
#include "renderobject.h"
#include "findintersection.h"
#include "weapon.h"
#include "weather.h"
#include "viseffect.h"
#ifdef EDITOR
#include "editor\d3edit.h"
#endif
#include "fireball.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "gameloop.h"
#include "postrender.h"
#include "Macros.h"
#include "psrand.h"
#include "player.h"
#include <glad/gl.h>
#include "../renderer/HardwareInternal.h"
#include "../renderer/gl_mesh.h"

#define TERRAIN_PERSPECTIVE_TEXTURE_DEPTH 1*TERRAIN_SIZE
#define LOD_ROW_SIZE	(MAX_LOD_SIZE+1)
int DrawTerrainTrianglesSoftware(int index, int bm_handle, int upper_left, int lower_right);
int DrawTerrainTrianglesHardware(int index, int bm_handle, int upper_left, int lower_right);
int DrawTerrainTrianglesHardwareNoLight(int index, int bm_handle, int upper_left, int lower_right);
void DrawTerrainLightmapsHardware(int index, int upper_left, int lower_right);
void DrawSky(vector* veye, matrix* vorient);
int BuildEdgeLists(int* n, int tlist_index);

function_mode View_mode;
int MinAllowableFramerate = 15;
ubyte Fast_terrain = 1;
float Far_fog_border;
vector Terrain_viewer_eye;
ubyte Terrain_from_mine = 0;
ubyte Show_invisible_terrain = 0;
int Terrain_renderer_mode = TERRAIN_RENDERER_COMPUTE;
char Terrain_compute_status_text[96] = "Compute: no frame yet";
int Terrain_objects_drawn = 0;
vector Last_frame_stars[MAX_STARS];
float Terrain_texture_distance = DEFAULT_TEXTURE_DISTANCE;
int Check_terrain_portal = 0;
static vector Temp_sky_vectors[MAX_HORIZON_PIECES][6];
static vector Temp_sky_vectors_unrotated[MAX_HORIZON_PIECES][6];

// Last time terrain was rendered
float Last_terrain_render_time = -1;
// Sets UV's based on 90 degree rotations
float TerrainUSpeedup[4][LOD_ROW_SIZE * LOD_ROW_SIZE];
float TerrainVSpeedup[4][LOD_ROW_SIZE * LOD_ROW_SIZE];

#if (!defined(RELEASE) || defined(NEWEDITOR))
//for building a render list for each terrain cell
int render_next[MAX_OBJECTS];
#endif
void DrawPlayerOnWireframe();
float Clip_scale_left, Clip_scale_top, Clip_scale_right, Clip_scale_bot;
#ifdef NEWEDITOR
bool Rendering_main_view = true;
#endif

static uint32_t Terrain_legacy_compute_handle = 0xFFFFFFFFu;
static uint32_t Terrain_legacy_compute_fog_handle = 0xFFFFFFFFu;

extern vector Clip_plane_point;

struct TerrainGpuCellInput
{
	uint32_t packed[4];
	float height[4];
};

static_assert(sizeof(TerrainGpuCellInput) == 32, "TerrainGpuCellInput must match GLSL std430 array stride");
static_assert(offsetof(TerrainGpuCellInput, packed) == 0, "TerrainGpuCellInput packed offset must match GLSL");
static_assert(offsetof(TerrainGpuCellInput, height) == 16, "TerrainGpuCellInput height offset must match GLSL");

struct TerrainGpuVertexOutput
{
	vector position;
	uint32_t color;
	vector normal;
	int lmpage;
	float u1;
	float v1;
	float u2;
	float v2;
	float uslide;
	float vslide;
	float pad0;
	float pad1;
};

static_assert(sizeof(TerrainGpuVertexOutput) == 64, "TerrainGpuVertexOutput must match GLSL std430 array stride");
static_assert(offsetof(TerrainGpuVertexOutput, color) == 12, "TerrainGpuVertexOutput color offset must match GLSL");
static_assert(offsetof(TerrainGpuVertexOutput, normal) == 16, "TerrainGpuVertexOutput normal offset must match GLSL");
static_assert(offsetof(TerrainGpuVertexOutput, lmpage) == 28, "TerrainGpuVertexOutput lmpage offset must match GLSL");
static_assert(offsetof(TerrainGpuVertexOutput, u1) == 32, "TerrainGpuVertexOutput uv1 offset must match GLSL");
static_assert(offsetof(TerrainGpuVertexOutput, u2) == 40, "TerrainGpuVertexOutput uv2 offset must match GLSL");
static_assert(offsetof(TerrainGpuVertexOutput, uslide) == 48, "TerrainGpuVertexOutput uvslide offset must match GLSL");

struct TerrainGpuCellWork
{
	TerrainGpuCellInput input;
	int texture;
};

struct TerrainGpuBatch
{
	int texture;
	int first_cell;
	int cell_count;
	int first_vertex;
	int vertex_count;
};

struct TerrainGpuDrawCommand
{
	uint32_t count;
	uint32_t instance_count;
	uint32_t first;
	uint32_t base_instance;
};

static_assert(sizeof(TerrainGpuDrawCommand) == 16, "TerrainGpuDrawCommand must match DrawArraysIndirectCommand");

static GLuint Terrain_compute_program = 0;
static GLuint Terrain_compute_input_buffer = 0;
static GLuint Terrain_compute_vertex_buffer = 0;
static GLuint Terrain_compute_indirect_buffer = 0;
static GLuint Terrain_compute_vertex_array = 0;
static GLuint Terrain_compute_base_texture_array = 0;
static GLuint Terrain_compute_lightmap_array = 0;
static GLint Terrain_compute_cell_count_uniform = -1;
static GLint Terrain_compute_row0_uniform = -1;
static GLint Terrain_compute_xstep_uniform = -1;
static GLint Terrain_compute_zstep_uniform = -1;
static GLint Terrain_compute_ystep_uniform = -1;
static bool Terrain_compute_ready = false;
static bool Terrain_compute_unavailable = false;
static size_t Terrain_compute_vertex_capacity = 0;
static bool Terrain_compute_draw_work_ready = false;
static bool Terrain_compute_input_uploaded = false;
static int Terrain_compute_draw_work_checksum = -2;
static ubyte Terrain_compute_draw_work_show_invisible = 0xff;
static std::vector<TerrainGpuCellWork> Terrain_compute_cell_work;
static std::vector<TerrainGpuCellInput> Terrain_compute_cell_inputs;
static std::vector<TerrainGpuBatch> Terrain_compute_batches;
static std::vector<TerrainGpuDrawCommand> Terrain_compute_draw_commands;
static int Terrain_compute_base_texture_array_width = 0;
static int Terrain_compute_base_texture_array_height = 0;
static std::vector<int> Terrain_compute_base_texture_handles;
static std::vector<uint32_t> Terrain_compute_base_texture_checksums;
static int Terrain_compute_lightmap_array_size = 0;
static int Terrain_compute_lightmap_array_handles[4] = { -1, -1, -1, -1 };
static uint32_t Terrain_compute_lightmap_checksums[4] = { 0, 0, 0, 0 };
static constexpr int TERRAIN_COMPUTE_VERTS_PER_TRIANGLE = 18;
static constexpr int TERRAIN_COMPUTE_VERTS_PER_CELL = TERRAIN_COMPUTE_VERTS_PER_TRIANGLE * 2;
static constexpr int TERRAIN_COMPUTE_LIGHTMAP_LAYERS = 4;

static void SetTerrainComputeStatus(const char* status)
{
	snprintf(Terrain_compute_status_text, sizeof(Terrain_compute_status_text), "Compute: %s", status);
	Terrain_compute_status_text[sizeof(Terrain_compute_status_text) - 1] = '\0';
}

static void SetTerrainComputeStatusFromLog(const char* status, const char* log)
{
	char compact_log[48];
	size_t out = 0;
	for (size_t in = 0; log && log[in] != '\0' && out < sizeof(compact_log) - 1; in++)
	{
		char ch = log[in];
		if (ch == '\r' || ch == '\n')
			break;
		if ((unsigned char)ch < 32)
			ch = ' ';
		compact_log[out++] = ch;
	}
	compact_log[out] = '\0';

	if (compact_log[0])
		snprintf(Terrain_compute_status_text, sizeof(Terrain_compute_status_text), "Compute: %s %s", status, compact_log);
	else
		snprintf(Terrain_compute_status_text, sizeof(Terrain_compute_status_text), "Compute: %s", status);
	Terrain_compute_status_text[sizeof(Terrain_compute_status_text) - 1] = '\0';
}

static void SetTerrainComputeStatusActive(int cellcount)
{
	snprintf(Terrain_compute_status_text, sizeof(Terrain_compute_status_text),
		"Compute: ACTIVE %dc %zut %zub", cellcount, Terrain_compute_cell_inputs.size() * 2, Terrain_compute_batches.size());
	Terrain_compute_status_text[sizeof(Terrain_compute_status_text) - 1] = '\0';
}

static uint32_t TerrainComputeBitmapChecksum(int bmhandle)
{
	uint32_t hash = 2166136261u;
	int width = bm_w(bmhandle, 0);
	int height = bm_h(bmhandle, 0);
	int format = bm_format(bmhandle);
	ushort* data = bm_data(bmhandle, 0);

	hash = (hash ^ (uint32_t)width) * 16777619u;
	hash = (hash ^ (uint32_t)height) * 16777619u;
	hash = (hash ^ (uint32_t)format) * 16777619u;

	if (!data || width <= 0 || height <= 0)
		return hash;

	for (int i = 0; i < width * height; i++)
		hash = (hash ^ (uint32_t)data[i]) * 16777619u;

	return hash;
}

static uint32_t TerrainComputeBitmapPixelToRgba(ushort src, int format)
{
	if (format == BITMAP_FORMAT_4444)
	{
		int a = (src >> 12) & 0xf;
		int r = (src >> 8) & 0xf;
		int g = (src >> 4) & 0xf;
		int b = src & 0xf;
		a = (int)((float)a * (255.0f / 15.0f));
		r = (int)((float)r * (255.0f / 15.0f));
		g = (int)((float)g * (255.0f / 15.0f));
		b = (int)((float)b * (255.0f / 15.0f));
		return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
	}

	int a = (src & OPAQUE_FLAG) ? 255 : 0;
	int r = (src >> 10) & 0x1f;
	int g = (src >> 5) & 0x1f;
	int b = src & 0x1f;
	r = (int)((float)r * (255.0f / 31.0f));
	g = (int)((float)g * (255.0f / 31.0f));
	b = (int)((float)b * (255.0f / 31.0f));
	return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static void ConvertTerrainBaseTextureToRgba(int bmhandle, int width, int height, std::vector<uint32_t>& pixels)
{
	pixels.assign(width * height, 0);
	int source_width = bm_w(bmhandle, 0);
	int source_height = bm_h(bmhandle, 0);
	ushort* data = bm_data(bmhandle, 0);
	if (!data || source_width <= 0 || source_height <= 0 || width <= 0 || height <= 0)
		return;

	int format = bm_format(bmhandle);
	for (int y = 0; y < height; y++)
	{
		int source_y = (y * source_height) / height;
		for (int x = 0; x < width; x++)
		{
			int source_x = (x * source_width) / width;
			pixels[y * width + x] = TerrainComputeBitmapPixelToRgba(data[source_y * source_width + source_x], format);
		}
	}
}

static bool TerrainComputeBaseTextureMayChange(int texture, int bmhandle)
{
	int texture_flags = (texture >= 0 && texture < MAX_TEXTURES) ? GameTextures[texture].flags : 0;
	return (texture_flags & (TF_ANIMATED | TF_PROCEDURAL | TF_WATER_PROCEDURAL)) ||
		(GameBitmaps[bmhandle].flags & (BF_CHANGED | BF_BRAND_NEW));
}

static bool EnsureTerrainComputeBaseTextureArray()
{
	int layer_count = (int)Terrain_compute_batches.size();
	if (layer_count <= 0)
		return true;

	GLint max_layers = 0;
	glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
	if (layer_count > max_layers)
	{
		SetTerrainComputeStatus("fallback texture layers");
		return false;
	}

	std::vector<int> bitmap_handles(layer_count);
	int array_width = 0;
	int array_height = 0;
	for (int i = 0; i < layer_count; i++)
	{
		int bmhandle = GetTextureBitmap(Terrain_compute_batches[i].texture, 0);
		if (bmhandle < BAD_BITMAP_HANDLE)
			bmhandle = BAD_BITMAP_HANDLE;

		int width = bm_w(bmhandle, 0);
		int height = bm_h(bmhandle, 0);
		if (width <= 0 || height <= 0 || bm_data(bmhandle, 0) == nullptr)
		{
			SetTerrainComputeStatus("fallback base texture");
			return false;
		}

		bitmap_handles[i] = bmhandle;
		if (width > array_width)
			array_width = width;
		if (height > array_height)
			array_height = height;
	}

	bool recreate = Terrain_compute_base_texture_array == 0 ||
		Terrain_compute_base_texture_array_width != array_width ||
		Terrain_compute_base_texture_array_height != array_height ||
		Terrain_compute_base_texture_handles.size() != bitmap_handles.size();

	if (!recreate)
	{
		for (size_t i = 0; i < bitmap_handles.size(); i++)
		{
			if (Terrain_compute_base_texture_handles[i] != bitmap_handles[i])
			{
				recreate = true;
				break;
			}
		}
	}

	if (Terrain_compute_base_texture_array == 0)
		glGenTextures(1, &Terrain_compute_base_texture_array);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, Terrain_compute_base_texture_array);

	if (recreate)
	{
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, array_width, array_height, layer_count, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
		Terrain_compute_base_texture_array_width = array_width;
		Terrain_compute_base_texture_array_height = array_height;
		Terrain_compute_base_texture_handles = bitmap_handles;
		Terrain_compute_base_texture_checksums.assign(bitmap_handles.size(), 0);
	}

	bool uploaded = false;
	std::vector<uint32_t> pixels;
	for (int i = 0; i < layer_count; i++)
	{
		int bmhandle = bitmap_handles[i];
		uint32_t checksum = Terrain_compute_base_texture_checksums[i];
		bool should_check = recreate || checksum == 0 ||
			TerrainComputeBaseTextureMayChange(Terrain_compute_batches[i].texture, bmhandle);
		if (should_check)
			checksum = TerrainComputeBitmapChecksum(bmhandle);
		if (!recreate && checksum == Terrain_compute_base_texture_checksums[i])
			continue;

		ConvertTerrainBaseTextureToRgba(bmhandle, array_width, array_height, pixels);
		glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, array_width, array_height, 1,
			GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		Terrain_compute_base_texture_checksums[i] = checksum;
		uploaded = true;
	}

	if (uploaded)
		glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

	return true;
}

static void BindTerrainComputeTextureArrays()
{
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D_ARRAY, Terrain_compute_base_texture_array);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D_ARRAY, Terrain_compute_lightmap_array);
}

static uint32_t TerrainComputeLightmapChecksum(int lmhandle)
{
	uint32_t hash = 2166136261u;
	int width = lm_w(lmhandle);
	int height = lm_h(lmhandle);
	int size = GameLightmaps[lmhandle].square_res;
	ushort* data = lm_data(lmhandle);

	hash = (hash ^ (uint32_t)width) * 16777619u;
	hash = (hash ^ (uint32_t)height) * 16777619u;
	hash = (hash ^ (uint32_t)size) * 16777619u;

	if (!data || width <= 0 || height <= 0)
		return hash;

	for (int i = 0; i < width * height; i++)
		hash = (hash ^ (uint32_t)data[i]) * 16777619u;

	return hash;
}

static void ConvertTerrainLightmapToRgba(int lmhandle, int size, std::vector<uint32_t>& pixels)
{
	pixels.assign(size * size, 0);
	int width = lm_w(lmhandle);
	int height = lm_h(lmhandle);
	ushort* data = lm_data(lmhandle);
	if (!data || width <= 0 || height <= 0)
		return;

	for (int y = 0; y < height && y < size; y++)
	{
		for (int x = 0; x < width && x < size; x++)
		{
			ushort src = data[y * width + x];
			if (!(src & OPAQUE_FLAG))
				continue;

			int r = (src >> 10) & 0x1f;
			int g = (src >> 5) & 0x1f;
			int b = src & 0x1f;
			r = (int)((float)r * (255.0f / 31.0f));
			g = (int)((float)g * (255.0f / 31.0f));
			b = (int)((float)b * (255.0f / 31.0f));
			pixels[y * size + x] = (255u << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
		}
	}
}

static bool EnsureTerrainComputeLightmapArray()
{
	int size = GameLightmaps[TerrainLightmaps[0]].square_res;
	if (size <= 0)
		size = lm_w(TerrainLightmaps[0]);
	if (size <= 0)
	{
		SetTerrainComputeStatus("fallback lightmap array");
		return false;
	}

	bool recreate = Terrain_compute_lightmap_array == 0 || Terrain_compute_lightmap_array_size != size;
	for (int i = 0; i < TERRAIN_COMPUTE_LIGHTMAP_LAYERS; i++)
	{
		if (Terrain_compute_lightmap_array_handles[i] != TerrainLightmaps[i])
			recreate = true;
	}

	if (Terrain_compute_lightmap_array == 0)
		glGenTextures(1, &Terrain_compute_lightmap_array);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D_ARRAY, Terrain_compute_lightmap_array);

	if (recreate)
	{
		glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size, size, TERRAIN_COMPUTE_LIGHTMAP_LAYERS, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 0);
		Terrain_compute_lightmap_array_size = size;
		for (int i = 0; i < TERRAIN_COMPUTE_LIGHTMAP_LAYERS; i++)
		{
			Terrain_compute_lightmap_array_handles[i] = TerrainLightmaps[i];
			Terrain_compute_lightmap_checksums[i] = 0;
		}
	}

	std::vector<uint32_t> pixels;
	for (int i = 0; i < TERRAIN_COMPUTE_LIGHTMAP_LAYERS; i++)
	{
		uint32_t checksum = TerrainComputeLightmapChecksum(TerrainLightmaps[i]);
		if (!recreate && checksum == Terrain_compute_lightmap_checksums[i])
			continue;

		ConvertTerrainLightmapToRgba(TerrainLightmaps[i], size, pixels);
		glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, size, size, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		Terrain_compute_lightmap_checksums[i] = checksum;
	}

	return true;
}

static void SetTerrainComputeDynamicLightUniforms()
{
	GLint program = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	if (program == 0)
		return;

	GLint count_location = glGetUniformLocation((GLuint)program, "terrain_dynamic_light_count");
	if (count_location == -1)
		return;

	GLint position_location = glGetUniformLocation((GLuint)program, "terrain_dynamic_light_positions");
	GLint color_location = glGetUniformLocation((GLuint)program, "terrain_dynamic_light_colors");
	GLint radius_location = glGetUniformLocation((GLuint)program, "terrain_dynamic_light_radii");
	GLint direction_location = glGetUniformLocation((GLuint)program, "terrain_dynamic_light_directions");
	GLint dot_range_location = glGetUniformLocation((GLuint)program, "terrain_dynamic_light_dot_ranges");
	GLint directional_location = glGetUniformLocation((GLuint)program, "terrain_dynamic_light_directional");

	GLint counts[TERRAIN_COMPUTE_LIGHTMAP_LAYERS] = {};
	GLfloat positions[TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS][3] = {};
	GLfloat colors[TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS][3] = {};
	GLfloat radii[TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS] = {};
	GLfloat directions[TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS][3] = {};
	GLfloat dot_ranges[TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS] = {};
	GLint directional[TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS] = {};

	if (Render_preferred_state.per_pixel_lighting)
	{
		for (int layer = 0; layer < TERRAIN_COMPUTE_LIGHTMAP_LAYERS; layer++)
		{
			renderer_per_pixel_light lights[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS];
			int light_count = GetPerPixelLightmapTextureLights(TerrainLightmaps[layer], lights,
				RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS);
			counts[layer] = light_count;

			for (int i = 0; i < light_count; i++)
			{
				int index = layer * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS + i;
				positions[index][0] = lights[i].position[0];
				positions[index][1] = lights[i].position[1];
				positions[index][2] = lights[i].position[2];
				colors[index][0] = lights[i].color[0];
				colors[index][1] = lights[i].color[1];
				colors[index][2] = lights[i].color[2];
				radii[index] = lights[i].radius;
				directions[index][0] = lights[i].direction[0];
				directions[index][1] = lights[i].direction[1];
				directions[index][2] = lights[i].direction[2];
				dot_ranges[index] = lights[i].dot_range;
				directional[index] = lights[i].directional ? 1 : 0;
			}
		}
	}

	glUniform1iv(count_location, TERRAIN_COMPUTE_LIGHTMAP_LAYERS, counts);
	if (position_location != -1)
		glUniform3fv(position_location, TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS,
			&positions[0][0]);
	if (color_location != -1)
		glUniform3fv(color_location, TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS,
			&colors[0][0]);
	if (radius_location != -1)
		glUniform1fv(radius_location, TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS, radii);
	if (direction_location != -1)
		glUniform3fv(direction_location, TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS,
			&directions[0][0]);
	if (dot_range_location != -1)
		glUniform1fv(dot_range_location, TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS,
			dot_ranges);
	if (directional_location != -1)
		glUniform1iv(directional_location, TERRAIN_COMPUTE_LIGHTMAP_LAYERS * RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS,
			directional);
}

static void EnsureTerrainPipelinesReady()
{
	if (Terrain_legacy_compute_handle == 0xFFFFFFFFu)
		Terrain_legacy_compute_handle = rend_GetPipelineByName("terrain_legacy_compute");
	if (Terrain_legacy_compute_fog_handle == 0xFFFFFFFFu)
		Terrain_legacy_compute_fog_handle = rend_GetPipelineByName("terrain_legacy_compute_fog");
}

static void ConfigureTerrainComputeVertexArray()
{
	glBindVertexArray(Terrain_compute_vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, Terrain_compute_vertex_buffer);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainGpuVertexOutput), (void*)offsetof(TerrainGpuVertexOutput, position));

	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TerrainGpuVertexOutput), (void*)offsetof(TerrainGpuVertexOutput, color));

	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainGpuVertexOutput), (void*)offsetof(TerrainGpuVertexOutput, normal));

	glEnableVertexAttribArray(3);
	glVertexAttribIPointer(3, 1, GL_INT, sizeof(TerrainGpuVertexOutput), (void*)offsetof(TerrainGpuVertexOutput, lmpage));

	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(TerrainGpuVertexOutput), (void*)offsetof(TerrainGpuVertexOutput, u1));

	glEnableVertexAttribArray(5);
	glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(TerrainGpuVertexOutput), (void*)offsetof(TerrainGpuVertexOutput, u2));

	glEnableVertexAttribArray(6);
	glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, sizeof(TerrainGpuVertexOutput), (void*)offsetof(TerrainGpuVertexOutput, uslide));
}

static bool CompileTerrainComputeProgram()
{
	if (Terrain_compute_ready)
		return true;
	if (Terrain_compute_unavailable)
		return false;
	if (!GLAD_GL_VERSION_4_3)
	{
		SetTerrainComputeStatus("fallback no GL 4.3");
		Terrain_compute_unavailable = true;
		return false;
	}

	static const char* compute_source = R"glsl(
#version 450 core
layout(local_size_x = 128) in;

struct TerrainCellInput
{
	uvec4 packed;
	vec4 height;
};

struct TerrainVertexOutput
{
	vec3 position;
	uint color;
	vec3 normal;
	int lmpage;
	vec2 uv1;
	vec2 uv2;
	vec2 uvslide;
};

layout(std430, binding = 0) readonly buffer TerrainInputBuffer
{
	TerrainCellInput cells[];
};

layout(std430, binding = 1) writeonly buffer TerrainVertexBuffer
{
	TerrainVertexOutput vertices[];
};

struct DrawArraysIndirectCommand
{
	uint count;
	uint instance_count;
	uint first;
	uint base_instance;
};

layout(std430, binding = 2) buffer TerrainDrawCommandBuffer
{
	DrawArraysIndirectCommand draws[];
};

uniform uint cell_count;
uniform vec3 terrain_row0;
uniform vec3 terrain_x_step;
uniform vec3 terrain_z_step;
uniform vec3 terrain_y_step;

const float TERRAIN_COMPUTE_MIN_Z = 0.000001;
const int TERRAIN_CLIP_MAX_VERTS = 8;
const int TERRAIN_CLIP_LEFT = 1;
const int TERRAIN_CLIP_RIGHT = 2;
const int TERRAIN_CLIP_BOT = 4;
const int TERRAIN_CLIP_TOP = 8;
const int TERRAIN_CLIP_BEHIND = 128;
struct ClipVertex
{
	vec3 world;
	vec3 rotated;
	vec2 base_uv;
	vec2 lightmap_uv;
};

vec3 RotatePoint(vec3 world)
{
	return terrain_row0 +
		terrain_x_step * world.x +
		terrain_y_step * world.y +
		terrain_z_step * world.z;
}

vec3 CellPosition(uint segment, vec4 height, uint corner)
{
	uint cx = segment & 255u;
	uint cz = segment >> 8u;
	uint x = cx;
	uint z = cz;
	float y = height.w;

	if (corner == 0u)
	{
		z = cz + 1u;
		y = height.x;
	}
	else if (corner == 1u)
	{
		x = cx + 1u;
		z = cz + 1u;
		y = height.y;
	}
	else if (corner == 2u)
	{
		x = cx + 1u;
		y = height.z;
	}

	return vec3(float(x) * 16.0, y, float(z) * 16.0);
}

vec2 BaseUv(uint segment, uint rotation, uint corner)
{
	uint cx = segment & 255u;
	uint cz = segment >> 8u;
	float subx = float(cx & 7u);
	float subz = float(7u - (cz & 7u));

	if (corner == 1u || corner == 2u)
		subx += 1.0;
	if (corner == 2u || corner == 3u)
		subz += 1.0;

	float x = subx * 0.125;
	float y = subz * 0.125;
	float tile = float(rotation >> 4u);
	uint rotator = rotation & 15u;

	vec2 uv;
	if (rotator == 1u)
		uv = vec2(1.0 - y, x);
	else if (rotator == 2u)
		uv = vec2(1.0 - x, 1.0 - y);
	else if (rotator == 3u)
		uv = vec2(y, 1.0 - x);
	else
		uv = vec2(x, y);

	return uv * tile;
}

vec2 LightmapUv(uint segment, uint corner)
{
	uint cx = segment & 255u;
	uint cz = segment >> 8u;
	vec2 uv = vec2(float(cx & 127u) * 0.0078125,
		float(128u - ((cz & 127u) + 1u)) * 0.0078125);

	if (corner == 1u || corner == 2u)
		uv.x += 0.0078125;
	if (corner == 2u || corner == 3u)
		uv.y += 0.0078125;

	return uv;
}

void WriteVertex(uint index, uint texture_page, uint lightmap_page, vec3 world, vec3 rotated, vec2 base_uv, vec2 lightmap_uv)
{
	float eye_z = max(rotated.z, TERRAIN_COMPUTE_MIN_Z);
	float texw = 1.0 / eye_z;
	float legacy_depth = clamp(1.0 - texw, 0.0, 1.0);

	vertices[index].position = vec3(rotated.x * texw, rotated.y * texw, legacy_depth * 2.0 - 1.0);
	vertices[index].color = 0xffffffffu;
	vertices[index].normal = world;
	vertices[index].lmpage = int((texture_page << 8u) | (lightmap_page & 255u));
	vertices[index].uv1 = base_uv * texw;
	vertices[index].uv2 = lightmap_uv * texw;
	vertices[index].uvslide = vec2(texw, legacy_depth);
}

ClipVertex MixClipVertex(ClipVertex a, ClipVertex b, float t)
{
	ClipVertex result;
	result.world = mix(a.world, b.world, t);
	result.rotated = mix(a.rotated, b.rotated, t);
	result.base_uv = mix(a.base_uv, b.base_uv, t);
	result.lightmap_uv = mix(a.lightmap_uv, b.lightmap_uv, t);
	return result;
}

int TerrainClipCode(ClipVertex vertex)
{
	int code = 0;
	if (vertex.rotated.x < -vertex.rotated.z)
		code |= TERRAIN_CLIP_LEFT;
	if (vertex.rotated.x > vertex.rotated.z)
		code |= TERRAIN_CLIP_RIGHT;
	if (vertex.rotated.y < -vertex.rotated.z)
		code |= TERRAIN_CLIP_BOT;
	if (vertex.rotated.y > vertex.rotated.z)
		code |= TERRAIN_CLIP_TOP;
	if (vertex.rotated.z < 0.0)
		code |= TERRAIN_CLIP_BEHIND;
	return code;
}

void TerrainClipCodes(ClipVertex poly[TERRAIN_CLIP_MAX_VERTS], int count, out int code_or, out int code_and)
{
	code_or = 0;
	code_and = 255;
	for (int i = 0; i < count; i++)
	{
		int code = TerrainClipCode(poly[i]);
		code_or |= code;
		code_and &= code;
	}
}

float TerrainClipPlaneDistance(ClipVertex vertex, int plane)
{
	if (plane == TERRAIN_CLIP_LEFT)
		return vertex.rotated.x + vertex.rotated.z;
	if (plane == TERRAIN_CLIP_RIGHT)
		return vertex.rotated.z - vertex.rotated.x;
	if (plane == TERRAIN_CLIP_BOT)
		return vertex.rotated.y + vertex.rotated.z;
	return vertex.rotated.z - vertex.rotated.y;
}

ClipVertex IntersectTerrainClipPlane(ClipVertex inside_vertex, ClipVertex outside_vertex, int plane)
{
	float inside_distance = TerrainClipPlaneDistance(inside_vertex, plane);
	float outside_distance = TerrainClipPlaneDistance(outside_vertex, plane);
	float denom = inside_distance - outside_distance;
	float t = (abs(denom) > 0.000001) ? (inside_distance / denom) : 0.0;
	return MixClipVertex(inside_vertex, outside_vertex, clamp(t, 0.0, 1.0));
}

void TerrainClipAppend(inout ClipVertex poly[TERRAIN_CLIP_MAX_VERTS], inout int count, ClipVertex vertex)
{
	if (count < TERRAIN_CLIP_MAX_VERTS)
		poly[count++] = vertex;
}

void ClipTerrainPolyToPlane(inout ClipVertex poly[TERRAIN_CLIP_MAX_VERTS], inout int count, int plane)
{
	ClipVertex output_poly[TERRAIN_CLIP_MAX_VERTS];
	int output_count = 0;
	ClipVertex previous = poly[count - 1];
	bool previous_inside = TerrainClipPlaneDistance(previous, plane) >= 0.0;

	for (int i = 0; i < count; i++)
	{
		ClipVertex current = poly[i];
		bool current_inside = TerrainClipPlaneDistance(current, plane) >= 0.0;

		if (current_inside)
		{
			if (!previous_inside)
				TerrainClipAppend(output_poly, output_count, IntersectTerrainClipPlane(current, previous, plane));
			TerrainClipAppend(output_poly, output_count, current);
		}
		else if (previous_inside)
		{
			TerrainClipAppend(output_poly, output_count, IntersectTerrainClipPlane(previous, current, plane));
		}

		previous = current;
		previous_inside = current_inside;
	}

	count = output_count;
	for (int i = 0; i < TERRAIN_CLIP_MAX_VERTS; i++)
	{
		if (i < count)
			poly[i] = output_poly[i];
	}
}

bool ClipTerrainTriangle(ClipVertex input_poly[3], out ClipVertex output_poly[TERRAIN_CLIP_MAX_VERTS], out int output_count)
{
	output_count = 3;
	for (int i = 0; i < 3; i++)
		output_poly[i] = input_poly[i];

	int code_or = 0;
	int code_and = 255;
	TerrainClipCodes(output_poly, output_count, code_or, code_and);
	if (code_and != 0)
		return false;

	if ((code_or & TERRAIN_CLIP_LEFT) != 0)
	{
		ClipTerrainPolyToPlane(output_poly, output_count, TERRAIN_CLIP_LEFT);
		TerrainClipCodes(output_poly, output_count, code_or, code_and);
		if (output_count < 3 || code_and != 0)
			return false;
	}
	if ((code_or & TERRAIN_CLIP_RIGHT) != 0)
	{
		ClipTerrainPolyToPlane(output_poly, output_count, TERRAIN_CLIP_RIGHT);
		TerrainClipCodes(output_poly, output_count, code_or, code_and);
		if (output_count < 3 || code_and != 0)
			return false;
	}
	if ((code_or & TERRAIN_CLIP_BOT) != 0)
	{
		ClipTerrainPolyToPlane(output_poly, output_count, TERRAIN_CLIP_BOT);
		TerrainClipCodes(output_poly, output_count, code_or, code_and);
		if (output_count < 3 || code_and != 0)
			return false;
	}
	if ((code_or & TERRAIN_CLIP_TOP) != 0)
	{
		ClipTerrainPolyToPlane(output_poly, output_count, TERRAIN_CLIP_TOP);
		TerrainClipCodes(output_poly, output_count, code_or, code_and);
		if (output_count < 3 || code_and != 0)
			return false;
	}

	return (code_or & TERRAIN_CLIP_BEHIND) == 0;
}

uint ClippedTriangleVertexCount(int vertex_count)
{
	return vertex_count >= 3 ? uint(vertex_count - 2) * 3u : 0u;
}

void WriteClippedTriangleFan(uint vertex_base, uint texture_page, uint lightmap_page, ClipVertex poly[TERRAIN_CLIP_MAX_VERTS], int vertex_count)
{
	uint cursor = 0u;
	if (vertex_count >= 3)
	{
		for (int i = 1; i < vertex_count - 1; i++)
		{
			WriteVertex(vertex_base + cursor + 0u, texture_page, lightmap_page, poly[0].world, poly[0].rotated, poly[0].base_uv, poly[0].lightmap_uv);
			WriteVertex(vertex_base + cursor + 1u, texture_page, lightmap_page, poly[i].world, poly[i].rotated, poly[i].base_uv, poly[i].lightmap_uv);
			WriteVertex(vertex_base + cursor + 2u, texture_page, lightmap_page, poly[i + 1].world, poly[i + 1].rotated, poly[i + 1].base_uv, poly[i + 1].lightmap_uv);
			cursor += 3u;
		}
	}
}

void EmitTriangle(uint batch_index, uint batch_output_first_vertex, uint texture_page, uint lightmap_page,
	vec3 world0, vec3 world1, vec3 world2,
	vec2 uv0, vec2 uv1, vec2 uv2,
	vec2 lm0, vec2 lm1, vec2 lm2)
{
	vec3 rotated0 = RotatePoint(world0);
	vec3 rotated1 = RotatePoint(world1);
	vec3 rotated2 = RotatePoint(world2);
	vec3 facing_normal = cross(rotated1 - rotated0, rotated2 - rotated1);
	bool visible = dot(facing_normal, rotated1) < 0.0;

	if (!visible)
	{
		return;
	}

	ClipVertex input_poly[3];
	input_poly[0].world = world0;
	input_poly[0].rotated = rotated0;
	input_poly[0].base_uv = uv0;
	input_poly[0].lightmap_uv = lm0;
	input_poly[1].world = world1;
	input_poly[1].rotated = rotated1;
	input_poly[1].base_uv = uv1;
	input_poly[1].lightmap_uv = lm1;
	input_poly[2].world = world2;
	input_poly[2].rotated = rotated2;
	input_poly[2].base_uv = uv2;
	input_poly[2].lightmap_uv = lm2;

	ClipVertex clipped_poly[TERRAIN_CLIP_MAX_VERTS];
	int clipped_count = 0;
	if (!ClipTerrainTriangle(input_poly, clipped_poly, clipped_count))
		return;
	uint write_count = ClippedTriangleVertexCount(clipped_count);
	if (write_count == 0u)
		return;

	uint vertex_base = batch_output_first_vertex + atomicAdd(draws[batch_index].count, write_count);
	WriteClippedTriangleFan(vertex_base, texture_page, lightmap_page, clipped_poly, clipped_count);
}

void main()
{
	uint id = gl_GlobalInvocationID.x;
	if (id >= cell_count)
		return;

	TerrainCellInput cell = cells[id];
	uint segment = cell.packed.x;
	uint rotation = cell.packed.y & 255u;
	uint lightmap_page = (cell.packed.y >> 8u) & 255u;
	uint texture_page = cell.packed.y >> 16u;
	uint batch_index = cell.packed.z;
	uint batch_output_first_vertex = cell.packed.w;

	vec3 world0 = CellPosition(segment, cell.height, 0u);
	vec3 world1 = CellPosition(segment, cell.height, 1u);
	vec3 world2 = CellPosition(segment, cell.height, 2u);
	vec3 world3 = CellPosition(segment, cell.height, 3u);

	vec2 uv0 = BaseUv(segment, rotation, 0u);
	vec2 uv1 = BaseUv(segment, rotation, 1u);
	vec2 uv2 = BaseUv(segment, rotation, 2u);
	vec2 uv3 = BaseUv(segment, rotation, 3u);

	vec2 lm0 = LightmapUv(segment, 0u);
	vec2 lm1 = LightmapUv(segment, 1u);
	vec2 lm2 = LightmapUv(segment, 2u);
	vec2 lm3 = LightmapUv(segment, 3u);

	EmitTriangle(batch_index, batch_output_first_vertex, texture_page, lightmap_page, world0, world1, world3, uv0, uv1, uv3, lm0, lm1, lm3);
	EmitTriangle(batch_index, batch_output_first_vertex, texture_page, lightmap_page, world3, world1, world2, uv3, uv1, uv2, lm3, lm1, lm2);
}
)glsl";

	GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
	glShaderSource(shader, 1, &compute_source, nullptr);
	glCompileShader(shader);

	GLint status = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE)
	{
		GLint length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
		int log_size = length > 1 ? length : 1;
		std::vector<char> log(log_size);
		GLsizei written = 0;
		glGetShaderInfoLog(shader, log_size, &written, log.data());
		mprintf((0, "Terrain compute emit shader compile failed, falling back to legacy terrain rendering:\n%s\n", log.data()));
		glDeleteShader(shader);
		SetTerrainComputeStatusFromLog("shader compile:", log.data());
		Terrain_compute_unavailable = true;
		return false;
	}

	Terrain_compute_program = glCreateProgram();
	glAttachShader(Terrain_compute_program, shader);
	glLinkProgram(Terrain_compute_program);
	glDeleteShader(shader);

	glGetProgramiv(Terrain_compute_program, GL_LINK_STATUS, &status);
	if (status == GL_FALSE)
	{
		GLint length = 0;
		glGetProgramiv(Terrain_compute_program, GL_INFO_LOG_LENGTH, &length);
		int log_size = length > 1 ? length : 1;
		std::vector<char> log(log_size);
		GLsizei written = 0;
		glGetProgramInfoLog(Terrain_compute_program, log_size, &written, log.data());
		mprintf((0, "Terrain compute emit shader link failed, falling back to legacy terrain rendering:\n%s\n", log.data()));
		glDeleteProgram(Terrain_compute_program);
		Terrain_compute_program = 0;
		SetTerrainComputeStatusFromLog("shader link:", log.data());
		Terrain_compute_unavailable = true;
		return false;
	}

	Terrain_compute_cell_count_uniform = glGetUniformLocation(Terrain_compute_program, "cell_count");
	Terrain_compute_row0_uniform = glGetUniformLocation(Terrain_compute_program, "terrain_row0");
	Terrain_compute_xstep_uniform = glGetUniformLocation(Terrain_compute_program, "terrain_x_step");
	Terrain_compute_zstep_uniform = glGetUniformLocation(Terrain_compute_program, "terrain_z_step");
	Terrain_compute_ystep_uniform = glGetUniformLocation(Terrain_compute_program, "terrain_y_step");

	glGenBuffers(1, &Terrain_compute_input_buffer);
	glGenBuffers(1, &Terrain_compute_vertex_buffer);
	glGenBuffers(1, &Terrain_compute_indirect_buffer);
	glGenVertexArrays(1, &Terrain_compute_vertex_array);
	ConfigureTerrainComputeVertexArray();

	Terrain_compute_ready = true;
	return true;
}

static bool TerrainComputeCanRender(bool from_automap, bool draw_lightmap)
{
#if (defined(EDITOR) || defined(NEWEDITOR))
	if (View_mode == EDITOR_MODE)
	{
		SetTerrainComputeStatus("fallback editor");
		return false;
	}
#endif

	if (from_automap)
	{
		SetTerrainComputeStatus("fallback automap");
		return false;
	}

	if (!draw_lightmap)
	{
		SetTerrainComputeStatus("fallback no lightmap");
		return false;
	}

	if (!UseHardware)
	{
		SetTerrainComputeStatus("fallback software renderer");
		return false;
	}

	if (OpenGLProfile != GLPROFILE_CORE)
	{
		SetTerrainComputeStatus("fallback compatibility GL");
		return false;
	}

	if (Terrain_renderer_mode != TERRAIN_RENDERER_COMPUTE)
	{
		SetTerrainComputeStatus("not selected");
		return false;
	}

	if (Viewer_object && Viewer_object->effect_info && (Viewer_object->effect_info->type_flags & EF_DEFORM))
	{
		SetTerrainComputeStatus("fallback deform effect");
		return false;
	}

	return true;
}

static bool TerrainComputeNoFarCullOrLod()
{
#if (defined(EDITOR) || defined(NEWEDITOR))
	if (View_mode == EDITOR_MODE)
		return false;
#endif

	if (Terrain_compute_unavailable)
		return false;

	if (Viewer_object && Viewer_object->effect_info && (Viewer_object->effect_info->type_flags & EF_DEFORM))
		return false;

	return Terrain_renderer_mode == TERRAIN_RENDERER_COMPUTE && UseHardware && OpenGLProfile == GLPROFILE_CORE;
}

static void MarkTerrainComputePoint(int seg)
{
	Terrain_rotate_list[seg] = TS_FrameCount;
	GlobalTransCount++;
}

static float TerrainGpuCellHeight(int seg)
{
	float y = Terrain_seg[seg].y;

	if (y < 0.0f)
		y = 0.0f;
	if (y > MAX_TERRAIN_HEIGHT)
		y = MAX_TERRAIN_HEIGHT;
	return y;
}

static void TerrainGpuAppendCell(int segment)
{
	int cx = segment % TERRAIN_WIDTH;
	int cz = segment / TERRAIN_WIDTH;
	if (cx >= TERRAIN_WIDTH - 1 || cz >= TERRAIN_DEPTH - 1)
		return;

	terrain_segment* tseg = &Terrain_seg[segment];
	if ((tseg->flags & TF_INVISIBLE) && !Show_invisible_terrain)
		return;

	terrain_tex_segment* texseg = &Terrain_tex_seg[tseg->texseg_index];
	TerrainGpuCellWork work = {};
	work.texture = texseg->tex_index;
	work.input.packed[0] = (uint32_t)segment;
	work.input.packed[1] = (uint32_t)texseg->rotation | ((uint32_t)tseg->lm_quad << 8);

	work.input.height[0] = TerrainGpuCellHeight(segment + TERRAIN_WIDTH);
	work.input.height[1] = TerrainGpuCellHeight(segment + TERRAIN_WIDTH + 1);
	work.input.height[2] = TerrainGpuCellHeight(segment + 1);
	work.input.height[3] = TerrainGpuCellHeight(segment);
	Terrain_compute_cell_work.push_back(work);
}

static bool TerrainGpuCellLess(const TerrainGpuCellWork& left, const TerrainGpuCellWork& right)
{
	if (left.texture != right.texture)
		return left.texture < right.texture;
	uint32_t left_lightmap_page = left.input.packed[1] >> 8;
	uint32_t right_lightmap_page = right.input.packed[1] >> 8;
	if (left_lightmap_page != right_lightmap_page)
		return left_lightmap_page < right_lightmap_page;
	return left.input.packed[0] < right.input.packed[0];
}

static void FinalizeTerrainComputeBatches()
{
	std::sort(Terrain_compute_cell_work.begin(), Terrain_compute_cell_work.end(), TerrainGpuCellLess);

	Terrain_compute_cell_inputs.resize(Terrain_compute_cell_work.size());
	Terrain_compute_batches.clear();

	for (size_t i = 0; i < Terrain_compute_cell_work.size(); i++)
	{
		const TerrainGpuCellWork& work = Terrain_compute_cell_work[i];
		Terrain_compute_cell_inputs[i] = work.input;

		if (Terrain_compute_batches.empty() ||
			Terrain_compute_batches.back().texture != work.texture)
		{
			TerrainGpuBatch batch = {};
			batch.texture = work.texture;
			batch.first_cell = (int)i;
			batch.cell_count = 0;
			batch.first_vertex = (int)(i * TERRAIN_COMPUTE_VERTS_PER_CELL);
			batch.vertex_count = 0;
			Terrain_compute_batches.push_back(batch);
		}

		TerrainGpuBatch& batch = Terrain_compute_batches.back();
		batch.cell_count++;
		batch.vertex_count += TERRAIN_COMPUTE_VERTS_PER_CELL;
		uint32_t texture_layer = (uint32_t)(Terrain_compute_batches.size() - 1);
		Terrain_compute_cell_inputs[i].packed[1] =
			(Terrain_compute_cell_inputs[i].packed[1] & 0x0000ffffu) | (texture_layer << 16);
		Terrain_compute_cell_inputs[i].packed[2] = texture_layer;
		Terrain_compute_cell_inputs[i].packed[3] = (uint32_t)batch.first_vertex;
	}

	GlobalTransCount = (int)(Terrain_compute_cell_inputs.size() * 4);
}

static void InvalidateTerrainComputeInputUpload()
{
	Terrain_compute_input_uploaded = false;
}

static void BuildTerrainComputeFullDrawWork()
{
	Terrain_compute_cell_work.clear();
	Terrain_compute_cell_inputs.clear();
	Terrain_compute_batches.clear();
	Terrain_compute_cell_work.reserve((TERRAIN_WIDTH - 1) * (TERRAIN_DEPTH - 1));

	for (int z = 0; z < TERRAIN_DEPTH - 1; z++)
	{
		for (int x = 0; x < TERRAIN_WIDTH - 1; x++)
			TerrainGpuAppendCell(x + (z * TERRAIN_WIDTH));
	}

	FinalizeTerrainComputeBatches();
	Terrain_compute_draw_work_ready = true;
	Terrain_compute_draw_work_checksum = Terrain_checksum;
	Terrain_compute_draw_work_show_invisible = Show_invisible_terrain;
	InvalidateTerrainComputeInputUpload();
}

static void EnsureTerrainComputeFullDrawWork()
{
	if (!Terrain_compute_draw_work_ready ||
		Terrain_compute_draw_work_checksum != Terrain_checksum ||
		Terrain_compute_draw_work_show_invisible != Show_invisible_terrain)
	{
		BuildTerrainComputeFullDrawWork();
	}
}

static void EnsureTerrainComputeVertexCapacity(size_t vertex_count)
{
	size_t vertex_bytes = vertex_count * sizeof(TerrainGpuVertexOutput);
	if (vertex_bytes > Terrain_compute_vertex_capacity)
	{
		Terrain_compute_vertex_capacity = vertex_bytes + (vertex_bytes / 2) + (64 * 1024);
		ConfigureTerrainComputeVertexArray();
	}

	glBindBuffer(GL_ARRAY_BUFFER, Terrain_compute_vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)Terrain_compute_vertex_capacity, nullptr, GL_STREAM_DRAW);
}

static void BindTerrainComputeInputBuffer()
{
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, Terrain_compute_input_buffer);
	if (!Terrain_compute_input_uploaded)
	{
		glBufferData(GL_SHADER_STORAGE_BUFFER,
			(GLsizeiptr)(Terrain_compute_cell_inputs.size() * sizeof(TerrainGpuCellInput)),
			Terrain_compute_cell_inputs.empty() ? nullptr : Terrain_compute_cell_inputs.data(), GL_STATIC_DRAW);
		Terrain_compute_input_uploaded = true;
	}
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, Terrain_compute_input_buffer);
}

static void PrepareTerrainComputeIndirectCommands()
{
	Terrain_compute_draw_commands.resize(Terrain_compute_batches.size());
	for (size_t i = 0; i < Terrain_compute_batches.size(); i++)
	{
		TerrainGpuBatch& batch = Terrain_compute_batches[i];
		Terrain_compute_draw_commands[i].count = 0;
		Terrain_compute_draw_commands[i].instance_count = 1;
		Terrain_compute_draw_commands[i].first = (uint32_t)batch.first_vertex;
		Terrain_compute_draw_commands[i].base_instance = 0;
	}

	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, Terrain_compute_indirect_buffer);
	glBufferData(GL_DRAW_INDIRECT_BUFFER,
		(GLsizeiptr)(Terrain_compute_draw_commands.size() * sizeof(TerrainGpuDrawCommand)),
		Terrain_compute_draw_commands.empty() ? nullptr : Terrain_compute_draw_commands.data(), GL_STREAM_DRAW);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, Terrain_compute_indirect_buffer);
}

static void SetTerrainComputeViewUniforms()
{
	vector viewer_eye;
	matrix viewer_orient;
	g3_GetViewPosition(&viewer_eye);
	g3_GetViewMatrix(&viewer_orient);

	vector x_step_source = { 1.0f, 0.0f, 0.0f };
	vector z_step_source = { 0.0f, 0.0f, 1.0f };
	vector y_step_source = { 0.0f, 1.0f, 0.0f };
	vector origin = { 0.0f, 0.0f, 0.0f };

	vector row0 = (origin - viewer_eye) * viewer_orient;
	vector x_step = x_step_source * viewer_orient;
	vector z_step = z_step_source * viewer_orient;
	vector y_step = y_step_source * viewer_orient;

	glUniform1ui(Terrain_compute_cell_count_uniform, (GLuint)Terrain_compute_cell_inputs.size());
	glUniform3f(Terrain_compute_row0_uniform, row0.x, row0.y, row0.z);
	glUniform3f(Terrain_compute_xstep_uniform, x_step.x, x_step.y, x_step.z);
	glUniform3f(Terrain_compute_zstep_uniform, z_step.x, z_step.y, z_step.z);
	glUniform3f(Terrain_compute_ystep_uniform, y_step.x, y_step.y, y_step.z);
}

static bool DisplayTerrainListCompute(int cellcount, bool from_automap, bool fog_enabled, bool scissor_to_window,
	int left, int top, int right, int bot, int render_width, int render_height)
{
	PERF_MARKER_SCOPE("DisplayTerrainListCompute");
	(void)cellcount;
	bool draw_lightmap = !StateLimited || UseMultitexture;

	if (!TerrainComputeCanRender(from_automap, draw_lightmap))
		return false;

	if (!CompileTerrainComputeProgram())
		return false;

	{
		PERF_MARKER_SCOPE("Compute.EnsureFullDrawWork");
		EnsureTerrainComputeFullDrawWork();
	}
	GlobalTransCount = (int)(Terrain_compute_cell_inputs.size() * 4);
	TotalDepth = 0;
	if (Terrain_compute_cell_inputs.empty())
	{
		SetTerrainComputeStatusActive((int)Terrain_compute_cell_inputs.size());
		return true;
	}
	{
		PERF_MARKER_SCOPE("Compute.EnsureLightmapArray");
		if (!EnsureTerrainComputeLightmapArray())
			return false;
	}
	{
		PERF_MARKER_SCOPE("Compute.EnsureBaseTextureArray");
		if (!EnsureTerrainComputeBaseTextureArray())
			return false;
	}

	{
		PERF_MARKER_SCOPE("Compute.EnsureVertexCapacity");
		EnsureTerrainComputeVertexCapacity(Terrain_compute_cell_inputs.size() * TERRAIN_COMPUTE_VERTS_PER_CELL);
	}

	glUseProgram(Terrain_compute_program);
	{
		PERF_MARKER_SCOPE("Compute.SetViewUniforms");
		SetTerrainComputeViewUniforms();
	}

	{
		PERF_MARKER_SCOPE("Compute.BindInputBuffer");
		BindTerrainComputeInputBuffer();
	}
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, Terrain_compute_vertex_buffer);
	{
		PERF_MARKER_SCOPE("Compute.PrepareIndirectCommands");
		PrepareTerrainComputeIndirectCommands();
	}

	GLuint groups = (GLuint)((Terrain_compute_cell_inputs.size() + 127) / 128);
	{
		PERF_MARKER_SCOPE("Compute.Dispatch");
		glDispatchCompute(groups, 1, 1);
	}
	{
		PERF_MARKER_SCOPE("Compute.MemoryBarrier");
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
	}
	rendTEMP_ClearShaderBinding();

	{
		PERF_MARKER_SCOPE("Compute.EnsureTerrainPipelines");
		EnsureTerrainPipelinesReady();
	}
	if (fog_enabled)
		rend_BindPipeline(Terrain_legacy_compute_fog_handle);
	else
		rend_BindPipeline(Terrain_legacy_compute_handle);
	SetTerrainComputeDynamicLightUniforms();

	rend_SetColorModel(CM_RGB);
	rend_SetTextureType(TT_LINEAR);
	rend_SetAlphaType(ATF_CONSTANT + ATF_TEXTURE);
	rend_SetLighting(LS_NONE);
	rend_SetWrapType(WT_WRAP);
	rend_ClearBoundTextures();
	{
		PERF_MARKER_SCOPE("Compute.BindTextureArrays");
		BindTerrainComputeTextureArrays();
	}

	glBindVertexArray(Terrain_compute_vertex_array);
	bool depth_clamp_enabled = rendTEMP_DepthClampEnabled();
	rendTEMP_SetDepthClamp(true);
	rendTEMP_ScissorState scissor_state = {};
	if (scissor_to_window)
	{
		rendTEMP_SaveScissorState(&scissor_state);
		rendTEMP_SetScissorRect(left, top, right, bot, render_width, render_height);
	}

	if (!Terrain_compute_batches.empty())
	{
		PERF_MARKER_SCOPE("Compute.MultiDrawIndirect");
		glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, (GLsizei)Terrain_compute_batches.size(),
			sizeof(TerrainGpuDrawCommand));
	}
	rend_SetPerPixelDynamicLighting(nullptr, 0, nullptr);
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

	rendTEMP_UnbindVertexBuffer();
	if (scissor_to_window)
		rendTEMP_RestoreScissorState(&scissor_state);
	rendTEMP_SetDepthClamp(depth_clamp_enabled);

	mprintf_at((2, 1, 0, "%5d cells", (int)Terrain_compute_cell_inputs.size()));
	mprintf_at((2, 2, 0, "%5d trans", GlobalTransCount));
	mprintf_at((2, 3, 0, "Tdepth=%5d", TotalDepth));
	SetTerrainComputeStatusActive((int)Terrain_compute_cell_inputs.size());
	return true;
}

void InitTerrainRenderSpeedups()
{
	// Figure out a table of values for rotated uv points
	for (int y = 0; y <= MAX_LOD_SIZE; y++)
	{
		for (int x = 0; x <= MAX_LOD_SIZE; x++)
		{
			TerrainUSpeedup[0][(y * LOD_ROW_SIZE) + x] = (float)x / (float)MAX_LOD_SIZE;
			TerrainVSpeedup[0][(y * LOD_ROW_SIZE) + x] = (float)y / (float)MAX_LOD_SIZE;
			TerrainUSpeedup[1][(y * LOD_ROW_SIZE) + x] = 1.0 - ((float)y / (float)MAX_LOD_SIZE);
			TerrainVSpeedup[1][(y * LOD_ROW_SIZE) + x] = (float)x / (float)MAX_LOD_SIZE;
			TerrainUSpeedup[2][(y * LOD_ROW_SIZE) + x] = 1.0 - ((float)x / (float)MAX_LOD_SIZE);
			TerrainVSpeedup[2][(y * LOD_ROW_SIZE) + x] = 1.0 - ((float)y / (float)MAX_LOD_SIZE);
			TerrainUSpeedup[3][(y * LOD_ROW_SIZE) + x] = ((float)y / (float)MAX_LOD_SIZE);
			TerrainVSpeedup[3][(y * LOD_ROW_SIZE) + x] = 1.0 - ((float)x / (float)MAX_LOD_SIZE);
		}
	}
}

//codes a point for visibility in the window passed to RenderTerrain()
ubyte CodeTerrainPoint(g3Point* p)
{
	ubyte cc = 0;
	if (p->p3_sx > Clip_scale_right)
		cc |= CC_OFF_RIGHT;
	if (p->p3_sx < Clip_scale_left)
		cc |= CC_OFF_LEFT;
	if (p->p3_sy > Clip_scale_bot)
		cc |= CC_OFF_BOT;
	if (p->p3_sy < Clip_scale_top)
		cc |= CC_OFF_TOP;
	return cc;
}

// Returns true if light can hit this segment/heighth bit
int IsTerrainDynamicChecked(int seg, int bit)
{
	if (seg < 0 || seg >= (TERRAIN_WIDTH * TERRAIN_DEPTH))
		return 1;
	if (bit >= 8)
		return 1;
	ubyte val = Terrain_dynamic_table[seg] & (1 << bit);
	if (val)
		return 1;
	return 0;
}

// Gets the dynamic light value for this position
float GetTerrainDynamicScalar(vector* pos, int seg)
{
	float cube_values[10];
	int y_increment = MAX_TERRAIN_HEIGHT / 8;
	int y_int = pos->y / y_increment;
	int x_int = pos->x / TERRAIN_SIZE;
	int z_int = pos->z / TERRAIN_SIZE;

	float x_norm = (pos->x / TERRAIN_SIZE) - x_int;
	float z_norm = (pos->z / TERRAIN_SIZE) - z_int;
	float y_norm = (pos->y / y_increment) - y_int;
	if (y_norm < 0)
	{
		y_norm = 0;
		y_int = 0;
	}
	if (y_norm > 1)
	{
		y_norm = 1.0;
		y_int = 7;
	}
	if (x_norm < 0 || x_norm>1 || z_norm < 0 || z_norm>1)
		return .5;
	float left_norm, right_norm, top_norm, bottom_norm, scalar;

	cube_values[0] = IsTerrainDynamicChecked(seg, y_int);
	cube_values[1] = IsTerrainDynamicChecked(seg + TERRAIN_WIDTH, y_int);
	cube_values[2] = IsTerrainDynamicChecked(seg + TERRAIN_WIDTH + 1, y_int);
	cube_values[3] = IsTerrainDynamicChecked(seg + 1, y_int);
	cube_values[4] = IsTerrainDynamicChecked(seg, y_int + 1);
	cube_values[5] = IsTerrainDynamicChecked(seg + TERRAIN_WIDTH, y_int + 1);
	cube_values[6] = IsTerrainDynamicChecked(seg + TERRAIN_WIDTH + 1, y_int + 1);
	cube_values[7] = IsTerrainDynamicChecked(seg + 1, y_int + 1);
	left_norm = ((1 - z_norm) * cube_values[0]) + (z_norm * cube_values[1]);
	right_norm = ((1 - z_norm) * cube_values[3]) + (z_norm * cube_values[2]);
	bottom_norm = ((1 - x_norm) * left_norm) + (x_norm * right_norm);
	left_norm = ((1 - z_norm) * cube_values[4]) + (z_norm * cube_values[5]);
	right_norm = ((1 - z_norm) * cube_values[7]) + (z_norm * cube_values[6]);
	top_norm = ((1 - x_norm) * left_norm) + (x_norm * right_norm);
	scalar = ((1 - y_norm) * bottom_norm) + (y_norm * top_norm);
	ASSERT(scalar >= 0 && scalar <= 1);
	return scalar;
}

// Takes a min,max vector and makes a surrounding cube from it
void MakePointsFromMinMax(vector* corners, vector* minp, vector* maxp);

struct obj_sort_item
{
	int	objnum;
	float	dist;
	int vis_effect;
};

//Compare function for room face sort
static int obj_sort_func(const obj_sort_item* a, const obj_sort_item* b)
{
	if (a->dist < b->dist)
		return -1;
	else if (a->dist > b->dist)
		return 1;
	else
		return 0;
}

// Returns true if the object is outside of our terrain portal
int ObjectOutOfPortal(object* obj)
{
	g3Point pnt1, pnt2;
	ubyte anded = 0xff;
	g3_RotatePoint(&pnt1, &obj->min_xyz);
	if (pnt1.p3_codes & CC_BEHIND)
		return 0;
	g3_ProjectPoint(&pnt1);
	anded &= CodeTerrainPoint(&pnt1);
	g3_RotatePoint(&pnt2, &obj->max_xyz);
	if (pnt2.p3_codes & CC_BEHIND)
		return 0;
	g3_ProjectPoint(&pnt2);
	anded &= CodeTerrainPoint(&pnt2);
	if (anded)
		return 1;
	return 0;
}

obj_sort_item objs_to_render[MAX_OBJECTS + MAX_VIS_EFFECTS];
obj_sort_item rooms_to_render[MAX_ROOMS];

// Checks to see if this object can even be seen from our current viewpoint
// By shooting rays to it
// Returns true if any of the rays hit
int ShootRaysToObject(object* obj)
{
	vector corners[8];
	if (obj->type == OBJ_ROOM)
	{
		room* rp = &Rooms[obj->id];
		MakePointsFromMinMax(corners, &rp->min_xyz, &rp->max_xyz);
	}
	else
	{
		MakePointsFromMinMax(corners, &obj->min_xyz, &obj->max_xyz);
	}
	fvi_info hit_info;
	fvi_query fq;
	fq.p0 = &Viewer_object->pos;
	fq.startroom = Viewer_object->roomnum;
	fq.rad = 0.0f;
	fq.flags = FQ_NO_RELINK | FQ_EXTERNAL_ROOMS_AS_SPHERE | FQ_IGNORE_EXTERNAL_ROOMS;
	fq.thisobjnum = Viewer_object - Objects;
	fq.ignore_obj_list = NULL;

	for (int i = 0; i < 8; i++)
	{
		fq.p1 = &corners[i];
		int fate = fvi_FindIntersection(&fq, &hit_info);
		/*g3Point pnt1,pnt2;
		vector fpnt = *fq.p0 + 3.0 * Viewer_object->orient.fvec;
		g3_RotatePoint (&pnt1,&fpnt);
		g3_RotatePoint (&pnt2,&hit_info.hit_pnt);
		g3_DrawLine ((GR_RGB(255,255,255)),&pnt1,&pnt2);*/
		if (fate == HIT_NONE)
			return 1;
	}
	return 0;
}

// Returns true if the external room is in the view cone
// Else returns false
bool ExternalRoomVisible(room* rp, vector* center, float* zdist)
{
	ASSERT(rp->flags & RF_EXTERNAL);
	g3Point pnt;
	ubyte ccode;

	vector corners[8];
	g3_RotatePoint(&pnt, center);
	*zdist = pnt.p3_z;
	MakePointsFromMinMax(corners, &rp->min_xyz, &rp->max_xyz);
	ubyte andbyte = 0xff;
	for (int t = 0; t < 8; t++)
	{
		g3_RotatePoint(&pnt, &corners[t]);
		ccode = g3_CodePoint(&pnt);
		if (!ccode)
			return true;
		andbyte &= ccode;
	}
	if (andbyte)
		return false;
	return true;
}

// Render all the rooms out on the terrain for this frame
void RenderTerrainRooms()
{
	PERF_MARKER_SCOPE("RenderTerrainRooms");
	object* obj;
#ifdef EDITOR
	if (!Terrain_render_ext_room_objs)
		return;
#endif

	int room_count = 0;
	float zdist;
	int use_occlusion = 0;
	int src_occlusion_index;
	int i;
	if (Terrain_from_mine)
		return;
	if ((Terrain_checksum + 1) == Terrain_occlusion_checksum)
	{
		use_occlusion = 1;
		int oz = (Viewer_object->pos.z / TERRAIN_SIZE) / OCCLUSION_SIZE;
		int ox = (Viewer_object->pos.x / TERRAIN_SIZE) / OCCLUSION_SIZE;
		if (oz < 0 || oz >= OCCLUSION_SIZE || ox < 0 || ox >= OCCLUSION_SIZE)
			use_occlusion = 0;
		src_occlusion_index = oz * OCCLUSION_SIZE + ox;
	}
	{
		PERF_MARKER_SCOPE("TerrainRooms.Scan");
		for (i = 0; i <= Highest_object_index; i++)
		{
			obj = &Objects[i];
			if (obj->type != OBJ_ROOM)
				continue;

			if (obj->flags & OF_DEAD)
				continue;
			if (obj->render_type == RT_NONE)
				continue;
			if (!OBJECT_OUTSIDE(obj))
				continue;
			float size = obj->size;

			if (use_occlusion)
			{
				int y1 = (obj->pos.z / TERRAIN_SIZE) / OCCLUSION_SIZE;
				int x1 = (obj->pos.x / TERRAIN_SIZE) / OCCLUSION_SIZE;
				int dest_occlusion_index = (y1 * OCCLUSION_SIZE);
				dest_occlusion_index += x1;
				int occ_byte = dest_occlusion_index / 8;
				int occ_bit = dest_occlusion_index % 8;
				if (obj->pos.y < MAX_TERRAIN_HEIGHT)
				{
					if (!(Terrain_occlusion_map[src_occlusion_index][occ_byte] & (1 << occ_bit)))
						continue;
				}
			}
			if (!ExternalRoomVisible(&Rooms[obj->id], &obj->pos, &zdist))
				continue;
			if (!IsPointVisible(&obj->pos, size, &zdist))
				continue;

			if (Check_terrain_portal && ObjectOutOfPortal(obj))
				continue;
			/*if (!Terrain_from_mine)
			{
				if (!ShootRaysToObject (obj))
					continue;
			}*/
			rooms_to_render[room_count].vis_effect = 0;
			rooms_to_render[room_count].objnum = obj - Objects;
			rooms_to_render[room_count].dist = zdist;
			room_count++;
		}
	}
	// Sort and draw rooms
	{
		PERF_MARKER_SCOPE("TerrainRooms.Sort");
		qsort(rooms_to_render, room_count, sizeof(*rooms_to_render), (int (*)(const void*, const void*))obj_sort_func);
	}
	{
		PERF_MARKER_SCOPE("TerrainRooms.RenderMines");
		for (i = room_count - 1; i >= 0; i--)
		{
			int objnum = rooms_to_render[i].objnum;
			object* obj = &Objects[objnum];
#ifndef NEWEDITOR
			RenderMine(obj->id, 0, 1);
#else
			RenderMine(obj->id, 0, 1, 1, 0, 0, NULL);
#endif
			// Draw a surrounding sphere
#if (defined(_DEBUG) && !defined(NEWEDITOR))
			if (Game_show_sphere == 2)
				DrawDebugInfo(obj);
#endif
		}
	}
}

// Renders every visible terrain object
void RenderAllTerrainObjects()
{
	PERF_MARKER_SCOPE("RenderAllTerrainObjects");
	object* obj;
	int snows[500];
	int num_snows = 0;
	int obj_count = 0;
	float zdist;
	int use_occlusion = 0;
	int src_occlusion_index;
	int i;
	if ((Terrain_checksum + 1) == Terrain_occlusion_checksum)
	{
		use_occlusion = 1;
		int oz = (Viewer_object->pos.z / TERRAIN_SIZE) / OCCLUSION_SIZE;
		int ox = (Viewer_object->pos.x / TERRAIN_SIZE) / OCCLUSION_SIZE;
		if (oz < 0 || oz >= OCCLUSION_SIZE || ox < 0 || ox >= OCCLUSION_SIZE)
			use_occlusion = 0;
		src_occlusion_index = oz * OCCLUSION_SIZE + ox;
	}
	{
		PERF_MARKER_SCOPE("TerrainObjects.ScanObjects");
		for (i = 0; i <= Highest_object_index; i++)
		{
			obj = &Objects[i];
			if (obj == Viewer_object)
				continue;
			// Don't draw piggybacked objects
			if (Viewer_object->type == OBJ_OBSERVER && i == Players[Viewer_object->id].piggy_objnum)
				continue;
			if (obj->type == OBJ_ROOM)
				continue;

			if (obj->type == OBJ_NONE)
				continue;
			if (obj->flags & OF_DEAD)
				continue;
			if (obj->render_type == RT_NONE)
				continue;
			if (!OBJECT_OUTSIDE(obj))
				continue;
			float size = obj->size;

			// Special case weapons with streamers
			if (obj->type == OBJ_WEAPON && (Weapons[obj->id].flags & WF_STREAMER))
				size = Weapons[obj->id].phys_info.velocity.z;
			if (use_occlusion)
			{
				int y1 = (obj->pos.z / TERRAIN_SIZE) / OCCLUSION_SIZE;
				int x1 = (obj->pos.x / TERRAIN_SIZE) / OCCLUSION_SIZE;
				int dest_occlusion_index = (y1 * OCCLUSION_SIZE);
				dest_occlusion_index += x1;
				int occ_byte = dest_occlusion_index / 8;
				int occ_bit = dest_occlusion_index % 8;
				if (obj->pos.y + obj->size < MAX_TERRAIN_HEIGHT)
				{
					if (!(Terrain_occlusion_map[src_occlusion_index][occ_byte] & (1 << occ_bit)))
						continue;
				}
			}
			if (obj->type == OBJ_WEAPON && Weapons[obj->id].flags & WF_ELECTRICAL)
			{
				// Automatically render all electrical objects
				zdist = 0;
			}
			else
			{
				if (!IsPointVisible(&obj->pos, size, &zdist))
					continue;

				if (Check_terrain_portal && ObjectOutOfPortal(obj))
					continue;
			}

			if (Num_postrenders < MAX_POSTRENDERS)
			{
				Postrender_list[Num_postrenders].type = PRT_OBJECT;
				Postrender_list[Num_postrenders].z = zdist;
				Postrender_list[Num_postrenders++].objnum = obj - Objects;
			}
		}
	}
#ifndef NEWEDITOR
	{
		PERF_MARKER_SCOPE("TerrainObjects.ScanVisEffects");
		for (i = 0; i <= Highest_vis_effect_index; i++)
		{
			vis_effect* vis = &VisEffects[i];

			if (vis->type == VIS_NONE)
				continue;
			if (vis->flags & VF_DEAD)
				continue;
			if (!ROOMNUM_OUTSIDE(vis->roomnum))
				continue;
			// Special case snow
			if (vis->id == SNOWFLAKE_INDEX)
			{
				snows[num_snows] = vis - VisEffects;
				num_snows++;
			}
			else
			{
				if ((vis->flags & VF_WINDSHIELD_EFFECT) || IsPointVisible(&vis->pos, vis->size, &zdist))
				{
					if (vis->flags & VF_WINDSHIELD_EFFECT)
						zdist = 0;

					if (Num_postrenders < MAX_POSTRENDERS)
					{
						Postrender_list[Num_postrenders].type = PRT_VISEFFECT;
						Postrender_list[Num_postrenders].z = zdist;
						Postrender_list[Num_postrenders++].objnum = vis - VisEffects;
					}
				}
			}
		}
	}
#endif

	// Sort objects
	{
		PERF_MARKER_SCOPE("TerrainObjects.Sort");
		qsort(objs_to_render, obj_count, sizeof(*objs_to_render), (int (*)(const void*, const void*))obj_sort_func);
	}
	//Render the objects
	{
		PERF_MARKER_SCOPE("TerrainObjects.RenderObjects");
		for (i = obj_count - 1; i >= 0; i--)
		{
			int vis_effect = objs_to_render[i].vis_effect;
			int objnum = objs_to_render[i].objnum;
			if (vis_effect)
				DrawVisEffect(&VisEffects[objnum]);
			else
				RenderObject(&Objects[objnum]);
		}
	}
	// Render snows
	rend_SetZBufferWriteMask(0);
	{
		PERF_MARKER_SCOPE("TerrainObjects.RenderSnow");
		for (i = 0; i < num_snows; i++)
			DrawVisEffect(&VisEffects[snows[i]]);
	}
	rend_SetZBufferWriteMask(1);
	rend_SetZBufferState(1);
	rend_SetWrapType(WT_WRAP);
}

#define FOG_LAYER_HEIGHT	(TERRAIN_HEIGHT_INCREMENT * 30.5f)
// Draws a flat fog layer
void DrawFogLayer()
{
	vector worldvec[4];
	g3Point pnt[4], * pntlist[6];
	rend_SetFlatColor(GR_RGB(132, 132, 255));
	rend_SetAlphaValue(64);
	rend_SetAlphaType(AT_CONSTANT);
	rend_SetTextureType(TT_FLAT);
	rend_SetLighting(LS_NONE);
	worldvec[0].x = 0;
	worldvec[0].y = FOG_LAYER_HEIGHT;
	worldvec[0].z = 0;
	worldvec[1].x = 0;
	worldvec[1].y = FOG_LAYER_HEIGHT;
	worldvec[1].z = TERRAIN_DEPTH * TERRAIN_SIZE;
	worldvec[2].x = TERRAIN_WIDTH * TERRAIN_SIZE;
	worldvec[2].y = FOG_LAYER_HEIGHT;
	worldvec[2].z = TERRAIN_DEPTH * TERRAIN_SIZE;
	worldvec[3].x = TERRAIN_WIDTH * TERRAIN_SIZE;
	worldvec[3].y = FOG_LAYER_HEIGHT;
	worldvec[3].z = 0;
	for (int i = 0; i < 4; i++)
	{
		g3_RotatePoint(&pnt[i], &worldvec[i]);
		pntlist[i] = &pnt[i];
	}
	g3_DrawPoly(4, pntlist, 0);
}

#define CLOUD_LAYER_HEIGHT	(TERRAIN_HEIGHT_INCREMENT * 320.0f)
// Draws a flat fog layer
void DrawCloudLayer()
{
	vector worldvec[4];
	g3Point pnt[4], * pntlist[6];
	rend_SetFlatColor(GR_RGB(192, 192, 255));
	rend_SetAlphaValue(32);
	rend_SetAlphaType(AT_CONSTANT);
	rend_SetTextureType(TT_FLAT);
	rend_SetLighting(LS_NONE);
	worldvec[0].x = -(TERRAIN_SIZE * 100);
	worldvec[0].y = CLOUD_LAYER_HEIGHT;
	worldvec[0].z = (TERRAIN_SIZE * (100 + TERRAIN_DEPTH));

	worldvec[1].x = -(TERRAIN_SIZE * 100);
	worldvec[1].y = CLOUD_LAYER_HEIGHT;
	worldvec[1].z = -(TERRAIN_SIZE * (100));

	worldvec[2].x = (TERRAIN_SIZE * (100 + TERRAIN_WIDTH));
	worldvec[2].y = CLOUD_LAYER_HEIGHT;
	worldvec[2].z = -(TERRAIN_SIZE * (100));

	worldvec[3].x = (TERRAIN_SIZE * (100 + TERRAIN_WIDTH));
	worldvec[3].y = CLOUD_LAYER_HEIGHT;
	worldvec[3].z = (TERRAIN_SIZE * (100 + TERRAIN_DEPTH));
	for (int i = 0; i < 4; i++)
	{
		g3_RotatePoint(&pnt[i], &worldvec[i]);
		pntlist[i] = &pnt[i];
	}
	g3_DrawPoly(4, pntlist, 0);
}

//left,top,right,bot are optional parameters.  Omiting them (or setting them to -1) will
//render to the whole screen.  Passing valid values will only render tiles visible in the
//specified window (though it won't clip those tiles to the window)
void RenderTerrain(ubyte from_mine, int left, int top, int right, int bot)
{
	PERF_MARKER_SCOPE(from_mine ? "RenderTerrain.FromMine" : "RenderTerrain.Main");
	static int first = 1;
	if (first)
	{
		PERF_MARKER_SCOPE("Terrain.InitSpeedups");
		InitTerrainRenderSpeedups();
		first = 0;
	}

	//Get the size of the current render window
	int render_width, render_height;
	rend_GetProjectionParameters(&render_width, &render_height);
	float w2 = ((float)render_width - 1) / 2.0f;
	float h2 = ((float)render_height - 1) / 2.0f;

	//Set up vars for (psuedo-)clipping window
	bool valid_render_window = left >= 0;
	if (left < 0)
	{
		Check_terrain_portal = 0;
		left = 0;
	}
	else
	{
		int w = right - left;
		int h = bot - top;

		// If the portal takes up more than 50% the screen space then we don't check against it
		float threshold = (render_width * render_height) * 0.5f;
		if (w * h > threshold)
			Check_terrain_portal = 0;
		else
			Check_terrain_portal = 1;
	}

	if (top < 0)
	{
		top = 0;
	}
	if ((right == -1) || right > render_width)
		right = render_width - 1;
	if ((bot == -1) || bot > render_height)
		bot = render_height - 1;
	Clip_scale_left = left;
	Clip_scale_right = right;
	Clip_scale_top = top;
	Clip_scale_bot = bot;
	if (!Terrain_sky.textured)
		rend_FillRect(Terrain_sky.sky_color, left, top, right + 1, bot + 1);

	rend_SetFlatColor(Terrain_sky.sky_color);
	View_mode = GetFunctionMode();
	Terrain_renderer_mode = (UseHardware && OpenGLProfile == GLPROFILE_CORE) ? TERRAIN_RENDERER_COMPUTE : TERRAIN_RENDERER_LEGACY;

	// Set this so we don't do reentrant rendering between terrain/mine
	Terrain_from_mine = from_mine;
	bool use_compute_no_far_lod = TerrainComputeNoFarCullOrLod();

	const float kComputeTerrainFogDistance = 200.0f * TERRAIN_SIZE;
#ifndef NEWEDITOR
	const float kDefaultTerrainFogDistance = Detail_settings.Terrain_render_distance;
#else
	const float kDefaultTerrainFogDistance = 1200.0f;
#endif
	const float kTerrainFogDistance = use_compute_no_far_lod ? kComputeTerrainFogDistance : kDefaultTerrainFogDistance;
	const float terrain_fog_z = kTerrainFogDistance * Matrix_scale.z;
	const float terrain_search_z = use_compute_no_far_lod ? 60000.0f : terrain_fog_z;
	VisibleTerrainZ = terrain_search_z;
	Far_fog_border = terrain_fog_z;

	// Set up our z wall
	g3_SetFarClipZ(VisibleTerrainZ);

	//Get the viewer position & orientation
	vector viewer_eye;
	matrix viewer_orient;
	g3_GetViewPosition(&viewer_eye);
	g3_GetViewMatrix(&viewer_orient);

	// Get all of the cells visible to us
	int nt = 0;
	if (!use_compute_no_far_lod)
	{
		PERF_MARKER_SCOPE("Terrain.GetVisibleTerrain");
		nt = GetVisibleTerrain(&viewer_eye, &viewer_orient);
	}
	VisibleTerrainZ = terrain_fog_z;
	Far_fog_border = terrain_fog_z;

	// Set this to really far away so our sky can render
	g3_SetFarClipZ(60000);
	rend_SetFogState(0);

	// Draw the sky
	{
		PERF_MARKER_SCOPE("Terrain.DrawSky");
		DrawSky(&viewer_eye, &viewer_orient);
	}

	//// Set up our z wall
	rend_SetZBufferState(1);
	rend_SetZBufferWriteMask(1);
	const float terrain_draw_far_clip = use_compute_no_far_lod ? 60000.0f : VisibleTerrainZ;
	if (use_compute_no_far_lod)
		g3_SetFarClipZ(60000);
	else
		g3_SetFarClipZ(VisibleTerrainZ);

#ifndef NEWEDITOR
	if ((Terrain_sky.flags & TF_FOG))
	{
		rend_SetZValues(0, terrain_draw_far_clip);
		rend_SetFogState(1);
		rend_SetFogBorders(VisibleTerrainZ * Terrain_sky.fog_scalar, Far_fog_border);
		rend_SetFogColor(Terrain_sky.fog_color);
	}
	else
#endif
	{
		rend_SetZValues(0, 5000);
	}

	if (Terrain_renderer_mode == TERRAIN_RENDERER_COMPUTE)
	{
		PERF_MARKER_SCOPE("Terrain.Surface.Compute");
		if (use_compute_no_far_lod)
		{
			if (!DisplayTerrainListCompute(0, false, (Terrain_sky.flags & TF_FOG) != 0,
				from_mine && valid_render_window, left, top, right, bot, render_width, render_height))
			{
				PERF_MARKER_SCOPE("Terrain.ComputeFallbackLegacy");
				int saved_terrain_lod_engine_off = Terrain_LOD_engine_off;
				Terrain_LOD_engine_off = 1;
				VisibleTerrainZ = terrain_search_z;
				g3_SetFarClipZ(VisibleTerrainZ);
				{
					PERF_MARKER_SCOPE("Terrain.Fallback.GetVisibleTerrain");
					nt = GetVisibleTerrain(&viewer_eye, &viewer_orient);
				}
				Terrain_LOD_engine_off = saved_terrain_lod_engine_off;
				VisibleTerrainZ = terrain_fog_z;
				g3_SetFarClipZ(terrain_draw_far_clip);
				if (nt > 0)
					DisplayTerrainList(nt);
			}
		}
		else if (nt <= 0)
		{
			SetTerrainComputeStatus("ACTIVE 0c 0t 0b");
		}
		else if (!DisplayTerrainListCompute(nt, false, (Terrain_sky.flags & TF_FOG) != 0,
			from_mine && valid_render_window, left, top, right, bot, render_width, render_height))
		{
			DisplayTerrainList(nt);
		}
	}
	else
	{
		// And display!
		if (nt > 0)
		{
			PERF_MARKER_SCOPE("Terrain.Surface.Legacy");
			DisplayTerrainList(nt);
		}
	}

	if (use_compute_no_far_lod)
		g3_SetFarClipZ(VisibleTerrainZ);

	// Draw rooms
	{
		PERF_MARKER_SCOPE("Terrain.RenderRooms");
		RenderTerrainRooms();
	}

	// Show objects
	{
		PERF_MARKER_SCOPE("Terrain.RenderObjects");
		RenderAllTerrainObjects();
	}

	mprintf_at((2, 5, 0, "Objs Drawn=%5d", Terrain_objects_drawn));
	Last_terrain_render_time = Gametime;
}

// Draws a segment of lightning that is always facing you
// Vectors are in world coords
void DrawLightningSegment(vector* from, vector* to)
{
	vector src_vecs[2], world_vecs[6];
	g3Point rot_src_pnts[2], world_points[6], * pntlist[6];
	static float alphas[] = { 0.3f,1.0f,0.3f,0.3f,1.0f,0.3f };
	src_vecs[0] = *from;
	src_vecs[1] = *to;

	g3_RotatePoint(&rot_src_pnts[0], &src_vecs[0]);
	g3_RotatePoint(&rot_src_pnts[1], &src_vecs[1]);
	if (rot_src_pnts[0].p3_codes & rot_src_pnts[1].p3_codes)
		return;		// Don't draw because both points are off screen
	vector rvec = Viewer_object->orient.rvec * 10;

	// Put all points so that they face the viewer
	world_vecs[0] = src_vecs[0] - rvec;
	world_vecs[1] = src_vecs[0];
	world_vecs[2] = src_vecs[0] + rvec;
	world_vecs[3] = src_vecs[1] + rvec;
	world_vecs[4] = src_vecs[1];
	world_vecs[5] = src_vecs[1] - rvec;
	for (int i = 0; i < 6; i++)
	{
		g3_RotatePoint(&world_points[i], &world_vecs[i]);
		world_points[i].p3_flags |= PF_RGBA;
		world_points[i].p3_r = .2f;
		world_points[i].p3_g = .4f;
		world_points[i].p3_b = 1.0f;
		world_points[i].p3_a = alphas[i];
		pntlist[i] = &world_points[i];
	}
	rend_SetTextureType(TT_FLAT);
	rend_SetLighting(LS_GOURAUD);
	rend_SetAlphaType(AT_SATURATE_VERTEX);
	rend_SetColorModel(CM_RGB);
	g3_ProjectPoint(&world_points[1]);
	g3_ProjectPoint(&world_points[4]);
	rend_DrawSpecialLine(&world_points[1], &world_points[4]);
	g3_DrawPoly(6, pntlist, 0);
}

#define PUSH_LIGHTNING_TREE(f,l,sp) {froms[si]=f; stack_level[si]=l; splits[si]=sp; si++;}
#define POP_LIGHTNING_TREE()	{si--; cur_from=froms[si]; level=stack_level[si]; cur_splits=splits[si];}
// Draws an entire strip of lightning
void DrawLightning(void)
{
	static PSRand lightning_rand;
	angvec player_angs;
	matrix mat;
	vector froms[50];
	int si = 0, level;
	int stack_level[50];
	int splits[50];
	int cur_splits = 0;
	int new_heading;
	float scalar;
	scalar = ((lightning_rand() % 1000) - 500) / 500.0;
	scalar *= 15000;
	vm_ExtractAnglesFromMatrix(&player_angs, &Viewer_object->orient);
	new_heading = (player_angs.h + (int)scalar) % 65536;
	vm_AnglesToMatrix(&mat, 0, new_heading, 0);
	// Put the starting point way up in the air
	float ylimit = (-(Viewer_object->pos.y * 2)) + (lightning_rand() % 400);
	vector cur_from = Viewer_object->pos + (mat.fvec * 4000);
	vector new_vec;
	cur_from.y += 800.0f;
	cur_from.y += (lightning_rand() % 100);
	// Set some states

	rend_SetAlphaType(AT_SATURATE_TEXTURE);
	rend_SetAlphaValue(.5 * 255);
	rend_SetLighting(LS_NONE);
	int bm_handle;

	// See if we should drawn an origin bitmap
	if (lightning_rand() % 3)
	{
		// Pick an origin bitmap
		if (lightning_rand() % 2)
			bm_handle = Fireballs[LIGHTNING_ORIGIN_INDEXA].bm_handle;
		else
			bm_handle = Fireballs[LIGHTNING_ORIGIN_INDEXB].bm_handle;
		//Draw the origin bitmap
		int size = 300 + (lightning_rand() % 200);
		g3_DrawRotatedBitmap(&cur_from, 0, size, (size * bm_h(bm_handle, 0)) / bm_w(bm_handle, 0), bm_handle);
	}
	PUSH_LIGHTNING_TREE(cur_from, 0, 0)
		while (si > 0)
		{
			POP_LIGHTNING_TREE()
				ASSERT(level < 50);
			float x_adjust = ((lightning_rand() % 200) - 100) / 100.0;
			float y_adjust = .3 + ((lightning_rand() % 100) / 100.0);

			new_vec = cur_from;
			new_vec += x_adjust * (mat.rvec * 70);
			new_vec -= y_adjust * (mat.uvec * 100);
			DrawLightningSegment(&cur_from, &new_vec);
			if (cur_from.y < ylimit) // We're close to the ground, so just bail!
				continue;

			if ((lightning_rand() % ((level * level * 20) + 8)) == 0 && cur_splits < 2)
			{
				// Make this branch split
				PUSH_LIGHTNING_TREE(new_vec, level, cur_splits + 1)
					PUSH_LIGHTNING_TREE(new_vec, level, cur_splits + 1)
			}
			else
			{
				PUSH_LIGHTNING_TREE(new_vec, level, cur_splits)
			}
		}
}

// Draws a lightning sky
void DrawLightningSky(void)
{
	int t, k, tw;
	float r, g, b;

	g3Point pnt[4], * pntlist[6];

	rend_SetTextureType(TT_FLAT);
	rend_SetColorModel(CM_RGB);
	rend_SetLighting(LS_GOURAUD);
	rend_SetAlphaType(AT_SATURATE_VERTEX);

	// figure out colors for sky
	r = .8f;
	g = .8f;
	b = 1.0;
	// Draw top part
	for (t = 0; t < MAX_HORIZON_PIECES; t++)
	{
		tw = (t + 1) % MAX_HORIZON_PIECES;
		pnt[0].p3_vec = Temp_sky_vectors[t][0];
		pnt[0].p3_vecPreRot = Temp_sky_vectors_unrotated[t][0];
		pnt[1].p3_vec = Temp_sky_vectors[tw][1];
		pnt[1].p3_vecPreRot = Temp_sky_vectors_unrotated[tw][1];
		pnt[2].p3_vec = Temp_sky_vectors[t][1];
		pnt[2].p3_vecPreRot = Temp_sky_vectors_unrotated[t][1];

		for (k = 0; k < 3; k++)
		{
			g3Point* p = &pnt[k];
			p->p3_flags = PF_RGBA | PF_ORIGPOINT;
			g3_CodePoint(p);
			p->p3_a = .3f;
			p->p3_r = r;
			p->p3_g = g;
			p->p3_b = b;
		}
		pntlist[0] = &pnt[0];
		pntlist[1] = &pnt[1];
		pntlist[2] = &pnt[2];
		g3_DrawPoly(3, pntlist, 0);
	}
	// Draw bottom part
	for (int i = 1; i < 5; i++)
	{
		for (t = 0; t < MAX_HORIZON_PIECES; t++)
		{
			tw = (t + 1) % MAX_HORIZON_PIECES;
			pnt[0].p3_vec = Temp_sky_vectors[t][i];
			pnt[0].p3_vecPreRot = Temp_sky_vectors_unrotated[t][i];
			pnt[1].p3_vec = Temp_sky_vectors[tw][i];
			pnt[1].p3_vecPreRot = Temp_sky_vectors_unrotated[tw][i];
			pnt[2].p3_vec = Temp_sky_vectors[tw][i + 1];
			pnt[2].p3_vecPreRot = Temp_sky_vectors_unrotated[tw][i + 1];
			pnt[3].p3_vec = Temp_sky_vectors[t][i + 1];
			pnt[3].p3_vecPreRot = Temp_sky_vectors_unrotated[t][i + 1];

			for (k = 0; k < 4; k++)
			{
				g3Point* p = &pnt[k];
				p->p3_flags = PF_RGBA | PF_ORIGPOINT;
				g3_CodePoint(p);
				p->p3_a = .3f;
				p->p3_r = r;
				p->p3_g = g;
				p->p3_b = b;
				pntlist[k] = p;

			}
			g3_DrawPoly(4, pntlist, 0);
		}
	}
}

// Draws the gouraud sky
void DrawGouraudSky(void)
{
	int t, k, tw;
	float sr, sg, sb, hr, hg, hb;

	g3Point pnt[4], * pntlist[6];
	g3UVL	uvls[10];
#ifndef MACINTOSH
	if (Terrain_sky.sky_color == Terrain_sky.horizon_color)
		return;	// No sense in drawing anything
#endif
	rend_SetTextureType(TT_FLAT);
	rend_SetColorModel(CM_RGB);
	rend_SetAlphaType(AT_ALWAYS);
	// figure out colors for sky
	{
		sr = (float)GR_COLOR_RED(Terrain_sky.sky_color) / 255.0;
		sg = (float)GR_COLOR_GREEN(Terrain_sky.sky_color) / 255.0;
		sb = (float)GR_COLOR_BLUE(Terrain_sky.sky_color) / 255.0;
		hr = (float)GR_COLOR_RED(Terrain_sky.horizon_color) / 255.0;
		hg = (float)GR_COLOR_GREEN(Terrain_sky.horizon_color) / 255.0;
		hb = (float)GR_COLOR_BLUE(Terrain_sky.horizon_color) / 255.0;
	}

	for (t = 0; t < MAX_HORIZON_PIECES; t++)
	{
		tw = (t + 1) % MAX_HORIZON_PIECES;
		pnt[0].p3_vec = Temp_sky_vectors[t][4];
		pnt[0].p3_vecPreRot = Temp_sky_vectors_unrotated[t][4];
		uvls[0].r = sr;
		uvls[0].g = sg;
		uvls[0].b = sb;

		pnt[1].p3_vec = Temp_sky_vectors[tw][4];
		pnt[1].p3_vecPreRot = Temp_sky_vectors_unrotated[tw][4];
		uvls[1].r = sr;
		uvls[1].g = sg;
		uvls[1].b = sb;

		pnt[2].p3_vec = Temp_sky_vectors[tw][5];
		pnt[2].p3_vecPreRot = Temp_sky_vectors_unrotated[tw][5];
		uvls[2].r = hr;
		uvls[2].g = hg;
		uvls[2].b = hb;

		pnt[3].p3_vec = Temp_sky_vectors[t][5];
		pnt[3].p3_vecPreRot = Temp_sky_vectors_unrotated[t][5];
		uvls[3].r = hr;
		uvls[3].g = hg;
		uvls[3].b = hb;

		for (k = 0; k < 4; k++)
		{
			g3Point* p = &pnt[k];
			p->p3_flags = PF_RGBA | PF_ORIGPOINT;
			g3_CodePoint(p);
			pntlist[k] = p;
			p->p3_uvl = uvls[k];

		}
		g3_DrawPoly(4, pntlist, 0);
	}
}

// Draws the sky textures
void DrawTexturedSky(void)
{
	int t, k, tw;
	float sr, sg, sb, hr, hg, hb;

	// Change terrain sky if needed
	int dome_bm = GetTextureBitmap(Terrain_sky.dome_texture, 0);

	g3Point pnt[6], * pntlist[6];
	g3UVL	uvls[10];

	rend_SetWrapType(WT_WRAP);
	rend_SetTextureType(TT_PERSPECTIVE);
	rend_SetColorModel(CM_MONO);
	rend_SetAlphaType(ATF_TEXTURE);
	g3_SetTriangulationTest(1);

	// Draw top part
	for (t = 0; t < MAX_HORIZON_PIECES; t++)
	{
		tw = (t + 1) % MAX_HORIZON_PIECES;
		pnt[0].p3_vec = Temp_sky_vectors[t][0];
		pnt[0].p3_vecPreRot = Temp_sky_vectors_unrotated[t][0];
		uvls[0].u = Terrain_sky.horizon_u[t][0];
		uvls[0].v = Terrain_sky.horizon_v[t][0];

		pnt[1].p3_vec = Temp_sky_vectors[tw][1];
		pnt[1].p3_vecPreRot = Temp_sky_vectors_unrotated[tw][1];
		uvls[1].u = Terrain_sky.horizon_u[tw][1];
		uvls[1].v = Terrain_sky.horizon_v[tw][1];

		pnt[2].p3_vec = Temp_sky_vectors[t][1];
		pnt[2].p3_vecPreRot = Temp_sky_vectors_unrotated[t][1];
		uvls[2].u = Terrain_sky.horizon_u[t][1];
		uvls[2].v = Terrain_sky.horizon_v[t][1];

		for (k = 0; k < 3; k++)
		{
			g3Point* p = &pnt[k];
			p->p3_flags = PF_UV | PF_L | PF_ORIGPOINT;
			g3_CodePoint(p);
			p->p3_uvl = uvls[k];
			p->p3_l = 1;
		}

#if (defined(EDITOR) || defined(NEWEDITOR))
		ddgr_color oldcolor;
		if (TSearch_on)
		{
			rend_SetPixel(GR_RGB(0, 255, 0), TSearch_x, TSearch_y);
			oldcolor = rend_GetPixel(TSearch_x, TSearch_y);			//will be different in 15/16-bit color
		}
#endif

		pntlist[0] = &pnt[0];
		pntlist[1] = &pnt[1];
		pntlist[2] = &pnt[2];
		g3_DrawPoly(3, pntlist, dome_bm);

#if (defined(EDITOR) || defined(NEWEDITOR))
		if (TSearch_on)
		{
			ddgr_color newcolor = rend_GetPixel(TSearch_x, TSearch_y);
			if (newcolor != oldcolor)
			{
				TSearch_seg = t;
				TSearch_found_type = TSEARCH_FOUND_SKY_DOME;
		}
	}
#endif
}

	// Draw bottom part
	for (int i = 1; i < 4; i++)
	{
		for (t = 0; t < MAX_HORIZON_PIECES; t++)
		{
			tw = (t + 1) % MAX_HORIZON_PIECES;
			pnt[0].p3_vec = Temp_sky_vectors[t][i];
			pnt[0].p3_vecPreRot = Temp_sky_vectors_unrotated[t][i];
			uvls[0].u = Terrain_sky.horizon_u[t][i];
			uvls[0].v = Terrain_sky.horizon_v[t][i];


			pnt[1].p3_vec = Temp_sky_vectors[tw][i];
			pnt[1].p3_vecPreRot = Temp_sky_vectors_unrotated[tw][i];
			uvls[1].u = Terrain_sky.horizon_u[tw][i];
			uvls[1].v = Terrain_sky.horizon_v[tw][i];

			pnt[2].p3_vec = Temp_sky_vectors[tw][i + 1];
			pnt[2].p3_vecPreRot = Temp_sky_vectors_unrotated[tw][i + 1];
			uvls[2].u = Terrain_sky.horizon_u[tw][i + 1];
			uvls[2].v = Terrain_sky.horizon_v[tw][i + 1];

			pnt[3].p3_vec = Temp_sky_vectors[t][i + 1];
			pnt[3].p3_vecPreRot = Temp_sky_vectors_unrotated[t][i + 1];
			uvls[3].u = Terrain_sky.horizon_u[t][i + 1];
			uvls[3].v = Terrain_sky.horizon_v[t][i + 1];

			for (k = 0; k < 4; k++)
			{
				g3Point* p = &pnt[k];
				p->p3_flags = PF_UV | PF_L | PF_ORIGPOINT;
				g3_CodePoint(p);
				pntlist[k] = p;
				p->p3_uvl = uvls[k];
				p->p3_l = 1;
			}
#if (defined(EDITOR) || defined(NEWEDITOR))
			ddgr_color oldcolor;
			if (TSearch_on)
			{
				rend_SetPixel(GR_RGB(0, 255, 0), TSearch_x, TSearch_y);
				oldcolor = rend_GetPixel(TSearch_x, TSearch_y);			//will be different in 15/16-bit color
			}
#endif

			g3_DrawPoly(4, pntlist, dome_bm);
#if (defined(EDITOR) || defined(NEWEDITOR))
			if (TSearch_on)
			{
				ddgr_color newcolor = rend_GetPixel(TSearch_x, TSearch_y);
				if (newcolor != oldcolor)
				{
					TSearch_seg = t;
					TSearch_found_type = TSEARCH_FOUND_SKY_DOME;
			}
			}
#endif
		}
	}
	// Now draw band
	rend_SetTextureType(TT_FLAT);
	rend_SetColorModel(CM_RGB);
	rend_SetAlphaType(AT_ALWAYS);

	// figure out colors for sky
	{
		sr = (float)GR_COLOR_RED(Terrain_sky.sky_color) / 255.0;
		sg = (float)GR_COLOR_GREEN(Terrain_sky.sky_color) / 255.0;
		sb = (float)GR_COLOR_BLUE(Terrain_sky.sky_color) / 255.0;
		hr = (float)GR_COLOR_RED(Terrain_sky.horizon_color) / 255.0;
		hg = (float)GR_COLOR_GREEN(Terrain_sky.horizon_color) / 255.0;
		hb = (float)GR_COLOR_BLUE(Terrain_sky.horizon_color) / 255.0;
	}

	for (t = 0; t < MAX_HORIZON_PIECES; t++)
	{
		tw = (t + 1) % MAX_HORIZON_PIECES;
		pnt[0].p3_vec = Temp_sky_vectors[t][4];
		pnt[0].p3_vecPreRot = Temp_sky_vectors_unrotated[t][4];
		uvls[0].r = sr;
		uvls[0].g = sg;
		uvls[0].b = sb;

		pnt[1].p3_vec = Temp_sky_vectors[tw][4];
		pnt[1].p3_vecPreRot = Temp_sky_vectors_unrotated[tw][4];
		uvls[1].r = sr;
		uvls[1].g = sg;
		uvls[1].b = sb;

		pnt[2].p3_vec = Temp_sky_vectors[tw][5];
		pnt[2].p3_vecPreRot = Temp_sky_vectors_unrotated[tw][5];
		uvls[2].r = hr;
		uvls[2].g = hg;
		uvls[2].b = hb;

		pnt[3].p3_vec = Temp_sky_vectors[t][5];
		pnt[3].p3_vecPreRot = Temp_sky_vectors_unrotated[t][5];
		uvls[3].r = hr;
		uvls[3].g = hg;
		uvls[3].b = hb;

		for (k = 0; k < 4; k++)
		{
			g3Point* p = &pnt[k];
			p->p3_flags = PF_RGBA | PF_ORIGPOINT;
			g3_CodePoint(p);
			pntlist[k] = p;
			p->p3_uvl = uvls[k];

		}
		g3_DrawPoly(4, pntlist, 0);
	}
	g3_SetTriangulationTest(0);
	rend_SetWrapType(WT_WRAP);
}

// Draws the sky textures
void DrawWireframeSky(void)
{
	int t, k, tw, i;
	g3Point pnt[6];

	// Draw top part
	for (t = 0; t < MAX_HORIZON_PIECES; t++)
	{
		tw = (t + 1) % MAX_HORIZON_PIECES;
		pnt[0].p3_vec = Temp_sky_vectors[t][0];
		pnt[1].p3_vec = Temp_sky_vectors[tw][1];
		pnt[2].p3_vec = Temp_sky_vectors[t][1];

		for (k = 0; k < 3; k++)
		{
			g3_CodePoint(&pnt[k]);
			pnt[k].p3_flags = 0;
		}

		for (k = 0; k < 3; k++)
			g3_DrawLine(GR_RGB(255, 255, 255), &pnt[k], &pnt[(k + 1) % 3]);
	}

	// Draw bottom parts
	for (i = 1; i < 5; i++)
	{
		for (t = 0; t < MAX_HORIZON_PIECES; t++)
		{
			tw = (t + 1) % MAX_HORIZON_PIECES;
			pnt[0].p3_vec = Temp_sky_vectors[t][i];
			pnt[1].p3_vec = Temp_sky_vectors[tw][i];
			pnt[2].p3_vec = Temp_sky_vectors[tw][i + 1];
			pnt[3].p3_vec = Temp_sky_vectors[t][i + 1];
			for (k = 0; k < 4; k++)
			{
				g3_CodePoint(&pnt[k]);
				pnt[k].p3_flags = 0;
			}

			for (k = 0; k < 4; k++)
				g3_DrawLine(GR_RGB(255, 255, 255), &pnt[k], &pnt[(k + 1) % 4]);
		}
	}
}

// Draws the atmosphere over a satellite
void DrawAtmosphereBlend(vector* pos, angle rotAngle, float w, float h, int bm, float r, float g, float b)
{
	g3Point pnt;
	if (g3_RotatePoint(&pnt, pos) & CC_BEHIND)
		return;

	// create the rotation matrix
	matrix rotMatrix;
	vm_AnglesToMatrix(&rotMatrix, 0, 0, rotAngle);

	// get the view matrix
	matrix viewToWorld;
	g3_GetUnscaledMatrix(&viewToWorld);
	viewToWorld = ~viewToWorld;

	// combine the matrices into one
	matrix rotationToWorld = rotMatrix * viewToWorld;

	// setup the rotation vectors
	vector rotVectors[4];
	rotVectors[0].x = -w;
	rotVectors[0].y = h;
	rotVectors[1].x = w;
	rotVectors[1].y = h;
	rotVectors[2].x = w;
	rotVectors[2].y = -h;
	rotVectors[3].x = -w;
	rotVectors[3].y = -h;

	// rotate the points
	g3Point rotPoints[8], * pntList[8];
	for (int i = 0; i < 4; ++i)
	{
		rotVectors[i].z = 0.0f;
		rotVectors[i] = (rotVectors[i] * rotationToWorld) + (*pos);

		// setup the point
		g3_RotatePoint(&rotPoints[i], &rotVectors[i]);
		rotPoints[i].p3_flags |= PF_UV | PF_RGBA;
		rotPoints[i].p3_r = r;
		rotPoints[i].p3_g = g;
		rotPoints[i].p3_b = b;
		rotPoints[i].p3_a = 0.4f;

		pntList[i] = &rotPoints[i];
	}

	rotPoints[0].p3_u = 0.0f;
	rotPoints[0].p3_v = 0.0f;
	rotPoints[1].p3_u = 1.0f;
	rotPoints[1].p3_v = 0.0f;
	rotPoints[2].p3_u = 1.0f;
	rotPoints[2].p3_v = 1.0f;
	rotPoints[3].p3_u = 0.0f;
	rotPoints[3].p3_v = 1.0f;

	// and draw!!
	rend_SetLighting(LS_NONE);
	rend_SetFlatColor(Terrain_sky.sky_color);
	rend_SetAlphaType(AT_TEXTURE_VERTEX);
	rend_SetTextureType(TT_FLAT);
	g3_DrawPoly(4, pntList, bm);
}

// Draws our pretty stars
void DrawStars(matrix* vorient)
{
	rend_SetLighting(LS_NONE);
	rend_SetTextureType(TT_FLAT);
	rend_SetOverlayType(OT_NONE);
	rend_SetColorModel(CM_MONO);
	rend_SetAlphaType(AT_VERTEX);
	rend_SetZBufferState(0);
	g3_SetFarClipZ(6000000);
	vector tempvec;

	if (Rendering_main_view && Terrain_sky.flags & TF_ROTATE_STARS && Terrain_sky.rotate_rate > 0)
	{
		matrix mat;
		vm_AnglesToMatrix(&mat, 0, Terrain_sky.rotate_rate * Frametime * (65536.0 / 360.0), 0);
		vm_Orthogonalize(&mat);
		for (int i = 0; i < MAX_STARS; i++)
		{
			vm_MatrixMulVector(&tempvec, &Terrain_sky.star_vectors[i], &mat);
			Terrain_sky.star_vectors[i] = tempvec;
		}
	}

	for (int i = 0; i < MAX_STARS; i++)
	{
		g3Point starpnt, lastpnt;
		vector streak_vec;
		float mag;
		// Rotate star 
		tempvec = Terrain_sky.star_vectors[i];
		vm_MatrixMulVector(&starpnt.p3_vec, &tempvec, vorient);
		starpnt.p3_flags = PF_RGBA;
		// Get streaking line from last frame
		if (Rendering_main_view)
		{
			streak_vec = Last_frame_stars[i] - starpnt.p3_vec;
			mag = vm_GetMagnitudeFast(&streak_vec);
		}
		else
		{
			mag = 0.0f;
		}

		if (Rendering_main_view && mag > 9000.0)
		{
			streak_vec /= mag;
			float norm = (mag / 90000);
			if (norm > 1)
				norm = 1.0;
			float revnorm = 1.0 - (norm * 4);
			if (revnorm < 0)
				revnorm = 0;
			float color_norm = (.6 + (revnorm * .4));

			lastpnt.p3_vec = starpnt.p3_vec + ((norm * .75 * 90000) * streak_vec);
			lastpnt.p3_flags = PF_RGBA;
			lastpnt.p3_a = 0;
			starpnt.p3_a = color_norm;

			g3_CodePoint(&starpnt);
			g3_CodePoint(&lastpnt);
			rend_SetFlatColor(Terrain_sky.star_color[i]);
			g3_DrawSpecialLine(&starpnt, &lastpnt);
		}
		else
		{
			starpnt.p3_flags = PF_RGBA;
			if (!g3_CodePoint(&starpnt))	// only draw if this point is on screen
			{
				starpnt.p3_a = 1.0;

				g3_ProjectPoint(&starpnt);
				lastpnt = starpnt;
				lastpnt.p3_sx++;
				rend_SetFlatColor(Terrain_sky.star_color[i]);
				rend_DrawSpecialLine(&starpnt, &lastpnt);
				//rend_SetPixel (Terrain_sky.star_color[i],starpnt.p3_sx,starpnt.p3_sy);
			}
		}
		if (Rendering_main_view)
			Last_frame_stars[i] = starpnt.p3_vec;
	}

	rend_SetZBufferState(1);
	g3_SetFarClipZ(60000);
}

// Draw the suns,moons,stars, horizon, etc
void DrawSky(vector* veye, matrix* vorient)
{
	int i, t;

	vector tempvec;

	rend_SetLighting(LS_GOURAUD);
	rend_SetZBufferState(0);
	rend_SetZBufferWriteMask(0);

	// If the sky is rotating, update the horizon vectors accordingly
	if (Rendering_main_view && Terrain_sky.flags & TF_ROTATE_SKY && Terrain_sky.rotate_rate > 0)
	{
		matrix mat;
		vm_AnglesToMatrix(&mat, 0, Terrain_sky.rotate_rate * Frametime * (65536.0 / 360.0), 0);
		vm_Orthogonalize(&mat);
		for (i = 0; i < 6; i++)
		{
			for (t = 0; t < MAX_HORIZON_PIECES; t++)
			{
				vector rot_vec;
				tempvec = Terrain_sky.horizon_vectors[t][i];
				vm_MatrixMulVector(&rot_vec, &tempvec, &mat);
				Terrain_sky.horizon_vectors[t][i] = rot_vec;
			}
		}
	}

	for (i = 0; i < 6; i++)
	{
		for (t = 0; t < MAX_HORIZON_PIECES; t++)
		{
			tempvec = Terrain_sky.horizon_vectors[t][i];
			tempvec.y -= veye->y * 0.5f;
			vm_MatrixMulVector(&Temp_sky_vectors[t][i], &tempvec, vorient);

			tempvec = Terrain_sky.horizon_vectors[t][i];
			tempvec.x += veye->x;
			tempvec.z += veye->z;
			tempvec.y += veye->y * 0.5f;
			Temp_sky_vectors_unrotated[t][i] = tempvec;
		}
	}

	float sr = (float)GR_COLOR_RED(Terrain_sky.sky_color) / 255.0;
	float sg = (float)GR_COLOR_GREEN(Terrain_sky.sky_color) / 255.0;
	float sb = (float)GR_COLOR_BLUE(Terrain_sky.sky_color) / 255.0;
	if (!Terrain_sky.textured)
		DrawGouraudSky();
	else
		DrawTexturedSky();

	if (Terrain_sky.flags & TF_STARS)
		DrawStars(vorient);

	if (Terrain_sky.flags & TF_SATELLITES)
	{
		rend_SetWrapType(WT_CLAMP);
		rend_SetColorModel(CM_MONO);

		// do satellites
		for (i = 0; i < Terrain_sky.num_satellites; i++)
		{
			int bm_handle = GetTextureBitmap(Terrain_sky.satellite_texture[i], 0);
			vector subvec = Terrain_sky.satellite_vectors[i] - *veye;
			float size = Terrain_sky.satellite_size[i];

			// Get position, angle of satellite
			vm_NormalizeVector(&subvec);
			tempvec = *veye + (subvec * (Terrain_sky.radius * 3));

#ifndef NEWEDITOR
			texture* tex = &GameTextures[Terrain_sky.satellite_texture[i]];
#else
			ned_texture_info* tex = &GameTextures[Terrain_sky.satellite_texture[i]];
#endif
			float str = Terrain_sky.satellite_r[i];
			float stg = Terrain_sky.satellite_g[i];
			float stb = Terrain_sky.satellite_b[i];
			float maxc = max(str, stg);
			maxc = max(stb, maxc);
			float r, g, b;
			if (maxc > 1.0)
			{
				r = str / maxc;
				g = stg / maxc;
				b = stb / maxc;
			}
			else
			{
				r = str;
				g = stg;
				b = stb;
			}

#ifndef NEWEDITOR
			// Draw halo
			if (Terrain_sky.satellite_flags[i] & TSF_HALO)
			{
				rend_SetZBufferWriteMask(0);
				DrawColoredRing(&tempvec, r, g, b, .4f, 0, size * 1.2, .3f, 0, 0);
				rend_SetZBufferWriteMask(1);
			}
#endif
			// Draw satellite
			if (tex->flags & TF_SATURATE)
				rend_SetAlphaType(AT_SATURATE_TEXTURE);
			else
				rend_SetAlphaType(AT_CONSTANT + AT_TEXTURE);
			rend_SetLighting(LS_NONE);
			rend_SetAlphaValue(tex->alpha * 255);
			// Check to see if the user clicked on a satellite
#if (defined(EDITOR) || defined(NEWEDITOR))
			ddgr_color oldcolor;
			if (TSearch_on)
			{
				rend_SetPixel(GR_RGB(0, 255, 0), TSearch_x, TSearch_y);
				oldcolor = rend_GetPixel(TSearch_x, TSearch_y);			//will be different in 15/16-bit color
			}
#endif

			g3_SetTriangulationTest(1);
			g3_DrawPlanarRotatedBitmap(&tempvec, &subvec, 0, size, (size * bm_h(bm_handle, 0)) / bm_w(bm_handle, 0), bm_handle);
			g3_SetTriangulationTest(0);

			// Draw atmosphere blend
			if (Terrain_sky.satellite_flags[i] & TSF_ATMOSPHERE)
			{
				angvec angs;
				vm_ExtractAnglesFromMatrix(&angs, vorient);
				DrawAtmosphereBlend(&tempvec, angs.b, size, (size * bm_h(bm_handle, 0)) / bm_w(bm_handle, 0), bm_handle, sr, sg, sb);
			}

#if (defined(EDITOR) || defined(NEWEDITOR))
			if (TSearch_on)
			{
				if (rend_GetPixel(TSearch_x, TSearch_y) != oldcolor)
				{
					TSearch_seg = i;
					TSearch_found_type = TSEARCH_FOUND_SATELLITE;
				}

			}
#endif
		}
	}

#ifndef NEWEDITOR
	if ((Weather.flags & WEATHER_FLAGS_LIGHTNING) && Weather.lightning_sequence == 2)
	{
		DrawLightning();
		Weather.lightning_sequence = 0;
	}

	if ((Weather.flags & WEATHER_FLAGS_LIGHTNING) && Weather.lightning_sequence == 1)
	{
		DrawLightningSky();
		Weather.lightning_sequence = 2;
	}
#endif

	//	DrawCloudLayer();
#if (!defined(RELEASE) || defined(NEWEDITOR))
	if (OUTLINE_ON(OM_SKY))
		DrawWireframeSky();
#endif

	rend_SetAlphaValue(255);
	rend_SetZBufferState(1);
}

int AddRenderObjectToTerrainSeg(int n, int objnum);
// Checks to see if we should render objects and also sets which order the objects
// should be rendered in
void SortTerrainObjectsForRendering(int cellcount)
{
	int i, segnum;
	object* obj;
#if (!defined(RELEASE) || defined(NEWEDITOR))
	for (i = 0; i < cellcount; i++)
		Terrain_seg_render_objs[Terrain_list[i].segment] = -1;
#endif

	// Go through each object and do trivial rejection
	for (i = 0; i <= Highest_object_index; i++)
	{
#if (!defined(RELEASE) || defined(NEWEDITOR))
		render_next[i] = -1;
#endif
		obj = &Objects[i];
		if (obj->type == OBJ_NONE)
			continue;
		if (obj->render_type == RT_NONE)
			continue;
		if (!OBJECT_OUTSIDE(obj))
			continue;
		// Ok, we know that we can see this point.  If its segment is in our list
		// then make this object draw right after this segment is drawn.  Otherwise
		// just draw it
		segnum = CELLNUM(obj->roomnum);
		ASSERT(segnum >= 0 && segnum <= (TERRAIN_WIDTH * TERRAIN_DEPTH));
		if (Terrain_rotate_list[segnum] == TS_FrameCount)
		{
			AddRenderObjectToTerrainSeg(segnum, i);
		}
		else
			AddRenderObjectToTerrainSeg(Terrain_list[cellcount - 1].segment, i);
	}
}

#if (defined(EDITOR) || defined(NEWEDITOR))
#define CROSS_WIDTH  8.0
#define CROSS_HEIGHT 8.0
void TerrainDrawCurrentVert(int tcell)
{
	if (TerrainSelected[tcell])
	{
		g3Point pnt;
		pnt.p3_flags = 0;
		pnt.p3_vec = World_point_buffer[tcell].p3_vec;
		g3_ProjectPoint(&pnt);     //make sure projected

		rend_SetFlatColor(GR_RGB(0, 250, 0));
		rend_DrawLine(pnt.p3_sx - CROSS_WIDTH, pnt.p3_sy, pnt.p3_sx, pnt.p3_sy - CROSS_HEIGHT);
		rend_DrawLine(pnt.p3_sx, pnt.p3_sy - CROSS_HEIGHT, pnt.p3_sx + CROSS_WIDTH, pnt.p3_sy);
		rend_DrawLine(pnt.p3_sx + CROSS_WIDTH, pnt.p3_sy, pnt.p3_sx, pnt.p3_sy + CROSS_HEIGHT);
		rend_DrawLine(pnt.p3_sx, pnt.p3_sy + CROSS_HEIGHT, pnt.p3_sx - CROSS_WIDTH, pnt.p3_sy);
	}
}
#endif
#if (!defined(RELEASE) || defined(NEWEDITOR))
__inline void DrawTerrainOutline(int tcell, int nverts, g3Point** pointlist)
{
	int i;
	g3Point tpnt[256];
	g3Point* tpnt_list[256];
	for (i = 0; i < nverts; i++)
	{
		tpnt[i] = *pointlist[i];
		tpnt_list[i] = &tpnt[i];
	}
#if (defined(EDITOR) || defined(NEWEDITOR))
	if (TerrainSelected[tcell])
	{
		for (i = 0; i < nverts - 1; i++)
			g3_DrawLine(GR_RGB(255, 255, 255), tpnt_list[i], tpnt_list[i + 1]);
		g3_DrawLine(GR_RGB(255, 255, 255), tpnt_list[i], tpnt_list[0]);
}
	else
#endif
	{
		for (i = 0; i < nverts - 1; i++)
			g3_DrawLine(GR_RGB(128, 128, 128), tpnt_list[i], tpnt_list[i + 1]);
		g3_DrawLine(GR_RGB(128, 128, 128), tpnt_list[i], tpnt_list[0]);
	}
}
#endif

// Returns the number of points to rotate, plus the actual numbers of the points
// are returned in the "n" array
int BuildEdgeLists(int* n, int tlist_index)
{
	int lod, simplemul;
	int t, k;
	int smul_x, smul_z;		// for tracing the very edge of the terrain

	// Now match up all edges of the differing levels of detail
	int cx, cz;
	int transcount;
	int offset;
	int maplod;
	int start = 0;
	int seg;
	int answer;
	int edgecount = 0;
	int edge1, edge2;
	float delta, cury;
	int max_edge_size = 1 << (MAX_TERRAIN_LOD - 1);

	t = Terrain_list[tlist_index].segment;
	lod = Terrain_list[tlist_index].lod;
	simplemul = 1 << ((MAX_TERRAIN_LOD - 1) - lod);
	ASSERT(simplemul <= max_edge_size);
	cx = t % TERRAIN_WIDTH;
	cz = t / TERRAIN_WIDTH;
	if (cx == TERRAIN_WIDTH - simplemul)
	{
		if (lod == MAX_TERRAIN_LOD - 1)
			return 0;

		smul_x = simplemul - 1;
	}
	else
		smul_x = simplemul;

	if (cz == TERRAIN_DEPTH - simplemul)
	{
		if (lod == MAX_TERRAIN_LOD - 1)
			return 0;

		smul_z = simplemul - 1;
	}
	else
		smul_z = simplemul;

	if (lod != MAX_TERRAIN_LOD - 1)
	{
		// Bottom edge
		// |       |
		// 2-------1

		Terrain_list[tlist_index].bottom_edge = 0;
		Terrain_list[tlist_index].bottom_count = 0;
		transcount = 0;
		if (cz != 0)
		{
			edge1 = t + smul_x;
			edge2 = t;
			delta = (Terrain_seg[edge2].mody - Terrain_seg[edge1].mody) / smul_x;
			cury = Terrain_seg[edge1].mody;

			offset = ((cz - 1) * TERRAIN_WIDTH) + cx + smul_x;
			for (k = 0; k < smul_x; k++, cury += delta)
			{
				maplod = TerrainJoinMap[offset - k];
				answer = TerrainEdgeTest[maplod][k + simplemul - smul_x];
				if (answer || k == 0)
				{
					seg = t + smul_x - k;
					n[transcount + start] = seg;
					Terrain_seg[seg].mody = cury;

					Terrain_list[tlist_index].bottom_edge |= (1 << k);
					Terrain_list[tlist_index].bottom_count++;

					transcount++;
				}
			}
		}
		else
		{
			Terrain_list[tlist_index].bottom_edge |= 1;
			Terrain_list[tlist_index].bottom_count++;
			seg = t + smul_x;
			n[transcount + start] = seg;
			transcount++;
		}

		start += transcount;
		// Right edge
		//---1
		//	  |
		//	  |
		//-- 2

		Terrain_list[tlist_index].right_edge = 0;
		Terrain_list[tlist_index].right_count = 0;
		transcount = 0;
		offset = ((cz + smul_z) * TERRAIN_WIDTH) + cx + smul_x;
		edge1 = t + smul_x + (smul_z * TERRAIN_WIDTH);
		edge2 = t + smul_x;
		delta = (Terrain_seg[edge2].mody - Terrain_seg[edge1].mody) / smul_z;
		cury = Terrain_seg[edge1].mody;
		for (k = 0; k < smul_z; k++, cury += delta)
		{
			maplod = TerrainJoinMap[offset - (k * TERRAIN_WIDTH)];
			answer = TerrainEdgeTest[maplod][k + simplemul - smul_z];
			if (answer || k == 0)
			{
				seg = t + smul_x + ((smul_z - k) * TERRAIN_WIDTH);
				n[transcount + start] = seg;
				Terrain_seg[seg].mody = cury;
				Terrain_list[tlist_index].right_edge |= (1 << k);
				Terrain_list[tlist_index].right_count++;
				transcount++;
			}
		}
		start += transcount;
		// Top edge
		// 1--------2
		// |        |

		Terrain_list[tlist_index].top_edge = 0;
		Terrain_list[tlist_index].top_count = 0;
		transcount = 0;
		offset = cx + ((cz + smul_z) * TERRAIN_WIDTH);
		edge1 = t + (smul_z * TERRAIN_WIDTH);
		edge2 = t + smul_x + (smul_z * TERRAIN_WIDTH);
		delta = (Terrain_seg[edge2].mody - Terrain_seg[edge1].mody) / smul_x;
		cury = Terrain_seg[edge1].mody;
		for (k = 0; k < smul_x; k++, cury += delta)
		{
			maplod = TerrainJoinMap[offset + k];
			answer = TerrainEdgeTest[maplod][k];
			if (answer || k == 0)
			{
				seg = (t + k) + (smul_z * TERRAIN_WIDTH);
				n[transcount + start] = seg;
				Terrain_seg[seg].mody = cury;
				Terrain_list[tlist_index].top_edge |= (1 << k);
				Terrain_list[tlist_index].top_count++;
				transcount++;
			}

		}
		start += transcount;
		// left edge
		// 2----
		// |
		// |
		// 1----
		Terrain_list[tlist_index].left_edge = 0;
		Terrain_list[tlist_index].left_count = 0;
		transcount = 0;
		if (cx != 0)
		{
			offset = (cz * TERRAIN_WIDTH) + cx - 1;
			edge1 = t;
			edge2 = t + (smul_z * TERRAIN_WIDTH);
			delta = (Terrain_seg[edge2].mody - Terrain_seg[edge1].mody) / smul_z;
			cury = Terrain_seg[edge1].mody;
			for (k = 0; k < smul_z; k++, cury += delta)
			{
				maplod = TerrainJoinMap[offset + (k * TERRAIN_WIDTH)];
				answer = TerrainEdgeTest[maplod][k];
				if (answer || k == 0)
				{
					seg = t + (k * TERRAIN_WIDTH);
					n[transcount + start] = seg;
					Terrain_seg[seg].mody = cury;
					Terrain_list[tlist_index].left_edge |= (1 << k);
					Terrain_list[tlist_index].left_count++;
					transcount++;
				}

			}
		}
		else
		{
			seg = t;
			n[transcount + start] = seg;
			Terrain_list[tlist_index].left_edge |= 1;
			Terrain_list[tlist_index].left_count++;
			transcount++;
		}

		edgecount = start + transcount;
	}
	else
	{
		n[0] = t;
		n[1] = t + TERRAIN_WIDTH;
		n[2] = t + TERRAIN_WIDTH + 1;
		n[3] = t + 1;
		Terrain_list[tlist_index].left_edge = 1;
		Terrain_list[tlist_index].bottom_edge = 1;
		Terrain_list[tlist_index].right_edge = 1;
		Terrain_list[tlist_index].top_edge = 1;
		edgecount = 4;
	}
	return edgecount;
}

// This function rotates all the points that we can see.
// If a cell is a low-res cell, then we rotate all the points down each edge
// of the cell to make sure any higher res blocks that touch the cell don't 
// appear with cracks
vector Terrain_alter_vec = { 19,-19,19 };
void RotateTerrainList(int cellcount, bool from_automap)
{
	static PSRand legacy_jitter_rand;
	int lod, simplemul, edgecount;
	int i, n[200], t, k, cx, cz;
	vector camlight = Terrain_sky.lightsource;
	vm_NormalizeVector(&camlight);
	// Reset all modified y values for the corners of each cell
	for (i = 0; i < cellcount; i++)
	{
		int ax, az;
		t = Terrain_list[i].segment;
		lod = Terrain_list[i].lod;
		simplemul = 1 << ((MAX_TERRAIN_LOD - 1) - lod);
		cx = t & (TERRAIN_WIDTH - 1);
		cz = t >> 8;

		ax = az = simplemul;

		if (cx + ax >= TERRAIN_WIDTH)
			ax = (TERRAIN_WIDTH - 1) - cx;
		if (cz + az >= TERRAIN_DEPTH)
			az = (TERRAIN_DEPTH - 1) - cz;

		n[0] = t;
		n[1] = t + (TERRAIN_WIDTH * az);
		n[2] = t + (az * TERRAIN_WIDTH) + ax;
		n[3] = t + ax;
		// This could be in a loop, but I unrolled it for speed
		Terrain_seg[n[0]].mody = Terrain_seg[n[0]].y;
		Terrain_seg[n[1]].mody = Terrain_seg[n[1]].y;
		Terrain_seg[n[2]].mody = Terrain_seg[n[2]].y;
		Terrain_seg[n[3]].mody = Terrain_seg[n[3]].y;
		if (StateLimited || from_automap)	// Setup for sorting later
		{
			int unique_id;
			unique_id = Terrain_tex_seg[Terrain_seg[t].texseg_index].tex_index;
			State_elements[i].facenum = i;
			State_elements[i].sort_key = unique_id + (Terrain_seg[t].lm_quad << 24);
		}
	}

	for (i = 0; i < cellcount; i++)
	{
		edgecount = BuildEdgeLists(n, i);
		for (k = 0; k < edgecount; k++)
		{
			if (Terrain_rotate_list[n[k]] != TS_FrameCount)
			{
				Terrain_rotate_list[n[k]] = TS_FrameCount;
				GlobalTransCount++;

				cx = n[k] % TERRAIN_WIDTH;
				cz = n[k] / TERRAIN_WIDTH;

				World_point_buffer[n[k]].p3_flags = PF_UV | PF_UV2;
				if (Terrain_seg[n[k]].mody == Terrain_seg[n[k]].y)
				{
					GetPreRotatedPoint(&World_point_buffer[n[k]], cx, cz, Terrain_seg[n[k]].ypos);
				}
				else
				{
					GetSpecialRotatedPoint(&World_point_buffer[n[k]], cx, cz, Terrain_seg[n[k]].mody);
				}

				if (Viewer_object->effect_info && (Viewer_object->effect_info->type_flags & EF_DEFORM))
				{
					float val = (((legacy_jitter_rand() % 1000) - 500.0f) / 500.0f) * Viewer_object->effect_info->deform_time;
					vector jitterVec = Terrain_alter_vec * (Viewer_object->effect_info->deform_range * val);
					World_point_buffer[n[k]].p3_vec += jitterVec;
					World_point_buffer[n[k]].p3_vecPreRot += jitterVec;
				}
				g3_CodePoint(&World_point_buffer[n[k]]);
			}
		}
	}
}

// Puts a 1 in upperleft,lowerright if those triangles are visible
void TerrainCellVisible(int index, int* upper_left, int* lower_right)
{
	int seg = Terrain_list[index].segment;
	int lod = Terrain_list[index].lod;
	int simplemul = 1 << ((MAX_TERRAIN_LOD - 1) - lod);
	int cx, cz, smul_x, smul_z;

	vector tempv;
	vector* corner[4];

	cx = seg % TERRAIN_WIDTH;
	cz = seg / TERRAIN_WIDTH;
	if (cx + simplemul == TERRAIN_WIDTH)
		smul_x = simplemul - 1;
	else
		smul_x = simplemul;

	if (cz + simplemul == TERRAIN_DEPTH)
		smul_z = simplemul - 1;
	else
		smul_z = simplemul;

	// Note - this is upper left and proceeds clockwise
	corner[0] = &World_point_buffer[seg + (TERRAIN_WIDTH * smul_z)].p3_vec;
	corner[1] = &World_point_buffer[seg + (TERRAIN_WIDTH * smul_z) + smul_x].p3_vec;
	corner[2] = &World_point_buffer[seg].p3_vec;
	corner[3] = &World_point_buffer[seg + smul_x].p3_vec;

	vm_GetPerp(&tempv, corner[0], corner[1], corner[2]);
	if ((tempv * *corner[1]) < 0)
		*upper_left = 1;
	else
		*upper_left = 0;

	// Now do lower right
	vm_GetPerp(&tempv, corner[2], corner[1], corner[3]);
	if ((tempv * *corner[1]) < 0)
		*lower_right = 1;
	else
		*lower_right = 0;
}

void DisplayTerrainList(int cellcount, bool from_automap)
{
	PERF_MARKER_SCOPE("DisplayTerrainList.Legacy");
	int total = 0, on, t, i, lod, simplemul;
	int bm_handle;
	bool draw_lightmap = false;
	int savecell;
	int obj_to_draw;
	Terrain_objects_drawn = 0;
	rend_SetWrapType(WT_WRAP);

	rend_SetColorModel(CM_RGB);
	rend_SetTextureType(TT_LINEAR);
	rend_SetAlphaType(ATF_CONSTANT + ATF_TEXTURE);
	rend_SetLighting(LS_NONE);
	if (!StateLimited || UseMultitexture)
		draw_lightmap = true;

	{
		PERF_MARKER_SCOPE("Legacy.RotateTerrainList");
		RotateTerrainList(cellcount, from_automap);
	}

	// If state limited, sort by texture
	if (StateLimited || from_automap)
	{
		PERF_MARKER_SCOPE("Legacy.SortStates");
		SortStates(State_elements, cellcount);
	}
	if (from_automap)
	{
		savecell = cellcount;
		cellcount = 0;
	}

	{
		PERF_MARKER_SCOPE("Legacy.DrawCells");
		for (i = 0; i < cellcount; i++)
		{
			int cx, cz;
			int seg_to_render;
			if (StateLimited)
				seg_to_render = State_elements[i].facenum;
			else
				seg_to_render = i;
			t = Terrain_list[seg_to_render].segment;
			lod = Terrain_list[seg_to_render].lod;
			simplemul = 1 << ((MAX_TERRAIN_LOD - 1) - lod);

			cx = t % TERRAIN_WIDTH;
			cz = t / TERRAIN_WIDTH;

			if (cx < TERRAIN_WIDTH - simplemul && cz < TERRAIN_DEPTH - simplemul || lod != (MAX_TERRAIN_LOD - 1))
			{
				int ul, lr;	// upper_left,lower_right
				if (Terrain_seg[t].flags & TF_INVISIBLE)
					if (!Show_invisible_terrain)
						goto draw_objects;	// bad! No gotos! -JL
				// Check to see if these triangles are visible if they're the smallest lod
				TerrainCellVisible(seg_to_render, &ul, &lr);

				total += (ul + lr);
				if (ul == 0 && lr == 0)
					goto draw_objects;

				bm_handle = GetTextureBitmap(Terrain_tex_seg[Terrain_seg[t].texseg_index].tex_index, 0);

				if (draw_lightmap)
					on = DrawTerrainTrianglesHardware(seg_to_render, bm_handle, ul, lr);
				else
					on = DrawTerrainTrianglesHardwareNoLight(seg_to_render, bm_handle, ul, lr);
			}

		draw_objects:;
			// Now draw any objects in this segment
#if (!defined(RELEASE) || defined(NEWEDITOR))
#endif
		}
	}
#if (defined(EDITOR) || defined(NEWEDITOR))
	if (!UseHardware)
	{
#if (!defined(RELEASE) || defined(NEWEDITOR))
		for (i = 0; i < cellcount; i++)
		{
			t = Terrain_list[i].segment;
			Terrain_seg_render_objs[t] = -1;
		}
#endif

		if ((View_mode == EDITOR_MODE) && OUTLINE_ON(OM_TERRAIN))
		{
			for (i = 0; i < cellcount; i++)
			{
				t = Terrain_list[i].segment;
				if (TerrainSelected[t] && Terrain_rotate_list[t] == TS_FrameCount)
					TerrainDrawCurrentVert(t);
		}
	}
}
#endif

	// Draw lightmaps if this is state limited
	if (!draw_lightmap || from_automap)
	{
		if (from_automap)
		{
			rend_SetAlphaType(AT_CONSTANT);
			cellcount = savecell;
		}
		else
			rend_SetAlphaType(AT_LIGHTMAP_BLEND);
		rend_SetAlphaValue(255);
		rend_SetLighting(LS_NONE);
		rend_SetOverlayType(OT_NONE);
		rend_SetTextureType(TT_PERSPECTIVE);
		rend_SetWrapType(WT_WRAP);
		rend_SetZBias(-.5f);

		{
			PERF_MARKER_SCOPE("Legacy.DrawLightmaps");
			for (i = 0; i < cellcount; i++)
			{
				int ul, lr;
				int seg_to_render;
				seg_to_render = State_elements[i].facenum;
				TerrainCellVisible(seg_to_render, &ul, &lr);
				if (ul == 0 && lr == 0)
					continue;
				DrawTerrainLightmapsHardware(seg_to_render, ul, lr);
			}
		}
		rend_SetZBias(0);
	}

	rend_SetOverlayType(OT_NONE);
	rend_SetWrapType(WT_WRAP);

	mprintf_at((2, 1, 0, "%5d cells", cellcount));
	mprintf_at((2, 2, 0, "%5d trans", GlobalTransCount));
	mprintf_at((2, 3, 0, "Tdepth=%5d", TotalDepth));
}
// Arrays for drawing
static int src[256];
static g3Point base[256];
static g3Point* slist[256];

// Draws the 2 triangles of the Terrainlist[index] (software)
int DrawTerrainTrianglesSoftware(int index, int bm_handle, int upper_left, int lower_right)
{
	return 0;
}

// Draws the 2 triangles of the Terrainlist[index] (hardware)
int DrawTerrainTrianglesHardware(int index, int bm_handle, int upper_left, int lower_right)
{
	int i;
	int cur_seg;
	int n = Terrain_list[index].segment;
	int lod = Terrain_list[index].lod;
	int bottom_start, left_start, right_start;
	int point_count = 0;
	int points_this_triangle = 0;

	terrain_segment* tseg = &Terrain_seg[n];
	terrain_tex_segment* texseg = &Terrain_tex_seg[tseg->texseg_index];
	int rotator = texseg->rotation & 0x0F;
	int tile = texseg->rotation >> 4;
	int simplemul = 1 << ((MAX_TERRAIN_LOD - 1) - lod);
	int cx, cz, smul_x, smul_z;
	cx = n % TERRAIN_WIDTH;
	cz = n / TERRAIN_WIDTH;

	// Get lightmap coordinates	
	float lightmap_u = (cx % 128) / 128.0;
	float lightmap_v = (128 - ((cz % 128) + simplemul)) / 128.0;
	float uvadjust;
	int draw_big_square = 0;
	int subx = cx % MAX_LOD_SIZE;
	int subz = (MAX_LOD_SIZE - 1) - ((cz + (simplemul - 1)) % MAX_LOD_SIZE);
	bool solid_square = 1;
	int testt = 0, testr = 0, testb = 0, testl = 0;
	// Check to make sure we don't access memory that is off the map
	if (cx + simplemul == TERRAIN_WIDTH)
	{
		smul_x = simplemul - 1;
		solid_square = 0;
	}
	else
		smul_x = simplemul;
	if (cz + simplemul == TERRAIN_DEPTH)
	{
		solid_square = 0;
		smul_z = simplemul - 1;
	}
	else
		smul_z = simplemul;

	// Build a list of points for our polygon.  We must do it this way to
	// prevent tjoint cracking
	// Do simpler operation if at highest level of detail
	if (lod == (MAX_TERRAIN_LOD - 1))
	{
		uvadjust = (simplemul / 128.0);
		cur_seg = n + (TERRAIN_WIDTH * smul_z);
		base[0] = World_point_buffer[cur_seg];

		cur_seg = n + (TERRAIN_WIDTH * smul_z) + smul_x;
		base[1] = World_point_buffer[cur_seg];

		cur_seg = n + smul_x;
		base[2] = World_point_buffer[cur_seg];

		base[3] = World_point_buffer[n];

		base[0].p3_u = tile * TerrainUSpeedup[rotator][subz * LOD_ROW_SIZE + subx];
		base[0].p3_v = tile * TerrainVSpeedup[rotator][subz * LOD_ROW_SIZE + subx];
		base[1].p3_u = tile * TerrainUSpeedup[rotator][subz * LOD_ROW_SIZE + subx + 1];
		base[1].p3_v = tile * TerrainVSpeedup[rotator][subz * LOD_ROW_SIZE + subx + 1];
		base[2].p3_u = tile * TerrainUSpeedup[rotator][(subz + 1) * LOD_ROW_SIZE + subx + 1];
		base[2].p3_v = tile * TerrainVSpeedup[rotator][(subz + 1) * LOD_ROW_SIZE + subx + 1];
		base[3].p3_u = tile * TerrainUSpeedup[rotator][(subz + 1) * LOD_ROW_SIZE + subx];
		base[3].p3_v = tile * TerrainVSpeedup[rotator][(subz + 1) * LOD_ROW_SIZE + subx];

		base[0].p3_u2 = lightmap_u;
		base[0].p3_v2 = lightmap_v;
		base[1].p3_u2 = lightmap_u + uvadjust;
		base[1].p3_v2 = lightmap_v;
		base[2].p3_u2 = lightmap_u + uvadjust;
		base[2].p3_v2 = lightmap_v + uvadjust;

		base[3].p3_u2 = lightmap_u;
		base[3].p3_v2 = lightmap_v + uvadjust;
	}
	else
	{
		uvadjust = (simplemul / 128.0) / simplemul;
		float uvmul = uvadjust * simplemul;
		if (solid_square)
		{
			right_start = Terrain_list[index].top_count;
			bottom_start = right_start + Terrain_list[index].right_count;
			left_start = bottom_start + Terrain_list[index].bottom_count;
			point_count = left_start + Terrain_list[index].left_count;

			for (i = 0; i < simplemul; i++)
			{
				// Top edge
				if (Terrain_list[index].top_edge & (1 << i))
				{
					cur_seg = n + i + (TERRAIN_WIDTH * smul_z);
					base[testt] = World_point_buffer[cur_seg];

					base[testt].p3_u = tile * TerrainUSpeedup[rotator][subz * LOD_ROW_SIZE + subx + i];
					base[testt].p3_v = tile * TerrainVSpeedup[rotator][subz * LOD_ROW_SIZE + subx + i];

					base[testt].p3_u2 = lightmap_u + (i * uvadjust);
					base[testt].p3_v2 = lightmap_v;
					slist[testt] = &base[testt];
					testt++;
				}

				// Right edge
				if (Terrain_list[index].right_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * (smul_z - i)) + smul_x;
					base[right_start + testr] = World_point_buffer[cur_seg];

					base[right_start + testr].p3_u = tile * TerrainUSpeedup[rotator][((subz + i) * LOD_ROW_SIZE) + subx + simplemul];
					base[right_start + testr].p3_v = tile * TerrainVSpeedup[rotator][((subz + i) * LOD_ROW_SIZE) + subx + simplemul];

					base[right_start + testr].p3_u2 = lightmap_u + uvmul;
					base[right_start + testr].p3_v2 = lightmap_v + (i * uvadjust);
					slist[right_start + testr] = &base[right_start + testr];
					testr++;
				}

				// Bottom edge
				if (Terrain_list[index].bottom_edge & (1 << i))
				{
					cur_seg = n + (smul_x - i);
					base[bottom_start + testb] = World_point_buffer[cur_seg];

					base[bottom_start + testb].p3_u = tile * TerrainUSpeedup[rotator][((subz + simplemul) * LOD_ROW_SIZE) + (subx + simplemul) - i];
					base[bottom_start + testb].p3_v = tile * TerrainVSpeedup[rotator][((subz + simplemul) * LOD_ROW_SIZE) + (subx + simplemul) - i];
					base[bottom_start + testb].p3_u2 = lightmap_u + uvmul - (i * uvadjust);
					base[bottom_start + testb].p3_v2 = lightmap_v + uvmul;
					slist[bottom_start + testb] = &base[bottom_start + testb];
					testb++;
				}

				// left edge
				if (Terrain_list[index].left_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * i);
					base[left_start + testl] = World_point_buffer[cur_seg];

					base[left_start + testl].p3_u = tile * TerrainUSpeedup[rotator][((subz + simplemul - i) * LOD_ROW_SIZE) + subx];
					base[left_start + testl].p3_v = tile * TerrainVSpeedup[rotator][((subz + simplemul - i) * LOD_ROW_SIZE) + subx];

					base[left_start + testl].p3_u2 = lightmap_u;
					base[left_start + testl].p3_v2 = lightmap_v + uvmul - (i * uvadjust);
					slist[left_start + testl] = &base[left_start + testl];
					testl++;
				}
			}
		}
		else
		{
			right_start = Terrain_list[index].top_count;
			bottom_start = right_start + Terrain_list[index].right_count;
			left_start = bottom_start + Terrain_list[index].bottom_count;
			point_count = left_start + Terrain_list[index].left_count;

			for (i = 0; i < smul_x; i++)
			{
				// top edge
				if (Terrain_list[index].top_edge & (1 << i))
				{
					cur_seg = n + i + (TERRAIN_WIDTH * smul_z);
					base[testt] = World_point_buffer[cur_seg];

					base[testt].p3_u = tile * TerrainUSpeedup[rotator][subz * LOD_ROW_SIZE + subx + i];
					base[testt].p3_v = tile * TerrainVSpeedup[rotator][subz * LOD_ROW_SIZE];

					base[testt].p3_u2 = lightmap_u + (i * uvadjust);
					base[testt].p3_v2 = lightmap_v;
					slist[testt] = &base[testt];
					testt++;
				}

				// Bottom edge
				if (Terrain_list[index].bottom_edge & (1 << i))
				{
					cur_seg = n + (smul_x - i);
					base[bottom_start + testb] = World_point_buffer[cur_seg];

					base[bottom_start + testb].p3_u = tile * TerrainUSpeedup[rotator][((subz + smul_z) * LOD_ROW_SIZE) + subx + smul_x - i];
					base[bottom_start + testb].p3_v = tile * TerrainVSpeedup[rotator][((subz + smul_z) * LOD_ROW_SIZE) + subx + smul_x - i];
					base[bottom_start + testb].p3_u2 = lightmap_u + uvmul - (i * uvadjust);
					base[bottom_start + testb].p3_v2 = lightmap_v + uvmul;
					slist[bottom_start + testb] = &base[bottom_start + testb];
					testb++;
				}
			}

			for (i = 0; i < smul_z; i++)
			{
				// Right edge
				if (Terrain_list[index].right_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * (smul_z - i)) + smul_x;
					base[right_start + testr] = World_point_buffer[cur_seg];

					base[right_start + testr].p3_u = tile * TerrainUSpeedup[rotator][((subz + i) * LOD_ROW_SIZE) + subx + smul_x];
					base[right_start + testr].p3_v = tile * TerrainVSpeedup[rotator][((subz + i) * LOD_ROW_SIZE) + subx + smul_x];

					base[right_start + testr].p3_u2 = lightmap_u + uvmul;
					base[right_start + testr].p3_v2 = lightmap_v + (i * uvadjust);
					slist[right_start + testr] = &base[right_start + testr];
					testr++;
				}
				// left edge
				if (Terrain_list[index].left_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * i);
					base[left_start + testl] = World_point_buffer[cur_seg];

					base[left_start + testl].p3_u = tile * TerrainUSpeedup[rotator][((subz + smul_z - i) * LOD_ROW_SIZE) + subx];
					base[left_start + testl].p3_v = tile * TerrainVSpeedup[rotator][((subz + smul_z - i) * LOD_ROW_SIZE) + subx];

					base[left_start + testl].p3_u2 = lightmap_u;
					base[left_start + testl].p3_v2 = lightmap_v + uvmul - (i * uvadjust);
					slist[left_start + testl] = &base[left_start + testl];
					testl++;
				}
			}
		}
	}

	rend_SetOverlayType(OT_BLEND);
	rend_SetOverlayMap(TerrainLightmaps[tseg->lm_quad]);

	// Make sure the triangle faces us and if so draw
	// Upper left triangle
	if (lod != (MAX_TERRAIN_LOD - 1))
		draw_big_square = 1;
	if (!upper_left && !draw_big_square)
		goto draw_lower_right;

	if (lod == (MAX_TERRAIN_LOD - 1))
	{
		slist[0] = &base[0];
		slist[1] = &base[1];
		slist[2] = &base[3];
		points_this_triangle = 3;
	}
	else
	{
		points_this_triangle = point_count;
	}

	g3_DrawPoly(points_this_triangle, slist, bm_handle);
#if (!defined(RELEASE) || defined(NEWEDITOR))
	if (OUTLINE_ON(OM_TERRAIN))
		DrawTerrainOutline(n, points_this_triangle, slist);
#endif
	// If we're LOD'd, we've already drawn our 1 polygon.  Return!
	if (draw_big_square)
		return 0;

	// Now do lower right triangle
draw_lower_right:
	if (!lower_right)
		return 0;
	slist[0] = &base[3];
	slist[1] = &base[1];
	slist[2] = &base[2];
	points_this_triangle = 3;

	g3_DrawPoly(points_this_triangle, slist, bm_handle);

#if (!defined(RELEASE) || defined(NEWEDITOR))
	if (OUTLINE_ON(OM_TERRAIN))
		DrawTerrainOutline(n, points_this_triangle, slist);
#endif
	return 0;
}

// Draws the 2 triangles of the Terrainlist[index] (hardware)
int DrawTerrainTrianglesHardwareNoLight(int index, int bm_handle, int upper_left, int lower_right)
{
	int i;
	int cur_seg;
	int n = Terrain_list[index].segment;
	int lod = Terrain_list[index].lod;
	int bottom_start, left_start, right_start;
	int point_count = 0;
	int points_this_triangle = 0;

	terrain_segment* tseg = &Terrain_seg[n];
	terrain_tex_segment* texseg = &Terrain_tex_seg[tseg->texseg_index];
	int rotator = texseg->rotation & 0x0F;
	int tile = texseg->rotation >> 4;
	int simplemul = 1 << ((MAX_TERRAIN_LOD - 1) - lod);
	int cx, cz, smul_x, smul_z;
	cx = n % TERRAIN_WIDTH;
	cz = n / TERRAIN_WIDTH;

	int draw_big_square = 0;
	int subx = cx % MAX_LOD_SIZE;
	int subz = (MAX_LOD_SIZE - 1) - ((cz + (simplemul - 1)) % MAX_LOD_SIZE);
	bool solid_square = 1;
	int testt = 0, testr = 0, testb = 0, testl = 0;
	// Check to make sure we don't access memory that is off the map
	if (cx + simplemul == TERRAIN_WIDTH)
	{
		smul_x = simplemul - 1;
		solid_square = 0;
	}
	else
		smul_x = simplemul;

	if (cz + simplemul == TERRAIN_DEPTH)
	{
		solid_square = 0;
		smul_z = simplemul - 1;
	}
	else
		smul_z = simplemul;

	// Build a list of points for our polygon.  We must do it this way to
	// prevent tjoint cracking
	// Do simpler operation if at highest level of detail
	if (lod == (MAX_TERRAIN_LOD - 1))
	{
		cur_seg = n + (TERRAIN_WIDTH * smul_z);
		base[0] = World_point_buffer[cur_seg];

		cur_seg = n + (TERRAIN_WIDTH * smul_z) + smul_x;
		base[1] = World_point_buffer[cur_seg];

		cur_seg = n + smul_x;
		base[2] = World_point_buffer[cur_seg];

		base[3] = World_point_buffer[n];

		base[0].p3_u = tile * TerrainUSpeedup[rotator][subz * LOD_ROW_SIZE + subx];
		base[0].p3_v = tile * TerrainVSpeedup[rotator][subz * LOD_ROW_SIZE + subx];
		base[1].p3_u = tile * TerrainUSpeedup[rotator][subz * LOD_ROW_SIZE + subx + 1];
		base[1].p3_v = tile * TerrainVSpeedup[rotator][subz * LOD_ROW_SIZE + subx + 1];
		base[2].p3_u = tile * TerrainUSpeedup[rotator][(subz + 1) * LOD_ROW_SIZE + subx + 1];
		base[2].p3_v = tile * TerrainVSpeedup[rotator][(subz + 1) * LOD_ROW_SIZE + subx + 1];
		base[3].p3_u = tile * TerrainUSpeedup[rotator][(subz + 1) * LOD_ROW_SIZE + subx];
		base[3].p3_v = tile * TerrainVSpeedup[rotator][(subz + 1) * LOD_ROW_SIZE + subx];
	}
	else
	{
		if (solid_square)
		{
			right_start = Terrain_list[index].top_count;
			bottom_start = right_start + Terrain_list[index].right_count;
			left_start = bottom_start + Terrain_list[index].bottom_count;
			point_count = left_start + Terrain_list[index].left_count;

			for (i = 0; i < simplemul; i++)
			{
				// Top edge
				if (Terrain_list[index].top_edge & (1 << i))
				{
					cur_seg = n + i + (TERRAIN_WIDTH * smul_z);
					base[testt] = World_point_buffer[cur_seg];

					base[testt].p3_u = tile * TerrainUSpeedup[rotator][subz * LOD_ROW_SIZE + subx + i];
					base[testt].p3_v = tile * TerrainVSpeedup[rotator][subz * LOD_ROW_SIZE + subx + i];

					slist[testt] = &base[testt];
					testt++;
				}
				// Right edge
				if (Terrain_list[index].right_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * (smul_z - i)) + smul_x;
					base[right_start + testr] = World_point_buffer[cur_seg];

					base[right_start + testr].p3_u = tile * TerrainUSpeedup[rotator][((subz + i) * LOD_ROW_SIZE) + subx + simplemul];
					base[right_start + testr].p3_v = tile * TerrainVSpeedup[rotator][((subz + i) * LOD_ROW_SIZE) + subx + simplemul];

					slist[right_start + testr] = &base[right_start + testr];
					testr++;
				}
				// Bottom edge
				if (Terrain_list[index].bottom_edge & (1 << i))
				{
					cur_seg = n + (smul_x - i);
					base[bottom_start + testb] = World_point_buffer[cur_seg];

					base[bottom_start + testb].p3_u = tile * TerrainUSpeedup[rotator][((subz + simplemul) * LOD_ROW_SIZE) + subx + simplemul - i];
					base[bottom_start + testb].p3_v = tile * TerrainVSpeedup[rotator][((subz + simplemul) * LOD_ROW_SIZE) + subx + simplemul - i];
					slist[bottom_start + testb] = &base[bottom_start + testb];
					testb++;
				}
				// left edge
				if (Terrain_list[index].left_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * i);
					base[left_start + testl] = World_point_buffer[cur_seg];

					base[left_start + testl].p3_u = tile * TerrainUSpeedup[rotator][((subz + simplemul - i) * LOD_ROW_SIZE) + subx];
					base[left_start + testl].p3_v = tile * TerrainVSpeedup[rotator][((subz + simplemul - i) * LOD_ROW_SIZE) + subx];

					slist[left_start + testl] = &base[left_start + testl];
					testl++;
				}
			}
		}
		else
		{
			right_start = Terrain_list[index].top_count;
			bottom_start = right_start + Terrain_list[index].right_count;
			left_start = bottom_start + Terrain_list[index].bottom_count;
			point_count = left_start + Terrain_list[index].left_count;

			for (i = 0; i < smul_x; i++)
			{
				// top edge
				if (Terrain_list[index].top_edge & (1 << i))
				{
					cur_seg = n + i + (TERRAIN_WIDTH * smul_z);
					base[testt] = World_point_buffer[cur_seg];

					base[testt].p3_u = tile * TerrainUSpeedup[rotator][subz * LOD_ROW_SIZE + subx + i];
					base[testt].p3_v = tile * TerrainVSpeedup[rotator][subz * LOD_ROW_SIZE + subx + i];

					slist[testt] = &base[testt];
					testt++;
				}
				// Bottom edge
				if (Terrain_list[index].bottom_edge & (1 << i))
				{
					cur_seg = n + (smul_x - i);
					base[bottom_start + testb] = World_point_buffer[cur_seg];

					base[bottom_start + testb].p3_u = tile * TerrainUSpeedup[rotator][((subz + simplemul) * LOD_ROW_SIZE) + subx + simplemul - i];
					base[bottom_start + testb].p3_v = tile * TerrainVSpeedup[rotator][((subz + simplemul) * LOD_ROW_SIZE) + subx + simplemul - i];

					slist[bottom_start + testb] = &base[bottom_start + testb];
					testb++;
				}
			}
			for (i = 0; i < smul_z; i++)
			{
				// Right edge
				if (Terrain_list[index].right_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * (smul_z - i)) + smul_x;
					base[right_start + testr] = World_point_buffer[cur_seg];

					base[right_start + testr].p3_u = tile * TerrainUSpeedup[rotator][((subz + i) * LOD_ROW_SIZE) + subx + simplemul];
					base[right_start + testr].p3_v = tile * TerrainVSpeedup[rotator][((subz + i) * LOD_ROW_SIZE) + subx + simplemul];

					slist[right_start + testr] = &base[right_start + testr];
					testr++;
				}
				// left edge
				if (Terrain_list[index].left_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * i);
					base[left_start + testl] = World_point_buffer[cur_seg];

					base[left_start + testl].p3_u = tile * TerrainUSpeedup[rotator][((subz + simplemul - i) * LOD_ROW_SIZE) + subx];
					base[left_start + testl].p3_v = tile * TerrainVSpeedup[rotator][((subz + simplemul - i) * LOD_ROW_SIZE) + subx];
					slist[left_start + testl] = &base[left_start + testl];
					testl++;
				}
			}
		}
	}

#if (defined(EDITOR) || defined(NEWEDITOR))
	ddgr_color oldcolor;
	if (TSearch_on)
	{
		rend_SetPixel(GR_RGB(0, 255, 0), TSearch_x, TSearch_y);
		oldcolor = rend_GetPixel(TSearch_x, TSearch_y);			//will be different in 15/16-bit color
	}
#endif

	rend_SetOverlayType(OT_NONE);

	// Make sure the triangle faces us and if so draw
	// Upper left triangle
	if (lod != (MAX_TERRAIN_LOD - 1))
		draw_big_square = 1;
	if (!upper_left && !draw_big_square)
		goto draw_lower_right;

	if (lod == (MAX_TERRAIN_LOD - 1))
	{
		slist[0] = &base[0];
		slist[1] = &base[1];
		slist[2] = &base[3];
		points_this_triangle = 3;
	}
	else
	{
		points_this_triangle = point_count;
	}

	g3_DrawPoly(points_this_triangle, slist, bm_handle);
#if (!defined(RELEASE) || defined(NEWEDITOR))
	if (OUTLINE_ON(OM_TERRAIN))
		DrawTerrainOutline(n, points_this_triangle, slist);
#endif
	// If we're LOD'd, we've already drawn our 1 polygon.  Return!
	if (draw_big_square)
		return 0;

	// Now do lower right triangle
draw_lower_right:
	if (!lower_right)
		return 0;
	slist[0] = &base[3];
	slist[1] = &base[1];
	slist[2] = &base[2];
	points_this_triangle = 3;

	g3_DrawPoly(points_this_triangle, slist, bm_handle);
#if (defined(EDITOR) || defined(NEWEDITOR))
	if (TSearch_on)
	{
		if (rend_GetPixel(TSearch_x, TSearch_y) != oldcolor)
		{
			TSearch_seg = n;
			TSearch_found_type = TSEARCH_FOUND_TERRAIN;
			}
		}
#endif
#if (!defined(RELEASE) || defined(NEWEDITOR))
	if (OUTLINE_ON(OM_TERRAIN))
		DrawTerrainOutline(n, points_this_triangle, slist);
#endif
	return 0;
	}

// Draws the 2 triangles of the Terrainlist[index] (hardware)
void DrawTerrainLightmapsHardware(int index, int upper_left, int lower_right)
{
	int i;
	int cur_seg;
	int n = Terrain_list[index].segment;
	int lod = Terrain_list[index].lod;
	int bottom_start, left_start, right_start;
	int point_count = 0;
	int points_this_triangle = 0;

	terrain_segment* tseg = &Terrain_seg[n];
	int simplemul = 1 << ((MAX_TERRAIN_LOD - 1) - lod);
	int cx, cz, smul_x, smul_z;
	cx = n % TERRAIN_WIDTH;
	cz = n / TERRAIN_WIDTH;

	// Get lightmap coordinates	
	float lightmap_u = (cx % 128) / 128.0;
	float lightmap_v = (128 - ((cz % 128) + simplemul)) / 128.0;
	float uvadjust;
	int draw_big_square = 0;
	bool solid_square = 1;
	int testt = 0, testr = 0, testb = 0, testl = 0;
	// Check to make sure we don't access memory that is off the map
	if (cx + simplemul == TERRAIN_WIDTH)
	{
		smul_x = simplemul - 1;
		solid_square = 0;
	}
	else
		smul_x = simplemul;

	if (cz + simplemul == TERRAIN_DEPTH)
	{
		solid_square = 0;
		smul_z = simplemul - 1;
	}
	else
		smul_z = simplemul;

	// Build a list of points for our polygon.  We must do it this way to
	// prevent tjoint cracking
	// Do simpler operation if at highest level of detail
	if (lod == (MAX_TERRAIN_LOD - 1))
	{
		uvadjust = (simplemul / 128.0);
		cur_seg = n + (TERRAIN_WIDTH * smul_z);
		base[0] = World_point_buffer[cur_seg];

		cur_seg = n + (TERRAIN_WIDTH * smul_z) + smul_x;
		base[1] = World_point_buffer[cur_seg];

		cur_seg = n + smul_x;
		base[2] = World_point_buffer[cur_seg];

		base[3] = World_point_buffer[n];

		base[0].p3_u = lightmap_u;
		base[0].p3_v = lightmap_v;
		base[1].p3_u = lightmap_u + uvadjust;
		base[1].p3_v = lightmap_v;
		base[2].p3_u = lightmap_u + uvadjust;
		base[2].p3_v = lightmap_v + uvadjust;

		base[3].p3_u = lightmap_u;
		base[3].p3_v = lightmap_v + uvadjust;
	}
	else
	{
		uvadjust = (simplemul / 128.0) / simplemul;
		float uvmul = uvadjust * simplemul;
		if (solid_square)
		{
			right_start = Terrain_list[index].top_count;
			bottom_start = right_start + Terrain_list[index].right_count;
			left_start = bottom_start + Terrain_list[index].bottom_count;
			point_count = left_start + Terrain_list[index].left_count;

			for (i = 0; i < simplemul; i++)
			{
				// Top edge
				if (Terrain_list[index].top_edge & (1 << i))
				{
					cur_seg = n + i + (TERRAIN_WIDTH * smul_z);
					base[testt] = World_point_buffer[cur_seg];

					base[testt].p3_u = lightmap_u + (i * uvadjust);
					base[testt].p3_v = lightmap_v;
					slist[testt] = &base[testt];
					testt++;
				}
				// Right edge
				if (Terrain_list[index].right_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * (smul_z - i)) + smul_x;
					base[right_start + testr] = World_point_buffer[cur_seg];

					base[right_start + testr].p3_u = lightmap_u + uvmul;
					base[right_start + testr].p3_v = lightmap_v + (i * uvadjust);
					slist[right_start + testr] = &base[right_start + testr];
					testr++;
				}
				// Bottom edge
				if (Terrain_list[index].bottom_edge & (1 << i))
				{
					cur_seg = n + (smul_x - i);
					base[bottom_start + testb] = World_point_buffer[cur_seg];

					base[bottom_start + testb].p3_u = lightmap_u + uvmul - (i * uvadjust);
					base[bottom_start + testb].p3_v = lightmap_v + uvmul;
					slist[bottom_start + testb] = &base[bottom_start + testb];
					testb++;
				}
				// left edge
				if (Terrain_list[index].left_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * i);
					base[left_start + testl] = World_point_buffer[cur_seg];

					base[left_start + testl].p3_u = lightmap_u;
					base[left_start + testl].p3_v = lightmap_v + uvmul - (i * uvadjust);
					slist[left_start + testl] = &base[left_start + testl];
					testl++;
				}
			}
		}
		else
		{
			right_start = Terrain_list[index].top_count;
			bottom_start = right_start + Terrain_list[index].right_count;
			left_start = bottom_start + Terrain_list[index].bottom_count;
			point_count = left_start + Terrain_list[index].left_count;

			for (i = 0; i < smul_x; i++)
			{
				// top edge
				if (Terrain_list[index].top_edge & (1 << i))
				{
					cur_seg = n + i + (TERRAIN_WIDTH * smul_z);
					base[testt] = World_point_buffer[cur_seg];

					base[testt].p3_u = lightmap_u + (i * uvadjust);
					base[testt].p3_v = lightmap_v;
					slist[testt] = &base[testt];
					testt++;
				}
				// Bottom edge
				if (Terrain_list[index].bottom_edge & (1 << i))
				{
					cur_seg = n + (smul_x - i);
					base[bottom_start + testb] = World_point_buffer[cur_seg];

					base[bottom_start + testb].p3_u = lightmap_u + uvmul - (i * uvadjust);
					base[bottom_start + testb].p3_v = lightmap_v + uvmul;
					slist[bottom_start + testb] = &base[bottom_start + testb];
					testb++;
				}

			}


			for (i = 0; i < smul_z; i++)
			{
				// Right edge
				if (Terrain_list[index].right_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * (smul_z - i)) + smul_x;
					base[right_start + testr] = World_point_buffer[cur_seg];

					base[right_start + testr].p3_u = lightmap_u + uvmul;
					base[right_start + testr].p3_v = lightmap_v + (i * uvadjust);
					slist[right_start + testr] = &base[right_start + testr];
					testr++;
				}
				// left edge
				if (Terrain_list[index].left_edge & (1 << i))
				{
					cur_seg = n + (TERRAIN_WIDTH * i);
					base[left_start + testl] = World_point_buffer[cur_seg];

					base[left_start + testl].p3_u = lightmap_u;
					base[left_start + testl].p3_v = lightmap_v + uvmul - (i * uvadjust);
					slist[left_start + testl] = &base[left_start + testl];
					testl++;
				}
			}
		}
	}

	// Make sure the triangle faces us and if so draw
	// Upper left triangle
	if (lod != (MAX_TERRAIN_LOD - 1))
		draw_big_square = 1;
	if (!upper_left && !draw_big_square)
		goto draw_lower_right;

	if (lod == (MAX_TERRAIN_LOD - 1))
	{
		slist[0] = &base[0];
		slist[1] = &base[1];
		slist[2] = &base[3];
		points_this_triangle = 3;
	}
	else
	{
		points_this_triangle = point_count;
	}

	g3_DrawPoly(points_this_triangle, slist, TerrainLightmaps[tseg->lm_quad], MAP_TYPE_LIGHTMAP);
#if (!defined(RELEASE) || defined(NEWEDITOR))
	if (OUTLINE_ON(OM_TERRAIN))
		DrawTerrainOutline(n, points_this_triangle, slist);
#endif
	// If we're LOD'd, we've already drawn our 1 polygon.  Return!
	if (draw_big_square)
		return;

	// Now do lower right triangle
draw_lower_right:
	if (!lower_right)
		return;
	slist[0] = &base[3];
	slist[1] = &base[1];
	slist[2] = &base[2];
	points_this_triangle = 3;

	g3_DrawPoly(points_this_triangle, slist, TerrainLightmaps[tseg->lm_quad], MAP_TYPE_LIGHTMAP);

#if (!defined(RELEASE) || defined(NEWEDITOR))
	if (OUTLINE_ON(OM_TERRAIN))
		DrawTerrainOutline(n, points_this_triangle, slist);
#endif
}

// Adds object obj to terrain segment n.  
// This object will be rendered immediately following the rendering of this
// terrain segment
int AddRenderObjectToTerrainSeg(int n, int objnum)
{
	// Uses a linked list to keep track of what objects are in this segment
#if (!defined(RELEASE) || defined(NEWEDITOR))
	if (Terrain_seg_render_objs[n] == objnum)
		return 0;
	//New object points at first in list
	render_next[objnum] = Terrain_seg_render_objs[n];
	//New object becomes first in list
	Terrain_seg_render_objs[n] = objnum;
#endif

	return (0);
}

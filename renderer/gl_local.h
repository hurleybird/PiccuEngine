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
#pragma once

//can't figure out which one of these little jokers is including Windows.h.. (for reference when SDL3 is defined)
#define NOMINMAX
#ifdef SDL3
#include <SDL3/SDL_video.h>
#elif WIN32
#include <Windows.h>
#endif
#include <algorithm>
#include <vector>
#include <glad/gl.h>
#if defined(WIN32) && !defined(SDL3)
#include "wglext.h"
#endif
#define DD_ACCESS_RING //need direct access to some stuff
#include "application.h"
#include "3d.h"
#include "renderer.h"
#include "vecmat.h"
#include "bitmap.h"
#include "lightmap.h"
#include "mem.h"
#include "mono.h"
#include "pserror.h"
#include "gl_shared.h"
#include "IRenderer.h"

struct color_array
{
	ubyte r, g, b, a;
};

struct tex_array
{
	float s, t, w;
};

struct normal_array
{
	float x, y, z, w;
};

struct vec4_array
{
	float x, y, z, w;
};

struct gl_vertex
{
	vector vert;
	color_array color;
	tex_array tex_coord;
	tex_array tex_coord2;
	normal_array normal;
	float motion_velocity_x;
	float motion_velocity_y;
	vec4_array motion_world_position;
};

struct gl_motion_vertex
{
	float x, y, z;
	float velocity_x, velocity_y;
};

constexpr int NUM_GL4_FBOS = 1;
class GL4Renderer : public IRenderer
{
	//MAIN
	oeApplication* ParentApplication = nullptr;

	bool OpenGL_multitexture_state = false;
	bool OpenGL_packed_pixels = false;
	bool Fast_test_render = false;

	Framebuffer framebuffers[NUM_GL4_FBOS];
	Framebuffer resolved_framebuffer;
	Framebuffer downscale_framebuffer;
	Framebuffer bloom_source_framebuffer;
	Framebuffer bloom_source_resolved_framebuffer;
	Framebuffer bloom_source_downscale_framebuffer;
	Framebuffer ao_scene_framebuffer;
	Framebuffer ao_composite_framebuffer;
	Framebuffer post_present_framebuffer;
	MotionVectorResources motion_vectors;
	PostProtectionMaskResources post_protection_mask;
	int framebuffer_current_draw = 0;
	bool bloom_source_valid = false;
	bool ao_scene_valid = false;
	int framebuffer_logical_width = 0;
	int framebuffer_logical_height = 0;
	int framebuffer_logical_offset_x = 0;
	int framebuffer_logical_offset_y = 0;
	bool post_present_pending_swap = false;
	int msaa_downshift_release_frames = 0;
	int msaa_forced_off_target_samples = 0;
	bool msaa_forced_off_scene_presented = false;
	renderer_preferred_state msaa_deferred_preferred_state = {};
	bool msaa_deferred_preferred_state_valid = false;
	bool msaa_deferred_apply_pending = false;
	bool framebuffer_state_snapshot_pending = false;

	unsigned int framebuffer_blit_x = 0, framebuffer_blit_y = 0, framebuffer_blit_w = 0, framebuffer_blit_h = 0;

	ShaderProgram blitshader;
	ShaderProgram downsampleshader;
	ShaderProgram fontshader;
	ShaderProgram motionvectorshader;
	ShaderProgram motionvectordebugshader;
	ShaderProgram ao_compositeshader;
	BloomResources bloom;
	GTAOResources gtao;
	//Cached projection matrix and near/far for GTAO. Updated on every
	//UpdateCommon(depth=0) call so the AO pass uses the main world projection.
	float last_projection[16] = {};
	float captured_scene_projection[16] = {};
	float last_nearz = 1.0f;
	float last_farz = 10000.f;
	bool captured_scene_projection_valid = false;
	float current_view_projection[16] = {};
	float current_inverse_view_projection[16] = {};
	float previous_view_projection[16] = {};
	bool have_current_view_projection = false;
	bool have_current_inverse_view_projection = false;
	bool have_previous_view_projection = false;
	//Temp shader to test the shader systems.
	ShaderProgram testshader;
	GLint blitshader_gamma = -1;
	GLint blitshader_uv_origin = -1;
	GLint blitshader_uv_scale = -1;
	GLint downsampleshader_gamma = -1;
	GLint downsampleshader_dest_origin = -1;
	GLint downsampleshader_source_visible_origin = -1;
	GLint downsampleshader_source_visible_size = -1;
	GLint fontshader_texture = -1;
	GLint motionvector_screen_size = -1;
	GLint motionvectordebug_velocity_source = -1;
	GLint motionvectordebug_uv_origin = -1;
	GLint motionvectordebug_uv_scale = -1;
	GLint motionvectordebug_screen_size = -1;
	GLint ao_composite_final_source = -1;
	GLint ao_composite_scene_source = -1;
	GLint ao_composite_ao_scene_source = -1;
	GLint ao_composite_protection_mask = -1;
	GLint ao_composite_use_protection_mask = -1;
	GLint ao_composite_visible_origin = -1;
	GLint ao_composite_visible_size = -1;
	GLint ao_composite_use_visible_rect = -1;
	GLfloat max_line_width = 1.0f;
	GLfloat max_point_size = 1.0f;

	int OpenGL_last_frame_polys_drawn = 0;
	int OpenGL_last_frame_verts_processed = 0;
	int OpenGL_last_uploaded = 0;

	//DRAW
	gl_vertex GL_vertices[100];
	std::vector<gl_vertex> font_batch_vertices[2];
	GLuint font_texture_array = 0;
	int font_texture_array_width = 0;
	int font_texture_array_height = 0;
	int font_texture_array_layers = 0;
	std::vector<int> font_texture_array_handles;

	float OpenGL_Alpha_factor = 1.0f;
	float Alpha_multiplier = 1.0f;

	int OpenGL_polys_drawn = 0;
	int OpenGL_verts_processed = 0;

	int Overlay_map = -1;
	int Bump_map = 0;
	int Bumpmap_ready = 0;
	ubyte Overlay_type = OT_NONE;

	bool OpenGL_blending_on = true;

	GLuint drawbuffer = 0;
	//The next committed vertex is where to start writing vertex data to the buffer
	GLuint nextcommittedvertex = 0;
	ShaderProgram drawshaders[8];
	GLint drawshader_phong_enabled_uniforms[8] = {};
	GLint drawshader_light_direction_uniforms[8] = {};
	GLint drawshader_dynamic_count_uniforms[8] = {};
	GLint drawshader_dynamic_face_normal_uniforms[8] = {};
	GLint drawshader_dynamic_positions_uniforms[8] = {};
	GLint drawshader_dynamic_colors_uniforms[8] = {};
	GLint drawshader_dynamic_radii_uniforms[8] = {};
	GLint drawshader_dynamic_directions_uniforms[8] = {};
	GLint drawshader_dynamic_dot_ranges_uniforms[8] = {};
	GLint drawshader_dynamic_directional_uniforms[8] = {};
	GLint drawshader_ao_suppression_uniforms[8] = {};
	GLint drawshader_bloom_suppression_uniforms[8] = {};
	GLint drawshader_ao_class_uniforms[8] = {};
	GLint drawshader_ao_weight_uniforms[8] = {};
	GLint drawshader_ao_capture_weight_mode_uniforms[8] = {};
	GLint drawshader_post_mask_luminance_uniforms[8] = {};
	GLint drawshader_cockpit_backing_enabled_uniforms[8] = {};
	GLint drawshader_cockpit_backing_alpha_uniforms[8] = {};
	GLint drawshader_cockpit_backing_darkness_uniforms[8] = {};
	GLint drawshader_cockpit_scanlines_enabled_uniforms[8] = {};
	GLint drawshader_cockpit_scanline_strength_uniforms[8] = {};
	GLint drawshader_cockpit_scanline_spacing_uniforms[8] = {};
	GLint drawshader_cockpit_scanline_thickness_uniforms[8] = {};
	GLint drawshader_cockpit_scanline_phase_uniforms[8] = {};
	GLint drawshader_motion_vector_mode_uniforms[8] = {};
	GLint drawshader_motion_vector_current_view_projection_uniforms[8] = {};
	GLint drawshader_motion_vector_previous_view_projection_uniforms[8] = {};
	GLint drawshader_motion_vector_screen_size_uniforms[8] = {};
	GLint drawshader_motion_vector_has_previous_uniforms[8] = {};
	int lastdrawshader = -1;
	bool legacy_draw_uniforms_dirty = true;
	float ao_suppression_draw_value = 0.0f;
	float bloom_suppression_draw_value = 0.0f;
	int ao_class_draw_value = 0;
	float ao_weight_draw_value = 1.0f;
	renderer_cockpit_backing_effect cockpit_backing_effect = {};
	bool post_mask_only_draw = false;
	bool post_protection_mask_dirty = false;
	bool post_protection_mask_cleared_this_frame = false;
	renderer_draw_call_category draw_call_category = RENDERER_DRAW_CALL_3D;
	vector per_pixel_light_direction = { 0, 0, -1 };
	vector per_pixel_dynamic_face_normal = { 0, 0, 1 };
	int per_pixel_dynamic_light_count = 0;
	GLfloat per_pixel_dynamic_positions[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS][3] = {};
	GLfloat per_pixel_dynamic_colors[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS][3] = {};
	GLfloat per_pixel_dynamic_radii[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS] = {};
	GLfloat per_pixel_dynamic_directions[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS][3] = {};
	GLfloat per_pixel_dynamic_dot_ranges[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS] = {};
	GLint per_pixel_dynamic_directional[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS] = {};

	GLuint drawvao = 0;
	void* drawbuffermap = 0;
	GLuint motionvector_vao = 0;
	GLuint motionvector_vbo = 0;
	bool motion_object_active = false;
	bool motion_vectors_dirty = false;
	bool motion_vectors_cleared_this_frame = false;
	bool motion_vectors_capture_locked = false;

	//IMAGE
	ubyte opengl_Framebuffer_ready = 0;
	chunked_bitmap opengl_Chunked_bitmap;

	ushort* OpenGL_bitmap_remap = nullptr;
	ushort* OpenGL_lightmap_remap = nullptr;
	ubyte* OpenGL_bitmap_states = nullptr;
	ubyte* OpenGL_lightmap_states = nullptr;

	unsigned int opengl_last_upload_res = 0;
	uint* opengl_Upload_data = nullptr;
	uint* opengl_Translate_table = nullptr;
	uint* opengl_4444_translate_table = nullptr;

	ushort* opengl_packed_Upload_data = nullptr;
	ushort* opengl_packed_Translate_table = nullptr;
	ushort* opengl_packed_4444_translate_table = nullptr;

	//Texture list
	GLuint texture_name_list[10000];
	int Cur_texture_object_num = 1;
	int Last_texel_unit_set = -1;

	int OpenGL_last_bound[2];
	int OpenGL_sets_this_frame[10];
	int OpenGL_uploads = 0;

	bool OpenGL_cache_initted = false;

	//SHADER
	GLuint commonbuffername = 0;
	GLuint legacycommonbuffername = 0;
	GLuint fogbuffername = 0;
	GLuint specularbuffername = 0;
	GLuint terrainfogbuffername = 0;
	int terrainfogcounter = 0;

	ShaderProgram* lastshaderprog = nullptr;

	//FRAMEBUFFER
	GLuint fbVAOName = 0;
	GLuint fbVBOName = 0;

	//INIT
	renderer_preferred_state OpenGL_preferred_state = { false, true, false, 32, 1.0, 0, 0, 0, 0, false, 1, 0, false, false, 0.75f, 0.75f, 0.75f, false, GTAO_RESOLUTION_HALF, 128, 6, 4.0f, 2.5f, 0.25f, 107, false, 0.5f, 0.5f, 0.5f, 1.0f, RENDERER_MOTION_VECTOR_OFF, false };
	rendering_state OpenGL_state = {};

	bool OpenGL_debugging_enabled = false;
	bool OpenGL_buffer_storage_enabled = false;

#if defined(SDL3)
	SDL_GLContext GLContext = nullptr;
	SDL_Window* GLWindow = nullptr;
#elif defined(WIN32)
	//	Moved from DDGR library
	HWND hOpenGLWnd = nullptr;
	HDC hOpenGLDC = nullptr;
	HGLRC ResourceContext = nullptr;
#endif

	// The font characteristics
	float rend_FontRed[4], rend_FontBlue[4], rend_FontGreen[4], rend_FontAlpha[4];

	//if false, never, ever do anything with framebuffers while I try to get out of here.
	bool framebuffer_ok = false;

private:
	//DRAW
	void UseDrawVAO();
	void InitPersistentDrawBuffer(size_t size);
	void DestroyPersistentDrawBuffer();
	int CopyVertices(int numvertices);
	int CopyVertices(const gl_vertex* vertices, int numvertices);
	void SetDrawDefaults();
	void SelectDrawShader();
	void BuildDrawVertex(gl_vertex& vert, const g3Point* pnt, float xscalar, float yscalar,
		ubyte fr, ubyte fg, ubyte fb);
	void FlushFontBatch();
	void FlushFontBatchVertices(int batch_index);
	bool FontBatchHasVertices() const;
	void ClearFontBatchVertices();
	void SetFontBatchFullscreenDrawState(GLint old_viewport[4]);
	void RestoreFontBatchDrawState(const GLint old_viewport[4]);
	int GetFontTextureLayer(int bm_handle);
	void UploadFontTextureLayer(int layer, int bm_handle);
	void DestroyFontBatchResources();
	void InitMotionVectorDraw();
	void DestroyMotionVectorDraw();
	void DrawMotionVectorPolygon(int nv, g3Point** p);
	void DrawMotionVectorTriangles(const gl_motion_vertex* vertices, int nv);
	bool MotionVectorTargetEnabled() const;
	bool PixelMotionVectorModeEnabled() const;
	bool MotionVectorWritesEnabled() const;
	bool CurrentDrawWritesPixelMotionVectors() const;
	void DrawMotionVectorDebugPreview(int supersampling_factor);
	void UseSceneDrawBuffers();

	// Turns on/off multitexture blending
	void SetMultitextureBlendMode(bool state);

	//INIT
	// Sets default states for our renderer
	void SetDefaults();
#ifdef SDL3
	int Setup(SDL_Window* window);
#elif WIN32
	int Setup(HDC glhdc);
#endif
	void ApplySwapInterval();
	void GetInformation();

	//IMAGES
	void InitImages();
	void FreeImages();
	int MakeTextureObject(int tn, bool wrap);
	int MakeBitmapCurrent(int handle, int map_type, int tn);
	void MakeWrapTypeCurrent(int handle, int map_type, int tn);
	void MakeFilterTypeCurrent(int handle, int map_type, int tn);
	int InitCache();
	void FreeCache();
	void FreeUploadBuffers();
	void SetUploadBufferSize(int width, int height);
	void TranslateBitmapToOpenGL(int texnum, int bm_handle, int map_type, int replace, int tn);
	void ChangeChunkedBitmap(int bm_handle, chunked_bitmap* chunk);

	bool CheckExtension(char* extName);
	void UpdateFramebuffer();
	void CloseFramebuffer();
	void SetViewport();
	void UpdateWindow();
	void RefreshViewSize();
	void UpdatePresentRect();
	int SupersamplingFactor() const;
	int FramebufferWidth() const;
	int FramebufferHeight() const;
	int ScaledX(int x) const;
	int ScaledY(int y) const;
	int ScaledW(int w) const;
	int ScaledH(int h) const;
	void SetAlwaysAlpha(bool state);
	float GetAlphaMultiplier();

	//Shader
	void InitShaders();
	void UpdateLegacyBlock(float* projection, float* modelview);

public:
	GL4Renderer();

	//INITIALIZATION

	// Init our renderer, pass the application object also.
	int Init(oeApplication* app, renderer_preferred_state* pref_state) override;
	// de-init the renderer
	void Close() override;

	//STATE

	// Tells the software renderer whether or not to use mipping
	void SetMipState(sbyte) override;

	// Sets the fog state to TRUE or FALSE
	void SetFogState(sbyte on) override;

	// Sets the near and far plane of fog
	void SetFogBorders(float fog_near, float fog_far) override;

	// Sets the color for fill based primitives;
	void SetFlatColor(ddgr_color color) override;

	void SetTextureType(texture_type) override;

	// Sets the state of bilinear filtering for our textures
	void SetFiltering(sbyte state) override;

	// Sets the state of zbuffering to on or off
	void SetZBufferState(sbyte state) override;

	// Sets the near and far planes for z buffer
	void SetZValues(float nearz, float farz) override;

	// Sets a bitmap as an overlay to rendered on top of the next texture map
	void SetOverlayMap(int handle) override;

	// Sets the type of overlay operation
	void SetOverlayType(ubyte type) override;

	// Sets the color of fog
	void SetFogColor(ddgr_color fogcolor) override;

	// sets the alpha type
	void SetAlphaType(sbyte alphatype) override;

	// Sets the constant alpha value
	void SetAlphaValue(ubyte val) override;

	// Sets the overall alpha scale factor (all alpha values are scaled by this value)
	// usefull for motion blur effect
	void SetAlphaFactor(float val) override;

	// Returns the current Alpha factor
	float GetAlphaFactor() override;

	// Sets the wrap parameter
	void SetWrapType(wrap_type val) override;


	// Sets some global preferences for the renderer
	// Returns -1 if it had to use the default resolution/bitdepth
	int SetPreferredState(renderer_preferred_state* pref_state) override;

	// Sets the hardware bias level for coplanar polygons
	// This helps reduce z buffer artifaces
	void SetCoplanarPolygonOffset(float factor) override;

	void SetCullFace(bool state) override;

	// color model
	void SetColorModel(color_model) override;

	void SetLighting(light_state) override;
	void SetPerPixelLightingDirection(const vector *lightdir) override;
	void SetPerPixelDynamicLighting(const vector *face_normal, int count,
		const renderer_per_pixel_light *lights) override;

	// Adds a bias to each coordinates z value.  This is useful for making 2d bitmaps
	// get drawn without being clipped by the zbuffer
	void SetZBias(float z_bias) override;

	// Enables/disables writes the depth buffer
	void SetZBufferWriteMask(int state) override;

	// Gets a bumpmap ready for drawing, or turns off bumpmapping
	void SetBumpmapReadyState(int state, int map) override;

	void SetGammaValue(float val) override;

	//INFORMATION

	// returns rendering statistics for the frame
	void GetStatistics(tRendererStats* stats) override;

	// Fills in some variables so the 3d math routines know how to project
	void GetProjectionParameters(int* width, int* height) override;
	void GetProjectionScreenParameters(int& screenLX, int& screenTY, int& screenW, int& screenH) override;

	// Returns the aspect ratio of the physical screen
	float GetAspectRatio() override;

	// Fills in the passed in pointer with the current rendering state
	void GetRenderState(rendering_state* rstate) override;

	// Fills in the passed in pointer with the current rendering state
	// Uses legacy structure for compatibiltity with current DLLs.
	void DLLGetRenderState(DLLrendering_state* rstate) override;

	// Returns 1 if there is mid video memory, 2 if there is low vid memory, or 0 if there is large vid memory
	int LowVidMem() override;

	// Returns 1 if the renderer supports bumpmapping
	int SupportsBumpmapping() override;

	//IMAGES

	// Preuploads a bitmap to the card
	void PreUploadTextureToCard(int, int) override;
	void FreePreUploadedTexture(int, int) override;

	// Clears the texture cache
	void ResetCache() override;

	//DRAWING

	// Tells the renderer we're starting a frame.  Clear flags tells the renderer
	// what buffer (if any) to clear
	void StartFrame(int x1, int y1, int x2, int y2, int clear_flags = RF_CLEAR_ZBUFFER) override;

	// Tells the renderer the frame is over
	void EndFrame() override;

	void CaptureBloomSource() override;
	void PerfGpuSceneMark(renderer_gpu_scene_mark mark) override;

	// Clears the display to a specified color
	void ClearScreen(ddgr_color color) override;

	// Clears the zbuffer
	void ClearZBuffer() override;

	// Given a handle to a bitmap and nv point vertices, draws a 3D polygon
	void DrawPolygon3D(int handle, g3Point** p, int nv, int map_type = MAP_TYPE_BITMAP) override;
	void DrawPolygon3DBatch(int handle, const renderer_poly_batch_item *items, int count,
		int map_type = MAP_TYPE_BITMAP) override;

	// Given a handle to a bitmap and nv point vertices, draws a 2D polygon
	void DrawPolygon2D(int handle, g3Point** p, int nv) override;

	void BeginMotionObject(int object_handle, float screen_x, float screen_y) override;
	void EndMotionObject() override;
	bool ProjectPreviousFramePoint(const vector *world_pos, float *screen_x, float *screen_y) override;
	void SetAOSuppression(float value) override;
	void SetBloomSuppression(float value) override;
	void SetAOClass(int value) override;
	void SetPostMaskOnly(int state) override;
	void SetCockpitBackingEffect(const renderer_cockpit_backing_effect *effect) override;

	// Draws a scaled 2d bitmap to our buffer
	// NOTE: scripts are expecting the old prototype that has a zvalue (which is ignored) before color
	void DrawScaledBitmap(int x1, int y1, int x2, int y2, int bm, float u0, float v0, float u1, float v1, int color = -1, float* alphas = nullptr) override;

	void DrawScaledBitmapWithZ(int x1, int y1, int x2, int y2, int bm, float u0, float v0, float u1, float v1, float zval, int color, float* alphas = nullptr) override;

	//	given a chunked bitmap, renders it.
	void DrawChunkedBitmap(chunked_bitmap* chunk, int x, int y, ubyte alpha) override;

	//	given a chunked bitmap, renders it.scaled
	void DrawScaledChunkedBitmap(chunked_bitmap* chunk, int x, int y, int neww, int newh, ubyte alpha) override;

	// Draws a simple bitmap at the specified x,y location
	void DrawSimpleBitmap(int bm_handle, int x, int y) override;

	// Fills a rectangle on the display
	void FillRect(ddgr_color color, int x1, int y1, int x2, int y2) override;

	// Sets a pixel on the display
	void SetPixel(ddgr_color color, int x, int y) override;

	// Sets a pixel on the display
	ddgr_color GetPixel(int x, int y) override;

	// Sets the argb characteristics of the font characters.  color1 is the upper left and proceeds clockwise
	void SetCharacterParameters(ddgr_color color1, ddgr_color color2, ddgr_color color3, ddgr_color color4) override;

	// Sets up a font character to draw.  We draw our fonts as pieces of textures
	void DrawFontCharacter(int bm_handle, int x1, int y1, int x2, int y2, float u, float v, float w, float h) override;
	void FlushTextLayer() override;

	// Draws a line
	void DrawLine(int x1, int y1, int x2, int y2) override;

	//	Draws spheres
	void FillCircle(ddgr_color col, int x, int y, int rad) override;

	//	draws circles
	void DrawCircle(int x, int y, int rad) override;

	// Flips the surface
	void Flip() override;
	bool BeginPostPresentFrame() override;
	bool IsPostPresentFramePending() const override;
	void StartPostPresentFrame(int x1, int y1, int x2, int y2, int clear_flags = RF_CLEAR_ZBUFFER) override;
	void EndPostPresentFrame() override;

	// Draws a line using the states of the renderer
	void DrawSpecialLine(g3Point* p0, g3Point* p1) override;

	//OTHER TRANSFERS

	// Takes a bitmap and blits it to the screen using linear frame buffer stuff
	// X and Y are the destination X,Y.
	void CopyBitmapToFramebuffer(int bm_handle, int x, int y) override;

	// Gets a renderer ready for a framebuffer copy, or stops a framebuffer copy
	void SetFrameBufferCopyState(bool state) override;

	// Takes a screenshot of the current frame and puts it into the handle passed
	void Screenshot(int bm_handle) override;
	int SaveScreenshotPNG(const char* filename) override;

	//NEW STATE

	void UpdateCommon(float* projection, float* modelview, int depth = 0) override;
	void SetCommonDepth(int depth) override;

	//Gets a handle to a shader by name
	uint32_t GetPipelineByName(const char* name) override;

	//Given a handle from rend_GetShaderByName, binds that particular pipeline object
	void BindPipeline(uint32_t handle) override;

	//Updates specular components
	void UpdateSpecular(SpecularBlock* specularstate) override;

	//Updates brightness/fog components
	void UpdateFogBrightness(RoomBlock* roomstate, int numrooms) override;
	void SetCurrentRoomNum(int roomblocknum) override;
	void UpdateTerrainFog(float color[4], float start, float end) override;

	//These are temporary, used to test shader code.
	//Use the test shader.
	void UseShaderTest() override;

	//Revert to non-shader rendering
	void EndShaderTest() override;

	void BindBitmap(int handle) override;
	void BindLightmap(int handle) override;

	void RestoreLegacy() override;

	void GetScreenSize(int& screen_width, int& screen_height) override;
	double GetDisplayRefreshRate() override;

	void ClearBoundTextures() override;
};

#define GL_DEBUG

#define GET_WRAP_STATE(x)	((x>>2) & 0x03)
#define GET_MIP_STATE(x)	((x>>1) & 0x01);
#define GET_FILTER_STATE(x)	(x & 0x01)

#define SET_WRAP_STATE(x,s) {x&=0xF3; x|=(s<<2);}
#define SET_MIP_STATE(x,s) {x&=0xFD; x|=(s<<1);}
#define SET_FILTER_STATE(x,s) {x&=0xFE; x|=(s);}

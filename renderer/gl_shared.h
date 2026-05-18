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

//gl_shared.h
//Things shared by both the compatibility and core implementations.

#pragma once
#include <glad/gl.h>
#include "pserror.h"

//uncomment to express your love for the best graphics API ever designed and enable extra error checking to show your love.
//#define I_LOVE_OPENGL

#ifdef I_LOVE_OPENGL
constexpr void CHECK_ERROR(int n) //need to decide what it does.
{
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
	{
		mprintf((1, "GL Error in context %d: %x\n", n, err));
		Int3();
	}
}
#else
#define CHECK_ERROR(n)
#endif

struct CommonBlock
{
	float projection[16];
	float modelview[16];
	float pad[32];
};

//Shader definition nonsense
constexpr int SF_HASCOMMON = 1; //Shader will use the common block
constexpr int SF_HASROOM = 2; //Shader will use the room (fog, brightness) block
constexpr int SF_HASSPECULAR = 4; //Shader will use the specular block

struct ShaderDefinition
{
	const char* name;
	int flags; //see SF_ flags above
	const char* vertex_filename;
	const char* fragment_filename;
};

class ShaderProgram
{
	GLuint m_name;
	GLint m_dynamic_light_count = -1;
	GLint m_dynamic_face_normal = -1;
	GLint m_dynamic_light_positions = -1;
	GLint m_dynamic_light_colors = -1;
	GLint m_dynamic_light_radii = -1;
	GLint m_dynamic_light_directions = -1;
	GLint m_dynamic_light_dot_ranges = -1;
	GLint m_dynamic_light_directional = -1;
	int m_last_dynamic_light_count = -1;

	//CreateCommonBindings will find common uniforms and set their default bindings.
	//This includes the common block, which must be named "common",
	//and sampler2Ds named "colortexture", "lightmaptexture", and others later.
	void CreateCommonBindings(int bindindex);
public:
	ShaderProgram()
	{
		m_name = 0;
	}

	void AttachSource(const char* vertexsource, const char* fragsource);
	void AttachSourceFromDefiniton(ShaderDefinition& def);
	//Attaches strings with some preprocessor statements.
	//Defines USE_TEXTURING if textured is true.
	//Defines USE_LIGHTMAP if lightmapped is true.
	//Defines USE_SPECULAR if speculared is true.
	void AttachSourcePreprocess(const char* vertexsource, const char* fragsource, bool textured, bool lightmapped, bool speculared, bool fogged);
	GLint FindUniform(const char* uniform);
	void Destroy();

	void Use();
	void ApplyDynamicLighting(int count, const float* face_normal, const GLfloat* positions,
		const GLfloat* colors, const GLfloat* radii, const GLfloat* directions,
		const GLfloat* dot_ranges, const GLint* directional);

	//Replacement for glUseProgram(0) that nulls the last binding.
	static void ClearBinding();
	static ShaderProgram* Current();

	GLuint Handle() const
	{
		return m_name;
	}
};

class Framebuffer
{
	GLuint		m_name, m_subname;
	GLuint		m_colorname, m_subcolorname, m_depthname, m_subdepthname;
	uint32_t	m_width, m_height;
	uint32_t	m_samples;
	uint32_t	m_requested_samples;
	bool		m_msaa_renderbuffer_storage;
	//Track whether the MSAA->sub resolve is up to date. Each MSAA resolve blit
	//costs W*H*samples of bandwidth, and multiple post-process stages can ask
	//for the resolved depth/color in a single frame. We skip the blit when the
	//cached sub framebuffer already matches the MSAA contents.
	bool		m_subcolor_dirty;
	bool		m_subdepth_dirty;

	bool Allocate(int width, int height, int msaa_samples);
	//Used when multisampling is enabled. Blits the requested buffers to the non-multisample sub framebuffer.
	//Leaves the sub framebuffer bound for reading to finish the blit.
	void SubFramebufferBlit(GLbitfield mask);
public:
	Framebuffer();
	void Update(int width, int height, int msaa_samples);
	void Destroy();
	//Mark the resolved sub framebuffer as stale. Call after writing to the MSAA
	//attachments so the next read triggers a fresh resolve.
	void MarkColorDirty() { m_subcolor_dirty = true; }
	void MarkDepthDirty() { m_subdepth_dirty = true; }
	void MarkAllDirty() { m_subcolor_dirty = true; m_subdepth_dirty = true; }
	//Clears only the color attachment alpha channel. Used after the scene
	//capture so later cockpit/HUD draws can accumulate overlay coverage.
	void ClearAlphaToZero();
	//Blits to the target framebuffer using glBlitFramebuffer.
	//Will set current read framebuffer to m_name.
	void BlitToRaw(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h, GLenum filter = GL_NEAREST);
	//Blits the depth buffer to the target framebuffer with nearest-neighbor scaling.
	//Resolves multisampling first if needed.
	void BlitDepthTo(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h);
	//Blits to the target framebuffer using a draw. Bind desired shader before calling.
	//Will set current read framebuffer to m_name. Will not trash viewport.
	void BlitTo(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h, bool linear_filter = false);
	void DownsampleTo(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		GLint gamma_uniform, float gamma, GLint dest_origin_uniform,
		GLint source_visible_origin_uniform = -1, GLint source_visible_size_uniform = -1,
		int source_visible_x = 0, int source_visible_y = 0, int source_visible_w = 0, int source_visible_h = 0);

	//When called without MSAA, just binds m_name to the read slot.
	//When called with MSAA, will resolve to m_subname and bind that.
	void BindForRead();
	GLuint ColorTextureForRead();
	GLuint DepthTextureForRead();
	GLuint ColorTextureRaw() const;
	GLuint DepthTextureRaw() const;
	void ClearAll();

	GLuint Handle() const
	{
		return m_name;
	}

	uint32_t Width() const
	{
		return m_width;
	}

	uint32_t Height() const
	{
		return m_height;
	}

	uint32_t Samples() const
	{
		return m_samples;
	}

	uint32_t RequestedSamples() const
	{
		return m_requested_samples;
	}

	bool UsesMsaaRenderbufferStorage() const
	{
		return m_msaa_renderbuffer_storage;
	}
};

class ColorFramebuffer
{
	GLuint m_name = 0;
	GLuint m_colorname = 0;
	uint32_t m_width = 0;
	uint32_t m_height = 0;

public:
	void Update(int width, int height, GLint internal_format, GLenum format, GLenum type);
	void Destroy();
	GLuint Handle() const
	{
		return m_name;
	}
	GLuint ColorTextureForRead() const
	{
		return m_colorname;
	}
	uint32_t Width() const
	{
		return m_width;
	}
	uint32_t Height() const
	{
		return m_height;
	}
};

struct MotionVectorResources
{
	GLuint velocity_texture = 0;
	GLuint resolved_texture = 0;
	GLuint resolve_framebuffer = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t samples = 0;

	void Update(uint32_t width, uint32_t height, uint32_t msaa_samples);
	void Destroy();
	void AttachToFramebuffer(GLuint framebuffer);
	void ClearAttached(GLuint framebuffer);
	GLuint TextureForRead(GLuint source_framebuffer);
};

struct PostProtectionMaskResources
{
	GLuint mask_texture = 0;
	GLuint resolved_texture = 0;
	GLuint resolve_framebuffer = 0;
	GLuint ao_class_texture = 0;
	GLuint ao_class_resolved_texture = 0;
	GLuint ao_class_resolve_framebuffer = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t samples = 0;
	bool msaa_renderbuffer_storage = false;

	void Update(uint32_t width, uint32_t height, uint32_t msaa_samples);
	void Destroy();
	void AttachToFramebuffer(GLuint framebuffer);
	void ClearAttached(GLuint framebuffer);
	void UseSceneDrawBuffers(GLuint framebuffer);
	GLuint TextureForRead(GLuint source_framebuffer);
	GLuint AOClassTextureForRead(GLuint source_framebuffer);
};

void GL_BindFramebufferTexture(GLuint texture, int unit, GLenum filter);
void GL_UnbindFramebufferTextures();
void GL_ConfigurePostMaskBlend();
void GL_DrawFramebufferQuad(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h);
void GL_DrawFramebufferQuadNoClear(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h);
int GL_GetSupportedMsaaSamples(int requested_samples);
//Lazy accessor for the framebuffer fullscreen-triangle VAO. Initialises it if
//needed. Use this when you need to draw a fullscreen triangle to a target you
//have already bound yourself (and do not want GL_DrawFramebufferQuad's clear).
GLuint GL_GetFramebufferVAO();

constexpr int NUM_BLOOM_FBOS = 16;
struct BloomResources
{
	Framebuffer framebuffers[NUM_BLOOM_FBOS];
	ShaderProgram thresholdshader;
	ShaderProgram downsampleshader;
	ShaderProgram mergeshader;
	ShaderProgram compositeshader;
	GLint threshold_gamma = -1;
	GLint threshold_value = -1;
	GLint threshold_use_depth_mask = -1;
	GLint threshold_use_protection_mask = -1;
	GLint threshold_source_origin = -1;
	GLint threshold_source_visible_size = -1;
	GLint merge_spread = -1;
	GLint composite_gamma = -1;
	GLint composite_intensity = -1;
	GLint composite_use_alpha_mask = -1;
	GLint composite_use_protection_mask = -1;
	GLint composite_uv_origin = -1;
	GLint composite_uv_scale = -1;
	GLint composite_source_origin = -1;

	void InitShaders();
	void DestroyShaders();
	void DestroyFramebuffers();
	Framebuffer* Apply(Framebuffer* source, const renderer_preferred_state& pref_state,
		const rendering_state& render_state, float display_gamma, GLuint depth_texture, GLuint protection_mask_texture,
		int visible_x = 0, int visible_y = 0, int visible_w = 0, int visible_h = 0);
};

//Ground-Truth Ambient Occlusion. Renders depth-driven AO, then modulates the
//scene color in place.
//Reconstructs view-space normals from the depth buffer (we have no normals G-buffer).
struct GTAOResources
{
	//Raw source depth resolved/downsampled to the AO working resolution.
	ColorFramebuffer ao_depth_framebuffer;
	//Framebuffer holding the AO result (RG16F: x=AO, y=depth).
	ColorFramebuffer ao_framebuffer;
	//Scratch used as ping-pong for the separable bilateral blur.
	ColorFramebuffer ao_blur_framebuffer;
	ColorFramebuffer suppression_framebuffer;

	GLuint noise_texture = 0;

	ShaderProgram depth_shader;
	ShaderProgram ao_shader;
	ShaderProgram blur_x_shader;
	ShaderProgram blur_y_shader;
	ShaderProgram suppression_shader;
	ShaderProgram apply_shader;

	//Depth downsample/resolve shader uniforms.
	GLint depth_source = -1;
	GLint depth_source_ms = -1;
	GLint depth_ao_class = -1;
	GLint depth_has_ao_class = -1;
	GLint depth_ao_weight_is_direct = -1;
	GLint depth_samples = -1;
	GLint depth_input_screen_size = -1;
	GLint depth_ao_screen_size = -1;
	GLint depth_terrain_occlusion = -1;
	GLint depth_polyobject_occlusion = -1;
	GLint depth_mine_rock_occlusion = -1;
	GLint depth_mine_occlusion = -1;

	//AO shader uniforms.
	GLint ao_proj_info = -1;        //(2/proj[0], 2/proj[5], -1/proj[0], -1/proj[5])
	GLint ao_near_far = -1;         //(nearZ, farZ)
	GLint ao_radius = -1;           //world-space radius
	GLint ao_radius_pixels = -1;    //radius converted to half-screen-height pixels at depth 1
	GLint ao_max_radius_pixels = -1;
	GLint ao_neg_inv_radius2 = -1;  //-1/(r*r)
	GLint ao_angle_bias = -1;
	GLint ao_inv_screen_size = -1;  //1/width, 1/height
	GLint ao_ao_inv_screen_size = -1;
	GLint ao_screen_size = -1;      //width, height
	GLint ao_noise_origin = -1;
	GLint ao_directions = -1;
	GLint ao_steps = -1;
	GLint ao_terrain_occlusion = -1;
	GLint ao_polyobject_occlusion = -1;
	GLint ao_mine_rock_occlusion = -1;
	GLint ao_mine_occlusion = -1;

	//Blur shader uniforms (same for both x and y).
	GLint blur_x_delta = -1;
	GLint blur_x_sharpness = -1;
	GLint blur_x_radius = -1;
	GLint blur_y_delta = -1;
	GLint blur_y_sharpness = -1;
	GLint blur_y_radius = -1;

	//Suppression mask shader uniforms.
	GLint suppression_existing_mask = -1;
	GLint suppression_existing_mask_ms = -1;
	GLint suppression_color = -1;
	GLint suppression_color_ms = -1;
	GLint suppression_has_mask = -1;
	GLint suppression_use_bloom_mask = -1;
	GLint suppression_source_samples = -1;
	GLint suppression_input_screen_size = -1;
	GLint suppression_ao_screen_size = -1;
	GLint suppression_source_visible_origin = -1;
	GLint suppression_source_visible_size = -1;
	GLint suppression_use_source_visible_rect = -1;
	GLint suppression_gamma = -1;
	GLint suppression_bloom_threshold = -1;

	//Apply (composite) shader uniforms.
	GLint apply_intensity = -1;
	GLint apply_has_mask = -1;
	GLint apply_ao_uv_origin = -1;
	GLint apply_ao_uv_scale = -1;

	void InitShaders();
	void DestroyShaders();
	bool HasFramebuffers() const;
	void DestroyFramebuffers();
	void Destroy();
	//Computes AO for the supplied source framebuffer (which must have valid
	//color + depth). Modulates target->color, or source->color when target is
	//null, by sampling pref_state and projection info.
	void Apply(Framebuffer* source, Framebuffer* target, const renderer_preferred_state& pref_state,
		const rendering_state& render_state, const float* projection,
		float nearz, float farz, GLuint suppression_mask_texture, GLuint ao_weight_texture,
		bool ao_weight_is_direct,
		int source_visible_x = 0, int source_visible_y = 0,
		int source_visible_w = 0, int source_visible_h = 0,
		int noise_origin_x = 0, int noise_origin_y = 0);
};

inline int RendererSupersamplingFactor(const renderer_preferred_state& state)
{
	if (state.supersampling_factor >= 4)
		return 4;
	if (state.supersampling_factor >= 2)
		return 2;
	return 1;
}

inline int RendererMsaaSamples(const renderer_preferred_state& state)
{
	if (state.msaa_samples >= 8)
		return 8;
	if (state.msaa_samples >= 4)
		return 4;
	if (state.msaa_samples >= 2)
		return 2;
	return state.antialised ? 4 : 0;
}

#if defined(WIN32) && !defined(SDL3)
extern PFNWGLSWAPINTERVALEXTPROC dwglSwapIntervalEXT;
extern PFNWGLGETSWAPINTERVALEXTPROC dwglGetSwapIntervalEXT;
extern PFNWGLCREATECONTEXTATTRIBSARBPROC dwglCreateContextAttribsARB;
#endif
extern bool Already_loaded;

//A dumb bit of global state that needs to be fixed
void GL_InitFramebufferVAO();

void GL_DestroyFramebufferVAO();

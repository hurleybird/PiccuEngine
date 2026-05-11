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
#include "gl_local.h"
#include <math.h>
#include <stdint.h>

namespace
{
	//Mersenne-Twister-derived noise values (lifted from the bundled HBAO
	//Unity reference). Two channels: (cos-of-angle-ish, jitter).
	const float kMtNoise[32] =
	{
		0.556725f, 0.005520f, 0.708315f, 0.583199f, 0.236644f, 0.992380f, 0.981091f, 0.119804f,
		0.510866f, 0.560499f, 0.961497f, 0.557862f, 0.539955f, 0.332871f, 0.417807f, 0.920779f,
		0.730747f, 0.076690f, 0.008562f, 0.660104f, 0.428921f, 0.511342f, 0.587871f, 0.906406f,
		0.437980f, 0.620309f, 0.062196f, 0.119485f, 0.235646f, 0.795892f, 0.044437f, 0.617311f,
	};

	int QualityDirections(int q)
	{
		switch (q)
		{
		case HBAO_QUALITY_LOW: return 4;
		case HBAO_QUALITY_HIGH: return 8;
		case HBAO_QUALITY_MEDIUM:
		default: return 6;
		}
	}

	int QualitySteps(int q)
	{
		switch (q)
		{
		case HBAO_QUALITY_LOW: return 3;
		case HBAO_QUALITY_HIGH: return 4;
		case HBAO_QUALITY_MEDIUM:
		default: return 4;
		}
	}

	int BlurKernelRadius(int b)
	{
		switch (b)
		{
		case HBAO_BLUR_NARROW: return 2;
		case HBAO_BLUR_MEDIUM: return 3;
		case HBAO_BLUR_WIDE: return 4;
		case HBAO_BLUR_NONE:
		default: return 0;
		}
	}
}

void HBAOResources::InitShaders()
{
	extern const char* blitVertexSrc;
	extern const char* hbaoAOFragmentSrc;
	extern const char* hbaoBlurFragmentSrc;
	extern const char* hbaoApplyFragmentSrc;

	ao_shader.AttachSource(blitVertexSrc, hbaoAOFragmentSrc);
	ao_shader.Use();
	GLint depth_loc = ao_shader.FindUniform("depth_tex");
	GLint noise_loc = ao_shader.FindUniform("noise_tex");
	if (depth_loc != -1) glUniform1i(depth_loc, 0);
	if (noise_loc != -1) glUniform1i(noise_loc, 1);
	ao_proj_info = ao_shader.FindUniform("proj_info");
	ao_near_far = ao_shader.FindUniform("near_far");
	ao_radius = ao_shader.FindUniform("radius");
	ao_radius_pixels = ao_shader.FindUniform("radius_pixels");
	ao_max_radius_pixels = ao_shader.FindUniform("max_radius_pixels");
	ao_neg_inv_radius2 = ao_shader.FindUniform("neg_inv_radius2");
	ao_angle_bias = ao_shader.FindUniform("angle_bias");
	ao_multiplier = ao_shader.FindUniform("ao_multiplier");
	ao_intensity = ao_shader.FindUniform("intensity");
	ao_inv_screen_size = ao_shader.FindUniform("inv_screen_size");
	ao_screen_size = ao_shader.FindUniform("screen_size");
	ao_temporal = ao_shader.FindUniform("temporal");
	ao_noise_scale = ao_shader.FindUniform("noise_scale");
	ao_directions = ao_shader.FindUniform("directions");
	ao_steps = ao_shader.FindUniform("steps");

	blur_x_shader.AttachSource(blitVertexSrc, hbaoBlurFragmentSrc);
	blur_x_shader.Use();
	GLint blur_x_heh = blur_x_shader.FindUniform("heh");
	if (blur_x_heh != -1) glUniform1i(blur_x_heh, 0);
	blur_x_delta = blur_x_shader.FindUniform("blur_delta");
	blur_x_sharpness = blur_x_shader.FindUniform("sharpness");
	blur_x_radius = blur_x_shader.FindUniform("kernel_radius");

	blur_y_shader.AttachSource(blitVertexSrc, hbaoBlurFragmentSrc);
	blur_y_shader.Use();
	GLint blur_y_heh = blur_y_shader.FindUniform("heh");
	if (blur_y_heh != -1) glUniform1i(blur_y_heh, 0);
	blur_y_delta = blur_y_shader.FindUniform("blur_delta");
	blur_y_sharpness = blur_y_shader.FindUniform("sharpness");
	blur_y_radius = blur_y_shader.FindUniform("kernel_radius");

	apply_shader.AttachSource(blitVertexSrc, hbaoApplyFragmentSrc);
	apply_shader.Use();
	GLint apply_heh = apply_shader.FindUniform("heh");
	if (apply_heh != -1) glUniform1i(apply_heh, 0);
	apply_intensity = apply_shader.FindUniform("intensity");

	ShaderProgram::ClearBinding();

	//Build the 4x4 noise texture. Each texel holds two random scalars used
	//for rotating the AO sampling directions and for the per-pixel ray jitter.
	if (noise_texture == 0)
	{
		glGenTextures(1, &noise_texture);
		glBindTexture(GL_TEXTURE_2D, noise_texture);
		uint8_t pixels[4 * 4 * 2];
		for (int i = 0; i < 16; i++)
		{
			float r = kMtNoise[(i * 2) % 32];
			float g = kMtNoise[(i * 2 + 1) % 32];
			pixels[i * 2 + 0] = (uint8_t)(r * 255.0f);
			pixels[i * 2 + 1] = (uint8_t)(g * 255.0f);
		}
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, 4, 4, 0, GL_RG, GL_UNSIGNED_BYTE, pixels);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
}

void HBAOResources::DestroyShaders()
{
	ao_shader.Destroy();
	blur_x_shader.Destroy();
	blur_y_shader.Destroy();
	apply_shader.Destroy();
	if (noise_texture)
	{
		glDeleteTextures(1, &noise_texture);
		noise_texture = 0;
	}
}

void HBAOResources::DestroyFramebuffers()
{
	ao_framebuffer.Destroy();
	ao_blur_framebuffer.Destroy();
}

void HBAOResources::Destroy()
{
	DestroyFramebuffers();
	DestroyShaders();
}

void HBAOResources::Apply(Framebuffer* source, const renderer_preferred_state& pref_state,
	const rendering_state& render_state, const float* projection,
	float nearz, float farz)
{
	if (!source || !pref_state.hbao_enabled)
		return;

	int width = (int)source->Width();
	int height = (int)source->Height();
	if (width <= 0 || height <= 0)
		return;

	//Match the AO buffer to the source resolution.
	ao_framebuffer.Update(width, height, 0);
	ao_blur_framebuffer.Update(width, height, 0);

	//Save GL state we are about to trample so we leave the caller in
	//the same state we found it in (matters for follow-up draw calls).
	GLint old_read = 0, old_draw = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
	GLint old_viewport[4];
	glGetIntegerv(GL_VIEWPORT, old_viewport);
	GLboolean blend_enabled = glIsEnabled(GL_BLEND);
	GLboolean depth_enabled = glIsEnabled(GL_DEPTH_TEST);
	GLboolean cull_enabled = glIsEnabled(GL_CULL_FACE);
	GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean multisample_enabled = glIsEnabled(GL_MULTISAMPLE);
	GLint blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO;
	GLint blend_src_alpha = GL_ONE, blend_dst_alpha = GL_ZERO;
	glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
	GLboolean depth_mask = GL_TRUE;
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
	GLboolean color_mask[4];
	glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);

	//Pull the resolved depth texture from the source framebuffer. When MSAA is
	//on this triggers a blit into the sub framebuffer; that's fine because the
	//main framebuffer is still drawn on for HUD work afterwards.
	GLuint depth_texture = source->DepthTextureForRead();

	//Compute uniform values.
	float m00 = projection[0];
	float m11 = projection[5];
	if (fabsf(m00) < 1e-6f) m00 = 1.0f;
	if (fabsf(m11) < 1e-6f) m11 = 1.0f;

	//Sanitize prefs into shader-friendly numbers.
	int quality = pref_state.hbao_quality;
	if (quality < 0) quality = 0;
	if (quality > HBAO_QUALITY_HIGH) quality = HBAO_QUALITY_HIGH;
	int directions = QualityDirections(quality);
	int steps = QualitySteps(quality);

	float radius = pref_state.hbao_radius;
	if (radius < 0.5f) radius = 0.5f;
	if (radius > 32.0f) radius = 32.0f;

	float bias = pref_state.hbao_bias;
	if (bias < 0.0f) bias = 0.0f;
	if (bias > 0.5f) bias = 0.5f;

	float intensity = pref_state.hbao_intensity;
	if (intensity < 0.0f) intensity = 0.0f;
	if (intensity > 4.0f) intensity = 4.0f;

	//Convert world-space radius to a pixel-space scale at depth==1. The shader
	//then divides by per-pixel depth to get the on-screen step size. This is
	//equivalent to the Unity reference: r_pixels = radius * 0.5 * (h / tan(fov/2)).
	//Since m11 = 1 / tan(half_y_fov_with_aspect_compensation), pixel size is:
	float radius_pixels = radius * 0.5f * (float)height * m11;
	float max_radius_pixels = 128.0f * sqrtf((float)(width * height) / (1080.0f * 1920.0f));
	if (max_radius_pixels < 16.0f) max_radius_pixels = 16.0f;

	float neg_inv_r2 = -1.0f / (radius * radius);
	float multiplier = (2.0f * (1.0f / (1.0f - bias))) / (float)(steps * directions);

	//Keep AO sampling stable frame-to-frame. The old temporal rotation/jitter
	//changed the sampling pattern every frame without any accumulation pass,
	//which showed up as visible flicker.
	float rotation = 0.0f;
	float jitter = 0.0f;

	//-------------------------------------------------------------------------
	// Pass 1: AO into ao_framebuffer.
	//-------------------------------------------------------------------------
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_MULTISAMPLE);
	glDepthMask(GL_FALSE);

	ao_shader.Use();
	glUniform4f(ao_proj_info,
		2.0f / m00,
		2.0f / m11,
		-1.0f / m00,
		-1.0f / m11);
	glUniform2f(ao_near_far, nearz, farz);
	glUniform1f(ao_radius, radius);
	glUniform1f(ao_radius_pixels, radius_pixels);
	glUniform1f(ao_max_radius_pixels, max_radius_pixels);
	glUniform1f(ao_neg_inv_radius2, neg_inv_r2);
	glUniform1f(ao_angle_bias, bias);
	glUniform1f(ao_multiplier, multiplier);
	glUniform1f(ao_intensity, intensity);
	glUniform2f(ao_inv_screen_size, 1.0f / (float)width, 1.0f / (float)height);
	glUniform2f(ao_screen_size, (float)width, (float)height);
	glUniform2f(ao_temporal, rotation, jitter);
	glUniform2f(ao_noise_scale, (float)width / 4.0f, (float)height / 4.0f);
	glUniform1i(ao_directions, directions);
	glUniform1i(ao_steps, steps);

	rend_ClearBoundTextures();
	GL_BindFramebufferTexture(depth_texture, 0, GL_NEAREST);
	GL_BindFramebufferTexture(noise_texture, 1, GL_NEAREST);
	GL_DrawFramebufferQuad(ao_framebuffer.Handle(), 0, 0, width, height);

	//-------------------------------------------------------------------------
	// Pass 2: Optional separable bilateral blur (X then Y).
	//-------------------------------------------------------------------------
	int blur_radius = BlurKernelRadius(pref_state.hbao_blur);
	Framebuffer* blurred = &ao_framebuffer;

	if (blur_radius > 0)
	{
		//AO shader outputs depth as P.z / farz, so .y lives in [0, 1].
		//The blur weight = exp2(-r^2/(2*sigma^2) - (sharpness * dz)^2). At
		//sharpness=50 the bilateral cutoff sits around dz=0.02 (i.e. 200
		//world units at far=10000), which preserves AO at room edges while
		//still smoothing across coplanar surfaces.
		const float sharpness = 50.0f;
		(void)farz;

		blur_x_shader.Use();
		glUniform2f(blur_x_delta, 1.0f / (float)width, 0.0f);
		glUniform1f(blur_x_sharpness, sharpness);
		glUniform1i(blur_x_radius, blur_radius);
		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(ao_framebuffer.ColorTextureForRead(), 0, GL_LINEAR);
		GL_DrawFramebufferQuad(ao_blur_framebuffer.Handle(), 0, 0, width, height);

		blur_y_shader.Use();
		glUniform2f(blur_y_delta, 0.0f, 1.0f / (float)height);
		glUniform1f(blur_y_sharpness, sharpness);
		glUniform1i(blur_y_radius, blur_radius);
		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(ao_blur_framebuffer.ColorTextureForRead(), 0, GL_LINEAR);
		GL_DrawFramebufferQuad(ao_framebuffer.Handle(), 0, 0, width, height);

		blurred = &ao_framebuffer;
	}

	//-------------------------------------------------------------------------
	// Pass 3: Modulate scene color in-place.
	//
	// Draw a fullscreen quad to the source framebuffer with blend mode
	// (DST_COLOR, ZERO) so the destination is multiplied by our AO factor.
	// MSAA is re-enabled while writing so the rasterizer covers every sample.
	//-------------------------------------------------------------------------
	apply_shader.Use();
	glUniform1f(apply_intensity, intensity);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, source->Handle());
	glViewport(0, 0, width, height);

	glEnable(GL_BLEND);
	glBlendFunc(GL_DST_COLOR, GL_ZERO);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDepthMask(GL_FALSE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	if (multisample_enabled)
		glEnable(GL_MULTISAMPLE);

	rend_ClearBoundTextures();
	GL_BindFramebufferTexture(blurred->ColorTextureForRead(), 0, GL_LINEAR);

	//Drawing the fullscreen triangle straight (we can't use GL_DrawFramebufferQuad
	//since it clears, and we want to multiply against the existing destination).
	glBindVertexArray(GL_GetFramebufferVAO());
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	//Restore prior GL state.
	glBlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha);
	if (!blend_enabled) glDisable(GL_BLEND); else glEnable(GL_BLEND);
	if (depth_enabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (cull_enabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
	if (scissor_enabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	if (multisample_enabled) glEnable(GL_MULTISAMPLE); else glDisable(GL_MULTISAMPLE);
	glDepthMask(depth_mask);
	glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);

	ShaderProgram::ClearBinding();

	glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
	glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);

	rend_ClearBoundTextures();
	rend_RestoreLegacy();
}

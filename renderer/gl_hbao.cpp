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

	float Clamp01(float value)
	{
		if (value < 0.0f)
			return 0.0f;
		if (value > 1.0f)
			return 1.0f;
		return value;
	}
}

void HBAOResources::InitShaders()
{
	extern const char* blitVertexSrc;
	extern const char* hbaoAOFragmentSrc;
	extern const char* hbaoBlurFragmentSrc;
	extern const char* hbaoTemporalFragmentSrc;
	extern const char* hbaoSuppressionFragmentSrc;
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

	temporal_shader.AttachSource(blitVertexSrc, hbaoTemporalFragmentSrc);
	temporal_shader.Use();
	temporal_current_ao = temporal_shader.FindUniform("current_ao_tex");
	temporal_history = temporal_shader.FindUniform("history_tex");
	temporal_depth = temporal_shader.FindUniform("depth_tex");
	temporal_motion = temporal_shader.FindUniform("motion_tex");
	if (temporal_current_ao != -1) glUniform1i(temporal_current_ao, 0);
	if (temporal_history != -1) glUniform1i(temporal_history, 1);
	if (temporal_depth != -1) glUniform1i(temporal_depth, 2);
	if (temporal_motion != -1) glUniform1i(temporal_motion, 3);
	temporal_current_inv_view_projection = temporal_shader.FindUniform("current_inv_view_projection");
	temporal_previous_view_projection = temporal_shader.FindUniform("previous_view_projection");
	temporal_has_history = temporal_shader.FindUniform("has_history");
	temporal_has_motion = temporal_shader.FindUniform("has_motion");
	temporal_history_weight = temporal_shader.FindUniform("history_weight");

	suppression_shader.AttachSource(blitVertexSrc, hbaoSuppressionFragmentSrc);
	suppression_shader.Use();
	suppression_existing_mask = suppression_shader.FindUniform("existing_mask");
	suppression_color = suppression_shader.FindUniform("color_tex");
	if (suppression_existing_mask != -1) glUniform1i(suppression_existing_mask, 0);
	if (suppression_color != -1) glUniform1i(suppression_color, 1);
	suppression_has_mask = suppression_shader.FindUniform("has_mask");
	suppression_use_bloom_mask = suppression_shader.FindUniform("use_bloom_mask");
	suppression_gamma = suppression_shader.FindUniform("gamma");
	suppression_bloom_threshold = suppression_shader.FindUniform("bloom_threshold");

	apply_shader.AttachSource(blitVertexSrc, hbaoApplyFragmentSrc);
	apply_shader.Use();
	GLint apply_heh = apply_shader.FindUniform("heh");
	GLint apply_mask = apply_shader.FindUniform("suppression_mask");
	if (apply_heh != -1) glUniform1i(apply_heh, 0);
	if (apply_mask != -1) glUniform1i(apply_mask, 1);
	apply_intensity = apply_shader.FindUniform("intensity");
	apply_has_mask = apply_shader.FindUniform("has_suppression_mask");

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
	temporal_shader.Destroy();
	suppression_shader.Destroy();
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
	temporal_framebuffers[0].Destroy();
	temporal_framebuffers[1].Destroy();
	suppression_framebuffer.Destroy();
	InvalidateHistory();
}

void HBAOResources::Destroy()
{
	DestroyFramebuffers();
	DestroyShaders();
}

void HBAOResources::InvalidateHistory()
{
	temporal_valid = false;
	temporal_settings_valid = false;
}

void HBAOResources::Apply(Framebuffer* source, const renderer_preferred_state& pref_state,
	const rendering_state& render_state, const float* projection,
	float nearz, float farz, GLuint motion_texture, GLuint suppression_mask_texture,
	const float* current_inv_view_projection,
	const float* previous_view_projection,
	bool has_previous_view_projection)
{
	(void)render_state;
	if (!source || !pref_state.hbao_enabled)
	{
		InvalidateHistory();
		return;
	}

	int width = (int)source->Width();
	int height = (int)source->Height();
	if (width <= 0 || height <= 0)
		return;

	//Match the AO buffers to the source resolution. These store AO plus
	//linear depth; half-float depth avoids depth-edge ghosts from 8-bit
	//quantization in blur and temporal rejection.
	ao_framebuffer.Update(width, height, GL_RG16F, GL_RG, GL_FLOAT);
	ao_blur_framebuffer.Update(width, height, GL_RG16F, GL_RG, GL_FLOAT);

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
	GLuint scene_color_texture = source->ColorTextureForRead();

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

	int blur = pref_state.hbao_blur;
	if (!temporal_settings_valid ||
		temporal_enabled != pref_state.hbao_temporal ||
		temporal_quality != quality ||
		temporal_blur != blur ||
		temporal_radius != radius ||
		temporal_intensity != intensity ||
		temporal_bias != bias)
	{
		temporal_valid = false;
		temporal_settings_valid = true;
		temporal_enabled = pref_state.hbao_temporal;
		temporal_quality = quality;
		temporal_blur = blur;
		temporal_radius = radius;
		temporal_intensity = intensity;
		temporal_bias = bias;
	}

	//Convert world-space radius to a pixel-space scale at depth==1. The shader
	//then divides by per-pixel depth to get the on-screen step size. This is
	//equivalent to the Unity reference: r_pixels = radius * 0.5 * (h / tan(fov/2)).
	//Since m11 = 1 / tan(half_y_fov_with_aspect_compensation), pixel size is:
	float radius_pixels = radius * 0.5f * (float)height * m11;
	float max_radius_pixels = 128.0f * sqrtf((float)(width * height) / (1080.0f * 1920.0f));
	if (max_radius_pixels < 16.0f) max_radius_pixels = 16.0f;

	float neg_inv_r2 = -1.0f / (radius * radius);
	float multiplier = (2.0f * (1.0f / (1.0f - bias))) / (float)(steps * directions);

	static const float kTemporalRotations[8] =
	{
		0.0f / 8.0f, 5.0f / 8.0f, 3.0f / 8.0f, 7.0f / 8.0f,
		1.0f / 8.0f, 4.0f / 8.0f, 2.0f / 8.0f, 6.0f / 8.0f,
	};
	static const float kTemporalJitters[8] =
	{
		0.5f / 8.0f, 4.5f / 8.0f, 2.5f / 8.0f, 6.5f / 8.0f,
		1.5f / 8.0f, 5.5f / 8.0f, 3.5f / 8.0f, 7.5f / 8.0f,
	};
	int temporal_phase = frame_counter & 7;
	float rotation = kTemporalRotations[temporal_phase];
	float jitter = kTemporalJitters[temporal_phase];
	if (!pref_state.hbao_temporal)
	{
		rotation = 0.0f;
		jitter = 0.5f;
		temporal_valid = false;
	}
	else
	{
		frame_counter++;
	}

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
	ColorFramebuffer* blurred = &ao_framebuffer;

	if (blur_radius > 0)
	{
		//AO shader outputs depth as P.z / farz, so .y lives in [0, 1].
		//The blur weight = exp2(-r^2/(2*sigma^2) - (sharpness * dz)^2).
		//Scale sharpness from radius so blur smooths within surfaces without
		//pulling dark occlusion lines across nearby geometry.
		float sharpness = farz / (radius * 4.0f);
		if (sharpness < 180.0f) sharpness = 180.0f;
		if (sharpness > 1400.0f) sharpness = 1400.0f;

		blur_x_shader.Use();
		glUniform2f(blur_x_delta, 1.0f / (float)width, 0.0f);
		glUniform1f(blur_x_sharpness, sharpness);
		glUniform1i(blur_x_radius, blur_radius);
		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(ao_framebuffer.ColorTextureForRead(), 0, GL_NEAREST);
		GL_DrawFramebufferQuad(ao_blur_framebuffer.Handle(), 0, 0, width, height);

		blur_y_shader.Use();
		glUniform2f(blur_y_delta, 0.0f, 1.0f / (float)height);
		glUniform1f(blur_y_sharpness, sharpness);
		glUniform1i(blur_y_radius, blur_radius);
		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(ao_blur_framebuffer.ColorTextureForRead(), 0, GL_NEAREST);
		GL_DrawFramebufferQuad(ao_framebuffer.Handle(), 0, 0, width, height);

		blurred = &ao_framebuffer;
	}

	//-------------------------------------------------------------------------
	// Pass 3: Reproject and accumulate AO history.
	//-------------------------------------------------------------------------
	ColorFramebuffer* apply_source = blurred;
	bool temporal_ready = pref_state.hbao_temporal &&
		current_inv_view_projection != nullptr &&
		previous_view_projection != nullptr &&
		has_previous_view_projection &&
		temporal_shader.Handle() != 0;
	if (temporal_ready)
	{
		if (temporal_framebuffers[0].Width() != (uint32_t)width ||
			temporal_framebuffers[0].Height() != (uint32_t)height ||
			temporal_framebuffers[1].Width() != (uint32_t)width ||
			temporal_framebuffers[1].Height() != (uint32_t)height)
		{
			temporal_valid = false;
		}

		temporal_framebuffers[0].Update(width, height, GL_RG16F, GL_RG, GL_FLOAT);
		temporal_framebuffers[1].Update(width, height, GL_RG16F, GL_RG, GL_FLOAT);

		uint32_t write_index = temporal_index & 1;
		uint32_t read_index = (temporal_index + 1) & 1;

		temporal_shader.Use();
		if (temporal_current_inv_view_projection != -1)
			glUniformMatrix4fv(temporal_current_inv_view_projection, 1, GL_FALSE, current_inv_view_projection);
		if (temporal_previous_view_projection != -1)
			glUniformMatrix4fv(temporal_previous_view_projection, 1, GL_FALSE, previous_view_projection);
		if (temporal_has_history != -1)
			glUniform1i(temporal_has_history, temporal_valid ? 1 : 0);
		if (temporal_has_motion != -1)
			glUniform1i(temporal_has_motion, motion_texture != 0 ? 1 : 0);
		if (temporal_history_weight != -1)
			glUniform1f(temporal_history_weight, 0.96f);

		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(blurred->ColorTextureForRead(), 0, GL_NEAREST);
		GL_BindFramebufferTexture(temporal_framebuffers[read_index].ColorTextureForRead(), 1, GL_NEAREST);
		GL_BindFramebufferTexture(depth_texture, 2, GL_NEAREST);
		if (motion_texture != 0)
			GL_BindFramebufferTexture(motion_texture, 3, GL_NEAREST);

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, temporal_framebuffers[write_index].Handle());
		glViewport(0, 0, width, height);
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_SCISSOR_TEST);
		glDepthMask(GL_FALSE);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindVertexArray(GL_GetFramebufferVAO());
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glBindVertexArray(0);

		apply_source = &temporal_framebuffers[write_index];
		temporal_index = read_index;
		temporal_valid = true;
	}
	else
	{
		temporal_valid = false;
	}

	//-------------------------------------------------------------------------
	// Pass 4: Build the final HBAO suppression mask.
	//-------------------------------------------------------------------------
	GLuint final_suppression_mask = suppression_mask_texture;
	bool bloom_mask_enabled = pref_state.bloom_enabled && scene_color_texture != 0;
	if (suppression_shader.Handle() != 0 && (suppression_mask_texture != 0 || bloom_mask_enabled))
	{
		suppression_framebuffer.Update(width, height, GL_R8, GL_RED, GL_UNSIGNED_BYTE);
		suppression_shader.Use();
		if (suppression_has_mask != -1)
			glUniform1i(suppression_has_mask, suppression_mask_texture != 0 ? 1 : 0);
		if (suppression_use_bloom_mask != -1)
			glUniform1i(suppression_use_bloom_mask, bloom_mask_enabled ? 1 : 0);
		float display_gamma = pref_state.gamma != 0.0f ? 1.0f / pref_state.gamma : 1.0f;
		if (suppression_gamma != -1)
			glUniform1f(suppression_gamma, display_gamma);
		if (suppression_bloom_threshold != -1)
			glUniform1f(suppression_bloom_threshold, Clamp01(pref_state.bloom_threshold));
		rend_ClearBoundTextures();
		if (suppression_mask_texture != 0)
			GL_BindFramebufferTexture(suppression_mask_texture, 0, GL_NEAREST);
		if (scene_color_texture != 0)
			GL_BindFramebufferTexture(scene_color_texture, 1, GL_LINEAR);
		GL_DrawFramebufferQuad(suppression_framebuffer.Handle(), 0, 0, width, height);
		final_suppression_mask = suppression_framebuffer.ColorTextureForRead();
	}

	//-------------------------------------------------------------------------
	// Pass 5: Modulate scene color in-place.
	//
	// Draw a fullscreen quad to the source framebuffer with blend mode
	// (DST_COLOR, ZERO) so the destination is multiplied by our AO factor.
	// MSAA is re-enabled while writing so the rasterizer covers every sample.
	//-------------------------------------------------------------------------
	apply_shader.Use();
	glUniform1f(apply_intensity, intensity);
	if (apply_has_mask != -1)
		glUniform1i(apply_has_mask, final_suppression_mask != 0 ? 1 : 0);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, source->Handle());
	GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &draw_buffer);
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
	GL_BindFramebufferTexture(apply_source->ColorTextureForRead(), 0, GL_LINEAR);
	if (final_suppression_mask != 0)
		GL_BindFramebufferTexture(final_suppression_mask, 1, GL_LINEAR);

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

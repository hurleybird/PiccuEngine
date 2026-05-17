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
#include "gameloop.h"
#include "game.h"
#include <math.h>
#include <stdint.h>

namespace
{
	//Mersenne-Twister-derived noise values. Two channels:
	//(cos-of-angle-ish, jitter).
	const float kMtNoise[32] =
	{
		0.556725f, 0.005520f, 0.708315f, 0.583199f, 0.236644f, 0.992380f, 0.981091f, 0.119804f,
		0.510866f, 0.560499f, 0.961497f, 0.557862f, 0.539955f, 0.332871f, 0.417807f, 0.920779f,
		0.730747f, 0.076690f, 0.008562f, 0.660104f, 0.428921f, 0.511342f, 0.587871f, 0.906406f,
		0.437980f, 0.620309f, 0.062196f, 0.119485f, 0.235646f, 0.795892f, 0.044437f, 0.617311f,
	};

	constexpr int GTAO_DEFAULT_SAMPLES = 128;

	void GTAOSamplePattern(int samples, int* directions_out, int* steps_out)
	{
		if (samples < 1)
			samples = GTAO_DEFAULT_SAMPLES;

		int directions;
		if (samples <= 12)
		{
			directions = samples < 4 ? samples : 4;
		}
		else if (samples <= 24)
		{
			directions = 6;
		}
		else if (samples <= 32)
		{
			directions = 8;
		}
		else
		{
			directions = (int)(sqrtf((float)samples * 2.0f) + 0.5f);
			if (directions < 8)
				directions = 8;
			if (directions > 32)
				directions = 32;
		}

		int steps = (samples + directions - 1) / directions;
		if (steps < 1)
			steps = 1;

		*directions_out = directions;
		*steps_out = steps;
	}

	float Clamp01(float value)
	{
		if (value < 0.0f)
			return 0.0f;
		if (value > 1.0f)
			return 1.0f;
		return value;
	}

	int ClampInt(int value, int min_value, int max_value)
	{
		if (value < min_value)
			return min_value;
		if (value > max_value)
			return max_value;
		return value;
	}

	float ClampFloat(float value, float min_value, float max_value)
	{
		if (value < min_value)
			return min_value;
		if (value > max_value)
			return max_value;
		return value;
	}

	int GTAOResolutionScale(const renderer_preferred_state& pref_state, int width, int height)
	{
		switch (pref_state.gtao_resolution)
		{
		case GTAO_RESOLUTION_FULL:
			return 1;
		case GTAO_RESOLUTION_HALF:
			return 2;
		case GTAO_RESOLUTION_QUARTER:
			return 4;
		case GTAO_RESOLUTION_AUTO:
		default:
			break;
		}

		//Start at scale 1 and double until the AO buffer fits the budget.
		//Seeding with SupersamplingFactor was redundant - the loop below
		//catches SSAA-inflated sources too - and worse, it forced unnecessary
		//downsampling on small back buffers when SSAA was high.
		int scale = 1;
		int target_pixels = 1920 * 1080;
		//MSAA-resolved depth still costs sample-count bandwidth per AO pixel
		//in upstream resolves; shrink the budget so we trade a bit of AO
		//resolution for fewer per-sample reads.
		int samples = (int)RendererMsaaSamples(pref_state);
		if (samples >= 8)
			target_pixels /= 4;
		else if (samples >= 4)
			target_pixels /= 2;
		while (scale < 4)
		{
			int ao_width = (width + scale - 1) / scale;
			int ao_height = (height + scale - 1) / scale;
			if (ao_width * ao_height <= target_pixels)
				break;
			scale *= 2;
		}
		return scale;
	}

	bool FramebufferLargerThan(const ColorFramebuffer& framebuffer, int width, int height)
	{
		return framebuffer.Handle() != 0 &&
			(framebuffer.Width() > (uint32_t)width || framebuffer.Height() > (uint32_t)height);
	}

	void AODrawFullscreen(GLuint framebuffer, int width, int height)
	{
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
		glViewport(0, 0, width, height);
		glBindVertexArray(GL_GetFramebufferVAO());
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glBindVertexArray(0);
	}

}

void GTAOResources::InitShaders()
{
	extern const char* blitVertexSrc;
	extern const char* gtaoDepthFragmentSrc;
	extern const char* gtaoAOFragmentSrc;
	extern const char* gtaoBlurFragmentSrc;
	extern const char* gtaoSuppressionFragmentSrc;
	extern const char* gtaoApplyFragmentSrc;

	depth_shader.AttachSource(blitVertexSrc, gtaoDepthFragmentSrc);
	depth_shader.Use();
	depth_source = depth_shader.FindUniform("depth_tex");
	depth_source_ms = depth_shader.FindUniform("depth_ms_tex");
	depth_ao_class = depth_shader.FindUniform("ao_class_tex");
	if (depth_source != -1) glUniform1i(depth_source, 0);
	if (depth_source_ms != -1) glUniform1i(depth_source_ms, 1);
	if (depth_ao_class != -1) glUniform1i(depth_ao_class, 2);
	depth_has_ao_class = depth_shader.FindUniform("has_ao_class");
	depth_ao_weight_is_direct = depth_shader.FindUniform("ao_weight_is_direct");
	depth_samples = depth_shader.FindUniform("depth_samples");
	depth_input_screen_size = depth_shader.FindUniform("input_screen_size");
	depth_ao_screen_size = depth_shader.FindUniform("ao_screen_size");
	depth_terrain_occlusion = depth_shader.FindUniform("terrain_ao_occlusion");
	depth_polyobject_occlusion = depth_shader.FindUniform("polyobject_ao_occlusion");
	depth_mine_rock_occlusion = depth_shader.FindUniform("mine_rock_ao_occlusion");
	depth_mine_occlusion = depth_shader.FindUniform("mine_ao_occlusion");

	ao_shader.AttachSource(blitVertexSrc, gtaoAOFragmentSrc);
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
	ao_inv_screen_size = ao_shader.FindUniform("inv_screen_size");
	ao_ao_inv_screen_size = ao_shader.FindUniform("ao_inv_screen_size");
	ao_screen_size = ao_shader.FindUniform("screen_size");
	ao_noise_origin = ao_shader.FindUniform("noise_origin");
	ao_directions = ao_shader.FindUniform("directions");
	ao_steps = ao_shader.FindUniform("steps");
	ao_terrain_occlusion = ao_shader.FindUniform("terrain_ao_occlusion");
	ao_polyobject_occlusion = ao_shader.FindUniform("polyobject_ao_occlusion");
	ao_mine_rock_occlusion = ao_shader.FindUniform("mine_rock_ao_occlusion");
	ao_mine_occlusion = ao_shader.FindUniform("mine_ao_occlusion");

	blur_x_shader.AttachSource(blitVertexSrc, gtaoBlurFragmentSrc);
	blur_x_shader.Use();
	GLint blur_x_heh = blur_x_shader.FindUniform("heh");
	if (blur_x_heh != -1) glUniform1i(blur_x_heh, 0);
	blur_x_delta = blur_x_shader.FindUniform("blur_delta");
	blur_x_sharpness = blur_x_shader.FindUniform("sharpness");
	blur_x_radius = blur_x_shader.FindUniform("kernel_radius");

	blur_y_shader.AttachSource(blitVertexSrc, gtaoBlurFragmentSrc);
	blur_y_shader.Use();
	GLint blur_y_heh = blur_y_shader.FindUniform("heh");
	if (blur_y_heh != -1) glUniform1i(blur_y_heh, 0);
	blur_y_delta = blur_y_shader.FindUniform("blur_delta");
	blur_y_sharpness = blur_y_shader.FindUniform("sharpness");
	blur_y_radius = blur_y_shader.FindUniform("kernel_radius");

	suppression_shader.AttachSource(blitVertexSrc, gtaoSuppressionFragmentSrc);
	suppression_shader.Use();
	suppression_existing_mask = suppression_shader.FindUniform("existing_mask");
	suppression_existing_mask_ms = suppression_shader.FindUniform("existing_mask_ms");
	suppression_color = suppression_shader.FindUniform("color_tex");
	suppression_color_ms = suppression_shader.FindUniform("color_ms_tex");
	if (suppression_existing_mask != -1) glUniform1i(suppression_existing_mask, 0);
	if (suppression_existing_mask_ms != -1) glUniform1i(suppression_existing_mask_ms, 2);
	if (suppression_color != -1) glUniform1i(suppression_color, 1);
	if (suppression_color_ms != -1) glUniform1i(suppression_color_ms, 3);
	suppression_has_mask = suppression_shader.FindUniform("has_mask");
	suppression_use_bloom_mask = suppression_shader.FindUniform("use_bloom_mask");
	suppression_source_samples = suppression_shader.FindUniform("source_samples");
	suppression_input_screen_size = suppression_shader.FindUniform("input_screen_size");
	suppression_ao_screen_size = suppression_shader.FindUniform("ao_screen_size");
	suppression_source_visible_origin = suppression_shader.FindUniform("source_visible_origin");
	suppression_source_visible_size = suppression_shader.FindUniform("source_visible_size");
	suppression_use_source_visible_rect = suppression_shader.FindUniform("use_source_visible_rect");
	suppression_gamma = suppression_shader.FindUniform("gamma");
	suppression_bloom_threshold = suppression_shader.FindUniform("bloom_threshold");

	apply_shader.AttachSource(blitVertexSrc, gtaoApplyFragmentSrc);
	apply_shader.Use();
	GLint apply_heh = apply_shader.FindUniform("heh");
	GLint apply_mask = apply_shader.FindUniform("suppression_mask");
	if (apply_heh != -1) glUniform1i(apply_heh, 0);
	if (apply_mask != -1) glUniform1i(apply_mask, 1);
	apply_intensity = apply_shader.FindUniform("intensity");
	apply_has_mask = apply_shader.FindUniform("has_suppression_mask");
	apply_ao_uv_origin = apply_shader.FindUniform("ao_uv_origin");
	apply_ao_uv_scale = apply_shader.FindUniform("ao_uv_scale");

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

void GTAOResources::DestroyShaders()
{
	depth_shader.Destroy();
	ao_shader.Destroy();
	blur_x_shader.Destroy();
	blur_y_shader.Destroy();
	suppression_shader.Destroy();
	apply_shader.Destroy();
	if (noise_texture)
	{
		glDeleteTextures(1, &noise_texture);
		noise_texture = 0;
	}
}

bool GTAOResources::HasFramebuffers() const
{
	return ao_depth_framebuffer.Handle() != 0 ||
		ao_framebuffer.Handle() != 0 ||
		ao_blur_framebuffer.Handle() != 0 ||
		suppression_framebuffer.Handle() != 0;
}

void GTAOResources::DestroyFramebuffers()
{
	ao_depth_framebuffer.Destroy();
	ao_framebuffer.Destroy();
	ao_blur_framebuffer.Destroy();
	suppression_framebuffer.Destroy();
}

void GTAOResources::Destroy()
{
	DestroyFramebuffers();
	DestroyShaders();
}

void GTAOResources::Apply(Framebuffer* source, Framebuffer* target, const renderer_preferred_state& pref_state,
	const rendering_state& render_state, const float* projection,
	float nearz, float farz, GLuint suppression_mask_texture, GLuint ao_weight_texture,
	bool ao_weight_is_direct,
	int source_visible_x, int source_visible_y, int source_visible_w, int source_visible_h,
	int noise_origin_x, int noise_origin_y)
{
	if (!source || !pref_state.gtao_enabled)
	{
		if (HasFramebuffers())
		{
			glFinish();
			DestroyFramebuffers();
		}
		return;
	}

	PERF_MARKER_SCOPE("GTAO.Total");

	if (!target)
		target = source;

	int source_width = (int)source->Width();
	int source_height = (int)source->Height();
	if (source_width <= 0 || source_height <= 0)
		return;

	if (source_visible_w <= 0 || source_visible_h <= 0)
	{
		source_visible_x = 0;
		source_visible_y = 0;
		source_visible_w = source_width;
		source_visible_h = source_height;
	}
	source_visible_x = ClampInt(source_visible_x, 0, source_width - 1);
	source_visible_y = ClampInt(source_visible_y, 0, source_height - 1);
	source_visible_w = ClampInt(source_visible_w, 1, source_width - source_visible_x);
	source_visible_h = ClampInt(source_visible_h, 1, source_height - source_visible_y);
	const bool use_source_visible_rect =
		source_visible_x != 0 || source_visible_y != 0 ||
		source_visible_w != source_width || source_visible_h != source_height;

	int ao_base_width = source_width;
	int ao_base_height = source_height;
	if (ao_base_width <= 0 || ao_base_height <= 0)
	{
		ao_base_width = source_width;
		ao_base_height = source_height;
	}

	int ao_scale = GTAOResolutionScale(pref_state, ao_base_width, ao_base_height);
	int ao_width = (ao_base_width + ao_scale - 1) / ao_scale;
	int ao_height = (ao_base_height + ao_scale - 1) / ao_scale;
	if (ao_width <= 0) ao_width = 1;
	if (ao_height <= 0) ao_height = 1;

	if (FramebufferLargerThan(ao_depth_framebuffer, ao_width, ao_height) ||
		FramebufferLargerThan(ao_framebuffer, ao_width, ao_height) ||
		FramebufferLargerThan(ao_blur_framebuffer, ao_width, ao_height) ||
		FramebufferLargerThan(suppression_framebuffer, ao_width, ao_height))
	{
		glFinish();
		DestroyFramebuffers();
	}

	//Run AO at an internal resolution decoupled from SSAA/MSAA source size.
	//The source may be supersampled; AO sample counts are defined relative
	//to the display resolution so they do not become 4xSSAA-resolution AO.
	//Depth is first reduced to this resolution so MSAA resolves are paid once
	//per AO pixel instead of inside every horizon sample.
	ao_depth_framebuffer.Update(ao_width, ao_height, GL_RG32F, GL_RG, GL_FLOAT);
	//These buffers store AO plus linear depth; half-float depth avoids
	//depth-edge ghosts from 8-bit quantization in the bilateral blur.
	ao_framebuffer.Update(ao_width, ao_height, GL_RG16F, GL_RG, GL_FLOAT);
	ao_blur_framebuffer.Update(ao_width, ao_height, GL_RG16F, GL_RG, GL_FLOAT);

	int target_width = (int)target->Width();
	int target_height = (int)target->Height();
	if (target_width <= 0 || target_height <= 0)
		return;

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

	int source_samples = (int)source->Samples();
	bool msaa_source = source_samples >= 2;
	GLuint depth_texture = msaa_source ? source->DepthTextureForRead() : source->DepthTextureRaw();
	GLuint scene_color_texture = 0;
	if (pref_state.bloom_enabled)
		scene_color_texture = (source == target) ?
			(msaa_source ? source->ColorTextureForRead() : source->ColorTextureRaw()) :
			target->ColorTextureForRead();
	//GTAO reads resolved 2D inputs. Hardware resolves are much cheaper than
	//doing source-block * sample-count loops in the AO/suppression shaders.
	int input_samples = 0;

	//Compute uniform values.
	float m00 = projection[0];
	float m11 = projection[5];
	if (fabsf(m00) < 1e-6f) m00 = 1.0f;
	if (fabsf(m11) < 1e-6f) m11 = 1.0f;

	int samples = ClampInt((int)pref_state.gtao_sample_count, 1, 1024);
	int directions = 0;
	int steps = 0;
	GTAOSamplePattern(samples, &directions, &steps);

	float radius = ClampFloat(pref_state.gtao_radius, 0.1f, 12.0f);
	float bias = ClampFloat(pref_state.gtao_bias, 0.0f, 1.0f);
	float intensity = ClampFloat(pref_state.gtao_intensity, 0.0f, 4.0f);

	//Convert world-space radius to a pixel-space scale at depth==1. The shader
	//then divides by per-pixel depth to get the on-screen step size. This is
	//Equivalent to: r_pixels = radius * 0.5 * (h / tan(fov/2)).
	//Since m11 = 1 / tan(half_y_fov_with_aspect_compensation), pixel size is:
	float radius_pixels = radius * 0.5f * (float)ao_height * m11;
	const int visible_width = render_state.screen_width > 0 ? render_state.screen_width : source_width;
	const int visible_height = render_state.screen_height > 0 ? render_state.screen_height : source_height;
	const int visible_ao_width = (visible_width + ao_scale - 1) / ao_scale;
	const int visible_ao_height = (visible_height + ao_scale - 1) / ao_scale;
	float max_radius_pixels = 128.0f * sqrtf((float)(visible_ao_width * visible_ao_height) / (1080.0f * 1920.0f));
	if (max_radius_pixels < 16.0f) max_radius_pixels = 16.0f;

	float neg_inv_r2 = -1.0f / (radius * radius);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_MULTISAMPLE);
	glDepthMask(GL_FALSE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	//-------------------------------------------------------------------------
	// Pass 1: downsample/resolve source depth into AO resolution.
	//-------------------------------------------------------------------------
	{
		PERF_MARKER_SCOPE("GTAO.DepthReduce");
		depth_shader.Use();
		if (depth_samples != -1)
			glUniform1i(depth_samples, input_samples);
		if (depth_has_ao_class != -1)
			glUniform1i(depth_has_ao_class, ao_weight_texture != 0 ? 1 : 0);
		if (depth_ao_weight_is_direct != -1)
			glUniform1i(depth_ao_weight_is_direct, ao_weight_is_direct ? 1 : 0);
		if (depth_input_screen_size != -1)
			glUniform2f(depth_input_screen_size, (float)source_width, (float)source_height);
		if (depth_ao_screen_size != -1)
			glUniform2f(depth_ao_screen_size, (float)ao_width, (float)ao_height);
		if (depth_terrain_occlusion != -1)
			glUniform1f(depth_terrain_occlusion, Clamp01(pref_state.gtao_terrain_occlusion));
		if (depth_polyobject_occlusion != -1)
			glUniform1f(depth_polyobject_occlusion, Clamp01(pref_state.gtao_polyobject_occlusion));
		if (depth_mine_rock_occlusion != -1)
			glUniform1f(depth_mine_rock_occlusion, Clamp01(pref_state.gtao_mine_rock_occlusion));
		if (depth_mine_occlusion != -1)
			glUniform1f(depth_mine_occlusion, Clamp01(pref_state.gtao_mine_occlusion));

		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(depth_texture, 0, GL_NEAREST);
		if (ao_weight_texture != 0)
			GL_BindFramebufferTexture(ao_weight_texture, 2, GL_NEAREST);
		AODrawFullscreen(ao_depth_framebuffer.Handle(), ao_width, ao_height);
	}

	//-------------------------------------------------------------------------
	// Pass 2: AO into ao_framebuffer.
	//-------------------------------------------------------------------------
	{
		PERF_MARKER_SCOPE("GTAO.Generate");
		ao_shader.Use();
		glUniform4f(ao_proj_info,
			2.0f / m00,
			2.0f / m11,
			(projection[8] - 1.0f) / m00,
			(projection[9] - 1.0f) / m11);
		glUniform2f(ao_near_far, nearz, farz);
		glUniform1f(ao_radius, radius);
		glUniform1f(ao_radius_pixels, radius_pixels);
		glUniform1f(ao_max_radius_pixels, max_radius_pixels);
		glUniform1f(ao_neg_inv_radius2, neg_inv_r2);
		glUniform1f(ao_angle_bias, bias);
		glUniform2f(ao_inv_screen_size, 1.0f / (float)ao_width, 1.0f / (float)ao_height);
		if (ao_ao_inv_screen_size != -1)
			glUniform2f(ao_ao_inv_screen_size, 1.0f / (float)ao_width, 1.0f / (float)ao_height);
		glUniform2f(ao_screen_size, (float)ao_width, (float)ao_height);
		if (ao_noise_origin != -1)
		{
			glUniform2f(ao_noise_origin,
				(float)noise_origin_x * (float)ao_width / (float)source_width,
				(float)noise_origin_y * (float)ao_height / (float)source_height);
		}
		glUniform1i(ao_directions, directions);
		glUniform1i(ao_steps, steps);
		if (ao_terrain_occlusion != -1)
			glUniform1f(ao_terrain_occlusion, Clamp01(pref_state.gtao_terrain_occlusion));
		if (ao_polyobject_occlusion != -1)
			glUniform1f(ao_polyobject_occlusion, Clamp01(pref_state.gtao_polyobject_occlusion));
		if (ao_mine_rock_occlusion != -1)
			glUniform1f(ao_mine_rock_occlusion, Clamp01(pref_state.gtao_mine_rock_occlusion));
		if (ao_mine_occlusion != -1)
			glUniform1f(ao_mine_occlusion, Clamp01(pref_state.gtao_mine_occlusion));

		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(ao_depth_framebuffer.ColorTextureForRead(), 0, GL_NEAREST);
		GL_BindFramebufferTexture(noise_texture, 1, GL_NEAREST);
		AODrawFullscreen(ao_framebuffer.Handle(), ao_width, ao_height);
	}

	//-------------------------------------------------------------------------
	// Pass 3: Optional separable bilateral blur (X then Y).
	//-------------------------------------------------------------------------
	int blur_radius = ClampInt((int)pref_state.gtao_blur_radius, 0, 20);
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

		{
			PERF_MARKER_SCOPE("GTAO.BlurX");
			blur_x_shader.Use();
			glUniform2f(blur_x_delta, 1.0f / (float)ao_width, 0.0f);
			glUniform1f(blur_x_sharpness, sharpness);
			glUniform1i(blur_x_radius, blur_radius);
			rend_ClearBoundTextures();
			GL_BindFramebufferTexture(ao_framebuffer.ColorTextureForRead(), 0, GL_NEAREST);
			AODrawFullscreen(ao_blur_framebuffer.Handle(), ao_width, ao_height);
		}

		{
			PERF_MARKER_SCOPE("GTAO.BlurY");
			blur_y_shader.Use();
			glUniform2f(blur_y_delta, 0.0f, 1.0f / (float)ao_height);
			glUniform1f(blur_y_sharpness, sharpness);
			glUniform1i(blur_y_radius, blur_radius);
			rend_ClearBoundTextures();
			GL_BindFramebufferTexture(ao_blur_framebuffer.ColorTextureForRead(), 0, GL_NEAREST);
			AODrawFullscreen(ao_framebuffer.Handle(), ao_width, ao_height);
		}

		blurred = &ao_framebuffer;
	}

	ColorFramebuffer* apply_source = blurred;

	//-------------------------------------------------------------------------
	// Pass 4: Build the final AO suppression mask.
	//-------------------------------------------------------------------------
	GLuint final_suppression_mask = 0;
	GLuint ao_exclusion_mask_texture = suppression_mask_texture;
	bool bloom_mask_enabled = pref_state.bloom_enabled && scene_color_texture != 0;
	if (suppression_shader.Handle() != 0 && (ao_exclusion_mask_texture != 0 || bloom_mask_enabled))
	{
		PERF_MARKER_SCOPE("GTAO.Suppression");

		suppression_framebuffer.Update(ao_width, ao_height, GL_R8, GL_RED, GL_UNSIGNED_BYTE);
		suppression_shader.Use();
		if (suppression_has_mask != -1)
			glUniform1i(suppression_has_mask, ao_exclusion_mask_texture != 0 ? 1 : 0);
		if (suppression_use_bloom_mask != -1)
			glUniform1i(suppression_use_bloom_mask, bloom_mask_enabled ? 1 : 0);
		if (suppression_source_samples != -1)
			glUniform1i(suppression_source_samples, input_samples);
		if (suppression_input_screen_size != -1)
			glUniform2f(suppression_input_screen_size, (float)source_width, (float)source_height);
		if (suppression_ao_screen_size != -1)
			glUniform2f(suppression_ao_screen_size, (float)ao_width, (float)ao_height);
		if (suppression_source_visible_origin != -1)
			glUniform2f(suppression_source_visible_origin, (float)source_visible_x, (float)source_visible_y);
		if (suppression_source_visible_size != -1)
			glUniform2f(suppression_source_visible_size, (float)source_visible_w, (float)source_visible_h);
		if (suppression_use_source_visible_rect != -1)
			glUniform1i(suppression_use_source_visible_rect, use_source_visible_rect ? 1 : 0);
		float display_gamma = pref_state.gamma != 0.0f ? 1.0f / pref_state.gamma : 1.0f;
		if (suppression_gamma != -1)
			glUniform1f(suppression_gamma, display_gamma);
		if (suppression_bloom_threshold != -1)
			glUniform1f(suppression_bloom_threshold, Clamp01(pref_state.bloom_threshold));
		rend_ClearBoundTextures();
		if (ao_exclusion_mask_texture != 0)
			GL_BindFramebufferTexture(ao_exclusion_mask_texture, 0, GL_NEAREST);
		if (scene_color_texture != 0)
			GL_BindFramebufferTexture(scene_color_texture, 1, GL_LINEAR);
		AODrawFullscreen(suppression_framebuffer.Handle(), ao_width, ao_height);
		final_suppression_mask = suppression_framebuffer.ColorTextureForRead();
	}

	//-------------------------------------------------------------------------
	// Pass 5: Modulate target scene color in-place.
	//
	// Draw a fullscreen quad to the target framebuffer with blend mode
	// (DST_COLOR, ZERO) so the destination is multiplied by our AO factor.
	//-------------------------------------------------------------------------
	{
		PERF_MARKER_SCOPE("GTAO.Composite");
		const bool debug_display_ao_only = pref_state.gtao_debug_preview && Game_mode != GM_NONE;
		apply_shader.Use();
		glUniform1f(apply_intensity, debug_display_ao_only ? 1.0f : intensity);
		if (apply_has_mask != -1)
			glUniform1i(apply_has_mask, !debug_display_ao_only && final_suppression_mask != 0 ? 1 : 0);
		if (apply_ao_uv_origin != -1)
			glUniform2f(apply_ao_uv_origin,
				(float)source_visible_x / (float)source_width,
				(float)source_visible_y / (float)source_height);
		if (apply_ao_uv_scale != -1)
			glUniform2f(apply_ao_uv_scale,
				(float)source_visible_w / (float)source_width,
				(float)source_visible_h / (float)source_height);

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target->Handle());
		GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
		glDrawBuffers(1, &draw_buffer);
		glViewport(0, 0, target_width, target_height);

		if (debug_display_ao_only)
		{
			glDisable(GL_BLEND);
		}
		else
		{
			glEnable(GL_BLEND);
			glBlendFunc(GL_DST_COLOR, GL_ZERO);
		}
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDepthMask(GL_FALSE);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(apply_source->ColorTextureForRead(), 0, GL_LINEAR);
		if (final_suppression_mask != 0)
			GL_BindFramebufferTexture(final_suppression_mask, 1, GL_LINEAR);

		AODrawFullscreen(target->Handle(), target_width, target_height);
	}

	//Pass 5 changed the target color attachment; any cached single-sample
	//resolve from earlier in this frame no longer matches.
	target->MarkColorDirty();

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

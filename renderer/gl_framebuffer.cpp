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

static GLuint fbVAOName;
static GLuint fbVBOName;

static const float framebuffer_buffer[] = { -1.0f, -1.0f, 0.0f, 0.0f,
					  3.0f, -1.0f, 2.0f, 0.0f,
					  -1.0f, 3.0f, 0.0f, 2.0f };

void GL_InitFramebufferVAO()
{
	if (fbVAOName)
		return;

	glGenVertexArrays(1, &fbVAOName);
	glGenBuffers(1, &fbVBOName);

	glBindVertexArray(fbVAOName);
	glBindBuffer(GL_ARRAY_BUFFER, fbVBOName);
	glBufferData(GL_ARRAY_BUFFER, sizeof(framebuffer_buffer), framebuffer_buffer, GL_STATIC_DRAW);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, 0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (GLvoid*)(sizeof(float) * 2));

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	rend_RestoreLegacy();
	//glBindVertexArray(0);
}

void GL_DestroyFramebufferVAO()
{
	glDeleteBuffers(1, &fbVBOName);
	glDeleteVertexArrays(1, &fbVAOName);
	fbVBOName = fbVAOName = 0;
}

//Returns the framebuffer fullscreen-triangle VAO, initialising it lazily.
//Exposed so post-processing passes that draw a fullscreen triangle without
//also clearing the destination (HBAO's "apply" pass) can reuse it.
GLuint GL_GetFramebufferVAO()
{
	if (!fbVAOName)
		GL_InitFramebufferVAO();
	return fbVAOName;
}

void GL_BindFramebufferTexture(GLuint texture, int unit, GLenum filter)
{
	if (OpenGLProfile == GLPROFILE_COMPAT)
		glClientActiveTextureARB(GL_TEXTURE0 + unit);

	glActiveTexture(GL_TEXTURE0 + unit);
	if (OpenGLProfile == GLPROFILE_COMPAT)
		glEnable(GL_TEXTURE_2D);

	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void GL_SetEnabledState(GLenum cap, GLboolean enabled)
{
	if (enabled)
		glEnable(cap);
	else
		glDisable(cap);
}

static void GL_RestoreFramebufferTextureState()
{
	for (int unit = 2; unit >= 1; unit--)
	{
		if (OpenGLProfile == GLPROFILE_COMPAT)
			glClientActiveTextureARB(GL_TEXTURE0 + unit);
		glActiveTexture(GL_TEXTURE0 + unit);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (OpenGLProfile == GLPROFILE_COMPAT)
			glDisable(GL_TEXTURE_2D);
	}

	if (OpenGLProfile == GLPROFILE_COMPAT)
		glClientActiveTextureARB(GL_TEXTURE0);
	glActiveTexture(GL_TEXTURE0);
	rend_ClearBoundTextures();
}

void GL_DrawFramebufferQuad(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h)
{
	GLboolean blend_enabled = glIsEnabled(GL_BLEND);
	GLboolean depth_enabled = glIsEnabled(GL_DEPTH_TEST);
	GLboolean cull_enabled = glIsEnabled(GL_CULL_FACE);
	GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
	GLboolean color_mask[4];
	GLboolean depth_mask;
	glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);

	glBindVertexArray(fbVAOName);
	GLint oldviewport[4];
	glGetIntegerv(GL_VIEWPORT, oldviewport);
	glViewport(x, y, w, h);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glViewport(oldviewport[0], oldviewport[1], oldviewport[2], oldviewport[3]);

	GL_SetEnabledState(GL_BLEND, blend_enabled);
	GL_SetEnabledState(GL_DEPTH_TEST, depth_enabled);
	GL_SetEnabledState(GL_CULL_FACE, cull_enabled);
	GL_SetEnabledState(GL_SCISSOR_TEST, scissor_enabled);
	glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
	glDepthMask(depth_mask);
	GL_RestoreFramebufferTextureState();
	rend_RestoreLegacy();
}

void ColorFramebuffer::Update(int width, int height, GLint internal_format, GLenum format, GLenum type)
{
	if (width <= 0 || height <= 0)
	{
		Destroy();
		return;
	}

	if ((uint32_t)width == m_width && (uint32_t)height == m_height && m_name != 0 && m_colorname != 0)
		return;

	Destroy();

	m_width = (uint32_t)width;
	m_height = (uint32_t)height;

	glGenFramebuffers(1, &m_name);
	glGenTextures(1, &m_colorname);

	glBindTexture(GL_TEXTURE_2D, m_colorname);
	glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, type, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindFramebuffer(GL_FRAMEBUFFER, m_name);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorname, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
#ifndef _NDEBUG
	GLenum fbstatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (fbstatus != GL_FRAMEBUFFER_COMPLETE)
		Error("ColorFramebuffer::Update: Framebuffer object is incomplete!");
#endif

	rend_ClearBoundTextures();
}

void ColorFramebuffer::Destroy()
{
	glDeleteTextures(1, &m_colorname);
	glDeleteFramebuffers(1, &m_name);
	m_colorname = 0;
	m_name = 0;
	m_width = 0;
	m_height = 0;
}

Framebuffer::Framebuffer()
{
	m_width = m_height = 0;
	m_name = m_subname = m_colorname = m_subcolorname = m_depthname = m_subdepthname = 0;
	m_samples = 0;
}

static int GetSupportedMsaaSamples(int requested_samples)
{
	if (requested_samples < 2)
		return 0;

	GLint max_samples = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &max_samples);

	for (int samples : { 8, 4, 2 })
	{
		if (requested_samples >= samples && max_samples >= samples)
			return samples;
	}

	return 0;
}

void Framebuffer::Update(int width, int height, int msaa_samples)
{
	if (width <= 0 || height <= 0)
	{
		Destroy();
		return;
	}

	msaa_samples = GetSupportedMsaaSamples(msaa_samples);
	bool msaa = msaa_samples > 1;

	if (width == m_width && height == m_height && (uint32_t)msaa_samples == m_samples)
		return;

	//The solution to all problems with changing framebuffer status is to delete everything and start over.
	if (!msaa && m_subname != 0)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		Destroy();
	}
	else if (msaa && m_name != 0 && m_subname == 0)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		Destroy();
	}

	m_width = width;
	m_height = height;
	m_samples = msaa_samples;

	glActiveTexture(GL_TEXTURE0);
	if (m_name == 0)
	{
		glGenTextures(1, &m_colorname);
		glGenTextures(1, &m_depthname);
		glGenFramebuffers(1, &m_name);
	}

	GLenum textureType = msaa ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;

	if (msaa)
	{
		//Create the sub framebuffer now if it hasn't already been.
		//When multisampling is used, the sub framebuffer is a non-multisampled framebuffer for the main framebuffer to resolve to.
		if (m_subname == 0)
		{
			glGenTextures(1, &m_subcolorname);
			glGenTextures(1, &m_subdepthname);
			glGenFramebuffers(1, &m_subname);
		}

		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_colorname);
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, msaa_samples, GL_RGBA8, width, height, GL_FALSE);

		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_depthname);
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, msaa_samples, GL_DEPTH_COMPONENT32F, width, height, GL_FALSE);

		glBindTexture(GL_TEXTURE_2D, m_subcolorname);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glBindTexture(GL_TEXTURE_2D, m_subdepthname);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		//Do attachment for the sub framebuffer
		glBindFramebuffer(GL_FRAMEBUFFER, m_subname);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_subcolorname, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_subdepthname, 0);

#ifndef _NDEBUG
		GLenum fbstatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (fbstatus != GL_FRAMEBUFFER_COMPLETE)
		{
			Error("Framebuffer::Update: Sub framebuffer object is incomplete!");
		}
#endif
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, m_colorname);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glBindTexture(GL_TEXTURE_2D, m_depthname);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	rend_ClearBoundTextures();

	glBindFramebuffer(GL_FRAMEBUFFER, m_name);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textureType, m_colorname, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, textureType, m_depthname, 0);
#ifndef _NDEBUG
	GLenum fbstatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (fbstatus != GL_FRAMEBUFFER_COMPLETE)
	{
		Error("Framebuffer::Update: Framebuffer object is incomplete!");
	}
#endif
}

void Framebuffer::Destroy()
{
	glDeleteTextures(1, &m_colorname);
	glDeleteTextures(1, &m_depthname);
	glDeleteFramebuffers(1, &m_name);
	m_name = m_colorname = m_depthname = 0;

	glDeleteFramebuffers(1, &m_subname);
	glDeleteTextures(1, &m_subcolorname);
	glDeleteTextures(1, &m_subdepthname);
	m_subname = m_subcolorname = m_subdepthname = 0;
	m_width = m_height = 0;
	m_samples = 0;
}

void Framebuffer::SubColorBlit()
{
	if (m_samples < 2)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_name);
	}
	else
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_name);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_subname);
		glBlitFramebuffer(0, 0, m_width, m_height, 0, 0,
			m_width, m_height, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);

#ifdef _DEBUG
		GLenum err = glGetError();
		if (err != GL_NO_ERROR)
		{
			mprintf((0, "Error resolving multisampling: %d\n", err));
		}
#endif

		//Leave the sub color buffer bound for reading by BlitToRaw.
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_subname);
	}
}

void Framebuffer::BlitToRaw(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h, GLenum filter)
{
	SubColorBlit();
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
	{
		mprintf((0, "Error unbinding draw framebuffer: %d\n", err));
	}
#endif

	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glBlitFramebuffer(0, 0, m_width, m_height,
		x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, filter);
}

void Framebuffer::BlitDepthTo(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h)
{
	SubColorBlit();
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glBlitFramebuffer(0, 0, m_width, m_height,
		x, y, x + w, y + h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
	{
		mprintf((0, "Error blitting depth: %d\n", err));
	}
#endif
}

void Framebuffer::BlitTo(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h, bool linear_filter)
{
	SubColorBlit();

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
	{
		mprintf((0, "Error leaving sub color blit: %d\n", err));
	}
#endif

	glBindVertexArray(fbVAOName);
	GLint oldviewport[4];
	glGetIntegerv(GL_VIEWPORT, oldviewport);
	glViewport(x, y, w, h);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
	{
		mprintf((0, "Error binding framebuffers: %d\n", err));
	}
#endif


	rend_ClearBoundTextures();
	if (OpenGLProfile == GLPROFILE_COMPAT)
		glClientActiveTextureARB(GL_TEXTURE0);

	glActiveTexture(GL_TEXTURE0);

	GLuint sourcename = (m_samples >= 2) ? m_subcolorname : m_colorname;
	if (OpenGLProfile == GLPROFILE_COMPAT)
		glEnable(GL_TEXTURE_2D);

	glBindTexture(GL_TEXTURE_2D, sourcename);
	GLint filter = linear_filter ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
	{
		mprintf((0, "Error unbinding draw framebuffer: %d\n", err));
	}
#endif

	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glViewport(oldviewport[0], oldviewport[1], oldviewport[2], oldviewport[3]);

	rend_RestoreLegacy();
}

void Framebuffer::DownsampleTo(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h,
	GLint gamma_uniform, float gamma, GLint dest_origin_uniform)
{
	SubColorBlit();

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
	{
		mprintf((0, "Error leaving sub color blit: %d\n", err));
	}
#endif

	glBindVertexArray(fbVAOName);
	GLint oldviewport[4];
	glGetIntegerv(GL_VIEWPORT, oldviewport);
	glViewport(x, y, w, h);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	if (gamma_uniform != -1)
		glUniform1f(gamma_uniform, gamma);
	if (dest_origin_uniform != -1)
		glUniform2i(dest_origin_uniform, x, y);

#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
	{
		mprintf((0, "Error binding framebuffers: %d\n", err));
	}
#endif

	rend_ClearBoundTextures();
	if (OpenGLProfile == GLPROFILE_COMPAT)
		glClientActiveTextureARB(GL_TEXTURE0);

	glActiveTexture(GL_TEXTURE0);

	GLuint sourcename = (m_samples >= 2) ? m_subcolorname : m_colorname;
	if (OpenGLProfile == GLPROFILE_COMPAT)
		glEnable(GL_TEXTURE_2D);

	glBindTexture(GL_TEXTURE_2D, sourcename);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
	{
		mprintf((0, "Error binding downsample source: %d\n", err));
	}
#endif

	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glViewport(oldviewport[0], oldviewport[1], oldviewport[2], oldviewport[3]);

	rend_RestoreLegacy();
}

void MotionVectorResources::Update(uint32_t new_width, uint32_t new_height, uint32_t msaa_samples)
{
	if (new_width == 0 || new_height == 0)
	{
		Destroy();
		return;
	}

	msaa_samples = GetSupportedMsaaSamples(msaa_samples);
	if (width == new_width && height == new_height && samples == msaa_samples && velocity_texture != 0)
		return;

	Destroy();

	width = new_width;
	height = new_height;
	samples = msaa_samples;

	glGenTextures(1, &velocity_texture);
	if (samples >= 2)
	{
		glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, velocity_texture);
		glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RG16F, width, height, GL_FALSE);

		glGenTextures(1, &resolved_texture);
		glBindTexture(GL_TEXTURE_2D, resolved_texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, width, height, 0, GL_RG, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glGenFramebuffers(1, &resolve_framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, resolve_framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolved_texture, 0);
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, velocity_texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, width, height, 0, GL_RG, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}

	rend_ClearBoundTextures();
}

void MotionVectorResources::Destroy()
{
	glDeleteTextures(1, &velocity_texture);
	glDeleteTextures(1, &resolved_texture);
	glDeleteFramebuffers(1, &resolve_framebuffer);
	velocity_texture = 0;
	resolved_texture = 0;
	resolve_framebuffer = 0;
	width = height = samples = 0;
}

void MotionVectorResources::AttachToFramebuffer(GLuint framebuffer)
{
	if (velocity_texture == 0)
		return;

	GLenum texture_type = samples >= 2 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, texture_type, velocity_texture, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
#ifndef _NDEBUG
	GLenum fbstatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (fbstatus != GL_FRAMEBUFFER_COMPLETE)
	{
		Error("MotionVectorResources::AttachToFramebuffer: Framebuffer object is incomplete!");
	}
#endif
}

void MotionVectorResources::ClearAttached(GLuint framebuffer)
{
	if (velocity_texture == 0)
		return;

	GLint old_draw = 0;
	GLint old_draw_buffer = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
	glGetIntegerv(GL_DRAW_BUFFER, &old_draw_buffer);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
	GLenum draw_buffer = GL_COLOR_ATTACHMENT1;
	glDrawBuffers(1, &draw_buffer);
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	glClearBufferfv(GL_COLOR, 0, zero);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
	glDrawBuffer(old_draw_buffer);
}

GLuint MotionVectorResources::TextureForRead(GLuint source_framebuffer)
{
	if (velocity_texture == 0)
		return 0;

	if (samples < 2)
		return velocity_texture;

	GLint old_read = 0, old_draw = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, source_framebuffer);
	glReadBuffer(GL_COLOR_ATTACHMENT1);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_framebuffer);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
	return resolved_texture;
}

void Framebuffer::BindForRead()
{
	if (m_samples >= 2)
	{
		SubColorBlit();
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_subname);
	}
	else
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_name);
	}
}

GLuint Framebuffer::ColorTextureForRead()
{
	SubColorBlit();
	return (m_samples >= 2) ? m_subcolorname : m_colorname;
}

GLuint Framebuffer::DepthTextureForRead()
{
	SubColorBlit();
	return (m_samples >= 2) ? m_subdepthname : m_depthname;
}

static float ClampBloomSetting(float value)
{
	if (value < 0.0f)
		return 0.0f;
	if (value > 1.0f)
		return 1.0f;
	return value;
}

void BloomResources::InitShaders()
{
	extern const char* blitVertexSrc;
	extern const char* bloomThresholdFragmentSrc;
	extern const char* bloomDownsampleFragmentSrc;
	extern const char* bloomMergeFragmentSrc;
	extern const char* bloomCompositeFragmentSrc;

	thresholdshader.AttachSource(blitVertexSrc, bloomThresholdFragmentSrc);
	thresholdshader.Use();
	GLint threshold_source = thresholdshader.FindUniform("heh");
	GLint threshold_depth = thresholdshader.FindUniform("depth_source");
	if (threshold_source != -1)
		glUniform1i(threshold_source, 0);
	if (threshold_depth != -1)
		glUniform1i(threshold_depth, 2);
	threshold_gamma = thresholdshader.FindUniform("gamma");
	threshold_value = thresholdshader.FindUniform("bloom_threshold");
	threshold_use_depth_mask = thresholdshader.FindUniform("use_depth_mask");
	if (threshold_gamma == -1 || threshold_value == -1 || threshold_use_depth_mask == -1)
		Error("BloomResources::InitShaders: Failed to find threshold uniforms!");

	downsampleshader.AttachSource(blitVertexSrc, bloomDownsampleFragmentSrc);
	downsampleshader.Use();
	GLint downsample_source = downsampleshader.FindUniform("heh");
	if (downsample_source != -1)
		glUniform1i(downsample_source, 0);

	mergeshader.AttachSource(blitVertexSrc, bloomMergeFragmentSrc);
	mergeshader.Use();
	GLint merge_cur = mergeshader.FindUniform("MergeCur");
	GLint merge_prev = mergeshader.FindUniform("MergePrev");
	if (merge_cur != -1)
		glUniform1i(merge_cur, 0);
	if (merge_prev != -1)
		glUniform1i(merge_prev, 1);
	merge_spread = mergeshader.FindUniform("bloomSpread");
	if (merge_spread == -1)
		Error("BloomResources::InitShaders: Failed to find merge spread uniform!");

	compositeshader.AttachSource(blitVertexSrc, bloomCompositeFragmentSrc);
	compositeshader.Use();
	GLint composite_source = compositeshader.FindUniform("heh");
	GLint composite_bloom = compositeshader.FindUniform("bloom");
	GLint composite_scene_source = compositeshader.FindUniform("scene_source");
	if (composite_source != -1)
		glUniform1i(composite_source, 0);
	if (composite_bloom != -1)
		glUniform1i(composite_bloom, 1);
	if (composite_scene_source != -1)
		glUniform1i(composite_scene_source, 2);
	composite_gamma = compositeshader.FindUniform("gamma");
	composite_intensity = compositeshader.FindUniform("bloom_intensity");
	if (composite_gamma == -1 || composite_intensity == -1)
		Error("BloomResources::InitShaders: Failed to find composite uniforms!");

	ShaderProgram::ClearBinding();
}

void BloomResources::DestroyShaders()
{
	thresholdshader.Destroy();
	downsampleshader.Destroy();
	mergeshader.Destroy();
	compositeshader.Destroy();
}

void BloomResources::DestroyFramebuffers()
{
	for (int i = 0; i < NUM_BLOOM_FBOS; i++)
		framebuffers[i].Destroy();
}

Framebuffer* BloomResources::Apply(Framebuffer* source, const renderer_preferred_state& pref_state,
	const rendering_state& render_state, float display_gamma, GLuint depth_texture)
{
	if (!pref_state.bloom_enabled || source == nullptr)
		return nullptr;

	int source_width = (int)source->Width();
	int source_height = (int)source->Height();
	if (source_width <= 0 || source_height <= 0)
	{
		source_width = render_state.screen_width;
		source_height = render_state.screen_height;
	}
	if (source_width < 16 || source_height < 16)
		return nullptr;

	const int max_downsample_levels = NUM_BLOOM_FBOS / 2;
	int widths[max_downsample_levels] = {};
	int heights[max_downsample_levels] = {};
	int downsample_count = 0;
	int width = source_width / 2;
	int height = source_height / 2;

	while (downsample_count < max_downsample_levels && width >= 8 && height >= 8)
	{
		widths[downsample_count] = width;
		heights[downsample_count] = height;
		downsample_count++;
		width /= 2;
		height /= 2;
	}

	if (downsample_count == 0)
		return nullptr;

	for (int i = 0; i < downsample_count; i++)
		framebuffers[i].Update(widths[i], heights[i], 0);

	int merge_count = downsample_count - 1;
	for (int i = 0; i < merge_count; i++)
	{
		int level = downsample_count - 2 - i;
		framebuffers[downsample_count + i].Update(widths[level], heights[level], 0);
	}

	for (int i = downsample_count + merge_count; i < NUM_BLOOM_FBOS; i++)
		framebuffers[i].Destroy();

	thresholdshader.Use();
	glUniform1f(threshold_gamma, display_gamma);
	glUniform1f(threshold_value, ClampBloomSetting(pref_state.bloom_threshold));
	glUniform1i(threshold_use_depth_mask, depth_texture != 0);
	rend_ClearBoundTextures();
	GL_BindFramebufferTexture(source->ColorTextureForRead(), 0, GL_LINEAR);
	if (depth_texture != 0)
		GL_BindFramebufferTexture(depth_texture, 2, GL_NEAREST);
	GL_DrawFramebufferQuad(framebuffers[0].Handle(), 0, 0, widths[0], heights[0]);

	downsampleshader.Use();
	for (int i = 1; i < downsample_count; i++)
	{
		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(framebuffers[i - 1].ColorTextureForRead(), 0, GL_LINEAR);
		GL_DrawFramebufferQuad(framebuffers[i].Handle(), 0, 0, widths[i], heights[i]);
		downsampleshader.Use();
	}

	float bloom_spread = 0.5f + ClampBloomSetting(pref_state.bloom_spread) * 0.375f;
	int previous_index = downsample_count - 1;
	int output_index = downsample_count;
	for (int level = downsample_count - 2; level >= 0; level--)
	{
		mergeshader.Use();
		glUniform1f(merge_spread, bloom_spread);
		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(framebuffers[level].ColorTextureForRead(), 0, GL_LINEAR);
		GL_BindFramebufferTexture(framebuffers[previous_index].ColorTextureForRead(), 1, GL_LINEAR);
		GL_DrawFramebufferQuad(framebuffers[output_index].Handle(), 0, 0, widths[level], heights[level]);
		previous_index = output_index;
		output_index++;
	}

	return &framebuffers[previous_index];
}

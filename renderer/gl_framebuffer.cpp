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

Framebuffer::Framebuffer()
{
	m_width = m_height = 0;
	m_name = m_subname = m_colorname = m_subcolorname = m_depthname = 0;
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

		//Do attachment for the sub framebuffer
		glBindFramebuffer(GL_FRAMEBUFFER, m_subname);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_subcolorname, 0);

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
	m_subname = m_subcolorname = 0;
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
			m_width, m_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

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

void Framebuffer::BlitToRaw(GLuint target, unsigned int x, unsigned int y, unsigned int w, unsigned int h)
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
		x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
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

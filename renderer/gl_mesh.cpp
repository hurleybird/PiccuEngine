/*
* Descent 3: Piccu Engine
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
#include "gl_mesh.h"

#include <algorithm>

VertexBuffer::VertexBuffer() : VertexBuffer(true, false)
{
}

VertexBuffer::VertexBuffer(bool allow_dynamic, bool dynamic_hint)
{
	m_name = 0;
	m_vaoname = 0;
	m_size = 0;
	m_vertexcount = 0;
	m_appendcounter = 0;
	m_dynamic_hint = dynamic_hint;
}

void VertexBuffer::Initialize(uint32_t numvertices, uint32_t datasize, void* data)
{
	if (m_vaoname == 0)
	{
		glGenVertexArrays(1, &m_vaoname);
		glGenBuffers(1, &m_name);
	}

	m_appendcounter = 0;
	glBindVertexArray(m_vaoname);

	glBindBuffer(GL_ARRAY_BUFFER, m_name);
	glBufferData(GL_ARRAY_BUFFER, datasize, data, m_dynamic_hint ? GL_STREAM_DRAW : GL_STATIC_DRAW);

	//Create the standard vertex attributes
	//Position
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RendVertex), (void*)offsetof(RendVertex, position));

	//Color
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_BYTE, GL_TRUE, sizeof(RendVertex), (void*)offsetof(RendVertex, r));

	//Normal
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(RendVertex), (void*)offsetof(RendVertex, normal));

	//Lightmap page
	glEnableVertexAttribArray(3);
	glVertexAttribIPointer(3, 1, GL_INT, sizeof(RendVertex), (void*)offsetof(RendVertex, lmpage));

	//Base UV
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(RendVertex), (void*)offsetof(RendVertex, u1));

	//Overlay UV
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(RendVertex), (void*)offsetof(RendVertex, u2));

	//UV slide
	glEnableVertexAttribArray(6);
	glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, sizeof(RendVertex), (void*)offsetof(RendVertex, u2));

	m_size = datasize;
	m_vertexcount = numvertices;

	rend_RestoreLegacy();
}

void VertexBuffer::Update(uint32_t byteoffset, uint32_t datasize, void* data)
{
	assert(m_vaoname != 0);
	assert(byteoffset + datasize <= m_size);
	glBindVertexArray(m_vaoname);

	glBindBuffer(GL_ARRAY_BUFFER, m_name);
	glBufferSubData(GL_ARRAY_BUFFER, byteoffset, datasize, data);
}

uint32_t VertexBuffer::Append(uint32_t size, void* data)
{
	assert(m_vaoname != 0);
	assert(size <= m_size);
	glBindVertexArray(m_vaoname);

	glBindBuffer(GL_ARRAY_BUFFER, m_name);
	if (m_appendcounter + size > m_size)
	{
		m_appendcounter = 0;
		glBufferData(GL_ARRAY_BUFFER, m_size, nullptr, m_dynamic_hint ? GL_STREAM_DRAW : GL_STATIC_DRAW);
	}
	glBufferSubData(GL_ARRAY_BUFFER, m_appendcounter, size, data);
	uint32_t initial = m_appendcounter;
	m_appendcounter += size;

	return initial;
}

void VertexBuffer::Bind() const
{
	assert(m_vaoname != 0);
	glBindVertexArray(m_vaoname);
}

void VertexBuffer::BindBitmap(int bmhandle) const
{
	rend_BindBitmap(bmhandle);
}

void VertexBuffer::BindLightmap(int lmhandle) const
{
	rend_BindLightmap(lmhandle);
}

static GLenum GetGLPrimitiveType(PrimitiveType mode)
{
	switch (mode)
	{
	case PrimitiveType::Triangles:
		return GL_TRIANGLES;
	case PrimitiveType::Lines:
		return GL_LINES;
	case PrimitiveType::Points:
		return GL_POINTS;
	}

	return GL_TRIANGLES; //blarg
}

void VertexBuffer::Draw(PrimitiveType mode) const
{
	glDrawArrays(GetGLPrimitiveType(mode), 0, m_vertexcount);
}

void VertexBuffer::Draw(PrimitiveType mode, ElementRange range) const
{
	assert(range.offset + range.count <= m_vertexcount);
	glDrawArrays(GetGLPrimitiveType(mode), range.offset, range.count);
}

void VertexBuffer::DrawIndexed(PrimitiveType mode, ElementRange range) const
{
	glDrawElements(GetGLPrimitiveType(mode), range.count, GL_UNSIGNED_INT, (const void*)(range.offset * sizeof(uint32_t)));
}

void VertexBuffer::Destroy()
{
	if (m_vaoname != 0)
	{
		glDeleteBuffers(1, &m_name);
		glDeleteVertexArrays(1, &m_vaoname);
		m_name = m_vaoname = 0;
	}
	m_size = m_vertexcount = 0;
}

IndexBuffer::IndexBuffer() : IndexBuffer(true, false)
{
}

IndexBuffer::IndexBuffer(bool allow_dynamic, bool dynamic_hint)
{
	m_name = 0;
	m_size = 0;
	m_dynamic_hint = dynamic_hint;
}

void IndexBuffer::Initialize(uint32_t numindices, uint32_t datasize, void* data)
{
	if (m_size != 0 && datasize > m_size)
	{
		//Need to make a new buffer
		Destroy();
	}

	if (m_name == 0)
	{
		glGenBuffers(1, &m_name);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_name);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, datasize, data, m_dynamic_hint ? GL_STREAM_DRAW : GL_STATIC_DRAW);
		m_size = datasize;
	}
	else
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_name);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, datasize, data, m_dynamic_hint ? GL_STREAM_DRAW : GL_STATIC_DRAW); //always orphan, maybe faster for dynamic meshes?
		//glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, datasize, data);
	}
}

void IndexBuffer::Update(uint32_t byteoffset, uint32_t datasize, void* data)
{
	assert(m_name != 0);
	assert(byteoffset + datasize <= m_size);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_name);
	glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, byteoffset, datasize, data);
}

void IndexBuffer::Bind() const
{
	assert(m_name != 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_name);
}

void IndexBuffer::Destroy()
{
	if (m_name != 0)
	{
		glDeleteBuffers(1, &m_name);
		m_name = 0;
	}
}

void rendTEMP_UnbindVertexBuffer()
{
	rend_RestoreLegacy();
}

void rendTEMP_ClearShaderBinding()
{
	ShaderProgram::ClearBinding();
}

bool rendTEMP_DepthClampEnabled()
{
	return glIsEnabled(GL_DEPTH_CLAMP) == GL_TRUE;
}

void rendTEMP_SetDepthClamp(bool state)
{
	if (state)
		glEnable(GL_DEPTH_CLAMP);
	else
		glDisable(GL_DEPTH_CLAMP);
}

void rendTEMP_SaveScissorState(rendTEMP_ScissorState* state)
{
	state->enabled = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
	glGetIntegerv(GL_SCISSOR_BOX, state->box);
}

static int ScaleFloor(int value, int src_size, int dst_size)
{
	return (value * dst_size) / src_size;
}

static int ScaleCeil(int value, int src_size, int dst_size)
{
	return ((value * dst_size) + src_size - 1) / src_size;
}

void rendTEMP_SetScissorRect(int left, int top, int right, int bottom, int screen_width, int screen_height)
{
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);

	if (screen_width <= 0 || screen_height <= 0)
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(viewport[0], viewport[1], 0, 0);
		return;
	}

	bool empty_rect = right < left || bottom < top;
	left = std::max(0, std::min(left, screen_width - 1));
	right = std::max(0, std::min(right, screen_width - 1));
	top = std::max(0, std::min(top, screen_height - 1));
	bottom = std::max(0, std::min(bottom, screen_height - 1));
	if (empty_rect || right < left || bottom < top)
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(viewport[0], viewport[1], 0, 0);
		return;
	}

	int x1 = viewport[0] + ScaleFloor(left, screen_width, viewport[2]);
	int x2 = viewport[0] + ScaleCeil(right + 1, screen_width, viewport[2]);
	int top_edge = ScaleFloor(top, screen_height, viewport[3]);
	int bottom_edge = ScaleCeil(bottom + 1, screen_height, viewport[3]);
	int y1 = viewport[1] + viewport[3] - bottom_edge;
	int y2 = viewport[1] + viewport[3] - top_edge;

	if (glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE)
	{
		GLint current[4];
		glGetIntegerv(GL_SCISSOR_BOX, current);
		x1 = std::max(x1, current[0]);
		y1 = std::max(y1, current[1]);
		x2 = std::min(x2, current[0] + current[2]);
		y2 = std::min(y2, current[1] + current[3]);
	}

	glEnable(GL_SCISSOR_TEST);
	glScissor(x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1));
}

void rendTEMP_RestoreScissorState(const rendTEMP_ScissorState* state)
{
	glScissor(state->box[0], state->box[1], state->box[2], state->box[3]);
	if (state->enabled)
		glEnable(GL_SCISSOR_TEST);
	else
		glDisable(GL_SCISSOR_TEST);
}

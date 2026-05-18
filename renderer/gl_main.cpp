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
#include "gl_local.h"
#include "gameloop.h"
#include "rtperformance.h"
#include <math.h>

static float mat4_identity[16] =
{ 1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 1, 0,
	0, 0, 0, 1 };

static void GL4PerfGpuDrain(const char* marker_name)
{
	(void)marker_name;
}

static constexpr int GL4_GPU_FRAME_QUERY_COUNT = 8;
static GLuint GL4_gpu_frame_queries[GL4_GPU_FRAME_QUERY_COUNT] = {};
static bool GL4_gpu_frame_queries_initialized = false;
static bool GL4_gpu_frame_query_active = false;
static int GL4_gpu_frame_query_active_index = -1;
static int GL4_gpu_frame_query_write_index = 0;
struct GL4GpuFrameQueryState
{
	bool pending = false;
	int requested_samples = 0;
	int actual_samples = 0;
	int ssaa_factor = 1;
};
static GL4GpuFrameQueryState GL4_gpu_frame_query_state[GL4_GPU_FRAME_QUERY_COUNT];

static bool GL4PerfGpuFrameQueriesAvailable()
{
	return glGenQueries != nullptr && glDeleteQueries != nullptr &&
		glBeginQuery != nullptr && glEndQuery != nullptr &&
		glGetQueryObjectiv != nullptr && glGetQueryObjectui64v != nullptr;
}

static void GL4PerfGpuFramePoll()
{
	if (!Perf_markers_enabled || !GL4_gpu_frame_queries_initialized)
		return;

	for (int i = 0; i < GL4_GPU_FRAME_QUERY_COUNT; i++)
	{
		if (!GL4_gpu_frame_query_state[i].pending)
			continue;

		GLint available = 0;
		glGetQueryObjectiv(GL4_gpu_frame_queries[i], GL_QUERY_RESULT_AVAILABLE, &available);
		if (!available)
			continue;

		GLuint64 elapsed_ns = 0;
		glGetQueryObjectui64v(GL4_gpu_frame_queries[i], GL_QUERY_RESULT, &elapsed_ns);

		char marker[96];
		snprintf(marker, sizeof(marker), "GPU.FrameToBeforeSwap req=%d actual=%d ssaa=%d",
			GL4_gpu_frame_query_state[i].requested_samples,
			GL4_gpu_frame_query_state[i].actual_samples,
			GL4_gpu_frame_query_state[i].ssaa_factor);
		PerfMarkersRecordDuration(marker, PerfMarkersNow(), (double)elapsed_ns / 1000000000.0);
		GL4_gpu_frame_query_state[i].pending = false;
	}
}

static void GL4PerfGpuFrameBegin(const Framebuffer& framebuffer, int ssaa_factor)
{
	if (!Perf_markers_enabled || GL4_gpu_frame_query_active || !GL4PerfGpuFrameQueriesAvailable())
		return;

	if (!GL4_gpu_frame_queries_initialized)
	{
		glGenQueries(GL4_GPU_FRAME_QUERY_COUNT, GL4_gpu_frame_queries);
		GL4_gpu_frame_queries_initialized = true;
	}

	GL4PerfGpuFramePoll();

	int index = GL4_gpu_frame_query_write_index;
	if (GL4_gpu_frame_query_state[index].pending)
		return;

	GL4_gpu_frame_query_state[index].requested_samples = (int)framebuffer.RequestedSamples();
	GL4_gpu_frame_query_state[index].actual_samples = (int)framebuffer.Samples();
	GL4_gpu_frame_query_state[index].ssaa_factor = ssaa_factor;
	glBeginQuery(GL_TIME_ELAPSED, GL4_gpu_frame_queries[index]);
	GL4_gpu_frame_query_active = true;
	GL4_gpu_frame_query_active_index = index;
}

static void GL4PerfGpuFrameEndBeforeSwap()
{
	if (!GL4_gpu_frame_query_active)
		return;

	glEndQuery(GL_TIME_ELAPSED);
	GL4_gpu_frame_query_state[GL4_gpu_frame_query_active_index].pending = true;
	GL4_gpu_frame_query_active = false;
	GL4_gpu_frame_query_write_index =
		(GL4_gpu_frame_query_active_index + 1) % GL4_GPU_FRAME_QUERY_COUNT;
	GL4_gpu_frame_query_active_index = -1;
}

static void GL4PerfGpuFrameShutdown()
{
	if (!GL4_gpu_frame_queries_initialized || !GL4PerfGpuFrameQueriesAvailable())
		return;

	if (GL4_gpu_frame_query_active)
	{
		glEndQuery(GL_TIME_ELAPSED);
		GL4_gpu_frame_query_active = false;
		GL4_gpu_frame_query_active_index = -1;
	}

	glDeleteQueries(GL4_GPU_FRAME_QUERY_COUNT, GL4_gpu_frame_queries);
	memset(GL4_gpu_frame_queries, 0, sizeof(GL4_gpu_frame_queries));
	memset(GL4_gpu_frame_query_state, 0, sizeof(GL4_gpu_frame_query_state));
	GL4_gpu_frame_queries_initialized = false;
	GL4_gpu_frame_query_write_index = 0;
}

enum GL4GpuSplitPoint
{
	GL4_GPU_SPLIT_START = 0,
	GL4_GPU_SPLIT_AFTER_WORLD_START,
	GL4_GPU_SPLIT_AFTER_WORLD_GEOMETRY,
	GL4_GPU_SPLIT_AFTER_WORLD_POSTRENDER,
	GL4_GPU_SPLIT_AFTER_WORLD_END,
	GL4_GPU_SPLIT_AFTER_MAIN_WORLD,
	GL4_GPU_SPLIT_AFTER_ROOM_CHANGE,
	GL4_GPU_SPLIT_AFTER_MATCENS,
	GL4_GPU_SPLIT_AFTER_RENDER_EVENTS,
	GL4_GPU_SPLIT_AFTER_CAPTURE_BLOOM,
	GL4_GPU_SPLIT_AFTER_MAIN_VIEW,
	GL4_GPU_SPLIT_AFTER_SMALL_VIEWS,
	GL4_GPU_SPLIT_AFTER_HUD,
	GL4_GPU_SPLIT_AFTER_CINEMATIC,
	GL4_GPU_SPLIT_AFTER_DEBUG,
	GL4_GPU_SPLIT_AFTER_UI,
	GL4_GPU_SPLIT_BEFORE_POST,
	GL4_GPU_SPLIT_AFTER_PREP,
	GL4_GPU_SPLIT_AFTER_GTAO,
	GL4_GPU_SPLIT_AFTER_BLOOM,
	GL4_GPU_SPLIT_BEFORE_SWAP,
	GL4_GPU_SPLIT_COUNT
};

static constexpr int GL4_GPU_SPLIT_QUERY_COUNT = 8;
static GLuint GL4_gpu_split_queries[GL4_GPU_SPLIT_QUERY_COUNT][GL4_GPU_SPLIT_COUNT] = {};
static bool GL4_gpu_split_queries_initialized = false;
static int GL4_gpu_split_write_index = 0;
static int GL4_gpu_split_active_index = -1;
struct GL4GpuSplitQueryState
{
	bool pending = false;
	bool active = false;
	bool has_point[GL4_GPU_SPLIT_COUNT] = {};
	int requested_samples = 0;
	int actual_samples = 0;
	int ssaa_factor = 1;
};
static GL4GpuSplitQueryState GL4_gpu_split_query_state[GL4_GPU_SPLIT_QUERY_COUNT];

static bool GL4PerfGpuSplitQueriesAvailable()
{
	return glGenQueries != nullptr && glDeleteQueries != nullptr &&
		glQueryCounter != nullptr && glGetQueryObjectiv != nullptr &&
		glGetQueryObjectui64v != nullptr;
}

static void GL4PerfGpuSplitRecord(const char* label, const GL4GpuSplitQueryState& state,
	GLuint64 start, GLuint64 end)
{
	if (end < start)
		return;

	char marker[96];
	snprintf(marker, sizeof(marker), "GPU.Split.%s req=%d actual=%d ssaa=%d",
		label, state.requested_samples, state.actual_samples, state.ssaa_factor);
	PerfMarkersRecordDuration(marker, PerfMarkersNow(), (double)(end - start) / 1000000000.0);
}

static void GL4PerfGpuSplitPoll()
{
	if (!Perf_markers_enabled || !GL4_gpu_split_queries_initialized)
		return;

	for (int i = 0; i < GL4_GPU_SPLIT_QUERY_COUNT; i++)
	{
		GL4GpuSplitQueryState& state = GL4_gpu_split_query_state[i];
		if (!state.pending)
			continue;

		GLint available = 0;
		glGetQueryObjectiv(GL4_gpu_split_queries[i][GL4_GPU_SPLIT_BEFORE_SWAP],
			GL_QUERY_RESULT_AVAILABLE, &available);
		if (!available)
			continue;

		GLuint64 t[GL4_GPU_SPLIT_COUNT] = {};
		for (int point = 0; point < GL4_GPU_SPLIT_COUNT; point++)
		{
			if (!state.has_point[point])
				continue;
			glGetQueryObjectui64v(GL4_gpu_split_queries[i][point], GL_QUERY_RESULT, &t[point]);
		}

		if (state.has_point[GL4_GPU_SPLIT_START] &&
			state.has_point[GL4_GPU_SPLIT_BEFORE_POST])
		{
			GL4PerfGpuSplitRecord("Scene", state, t[GL4_GPU_SPLIT_START],
				t[GL4_GPU_SPLIT_BEFORE_POST]);
		}
		if (state.has_point[GL4_GPU_SPLIT_START] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_MAIN_WORLD])
		{
			GL4PerfGpuSplitRecord("MainWorld", state, t[GL4_GPU_SPLIT_START],
				t[GL4_GPU_SPLIT_AFTER_MAIN_WORLD]);
		}
		if (state.has_point[GL4_GPU_SPLIT_START] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_WORLD_START])
		{
			GL4PerfGpuSplitRecord("WorldStart", state, t[GL4_GPU_SPLIT_START],
				t[GL4_GPU_SPLIT_AFTER_WORLD_START]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_WORLD_START] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_WORLD_GEOMETRY])
		{
			GL4PerfGpuSplitRecord("WorldGeometry", state, t[GL4_GPU_SPLIT_AFTER_WORLD_START],
				t[GL4_GPU_SPLIT_AFTER_WORLD_GEOMETRY]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_WORLD_GEOMETRY] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_WORLD_POSTRENDER])
		{
			GL4PerfGpuSplitRecord("WorldPostRender", state, t[GL4_GPU_SPLIT_AFTER_WORLD_GEOMETRY],
				t[GL4_GPU_SPLIT_AFTER_WORLD_POSTRENDER]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_WORLD_POSTRENDER] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_WORLD_END])
		{
			GL4PerfGpuSplitRecord("WorldEnd", state, t[GL4_GPU_SPLIT_AFTER_WORLD_POSTRENDER],
				t[GL4_GPU_SPLIT_AFTER_WORLD_END]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_MAIN_WORLD] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_ROOM_CHANGE])
		{
			GL4PerfGpuSplitRecord("RoomChange", state, t[GL4_GPU_SPLIT_AFTER_MAIN_WORLD],
				t[GL4_GPU_SPLIT_AFTER_ROOM_CHANGE]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_ROOM_CHANGE] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_MATCENS])
		{
			GL4PerfGpuSplitRecord("Matcens", state, t[GL4_GPU_SPLIT_AFTER_ROOM_CHANGE],
				t[GL4_GPU_SPLIT_AFTER_MATCENS]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_MATCENS] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_RENDER_EVENTS])
		{
			GL4PerfGpuSplitRecord("RenderEvents", state, t[GL4_GPU_SPLIT_AFTER_MATCENS],
				t[GL4_GPU_SPLIT_AFTER_RENDER_EVENTS]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_RENDER_EVENTS] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_CAPTURE_BLOOM])
		{
			GL4PerfGpuSplitRecord("CaptureBloom", state, t[GL4_GPU_SPLIT_AFTER_RENDER_EVENTS],
				t[GL4_GPU_SPLIT_AFTER_CAPTURE_BLOOM]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_CAPTURE_BLOOM] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_MAIN_VIEW])
		{
			GL4PerfGpuSplitRecord("MainViewEnd", state, t[GL4_GPU_SPLIT_AFTER_CAPTURE_BLOOM],
				t[GL4_GPU_SPLIT_AFTER_MAIN_VIEW]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_MAIN_WORLD] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_MAIN_VIEW])
		{
			GL4PerfGpuSplitRecord("MainLate", state, t[GL4_GPU_SPLIT_AFTER_MAIN_WORLD],
				t[GL4_GPU_SPLIT_AFTER_MAIN_VIEW]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_MAIN_VIEW] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_SMALL_VIEWS])
		{
			GL4PerfGpuSplitRecord("SmallViews", state, t[GL4_GPU_SPLIT_AFTER_MAIN_VIEW],
				t[GL4_GPU_SPLIT_AFTER_SMALL_VIEWS]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_SMALL_VIEWS] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_HUD])
		{
			GL4PerfGpuSplitRecord("HUD", state, t[GL4_GPU_SPLIT_AFTER_SMALL_VIEWS],
				t[GL4_GPU_SPLIT_AFTER_HUD]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_HUD] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_CINEMATIC])
		{
			GL4PerfGpuSplitRecord("Cinematic", state, t[GL4_GPU_SPLIT_AFTER_HUD],
				t[GL4_GPU_SPLIT_AFTER_CINEMATIC]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_CINEMATIC] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_DEBUG])
		{
			GL4PerfGpuSplitRecord("DebugStats", state, t[GL4_GPU_SPLIT_AFTER_CINEMATIC],
				t[GL4_GPU_SPLIT_AFTER_DEBUG]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_DEBUG] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_UI])
		{
			GL4PerfGpuSplitRecord("UI", state, t[GL4_GPU_SPLIT_AFTER_DEBUG],
				t[GL4_GPU_SPLIT_AFTER_UI]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_UI] &&
			state.has_point[GL4_GPU_SPLIT_BEFORE_POST])
		{
			GL4PerfGpuSplitRecord("SceneTail", state, t[GL4_GPU_SPLIT_AFTER_UI],
				t[GL4_GPU_SPLIT_BEFORE_POST]);
		}
		if (state.has_point[GL4_GPU_SPLIT_BEFORE_POST] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_PREP])
		{
			GL4PerfGpuSplitRecord("PostPrep", state, t[GL4_GPU_SPLIT_BEFORE_POST],
				t[GL4_GPU_SPLIT_AFTER_PREP]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_PREP] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_GTAO])
		{
			GL4PerfGpuSplitRecord("GTAO", state, t[GL4_GPU_SPLIT_AFTER_PREP],
				t[GL4_GPU_SPLIT_AFTER_GTAO]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_GTAO] &&
			state.has_point[GL4_GPU_SPLIT_AFTER_BLOOM])
		{
			GL4PerfGpuSplitRecord("Bloom", state, t[GL4_GPU_SPLIT_AFTER_GTAO],
				t[GL4_GPU_SPLIT_AFTER_BLOOM]);
		}
		if (state.has_point[GL4_GPU_SPLIT_AFTER_BLOOM] &&
			state.has_point[GL4_GPU_SPLIT_BEFORE_SWAP])
		{
			GL4PerfGpuSplitRecord("Backbuffer", state, t[GL4_GPU_SPLIT_AFTER_BLOOM],
				t[GL4_GPU_SPLIT_BEFORE_SWAP]);
		}
		if (state.has_point[GL4_GPU_SPLIT_START] &&
			state.has_point[GL4_GPU_SPLIT_BEFORE_SWAP])
		{
			GL4PerfGpuSplitRecord("Total", state, t[GL4_GPU_SPLIT_START],
				t[GL4_GPU_SPLIT_BEFORE_SWAP]);
		}

		memset(&state, 0, sizeof(state));
	}
}

static void GL4PerfGpuSplitMark(GL4GpuSplitPoint point)
{
	if (GL4_gpu_split_active_index < 0 || !GL4_gpu_split_queries_initialized)
		return;

	GL4GpuSplitQueryState& state = GL4_gpu_split_query_state[GL4_gpu_split_active_index];
	if (!state.active || state.has_point[point])
		return;

	glQueryCounter(GL4_gpu_split_queries[GL4_gpu_split_active_index][point], GL_TIMESTAMP);
	state.has_point[point] = true;
}

static GL4GpuSplitPoint GL4GpuSplitPointFromSceneMark(renderer_gpu_scene_mark mark)
{
	switch (mark)
	{
	case RENDERER_GPU_SCENE_AFTER_MAIN_WORLD:
		return GL4_GPU_SPLIT_AFTER_MAIN_WORLD;
	case RENDERER_GPU_SCENE_AFTER_WORLD_START:
		return GL4_GPU_SPLIT_AFTER_WORLD_START;
	case RENDERER_GPU_SCENE_AFTER_WORLD_GEOMETRY:
		return GL4_GPU_SPLIT_AFTER_WORLD_GEOMETRY;
	case RENDERER_GPU_SCENE_AFTER_WORLD_POSTRENDER:
		return GL4_GPU_SPLIT_AFTER_WORLD_POSTRENDER;
	case RENDERER_GPU_SCENE_AFTER_WORLD_END:
		return GL4_GPU_SPLIT_AFTER_WORLD_END;
	case RENDERER_GPU_SCENE_AFTER_ROOM_CHANGE:
		return GL4_GPU_SPLIT_AFTER_ROOM_CHANGE;
	case RENDERER_GPU_SCENE_AFTER_MATCENS:
		return GL4_GPU_SPLIT_AFTER_MATCENS;
	case RENDERER_GPU_SCENE_AFTER_RENDER_EVENTS:
		return GL4_GPU_SPLIT_AFTER_RENDER_EVENTS;
	case RENDERER_GPU_SCENE_AFTER_CAPTURE_BLOOM:
		return GL4_GPU_SPLIT_AFTER_CAPTURE_BLOOM;
	case RENDERER_GPU_SCENE_AFTER_MAIN_VIEW:
		return GL4_GPU_SPLIT_AFTER_MAIN_VIEW;
	case RENDERER_GPU_SCENE_AFTER_SMALL_VIEWS:
		return GL4_GPU_SPLIT_AFTER_SMALL_VIEWS;
	case RENDERER_GPU_SCENE_AFTER_HUD:
		return GL4_GPU_SPLIT_AFTER_HUD;
	case RENDERER_GPU_SCENE_AFTER_CINEMATIC:
		return GL4_GPU_SPLIT_AFTER_CINEMATIC;
	case RENDERER_GPU_SCENE_AFTER_DEBUG:
		return GL4_GPU_SPLIT_AFTER_DEBUG;
	case RENDERER_GPU_SCENE_AFTER_UI:
		return GL4_GPU_SPLIT_AFTER_UI;
	default:
		return GL4_GPU_SPLIT_COUNT;
	}
}

static void GL4PerfGpuSplitBegin(const Framebuffer& framebuffer, int ssaa_factor)
{
	if (!Perf_markers_enabled || GL4_gpu_split_active_index >= 0 || !GL4PerfGpuSplitQueriesAvailable())
		return;

	if (!GL4_gpu_split_queries_initialized)
	{
		glGenQueries(GL4_GPU_SPLIT_QUERY_COUNT * GL4_GPU_SPLIT_COUNT,
			&GL4_gpu_split_queries[0][0]);
		GL4_gpu_split_queries_initialized = true;
	}

	GL4PerfGpuSplitPoll();

	int index = GL4_gpu_split_write_index;
	if (GL4_gpu_split_query_state[index].pending || GL4_gpu_split_query_state[index].active)
		return;

	GL4GpuSplitQueryState& state = GL4_gpu_split_query_state[index];
	memset(&state, 0, sizeof(state));
	state.active = true;
	state.requested_samples = (int)framebuffer.RequestedSamples();
	state.actual_samples = (int)framebuffer.Samples();
	state.ssaa_factor = ssaa_factor;
	GL4_gpu_split_active_index = index;
	GL4PerfGpuSplitMark(GL4_GPU_SPLIT_START);
}

static void GL4PerfGpuSplitEndBeforeSwap()
{
	if (GL4_gpu_split_active_index < 0)
		return;

	GL4PerfGpuSplitMark(GL4_GPU_SPLIT_BEFORE_SWAP);
	GL4GpuSplitQueryState& state = GL4_gpu_split_query_state[GL4_gpu_split_active_index];
	state.pending = true;
	state.active = false;
	GL4_gpu_split_write_index = (GL4_gpu_split_active_index + 1) % GL4_GPU_SPLIT_QUERY_COUNT;
	GL4_gpu_split_active_index = -1;
}

static void GL4PerfGpuSplitShutdown()
{
	if (!GL4_gpu_split_queries_initialized || !GL4PerfGpuSplitQueriesAvailable())
		return;

	glDeleteQueries(GL4_GPU_SPLIT_QUERY_COUNT * GL4_GPU_SPLIT_COUNT,
		&GL4_gpu_split_queries[0][0]);
	memset(GL4_gpu_split_queries, 0, sizeof(GL4_gpu_split_queries));
	memset(GL4_gpu_split_query_state, 0, sizeof(GL4_gpu_split_query_state));
	GL4_gpu_split_queries_initialized = false;
	GL4_gpu_split_write_index = 0;
	GL4_gpu_split_active_index = -1;
}

static void GL4PerfFramebufferState(const char* phase, const Framebuffer& framebuffer,
	int slot, int ssaa_factor)
{
	if (!Perf_markers_enabled)
		return;

	char marker[96];
	snprintf(marker, sizeof(marker), "State.%s slot=%d %ux%u req=%u actual=%u ssaa=%d",
		phase, slot, (unsigned)framebuffer.Width(), (unsigned)framebuffer.Height(),
		(unsigned)framebuffer.RequestedSamples(), (unsigned)framebuffer.Samples(),
		ssaa_factor);
	PerfMarkersRecordDuration(marker, PerfMarkersNow(), 0.0);
}

static void GL4PerfFramebufferResourceState(const char* phase, const Framebuffer& framebuffer,
	int preferred_samples, int target_samples, int ssaa_factor, GLuint post_mask_texture,
	uint32_t post_mask_samples, bool post_mask_renderbuffer_storage, GLuint resolved_framebuffer,
	GLuint downscale_framebuffer, GLuint post_present_framebuffer)
{
	if (!Perf_markers_enabled)
		return;

	char marker[96];
	snprintf(marker, sizeof(marker), "State.GLFB.%s pref=%d target=%d req=%u act=%u %ux%u ssaa=%d",
		phase, preferred_samples, target_samples,
		(unsigned)framebuffer.RequestedSamples(), (unsigned)framebuffer.Samples(),
		(unsigned)framebuffer.Width(), (unsigned)framebuffer.Height(), ssaa_factor);
	PerfMarkersRecordDuration(marker, PerfMarkersNow(), 0.0);

	snprintf(marker, sizeof(marker), "State.GLObj.%s f=%u c=%u d=%u mask=%u/%u res=%u down=%u post=%u",
		phase, (unsigned)framebuffer.Handle(), (unsigned)framebuffer.ColorTextureRaw(),
		(unsigned)framebuffer.DepthTextureRaw(), (unsigned)post_mask_texture,
		(unsigned)post_mask_samples, (unsigned)resolved_framebuffer,
		(unsigned)downscale_framebuffer, (unsigned)post_present_framebuffer);
	PerfMarkersRecordDuration(marker, PerfMarkersNow(), 0.0);

	snprintf(marker, sizeof(marker), "State.GLStore.%s scene_rb=%d mask_rb=%d",
		phase, framebuffer.UsesMsaaRenderbufferStorage() ? 1 : 0,
		post_mask_renderbuffer_storage ? 1 : 0);
	PerfMarkersRecordDuration(marker, PerfMarkersNow(), 0.0);
}

static void GL4PerfRenderState(const char* phase)
{
	if (!Perf_markers_enabled)
		return;

	GLint draw_framebuffer = 0;
	GLint read_framebuffer = 0;
	GLint draw_buffer0 = 0;
	GLint draw_buffer1 = 0;
	GLint draw_buffer2 = 0;
	GLint read_buffer = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
	glGetIntegerv(GL_DRAW_BUFFER0, &draw_buffer0);
	glGetIntegerv(GL_DRAW_BUFFER1, &draw_buffer1);
	glGetIntegerv(GL_DRAW_BUFFER2, &draw_buffer2);
	glGetIntegerv(GL_READ_BUFFER, &read_buffer);

	GLboolean multisample_enabled = glIsEnabled(GL_MULTISAMPLE);
	GLboolean blend_enabled = glIsEnabled(GL_BLEND);
	GLboolean blend2_enabled = GL_FALSE;
	if (glIsEnabledi != nullptr)
		blend2_enabled = glIsEnabledi(GL_BLEND, 2);

	char marker[96];
	snprintf(marker, sizeof(marker), "State.GLState.%s draw=%d read=%d db=%x/%x/%x rb=%x ms=%d bl=%d/%d",
		phase, draw_framebuffer, read_framebuffer, draw_buffer0, draw_buffer1, draw_buffer2,
		read_buffer, multisample_enabled ? 1 : 0, blend_enabled ? 1 : 0,
		blend2_enabled ? 1 : 0);
	PerfMarkersRecordDuration(marker, PerfMarkersNow(), 0.0);
}

static void GL4PerfMsaaStageState(const char* phase, int frames_remaining, int target_samples)
{
	if (!Perf_markers_enabled)
		return;

	char marker[96];
	snprintf(marker, sizeof(marker), "State.MSAAStage.%s remaining=%d target=%d",
		phase, frames_remaining, target_samples);
	PerfMarkersRecordDuration(marker, PerfMarkersNow(), 0.0);
}

static void GL4PerfPresentState(const char* phase, int framebuffer_slot, bool post_present_pending)
{
	if (!Perf_markers_enabled)
		return;

	int swap_interval = -999;
#if defined(SDL3)
	swap_interval = SDL_GL_GetSwapInterval();
#elif defined(WIN32)
	if (dwglGetSwapIntervalEXT)
		swap_interval = dwglGetSwapIntervalEXT();
#endif

	char marker[96];
	snprintf(marker, sizeof(marker), "State.Present.%s swap_interval=%d slot=%d post_pending=%d",
		phase, swap_interval, framebuffer_slot, post_present_pending ? 1 : 0);
	PerfMarkersRecordDuration(marker, PerfMarkersNow(), 0.0);
}

static constexpr int MSAA_DOWNSHIFT_RELEASE_FRAMES = 4;

static float OpenGL_terrain_fog_start = 0.0f;
static float OpenGL_terrain_fog_end = 1.0f;

int GL4Renderer::SupersamplingFactor() const
{
	return RendererSupersamplingFactor(OpenGL_preferred_state);
}

static int GL4OverscanPercent(const renderer_preferred_state& state)
{
	if (!state.gtao_enabled)
		return 100;
	if (state.gtao_overscan_percent < 100)
		return 100;
	if (state.gtao_overscan_percent > 150)
		return 150;
	return state.gtao_overscan_percent;
}

int GL4Renderer::FramebufferWidth() const
{
	int logical_width = OpenGL_state.screen_width;
	const int overscan_percent = GL4OverscanPercent(OpenGL_preferred_state);
	if (overscan_percent > 100 && logical_width > 0)
		logical_width = (logical_width * overscan_percent + 99) / 100;
	return logical_width * SupersamplingFactor();
}

int GL4Renderer::FramebufferHeight() const
{
	int logical_height = OpenGL_state.screen_height;
	const int overscan_percent = GL4OverscanPercent(OpenGL_preferred_state);
	if (overscan_percent > 100 && logical_height > 0)
		logical_height = (logical_height * overscan_percent + 99) / 100;
	return logical_height * SupersamplingFactor();
}

int GL4Renderer::ScaledX(int x) const
{
	return (x + framebuffer_logical_offset_x) * SupersamplingFactor();
}

int GL4Renderer::ScaledY(int y) const
{
	return (y + framebuffer_logical_offset_y) * SupersamplingFactor();
}

int GL4Renderer::ScaledW(int w) const
{
	return w * SupersamplingFactor();
}

int GL4Renderer::ScaledH(int h) const
{
	return h * SupersamplingFactor();
}


void GL_Ortho(float* mat, float left, float right, float bottom, float top, float znear, float zfar)
{
	memset(mat, 0, sizeof(float[16]));
	mat[0] = 2 / (right - left);
	mat[5] = 2 / (top - bottom);
	mat[10] = -2 / (zfar - znear);
	mat[12] = -((right + left) / (right - left));
	mat[13] = -((top + bottom) / (top - bottom));
	mat[14] = -((zfar + znear) / (zfar - znear));
	mat[15] = 1;
}

void GL4Renderer::RefreshViewSize()
{
	int view_width = 0;
	int view_height = 0;

#if defined(SDL3)
	SDL_GetWindowSizeInPixels(GLWindow, &view_width, &view_height);
#elif defined(WIN32)
	RECT rect = {};
	if (hOpenGLWnd && GetClientRect((HWND)hOpenGLWnd, &rect))
	{
		view_width = rect.right - rect.left;
		view_height = rect.bottom - rect.top;
	}
#endif

	if (view_width <= 0)
		view_width = OpenGL_preferred_state.fullscreen ? OpenGL_preferred_state.width : OpenGL_preferred_state.window_width;
	if (view_height <= 0)
		view_height = OpenGL_preferred_state.fullscreen ? OpenGL_preferred_state.height : OpenGL_preferred_state.window_height;
	if (view_width <= 0)
		view_width = OpenGL_state.view_width > 0 ? OpenGL_state.view_width : 640;
	if (view_height <= 0)
		view_height = OpenGL_state.view_height > 0 ? OpenGL_state.view_height : 480;

	OpenGL_state.view_width = view_width;
	OpenGL_state.view_height = view_height;
}

void GL4Renderer::UpdatePresentRect()
{
	int width = OpenGL_preferred_state.width;
	int height = OpenGL_preferred_state.height;
	if (width <= 0)
		width = OpenGL_state.screen_width > 0 ? OpenGL_state.screen_width : OpenGL_state.view_width;
	if (height <= 0)
		height = OpenGL_state.screen_height > 0 ? OpenGL_state.screen_height : OpenGL_state.view_height;
	if (width <= 0)
		width = 640;
	if (height <= 0)
		height = 480;

	int view_width = OpenGL_state.view_width;
	int view_height = OpenGL_state.view_height;
	if (view_width <= 0)
		view_width = width;
	if (view_height <= 0)
		view_height = height;

	OpenGL_state.screen_width = width;
	OpenGL_state.screen_height = height;
	OpenGL_state.view_width = view_width;
	OpenGL_state.view_height = view_height;

	framebuffer_blit_x = 0;
	framebuffer_blit_y = 0;
	framebuffer_blit_w = view_width;
	framebuffer_blit_h = view_height;

	float baseAspect = width / (float)height;
	float trueAspect = view_width / (float)view_height;
	if (baseAspect <= 0.0f || trueAspect <= 0.0f || fabsf(baseAspect - trueAspect) < 0.001f)
		return;

	if (baseAspect < trueAspect) //base screen is less wide, so pillarbox it
	{
		int blit_w = (int)(view_height * baseAspect + 0.5f);
		if (blit_w < 1)
			blit_w = 1;
		if (blit_w > view_width)
			blit_w = view_width;
		framebuffer_blit_h = view_height;
		framebuffer_blit_y = 0;
		framebuffer_blit_w = blit_w;
		framebuffer_blit_x = (view_width - blit_w) / 2;
	}
	else //base screen is more wide, so letterbox it
	{
		int blit_h = (int)(view_width / baseAspect + 0.5f);
		if (blit_h < 1)
			blit_h = 1;
		if (blit_h > view_height)
			blit_h = view_height;
		framebuffer_blit_w = view_width;
		framebuffer_blit_x = 0;
		framebuffer_blit_h = blit_h;
		framebuffer_blit_y = (view_height - blit_h) / 2;
	}
}

void GL4Renderer::UpdateWindow()
{
	int width, height;
	if (!OpenGL_preferred_state.fullscreen)
	{
		OpenGL_state.view_width = OpenGL_preferred_state.window_width;
		OpenGL_state.view_height = OpenGL_preferred_state.window_height;
		width = OpenGL_preferred_state.width;
		height = OpenGL_preferred_state.height;

		//[ISB] center window
		ParentApplication->set_flags(OEAPP_WINDOWED);
#ifdef SDL3
		ParentApplication->set_sizepos(OEAPP_COORD_CENTERED, OEAPP_COORD_CENTERED, OpenGL_state.view_width, OpenGL_state.view_height);
#elif WIN32
		int mWidth = GetSystemMetrics(SM_CXSCREEN);
		int mHeight = GetSystemMetrics(SM_CYSCREEN);

		int orgX = (mWidth / 2 - OpenGL_state.view_width / 2);
		int orgY = (mHeight / 2 - OpenGL_state.view_height / 2);
		RECT rect = { orgX, orgY, orgX + OpenGL_state.view_width, orgY + OpenGL_state.view_height };
		AdjustWindowRectEx(&rect, WS_CAPTION, FALSE, 0);
		ParentApplication->set_sizepos(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
#endif
	}
	else
	{
		ParentApplication->set_flags(OEAPP_FULLSCREEN);
#ifdef SDL3
		SDL_GetWindowSizeInPixels(GLWindow, &OpenGL_state.view_width, &OpenGL_state.view_height);
#elif WIN32
		RECT rect;
		GetWindowRect((HWND)hOpenGLWnd, &rect);
		mprintf((0, "rect=%d %d %d %d\n", rect.top, rect.right, rect.bottom, rect.left));

		OpenGL_state.view_width = rect.right - rect.left;
		OpenGL_state.view_height = rect.bottom - rect.top;
#endif

		width = OpenGL_preferred_state.width;
		height = OpenGL_preferred_state.height;
	}

	OpenGL_state.screen_width = width;
	OpenGL_state.screen_height = height;
	RefreshViewSize();
	UpdatePresentRect();
}

void GL4Renderer::SetViewport()
{
	//[ISB] the hardware t&l code is AWFUL and the software t&l code won't compile.
	// Reverting it back to only ever using passthrough.
	// Projection
	//glMatrixMode(GL_PROJECTION);
	//glLoadIdentity();
	//glOrtho((GLfloat)0.0f, (GLfloat)(OpenGL_preferred_state.width), (GLfloat)(OpenGL_preferred_state.height), (GLfloat)0.0f, 0.0f, 1.0f);

	float left = 0;
	float right = OpenGL_preferred_state.width;
	float bottom = OpenGL_preferred_state.height;
	float top = 0;
	float znear = 0;
	float zfar = 1;

	float projection[16];
	GL_Ortho(projection, left, right, bottom, top, znear, zfar);

	UpdateLegacyBlock(projection, mat4_identity);
	// Viewport
	glViewport(0, 0, FramebufferWidth(), FramebufferHeight());

	// ModelView
	//glMatrixMode(GL_MODELVIEW);
	//glLoadIdentity();
}

// Sets some global preferences for the renderer
int GL4Renderer::SetPreferredState(renderer_preferred_state* pref_state)
{
	int retval = 1;
	renderer_preferred_state old_state = OpenGL_preferred_state;
	int old_msaa_samples = GL_GetSupportedMsaaSamples(RendererMsaaSamples(old_state));
	int new_msaa_samples = GL_GetSupportedMsaaSamples(RendererMsaaSamples(*pref_state));
	const bool staged_msaa_transition = false;
	const bool keep_deferred_msaa_transition = false;
	renderer_preferred_state applied_state = *pref_state;
	if (staged_msaa_transition || keep_deferred_msaa_transition)
	{
		msaa_deferred_preferred_state = *pref_state;
		msaa_deferred_preferred_state_valid = true;
		applied_state.msaa_samples = 0;
		applied_state.antialised = false;
	}
	else
	{
		msaa_deferred_preferred_state_valid = false;
		msaa_deferred_apply_pending = false;
		msaa_downshift_release_frames = 0;
		msaa_forced_off_target_samples = 0;
		msaa_forced_off_scene_presented = false;
	}

	OpenGL_preferred_state = applied_state;
	if (OpenGL_state.initted)
	{
		int reinit = 0;
		mprintf((0, "Inside pref state!\n"));

		// Change gamma if needed
		/*if( pref_state->width!=OpenGL_state.screen_width || pref_state->height!=OpenGL_state.screen_height || old_state.bit_depth!=pref_state->bit_depth)
		{
			reinit=1;
		}

		if( reinit )
		{
			opengl_Close();
			retval = opengl_Init( NULL, &OpenGL_preferred_state );
		}
		else
		{*/

		bool framebuffer_state_changed =
			pref_state->width != old_state.width || pref_state->height != old_state.height
			|| pref_state->window_width != old_state.window_width || pref_state->window_height != old_state.window_height
			|| pref_state->fullscreen != old_state.fullscreen || pref_state->antialised != old_state.antialised
			|| pref_state->supersampling_factor != old_state.supersampling_factor
			|| pref_state->msaa_samples != old_state.msaa_samples
			|| GL4OverscanPercent(*pref_state) != GL4OverscanPercent(old_state);
		if (pref_state->msaa_samples != old_state.msaa_samples ||
			pref_state->supersampling_factor != old_state.supersampling_factor ||
			pref_state->bloom_enabled != old_state.bloom_enabled ||
			pref_state->gtao_enabled != old_state.gtao_enabled)
		{
			mprintf((0, "GL4 SetPreferredState: msaa %u->%u aa %d->%d ssaa %u->%u bloom %d->%d gtao %d->%d.\n",
				(unsigned)old_state.msaa_samples, (unsigned)pref_state->msaa_samples,
				old_state.antialised ? 1 : 0, pref_state->antialised ? 1 : 0,
				(unsigned)old_state.supersampling_factor, (unsigned)pref_state->supersampling_factor,
				old_state.bloom_enabled ? 1 : 0, pref_state->bloom_enabled ? 1 : 0,
				old_state.gtao_enabled ? 1 : 0, pref_state->gtao_enabled ? 1 : 0));
		}
		if (staged_msaa_transition)
		{
			msaa_downshift_release_frames = MSAA_DOWNSHIFT_RELEASE_FRAMES;
			msaa_forced_off_target_samples = new_msaa_samples;
			msaa_forced_off_scene_presented = false;
			mprintf((0, "GL4 MSAA transition: forcing preferred %d->0->%d for %d presented frames.\n",
				old_msaa_samples, new_msaa_samples, msaa_downshift_release_frames));
		}
		bool gtao_buffers_changed =
			pref_state->gtao_enabled != old_state.gtao_enabled ||
			pref_state->gtao_resolution != old_state.gtao_resolution;

		if (framebuffer_state_changed)
		{
			UpdateWindow();
			SetViewport();
			UpdateFramebuffer();
		}
		else if (gtao_buffers_changed)
		{
			if (gtao.HasFramebuffers())
				glFinish();
			gtao.DestroyFramebuffers();
		}

	if (old_state.per_pixel_lighting != pref_state->per_pixel_lighting)
	{
		per_pixel_dynamic_light_count = 0;
		OpenGL_state.cur_light_state = (light_state)-1;
		legacy_draw_uniforms_dirty = true;
	}

		if (old_state.gamma != pref_state->gamma)
		{
			SetGammaValue(pref_state->gamma);
		}

		ApplySwapInterval();
		//}
	}
	else
	{
		OpenGL_preferred_state = applied_state;
	}

	return retval;
}

void GL4Renderer::StartFrame(int x1, int y1, int x2, int y2, int clear_flags)
{
	if (msaa_deferred_apply_pending && framebuffer_ok)
	{
		PERF_MARKER_SCOPE("Renderer.StartFrame.DeferredMsaaUpdate");
		msaa_deferred_apply_pending = false;
		GL4PerfMsaaStageState("ApplyStart", msaa_downshift_release_frames,
			msaa_forced_off_target_samples);
		if (msaa_deferred_preferred_state_valid)
		{
			OpenGL_preferred_state = msaa_deferred_preferred_state;
			msaa_deferred_preferred_state_valid = false;
		}
		UpdateWindow();
		SetViewport();
		UpdateFramebuffer();
		ApplySwapInterval();
	}

	if (post_present_pending_swap)
	{
		StartPostPresentFrame(x1, y1, x2, y2, clear_flags);
		return;
	}

	if (framebuffer_ok)
	{
		framebuffers[framebuffer_current_draw].MarkAllDirty();
		GL4PerfFramebufferState("StartFrame", framebuffers[framebuffer_current_draw],
			framebuffer_current_draw, SupersamplingFactor());
		GL4PerfGpuFrameBegin(framebuffers[framebuffer_current_draw], SupersamplingFactor());
		GL4PerfGpuSplitBegin(framebuffers[framebuffer_current_draw], SupersamplingFactor());
		if (RendererShouldCountMsaaTransitionFrame() &&
			msaa_downshift_release_frames > 0 &&
			framebuffers[framebuffer_current_draw].RequestedSamples() == 0)
		{
			msaa_forced_off_scene_presented = true;
			GL4PerfMsaaStageState("OffScene", msaa_downshift_release_frames,
				msaa_forced_off_target_samples);
		}
	}

	GLenum glclearflags = 0;
	if (clear_flags & RF_CLEAR_ZBUFFER)
		glclearflags |= GL_DEPTH_BUFFER_BIT;

	if (clear_flags & RF_CLEAR_COLOR)
	{
		glClearColor(0.0, 0.0, 0.0, 1.0);
		glclearflags |= GL_COLOR_BUFFER_BIT;
	}

	if (glclearflags != 0)
		glClear(glclearflags);
	if (framebuffer_ok)
	{
		if (!post_protection_mask_cleared_this_frame)
		{
			post_protection_mask.ClearAttached(framebuffers[framebuffer_current_draw].Handle());
			post_protection_mask_dirty = false;
			post_protection_mask_cleared_this_frame = true;
		}
		post_protection_mask.UseSceneDrawBuffers(framebuffers[framebuffer_current_draw].Handle());
	}
	motion_vectors_dirty = false;
	if (ao_suppression_draw_value != 0.0f)
		legacy_draw_uniforms_dirty = true;
	if (bloom_suppression_draw_value != 0.0f)
		legacy_draw_uniforms_dirty = true;
	if (ao_class_draw_value != 0)
		legacy_draw_uniforms_dirty = true;
	ao_suppression_draw_value = 0.0f;
	bloom_suppression_draw_value = 0.0f;
	ao_class_draw_value = 0;
	ao_weight_draw_value = 1.0f;
	if (framebuffer_ok && framebuffers[framebuffer_current_draw].Samples() >= 2)
		glEnable(GL_MULTISAMPLE);
	else
		glDisable(GL_MULTISAMPLE);
	if (framebuffer_state_snapshot_pending && framebuffer_ok)
	{
		framebuffer_state_snapshot_pending = false;
		const Framebuffer& framebuffer = framebuffers[framebuffer_current_draw];
		const int preferred_samples = GL_GetSupportedMsaaSamples(RendererMsaaSamples(OpenGL_preferred_state));
		const int target_samples = (int)framebuffer.RequestedSamples();
		GL4PerfFramebufferResourceState("StartAfterUpdate", framebuffer, preferred_samples,
			target_samples, SupersamplingFactor(), post_protection_mask.mask_texture,
			post_protection_mask.samples, post_protection_mask.msaa_renderbuffer_storage,
			resolved_framebuffer.Handle(),
			downscale_framebuffer.Handle(), post_present_framebuffer.Handle());
		GL4PerfRenderState("StartAfterUpdate");
	}

	OpenGL_state.clip_x1 = x1;
	OpenGL_state.clip_y1 = y1;
	OpenGL_state.clip_x2 = x2;
	OpenGL_state.clip_y2 = y2;

	//[ISB] Use the viewport to constrain the clipping window so that the new hardware code
	//can work with the legacy code.
	float projection[16];
	GL_Ortho(projection, 0, x2 - x1, y2 - y1, 0, 0, 1);

	UpdateLegacyBlock(projection, mat4_identity);

	glViewport(ScaledX(x1), FramebufferHeight() - ScaledY(y2), ScaledW(x2 - x1), ScaledH(y2 - y1));
}

// Flips the screen
void GL4Renderer::Flip()
{
	FlushFontBatch();

	if (post_present_pending_swap)
	{
		EndPostPresentFrame();
		return;
	}

	if (BeginPostPresentFrame())
		EndPostPresentFrame();
}

bool GL4Renderer::BeginPostPresentFrame()
{
	FlushFontBatch();

	if (post_present_pending_swap)
		return true;

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
	{
		Int3();
	}
#endif
#ifndef RELEASE
	RTP_INCRVALUE(texture_uploads, OpenGL_uploads);
	RTP_INCRVALUE(polys_drawn, OpenGL_polys_drawn);

	mprintf_at((1, 1, 0, "Uploads=%d    Polys=%d   Verts=%d   ", OpenGL_uploads, OpenGL_polys_drawn, OpenGL_verts_processed));
	mprintf_at((1, 2, 0, "Sets= 0:%d   1:%d   2:%d   3:%d   ", OpenGL_sets_this_frame[0], OpenGL_sets_this_frame[1], OpenGL_sets_this_frame[2], OpenGL_sets_this_frame[3]));
	mprintf_at((1, 3, 0, "Sets= 4:%d   5:%d  ", OpenGL_sets_this_frame[4], OpenGL_sets_this_frame[5]));
	for (int i = 0; i < 10; i++)
	{
		OpenGL_sets_this_frame[i] = 0;
	}
#endif

	OpenGL_last_frame_polys_drawn = OpenGL_polys_drawn;
	OpenGL_last_frame_verts_processed = OpenGL_verts_processed;
	OpenGL_last_uploaded = OpenGL_uploads;

	OpenGL_uploads = 0;
	OpenGL_polys_drawn = 0;
	OpenGL_verts_processed = 0;
	RefreshViewSize();
	UpdatePresentRect();

	GL4PerfGpuSplitMark(GL4_GPU_SPLIT_BEFORE_POST);

	Framebuffer* present_framebuffer = &framebuffers[framebuffer_current_draw];
	int supersampling_factor = SupersamplingFactor();
	float display_gamma = OpenGL_preferred_state.gamma != 0.0f ? 1.f / OpenGL_preferred_state.gamma : 1.f;
	const bool ao_enabled = OpenGL_preferred_state.gtao_enabled && framebuffer_ok;
	const bool bloom_enabled = OpenGL_preferred_state.bloom_enabled;
	const bool late_post_enabled = ao_enabled || bloom_enabled;
	const int framebuffer_logical_bottom_offset =
		framebuffer_logical_height - OpenGL_state.screen_height - framebuffer_logical_offset_y;
	const int post_visible_origin_x = framebuffer_logical_offset_x;
	const int post_visible_origin_y = framebuffer_logical_bottom_offset;
	const int post_visible_width = OpenGL_state.screen_width;
	const int post_visible_height = OpenGL_state.screen_height;
	post_present_framebuffer.Update(OpenGL_state.screen_width, OpenGL_state.screen_height, 0);
	if (supersampling_factor >= 4)
	{
		downsampleshader.Use();
		{
			PERF_MARKER_SCOPE("Post.PresentDownsample.4xTo2x");
			framebuffers[framebuffer_current_draw].DownsampleTo(downscale_framebuffer.Handle(), 0, 0,
				downscale_framebuffer.Width(), downscale_framebuffer.Height(), downsampleshader_gamma, display_gamma,
				downsampleshader_dest_origin, downsampleshader_source_visible_origin,
				downsampleshader_source_visible_size, post_visible_origin_x * 4,
				post_visible_origin_y * 4, post_visible_width * 4, post_visible_height * 4);
		}
		GL4PerfGpuDrain("GPU.PresentDownsample.4xTo2x");
		downsampleshader.Use();
		{
			PERF_MARKER_SCOPE("Post.PresentDownsample.2xTo1x");
			downscale_framebuffer.DownsampleTo(resolved_framebuffer.Handle(), 0, 0,
				resolved_framebuffer.Width(), resolved_framebuffer.Height(), downsampleshader_gamma, display_gamma,
				downsampleshader_dest_origin, downsampleshader_source_visible_origin,
				downsampleshader_source_visible_size, post_visible_origin_x * 2,
				post_visible_origin_y * 2, post_visible_width * 2, post_visible_height * 2);
		}
		GL4PerfGpuDrain("GPU.PresentDownsample.2xTo1x");
		present_framebuffer = &resolved_framebuffer;
	}
	else if (supersampling_factor >= 2)
	{
		downsampleshader.Use();
		{
			PERF_MARKER_SCOPE("Post.PresentDownsample.2xTo1x");
			framebuffers[framebuffer_current_draw].DownsampleTo(resolved_framebuffer.Handle(), 0, 0,
				resolved_framebuffer.Width(), resolved_framebuffer.Height(), downsampleshader_gamma, display_gamma,
				downsampleshader_dest_origin, downsampleshader_source_visible_origin,
				downsampleshader_source_visible_size, post_visible_origin_x * 2,
				post_visible_origin_y * 2, post_visible_width * 2, post_visible_height * 2);
		}
		GL4PerfGpuDrain("GPU.PresentDownsample.2xTo1x");
		present_framebuffer = &resolved_framebuffer;
	}
	else if (late_post_enabled && framebuffers[framebuffer_current_draw].Samples() >= 2)
	{
		resolved_framebuffer.Update(framebuffer_logical_width, framebuffer_logical_height, 0);
		{
			PERF_MARKER_SCOPE("Post.PresentResolve.MSAA");
			framebuffers[framebuffer_current_draw].BlitToRaw(resolved_framebuffer.Handle(), 0, 0,
				resolved_framebuffer.Width(), resolved_framebuffer.Height(), GL_NEAREST);
		}
		GL4PerfGpuDrain("GPU.PresentResolve.MSAA");
		present_framebuffer = &resolved_framebuffer;
	}

	if (late_post_enabled)
	{
		if (bloom_source_valid)
		{
			PERF_MARKER_SCOPE("Post.PresentDepth");
			bloom_source_framebuffer.BlitDepthTo(present_framebuffer->Handle(), 0, 0,
				present_framebuffer->Width(), present_framebuffer->Height());
			GL4PerfGpuDrain("GPU.PresentDepth");
		}
		else if (present_framebuffer != &framebuffers[framebuffer_current_draw])
		{
			PERF_MARKER_SCOPE("Post.PresentDepth");
			framebuffers[framebuffer_current_draw].BlitDepthTo(present_framebuffer->Handle(), 0, 0,
				present_framebuffer->Width(), present_framebuffer->Height());
			GL4PerfGpuDrain("GPU.PresentDepth");
		}
	}

	GLuint protection_mask_texture = post_protection_mask_dirty ?
		post_protection_mask.TextureForRead(framebuffers[framebuffer_current_draw].Handle()) : 0;
	GLuint ao_suppression_mask_texture = protection_mask_texture;
	GLuint ao_class_texture = ao_enabled ?
		post_protection_mask.AOClassTextureForRead(framebuffers[framebuffer_current_draw].Handle()) : 0;

	GL4PerfGpuSplitMark(GL4_GPU_SPLIT_AFTER_PREP);

	if (ao_enabled)
	{
		const float* gtao_projection = captured_scene_projection_valid ?
			captured_scene_projection : last_projection;
		GLuint gtao_ao_weight_texture = ao_class_texture;
		const bool gtao_ao_weight_is_direct = false;
		const int gtao_noise_origin_x = framebuffer_logical_offset_x;
		const int gtao_noise_origin_y =
			framebuffer_logical_height - OpenGL_state.screen_height - framebuffer_logical_offset_y;
		const int visible_origin_x = framebuffer_logical_offset_x;
		const int visible_origin_y =
			framebuffer_logical_height - OpenGL_state.screen_height - framebuffer_logical_offset_y;
		const int visible_width = OpenGL_state.screen_width;
		const int visible_height = OpenGL_state.screen_height;
		const bool mask_overscan_for_ao =
			visible_origin_x != 0 || visible_origin_y != 0 ||
			visible_width != (int)present_framebuffer->Width() ||
			visible_height != (int)present_framebuffer->Height();

		float near_z = last_nearz;
		float far_z = last_farz;
		if (fabsf(gtao_projection[10] - 1.0f) > 1e-6f &&
			fabsf(gtao_projection[10] + 1.0f) > 1e-6f)
		{
			near_z = gtao_projection[14] / (gtao_projection[10] - 1.0f);
			far_z = gtao_projection[14] / (gtao_projection[10] + 1.0f);
			if (near_z <= 0.0f) near_z = 1.0f;
			if (far_z <= near_z) far_z = near_z * 1000.0f;
		}

		bool deferred_ao = ao_scene_valid && bloom_source_valid &&
			bloom_source_framebuffer.Handle() != 0 &&
			bloom_source_framebuffer.Width() == present_framebuffer->Width() &&
			bloom_source_framebuffer.Height() == present_framebuffer->Height();

		if (deferred_ao)
		{
			ao_scene_framebuffer.Update(present_framebuffer->Width(), present_framebuffer->Height(), 0);
			bloom_source_framebuffer.BlitToRaw(ao_scene_framebuffer.Handle(), 0, 0,
				ao_scene_framebuffer.Width(), ao_scene_framebuffer.Height(), GL_NEAREST);
			GL4PerfGpuDrain("GPU.GTAO.SceneColorCopy");

			bloom_source_framebuffer.BlitDepthTo(ao_scene_framebuffer.Handle(), 0, 0,
				ao_scene_framebuffer.Width(), ao_scene_framebuffer.Height());
			GL4PerfGpuDrain("GPU.GTAO.SceneDepthCopy");

			gtao.Apply(&ao_scene_framebuffer, &ao_scene_framebuffer, OpenGL_preferred_state,
				OpenGL_state, gtao_projection, near_z, far_z, ao_suppression_mask_texture,
				gtao_ao_weight_texture, gtao_ao_weight_is_direct, 0, 0, 0, 0,
				gtao_noise_origin_x, gtao_noise_origin_y);
			GL4PerfGpuDrain("GPU.GTAO.Apply");

			ao_composite_framebuffer.Update(present_framebuffer->Width(), present_framebuffer->Height(), 0);
			ao_compositeshader.Use();
			glUniform1i(ao_composite_use_protection_mask, ao_suppression_mask_texture != 0 ? 1 : 0);
			if (ao_composite_visible_origin != -1)
				glUniform2f(ao_composite_visible_origin, (float)visible_origin_x, (float)visible_origin_y);
			if (ao_composite_visible_size != -1)
				glUniform2f(ao_composite_visible_size, (float)visible_width, (float)visible_height);
			if (ao_composite_use_visible_rect != -1)
				glUniform1i(ao_composite_use_visible_rect, mask_overscan_for_ao ? 1 : 0);
			rend_ClearBoundTextures();
			GL_BindFramebufferTexture(present_framebuffer->ColorTextureForRead(), 0, GL_NEAREST);
			GL_BindFramebufferTexture(bloom_source_framebuffer.ColorTextureForRead(), 1, GL_NEAREST);
			GL_BindFramebufferTexture(ao_scene_framebuffer.ColorTextureForRead(), 2, GL_NEAREST);
			if (ao_suppression_mask_texture != 0)
				GL_BindFramebufferTexture(ao_suppression_mask_texture, 3, GL_NEAREST);
			{
				PERF_MARKER_SCOPE("GTAO.DeferredComposite");
				GL_DrawFramebufferQuad(ao_composite_framebuffer.Handle(), 0, 0,
					ao_composite_framebuffer.Width(), ao_composite_framebuffer.Height());
			}
			GL4PerfGpuDrain("GPU.GTAO.DeferredComposite");
			bloom_source_framebuffer.BlitDepthTo(ao_composite_framebuffer.Handle(), 0, 0,
				ao_composite_framebuffer.Width(), ao_composite_framebuffer.Height());
			GL4PerfGpuDrain("GPU.GTAO.CompositeDepthCopy");
			present_framebuffer = &ao_composite_framebuffer;
		}
		else
		{
			gtao.Apply(present_framebuffer, present_framebuffer, OpenGL_preferred_state,
				OpenGL_state, gtao_projection, near_z, far_z, ao_suppression_mask_texture,
				gtao_ao_weight_texture, gtao_ao_weight_is_direct, 0, 0, 0, 0,
				gtao_noise_origin_x, gtao_noise_origin_y);
			GL4PerfGpuDrain("GPU.GTAO.Apply");
		}
	}

	GL4PerfGpuSplitMark(GL4_GPU_SPLIT_AFTER_GTAO);

	Framebuffer* bloom_framebuffer = bloom.Apply(bloom_enabled ? present_framebuffer : nullptr,
		OpenGL_preferred_state, OpenGL_state, display_gamma,
		late_post_enabled ? present_framebuffer->DepthTextureForRead() : 0, protection_mask_texture,
		post_visible_origin_x, post_visible_origin_y, post_visible_width, post_visible_height);
	GL4PerfGpuDrain("GPU.Bloom.Apply");
	const float post_uv_origin_x = present_framebuffer->Width() > 0 ?
		(float)framebuffer_logical_offset_x / (float)present_framebuffer->Width() : 0.0f;
	const float post_uv_origin_y = present_framebuffer->Height() > 0 ?
		(float)framebuffer_logical_bottom_offset / (float)present_framebuffer->Height() : 0.0f;
	const float post_uv_scale_x = present_framebuffer->Width() > 0 ?
		(float)OpenGL_state.screen_width / (float)present_framebuffer->Width() : 1.0f;
	const float post_uv_scale_y = present_framebuffer->Height() > 0 ?
		(float)OpenGL_state.screen_height / (float)present_framebuffer->Height() : 1.0f;
	if (bloom_framebuffer)
	{
		bloom.compositeshader.Use();
		glUniform1f(bloom.composite_gamma, display_gamma);
		glUniform1f(bloom.composite_intensity, OpenGL_preferred_state.bloom_intensity);
		glUniform1i(bloom.composite_use_alpha_mask, 0);
		glUniform1i(bloom.composite_use_protection_mask, protection_mask_texture != 0 ? 1 : 0);
		if (bloom.composite_uv_origin != -1)
			glUniform2f(bloom.composite_uv_origin, post_uv_origin_x, post_uv_origin_y);
		if (bloom.composite_uv_scale != -1)
			glUniform2f(bloom.composite_uv_scale, post_uv_scale_x, post_uv_scale_y);
		if (bloom.composite_source_origin != -1)
			glUniform2i(bloom.composite_source_origin, post_visible_origin_x, post_visible_origin_y);
		rend_ClearBoundTextures();
		GL_BindFramebufferTexture(present_framebuffer->ColorTextureForRead(), 0, GL_NEAREST);
		GL_BindFramebufferTexture(bloom_framebuffer->ColorTextureForRead(), 1, GL_LINEAR);
		if (protection_mask_texture != 0)
			GL_BindFramebufferTexture(protection_mask_texture, 3, GL_NEAREST);
		{
			PERF_MARKER_SCOPE("Bloom.Composite");
			GL_DrawFramebufferQuad(post_present_framebuffer.Handle(), 0, 0,
				post_present_framebuffer.Width(), post_present_framebuffer.Height());
		}
		GL4PerfGpuDrain("GPU.Bloom.Composite");
	}
	else
	{
		blitshader.Use();
		glUniform1f(blitshader_gamma, display_gamma);
		if (blitshader_uv_origin != -1)
			glUniform2f(blitshader_uv_origin, post_uv_origin_x, post_uv_origin_y);
		if (blitshader_uv_scale != -1)
			glUniform2f(blitshader_uv_scale, post_uv_scale_x, post_uv_scale_y);
		present_framebuffer->BlitTo(post_present_framebuffer.Handle(), 0, 0,
			post_present_framebuffer.Width(), post_present_framebuffer.Height(), false);
		if (blitshader_uv_origin != -1)
			glUniform2f(blitshader_uv_origin, 0.0f, 0.0f);
		if (blitshader_uv_scale != -1)
			glUniform2f(blitshader_uv_scale, 1.0f, 1.0f);
		GL4PerfGpuDrain("GPU.PostPresentBlit");
	}
	ShaderProgram::ClearBinding();

	GL4PerfGpuSplitMark(GL4_GPU_SPLIT_AFTER_BLOOM);

	if (framebuffer_ok && have_current_view_projection)
	{
		memcpy(previous_view_projection, current_view_projection, sizeof(previous_view_projection));
		have_previous_view_projection = true;
	}

	UseDrawVAO();

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

#ifdef _DEBUG
	err = glGetError();
	if (err != GL_NO_ERROR)
	{
		Int3();
	}
#endif
	post_present_pending_swap = true;
	return true;
}

bool GL4Renderer::IsPostPresentFramePending() const
{
	return post_present_pending_swap;
}

void GL4Renderer::StartPostPresentFrame(int x1, int y1, int x2, int y2, int clear_flags)
{
	FlushFontBatch();

	post_present_framebuffer.Update(OpenGL_state.screen_width, OpenGL_state.screen_height, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, post_present_framebuffer.Handle());
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	GLenum glclearflags = 0;
	if (clear_flags & RF_CLEAR_ZBUFFER)
		glclearflags |= GL_DEPTH_BUFFER_BIT;
	if (clear_flags & RF_CLEAR_COLOR)
	{
		glClearColor(0.0, 0.0, 0.0, 1.0);
		glclearflags |= GL_COLOR_BUFFER_BIT;
	}
	if (glclearflags != 0)
		glClear(glclearflags);

	glDisable(GL_MULTISAMPLE);
	OpenGL_state.clip_x1 = x1;
	OpenGL_state.clip_y1 = y1;
	OpenGL_state.clip_x2 = x2;
	OpenGL_state.clip_y2 = y2;

	float projection[16];
	GL_Ortho(projection, 0, x2 - x1, y2 - y1, 0, 0, 1);
	UpdateLegacyBlock(projection, mat4_identity);
	glViewport(x1, OpenGL_state.screen_height - y2, x2 - x1, y2 - y1);
}

void GL4Renderer::EndPostPresentFrame()
{
	FlushFontBatch();

	if (post_present_pending_swap)
	{
		UpdatePresentRect();
		blitshader.Use();
		glUniform1f(blitshader_gamma, 1.0f);
		if (blitshader_uv_origin != -1)
			glUniform2f(blitshader_uv_origin, 0.0f, 0.0f);
		if (blitshader_uv_scale != -1)
			glUniform2f(blitshader_uv_scale, 1.0f, 1.0f);
		{
			PERF_MARKER_SCOPE("Renderer.Flip.BackbufferBlit");
			post_present_framebuffer.BlitTo(0, framebuffer_blit_x, framebuffer_blit_y,
				framebuffer_blit_w, framebuffer_blit_h, false);
		}
		GL4PerfGpuDrain("GPU.BackbufferBlit");
		ShaderProgram::ClearBinding();
	}

	GL4PerfGpuSplitEndBeforeSwap();
	GL4PerfGpuFrameEndBeforeSwap();

#if defined(SDL3)
	{
		PERF_MARKER_SCOPE("Renderer.Flip.SwapBuffers");
		GL4PerfPresentState("BeforeSwap", framebuffer_current_draw, post_present_pending_swap);
		SDL_GL_SwapWindow(GLWindow);
	}
#elif defined(WIN32)
	{
		PERF_MARKER_SCOPE("Renderer.Flip.SwapBuffers");
		GL4PerfPresentState("BeforeSwap", framebuffer_current_draw, post_present_pending_swap);
		SwapBuffers((HDC)hOpenGLDC);
	}
#elif defined(__LINUX__)
	{
		PERF_MARKER_SCOPE("Renderer.Flip.SwapBuffers");
		GL4PerfPresentState("BeforeSwap", framebuffer_current_draw, post_present_pending_swap);
		SDL_GL_SwapBuffers();
	}
#endif

	{
		PERF_MARKER_SCOPE("Renderer.Flip.NextFramebuffer");
		framebuffer_current_draw = (framebuffer_current_draw + 1) % NUM_GL4_FBOS;
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[framebuffer_current_draw].Handle());
		//Scene rendering is about to write to this framebuffer's MSAA attachments,
		//so any cached resolve from the previous time we used this slot is stale.
		framebuffers[framebuffer_current_draw].MarkAllDirty();
		bloom_source_valid = false;
		ao_scene_valid = false;
		post_protection_mask_dirty = false;
		post_protection_mask_cleared_this_frame = false;
		post_present_pending_swap = false;
	}
	if (msaa_downshift_release_frames > 0)
	{
		if (msaa_forced_off_scene_presented)
		{
			msaa_forced_off_scene_presented = false;
			msaa_downshift_release_frames--;
			GL4PerfMsaaStageState("OffPresented", msaa_downshift_release_frames,
				msaa_forced_off_target_samples);
			if (msaa_downshift_release_frames == 0 && framebuffer_ok)
			{
				msaa_deferred_apply_pending = true;
				GL4PerfMsaaStageState("ApplyQueued", msaa_downshift_release_frames,
					msaa_forced_off_target_samples);
			}
		}
	}

#ifdef _DEBUG
	GLenum err = glGetError();
	if (err != GL_NO_ERROR)
	{
		Int3();
	}
#endif

#ifdef __PERMIT_GL_LOGGING
	if (__glLog == true)
	{
		DGL_LogNewFrame();
	}
#endif
}

void GL4Renderer::EndFrame(void)
{
}

void GL4Renderer::CaptureBloomSource()
{
	FlushFontBatch();

	bloom_source_valid = false;
	ao_scene_valid = false;
	captured_scene_projection_valid = false;

	const bool ao_enabled = OpenGL_preferred_state.gtao_enabled && framebuffer_ok;
	const bool bloom_enabled = OpenGL_preferred_state.bloom_enabled;
	const bool late_post_enabled = ao_enabled || bloom_enabled;

	if (!late_post_enabled)
	{
		bloom_source_framebuffer.Destroy();
		bloom_source_resolved_framebuffer.Destroy();
		bloom_source_downscale_framebuffer.Destroy();
		ao_scene_framebuffer.Destroy();
		ao_composite_framebuffer.Destroy();
	}

	if (framebuffer_ok)
	{
		if (late_post_enabled && OpenGL_state.screen_width > 0 && OpenGL_state.screen_height > 0)
		{
			GLint old_read = 0, old_draw = 0;
			glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);

			bloom_source_framebuffer.Update(framebuffer_logical_width, framebuffer_logical_height, 0);
			if (ao_enabled)
			{
				int supersampling_factor = SupersamplingFactor();
				float display_gamma = OpenGL_preferred_state.gamma != 0.0f ? 1.f / OpenGL_preferred_state.gamma : 1.f;
				const int framebuffer_logical_bottom_offset =
					framebuffer_logical_height - OpenGL_state.screen_height - framebuffer_logical_offset_y;
				const int visible_origin_x = framebuffer_logical_offset_x;
				const int visible_origin_y = framebuffer_logical_bottom_offset;
				const int visible_width = OpenGL_state.screen_width;
				const int visible_height = OpenGL_state.screen_height;
				if (supersampling_factor >= 4)
				{
					bloom_source_downscale_framebuffer.Update(framebuffer_logical_width * 2, framebuffer_logical_height * 2, 0);
					downsampleshader.Use();
					framebuffers[framebuffer_current_draw].DownsampleTo(bloom_source_downscale_framebuffer.Handle(), 0, 0,
						bloom_source_downscale_framebuffer.Width(), bloom_source_downscale_framebuffer.Height(),
						downsampleshader_gamma, display_gamma, downsampleshader_dest_origin,
						downsampleshader_source_visible_origin, downsampleshader_source_visible_size,
						visible_origin_x * 4, visible_origin_y * 4, visible_width * 4, visible_height * 4);
					GL4PerfGpuDrain("GPU.CaptureDownsample.4xTo2x");
					downsampleshader.Use();
					bloom_source_downscale_framebuffer.DownsampleTo(bloom_source_framebuffer.Handle(), 0, 0,
						bloom_source_framebuffer.Width(), bloom_source_framebuffer.Height(),
						downsampleshader_gamma, display_gamma, downsampleshader_dest_origin,
						downsampleshader_source_visible_origin, downsampleshader_source_visible_size,
						visible_origin_x * 2, visible_origin_y * 2, visible_width * 2, visible_height * 2);
					GL4PerfGpuDrain("GPU.CaptureDownsample.2xTo1x");
				}
				else if (supersampling_factor >= 2)
				{
					downsampleshader.Use();
					framebuffers[framebuffer_current_draw].DownsampleTo(bloom_source_framebuffer.Handle(), 0, 0,
						bloom_source_framebuffer.Width(), bloom_source_framebuffer.Height(),
						downsampleshader_gamma, display_gamma, downsampleshader_dest_origin,
						downsampleshader_source_visible_origin, downsampleshader_source_visible_size,
						visible_origin_x * 2, visible_origin_y * 2, visible_width * 2, visible_height * 2);
					GL4PerfGpuDrain("GPU.CaptureDownsample.2xTo1x");
				}
				else
				{
					framebuffers[framebuffer_current_draw].BlitToRaw(bloom_source_framebuffer.Handle(), 0, 0,
						bloom_source_framebuffer.Width(), bloom_source_framebuffer.Height(), GL_NEAREST);
					GL4PerfGpuDrain("GPU.CaptureBlit");
				}
				ao_scene_valid = true;
			}
			{
				PERF_MARKER_SCOPE("Post.CaptureDepth");
				framebuffers[framebuffer_current_draw].BlitDepthTo(bloom_source_framebuffer.Handle(), 0, 0,
					bloom_source_framebuffer.Width(), bloom_source_framebuffer.Height());
			}
			GL4PerfGpuDrain("GPU.CaptureDepth");
			memcpy(captured_scene_projection, last_projection, sizeof(captured_scene_projection));
			captured_scene_projection_valid = true;

			glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
			bloom_source_valid = true;
		}

		framebuffers[framebuffer_current_draw].ClearAlphaToZero();
		post_protection_mask.UseSceneDrawBuffers(framebuffers[framebuffer_current_draw].Handle());
	}

	rend_RestoreLegacy();
}

void GL4Renderer::PerfGpuSceneMark(renderer_gpu_scene_mark mark)
{
	GL4GpuSplitPoint split_point = GL4GpuSplitPointFromSceneMark(mark);
	if (split_point != GL4_GPU_SPLIT_COUNT)
		GL4PerfGpuSplitMark(split_point);
}

// returns true if the passed in extension name is supported
bool GL4Renderer::CheckExtension(char* extName)
{
	GLint extcount;
	glGetIntegerv(GL_NUM_EXTENSIONS, &extcount);
	for (int i = 0; i < extcount; i++)
	{
		const GLubyte* extname = glGetStringi(GL_EXTENSIONS, i);
		if (!stricmp((const char*)extname, extName))
			return true;
	}

	return false;
}

void GL4Renderer::SetGammaValue(float val)
{
	blitshader.Use();

	glUniform1f(blitshader_gamma, 1.f / val);
}

void GL4Renderer::SetFlatColor(ddgr_color color)
{
	OpenGL_state.cur_color = color;
}

// Sets the fog state to TRUE or FALSE
void GL4Renderer::SetFogState(sbyte state)
{
	if (state)
		post_protection_mask_dirty = true;

	if (state == OpenGL_state.cur_fog_state)
		return;

	OpenGL_state.cur_fog_state = state;
}

// Sets the near and far plane of fog
void GL4Renderer::SetFogBorders(float nearz, float farz)
{
	// Sets the near and far plane of fog
	float fog_start = std::max(0.f, std::min(1.0f, 1.0f - (1.0f / nearz)));
	float fog_end = std::max(0.f, std::min(1.0f, 1.0f - (1.0f / farz)));

	OpenGL_state.cur_fog_start = fog_start;
	OpenGL_state.cur_fog_end = fog_end;

	OpenGL_terrain_fog_start = nearz;
	OpenGL_terrain_fog_end = farz;
}

// Sets the color of fog
void GL4Renderer::SetFogColor(ddgr_color color)
{
	float fc[4];
	fc[0] = GR_COLOR_RED(color);
	fc[1] = GR_COLOR_GREEN(color);
	fc[2] = GR_COLOR_BLUE(color);
	fc[3] = 1;

	fc[0] /= 255.0f;
	fc[1] /= 255.0f;
	fc[2] /= 255.0f;

	UpdateTerrainFog(fc, OpenGL_terrain_fog_start, OpenGL_terrain_fog_end);
}

void GL4Renderer::SetLighting(light_state state)
{
	if (state == LS_PHONG && !OpenGL_preferred_state.per_pixel_lighting)
		state = LS_GOURAUD;

	if (state == OpenGL_state.cur_light_state)
		return;	// No redundant state setting
	if (UseMultitexture && Last_texel_unit_set != 0)
	{
		glActiveTexture(GL_TEXTURE0 + 0);
		Last_texel_unit_set = 0;
	}

	OpenGL_sets_this_frame[4]++;

	switch (state)
	{
	case LS_NONE:
		OpenGL_state.cur_light_state = LS_NONE;
		break;
	case LS_FLAT_GOURAUD:
		OpenGL_state.cur_light_state = LS_FLAT_GOURAUD;
		break;
	case LS_GOURAUD:
		OpenGL_state.cur_light_state = LS_GOURAUD;
		break;
	case LS_PHONG:
		OpenGL_state.cur_light_state = LS_PHONG;
		break;
	default:
		Int3();
		break;
	}
	legacy_draw_uniforms_dirty = true;

	CHECK_ERROR(13);
}

void GL4Renderer::SetPerPixelLightingDirection(const vector *lightdir)
{
	if (lightdir)
	{
		per_pixel_light_direction = *lightdir;
		legacy_draw_uniforms_dirty = true;
	}
}

static void UpdateCurrentDynamicLightingUniforms(int count, const vector &face_normal,
	GLfloat positions[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS][3],
	GLfloat colors[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS][3],
	GLfloat radii[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS],
	GLfloat directions[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS][3],
	GLfloat dot_ranges[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS],
	GLint directional[RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS])
{
	ShaderProgram* current = ShaderProgram::Current();
	if (current)
	{
		current->ApplyDynamicLighting(count, &face_normal.x, &positions[0][0], &colors[0][0],
			radii, &directions[0][0], dot_ranges, directional);
	}
}

void GL4Renderer::SetPerPixelDynamicLighting(const vector *face_normal, int count,
	const renderer_per_pixel_light *lights)
{
	if (!OpenGL_preferred_state.per_pixel_lighting || count <= 0 || lights == nullptr || face_normal == nullptr)
	{
		if (per_pixel_dynamic_light_count != 0)
			legacy_draw_uniforms_dirty = true;
		per_pixel_dynamic_light_count = 0;
		UpdateCurrentDynamicLightingUniforms(per_pixel_dynamic_light_count, per_pixel_dynamic_face_normal,
			per_pixel_dynamic_positions, per_pixel_dynamic_colors, per_pixel_dynamic_radii,
			per_pixel_dynamic_directions, per_pixel_dynamic_dot_ranges, per_pixel_dynamic_directional);
		return;
	}

	per_pixel_dynamic_face_normal = *face_normal;
	per_pixel_dynamic_light_count = std::min(count, RENDERER_MAX_PER_PIXEL_DYNAMIC_LIGHTS);
	legacy_draw_uniforms_dirty = true;
	for (int i = 0; i < per_pixel_dynamic_light_count; i++)
	{
		per_pixel_dynamic_positions[i][0] = lights[i].position[0];
		per_pixel_dynamic_positions[i][1] = lights[i].position[1];
		per_pixel_dynamic_positions[i][2] = lights[i].position[2];

		per_pixel_dynamic_colors[i][0] = lights[i].color[0];
		per_pixel_dynamic_colors[i][1] = lights[i].color[1];
		per_pixel_dynamic_colors[i][2] = lights[i].color[2];

		per_pixel_dynamic_radii[i] = lights[i].radius;

		per_pixel_dynamic_directions[i][0] = lights[i].direction[0];
		per_pixel_dynamic_directions[i][1] = lights[i].direction[1];
		per_pixel_dynamic_directions[i][2] = lights[i].direction[2];

		per_pixel_dynamic_dot_ranges[i] = lights[i].dot_range;
		per_pixel_dynamic_directional[i] = lights[i].directional ? 1 : 0;
	}

	UpdateCurrentDynamicLightingUniforms(per_pixel_dynamic_light_count, per_pixel_dynamic_face_normal,
		per_pixel_dynamic_positions, per_pixel_dynamic_colors, per_pixel_dynamic_radii,
		per_pixel_dynamic_directions, per_pixel_dynamic_dot_ranges, per_pixel_dynamic_directional);
}

void GL4Renderer::SetColorModel(color_model state)
{
	if (state == OpenGL_state.cur_color_model)
		return;

	switch (state)
	{
	case CM_MONO:
		OpenGL_state.cur_color_model = CM_MONO;
		break;
	case CM_RGB:
		OpenGL_state.cur_color_model = CM_RGB;
		break;
	default:
		Int3();
		break;
	}
}

void GL4Renderer::SetTextureType(texture_type state)
{
	if (state == OpenGL_state.cur_texture_type)
		return;	// No redundant state setting
	if (UseMultitexture && Last_texel_unit_set != 0)
	{
		glActiveTexture(GL_TEXTURE0 + 0);
		Last_texel_unit_set = 0;
	}
	OpenGL_sets_this_frame[3]++;

	switch (state)
	{
	case TT_FLAT:
		OpenGL_state.cur_texture_quality = 0;
		break;
	case TT_LINEAR:
	case TT_LINEAR_SPECIAL:
	case TT_PERSPECTIVE:
	case TT_PERSPECTIVE_SPECIAL:
		OpenGL_state.cur_texture_quality = 2;
		break;
	default:
		Int3();	// huh? Get Jason
		break;
	}

	CHECK_ERROR(12);
	OpenGL_state.cur_texture_type = state;
}

// Sets the state of bilinear filtering for our textures
void GL4Renderer::SetFiltering(sbyte state)
{
	if (state == OpenGL_state.cur_bilinear_state)
		return;

	OpenGL_state.cur_bilinear_state = state;
}

// Sets the state of z-buffering to on or off
void GL4Renderer::SetZBufferState(sbyte state)
{
	if (state == OpenGL_state.cur_zbuffer_state)
		return;	// No redundant state setting

	OpenGL_sets_this_frame[5]++;
	OpenGL_state.cur_zbuffer_state = state;

	//	mprintf ((0,"OPENGL: Setting zbuffer state to %d.\n",state));

	if (state)
	{
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);
	}
	else
	{
		glDisable(GL_DEPTH_TEST);
	}

	CHECK_ERROR(14);
}

// Sets the near and far planes for z buffer
void GL4Renderer::SetZValues(float nearz, float farz)
{
	OpenGL_state.cur_near_z = nearz;
	OpenGL_state.cur_far_z = farz;
	//	mprintf ((0,"OPENGL:Setting depth range to %f - %f\n",nearz,farz));
}

// Sets a bitmap as a overlay map to rendered on top of the next texture map
// a -1 value indicates no overlay map
void GL4Renderer::SetOverlayMap(int handle)
{
	if (handle == Overlay_map)
		return;

	Overlay_map = handle;
}

void GL4Renderer::SetOverlayType(ubyte type)
{
	if (type == Overlay_type)
		return;

	Overlay_type = type;
}

// Clears the display to a specified color
void GL4Renderer::ClearScreen(ddgr_color color)
{
	FlushFontBatch();

	int r = (color >> 16 & 0xFF);
	int g = (color >> 8 & 0xFF);
	int b = (color & 0xFF);

	glClearColor((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, 0);

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Clears the zbuffer for the screen
void GL4Renderer::ClearZBuffer(void)
{
	FlushFontBatch();

	glClear(GL_DEPTH_BUFFER_BIT);
}

// returns the alpha that we should use
float GL4Renderer::GetAlphaMultiplier()
{
	switch (OpenGL_state.cur_alpha_type)
	{
	case AT_ALWAYS:
		return 255;
	case AT_CONSTANT:
		return OpenGL_state.cur_alpha;
	case AT_TEXTURE:
		return 255;
	case AT_CONSTANT_TEXTURE:
		return OpenGL_state.cur_alpha;
	case AT_VERTEX:
		return 255;
	case AT_CONSTANT_TEXTURE_VERTEX:
	case AT_CONSTANT_VERTEX:
		return OpenGL_state.cur_alpha;
	case AT_TEXTURE_VERTEX:
		return 255;
	case AT_LIGHTMAP_BLEND:
	case AT_LIGHTMAP_BLEND_SATURATE:
		return OpenGL_state.cur_alpha;
	case AT_SATURATE_TEXTURE:
		return OpenGL_state.cur_alpha;
	case AT_SATURATE_VERTEX:
		return 255;
	case AT_SATURATE_CONSTANT_VERTEX:
		return OpenGL_state.cur_alpha;
	case AT_SATURATE_TEXTURE_VERTEX:
		return 255;
	case AT_SPECULAR:
		return 255;
	default:
		//Int3();		// no type defined,get jason
		return 0;
	}
}


void GL4Renderer::SetAlwaysAlpha(bool state)
{
	if (state && OpenGL_blending_on)
	{
		glDisable(GL_BLEND);
		//glDisable(GL_ALPHA_TEST);
		OpenGL_blending_on = false;
	}
	else if (!state)
	{
		glEnable(GL_BLEND);
		//glEnable(GL_ALPHA_TEST);
		OpenGL_blending_on = true;
	}
}

void GL4Renderer::SetAlphaType(sbyte atype)
{
	if (atype == OpenGL_state.cur_alpha_type)
		return;		// don't set it redundantly
	if (UseMultitexture && Last_texel_unit_set != 0)
	{
		glActiveTexture(GL_TEXTURE0 + 0);
		Last_texel_unit_set = 0;

	}
	OpenGL_sets_this_frame[6]++;

	switch (atype)
	{
	case AT_ALWAYS:
		SetAlphaValue(255);
		SetAlwaysAlpha(true);
		glBlendFunc(GL_ONE, GL_ZERO);
		break;
	case AT_CONSTANT:
		SetAlwaysAlpha(false);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case AT_TEXTURE:
		SetAlphaValue(255);
		SetAlwaysAlpha(true);
		glBlendFunc(GL_ONE, GL_ZERO);
		break;
	case AT_CONSTANT_TEXTURE:
	case AT_CONSTANT_TEXTURE_VERTEX:
	case AT_TEXTURE_VERTEX:
		SetAlwaysAlpha(false);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case AT_CONSTANT_VERTEX:
		SetAlwaysAlpha(false);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case AT_VERTEX:
		SetAlwaysAlpha(false);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case AT_LIGHTMAP_BLEND:
		SetAlwaysAlpha(false);
		glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case AT_SATURATE_TEXTURE:
	case AT_LIGHTMAP_BLEND_SATURATE:
		SetAlwaysAlpha(false);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case AT_SATURATE_VERTEX:
		SetAlwaysAlpha(false);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case AT_SATURATE_CONSTANT_VERTEX:
		SetAlwaysAlpha(false);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case AT_SATURATE_TEXTURE_VERTEX:
		SetAlwaysAlpha(false);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case AT_SPECULAR:
		SetAlwaysAlpha(false);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

		//hack
		OpenGL_state.cur_texture_quality = 2;
		OpenGL_state.cur_texture_type = TT_PERSPECTIVE;

		break;
	default:
		Int3();		// no type defined,get jason
		break;
	}
	OpenGL_state.cur_alpha_type = atype;
	legacy_draw_uniforms_dirty = true;
	Alpha_multiplier = GetAlphaMultiplier();
	GL_ConfigurePostMaskBlend();
	CHECK_ERROR(15);
}

// Sets the alpha value for constant alpha
void GL4Renderer::SetAlphaValue(ubyte val)
{
	OpenGL_state.cur_alpha = val;
	Alpha_multiplier = GetAlphaMultiplier();
}

void GL4Renderer::SetAOSuppression(float value)
{
	float clamped_value = std::max(0.0f, std::min(value, 1.0f));
	if (clamped_value == ao_suppression_draw_value)
		return;
	if (clamped_value != ao_suppression_draw_value)
		legacy_draw_uniforms_dirty = true;
	ao_suppression_draw_value = clamped_value;
	if (ao_suppression_draw_value > 0.0f)
	{
		post_protection_mask_dirty = true;
	}
}

void GL4Renderer::SetBloomSuppression(float value)
{
	float clamped_value = std::max(0.0f, std::min(value, 1.0f));
	if (clamped_value == bloom_suppression_draw_value)
		return;
	if (clamped_value != bloom_suppression_draw_value)
		legacy_draw_uniforms_dirty = true;
	bloom_suppression_draw_value = clamped_value;
	if (bloom_suppression_draw_value > 0.0f)
		post_protection_mask_dirty = true;
}

static float GL4AOClassWeight(const renderer_preferred_state& state, int value)
{
	switch (value)
	{
	case RENDERER_AO_CLASS_TERRAIN:
		return std::max(0.0f, std::min(state.gtao_terrain_occlusion, 1.0f));
	case RENDERER_AO_CLASS_POLYOBJECT:
		return std::max(0.0f, std::min(state.gtao_polyobject_occlusion, 1.0f));
	case RENDERER_AO_CLASS_MINE_ROCK:
		return std::max(0.0f, std::min(state.gtao_mine_rock_occlusion, 1.0f));
	case RENDERER_AO_CLASS_MINE:
		return std::max(0.0f, std::min(state.gtao_mine_occlusion, 1.0f));
	default:
		return 1.0f;
	}
}

void GL4Renderer::SetAOClass(int value)
{
	int clamped_value = std::max(0, std::min(value, 255));
	float weight_value = GL4AOClassWeight(OpenGL_preferred_state, clamped_value);
	if (clamped_value == ao_class_draw_value && weight_value == ao_weight_draw_value)
		return;
	if (clamped_value != ao_class_draw_value || weight_value != ao_weight_draw_value)
		legacy_draw_uniforms_dirty = true;
	ao_class_draw_value = clamped_value;
	ao_weight_draw_value = weight_value;

	ShaderProgram* current_shader = ShaderProgram::Current();
	if (current_shader)
	{
		GLint ao_class_uniform = current_shader->FindUniform("ao_class_value");
		if (ao_class_uniform != -1)
			glUniform1i(ao_class_uniform, ao_class_draw_value);
		GLint ao_weight_uniform = current_shader->FindUniform("ao_weight_value");
		if (ao_weight_uniform != -1)
			glUniform1f(ao_weight_uniform, ao_weight_draw_value);
		GLint ao_capture_weight_mode_uniform = current_shader->FindUniform("ao_capture_weight_mode");
		if (ao_capture_weight_mode_uniform != -1)
			glUniform1i(ao_capture_weight_mode_uniform, 0);
	}
}

void GL4Renderer::SetPostMaskOnly(int state)
{
	FlushFontBatch();

	bool enabled = state != 0;
	if (enabled == post_mask_only_draw)
		return;

	post_mask_only_draw = enabled;
	if (enabled)
	{
		glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glColorMaski(2, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glColorMaski(3, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	}
	else
	{
		glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glColorMaski(2, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glColorMaski(3, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}
}

// Sets the overall alpha scale factor (all alpha values are scaled by this value)
// usefull for motion blur effect
void GL4Renderer::SetAlphaFactor(float val)
{
	if (val < 0.0f) val = 0.0f;
	if (val > 1.0f) val = 1.0f;
	OpenGL_Alpha_factor = val;
}

// Returns the current Alpha factor
float GL4Renderer::GetAlphaFactor(void)
{
	return OpenGL_Alpha_factor;
}

// Sets the texture wrapping type
void GL4Renderer::SetWrapType(wrap_type val)
{
	OpenGL_state.cur_wrap_type = val;
}

void GL4Renderer::SetZBias(float z_bias)
{
	if (Z_bias != z_bias)
	{
		Z_bias = z_bias;
	}
}

// Enables/disables writes the depth buffer
void GL4Renderer::SetZBufferWriteMask(int state)
{
	OpenGL_sets_this_frame[5]++;
	if (state)
	{
		glDepthMask(GL_TRUE);
	}
	else
	{
		glDepthMask(GL_FALSE);
	}
}

// Returns the aspect ratio of the physical screen
void GL4Renderer::GetProjectionParameters(int* width, int* height)
{
	*width = OpenGL_state.clip_x2 - OpenGL_state.clip_x1;
	*height = OpenGL_state.clip_y2 - OpenGL_state.clip_y1;
}

void GL4Renderer::GetProjectionScreenParameters(int& screenLX, int& screenTY, int& screenW, int& screenH)
{
	screenLX = OpenGL_state.clip_x1;
	screenTY = OpenGL_state.clip_y1;
	screenW = OpenGL_state.clip_x2 - OpenGL_state.clip_x1 + 1;
	screenH = OpenGL_state.clip_y2 - OpenGL_state.clip_y1 + 1;
}

// Returns the aspect ratio of the physical screen
float GL4Renderer::GetAspectRatio()
{
	float aspect_ratio = (float)((3.0f * OpenGL_state.screen_width) / (4.0f * OpenGL_state.screen_height));
	return aspect_ratio;
}

// Sets the hardware bias level for coplanar polygons
// This helps reduce z buffer artifacts
void GL4Renderer::SetCoplanarPolygonOffset(float factor)
{
	if (factor == 0.0f)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
	else
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-1.0f, -1.0f);
	}
}

// Preuploads a texture to the video card
void GL4Renderer::PreUploadTextureToCard(int handle, int map_type)
{
}

// Frees an uploaded texture from the video card
void GL4Renderer::FreePreUploadedTexture(int handle, int map_type)
{
}

// Returns 1 if there is mid video memory, 2 if there is low vid memory, or 0 if there is large vid memory
int GL4Renderer::LowVidMem()
{
	return 0;
}

// Returns 1 if the renderer supports bumpmapping
int GL4Renderer::SupportsBumpmapping()
{
	return 0;
}

// Sets a bumpmap to be rendered, or turns off bumpmapping altogether
void GL4Renderer::SetBumpmapReadyState(int state, int map)
{
}

// returns rendering statistics for the frame
void GL4Renderer::GetStatistics(tRendererStats* stats)
{
	stats->poly_count = OpenGL_last_frame_polys_drawn;
	stats->vert_count = OpenGL_last_frame_verts_processed;
	stats->texture_uploads = OpenGL_last_uploaded;
}

// Tells the software renderer whether or not to use mipping
void GL4Renderer::SetMipState(sbyte mipstate)
{
	OpenGL_state.cur_mip_state = mipstate;
}

// Fills in the passed in pointer with the current rendering state
void GL4Renderer::GetRenderState(rendering_state* rstate)
{
	memcpy(rstate, &OpenGL_state, sizeof(rendering_state));
}

void GL4Renderer::DLLGetRenderState(DLLrendering_state* rstate)
{
#define COPY_ELEMENT(element) rstate->element = OpenGL_state.element;
	COPY_ELEMENT(initted);
	COPY_ELEMENT(cur_bilinear_state);
	COPY_ELEMENT(cur_zbuffer_state);
	COPY_ELEMENT(cur_fog_state);
	COPY_ELEMENT(cur_mip_state);
	COPY_ELEMENT(cur_texture_type);
	COPY_ELEMENT(cur_color_model);
	COPY_ELEMENT(cur_light_state);
	COPY_ELEMENT(cur_alpha_type);
	COPY_ELEMENT(cur_wrap_type);
	COPY_ELEMENT(cur_fog_start);
	COPY_ELEMENT(cur_fog_end);
	COPY_ELEMENT(cur_near_z);
	COPY_ELEMENT(cur_far_z);
	COPY_ELEMENT(gamma_value);
	COPY_ELEMENT(cur_alpha);
	COPY_ELEMENT(cur_color);
	COPY_ELEMENT(cur_fog_color);
	COPY_ELEMENT(cur_texture_quality);
	COPY_ELEMENT(clip_x1);
	COPY_ELEMENT(clip_x2);
	COPY_ELEMENT(clip_y1);
	COPY_ELEMENT(clip_y2);
	COPY_ELEMENT(screen_width);
	COPY_ELEMENT(screen_height);
#undef COPY_ELEMENT
}

// Takes a screenshot of the current frame and puts it into the handle passed
void GL4Renderer::Screenshot(int bm_handle)
{
	ushort* dest_data;
	uint* temp_data;
	int i, t;
	int total = OpenGL_state.screen_width * OpenGL_state.screen_height;

	ASSERT((bm_w(bm_handle, 0)) == OpenGL_state.screen_width);
	ASSERT((bm_h(bm_handle, 0)) == OpenGL_state.screen_height);

	int w = bm_w(bm_handle, 0);
	int h = bm_h(bm_handle, 0);

	temp_data = (uint*)mem_malloc(total * 4);
	ASSERT(temp_data);	// Ran out of memory?

	dest_data = bm_data(bm_handle, 0);

	int supersampling_factor = SupersamplingFactor();
	float display_gamma = OpenGL_preferred_state.gamma != 0.0f ? 1.f / OpenGL_preferred_state.gamma : 1.f;
	if (supersampling_factor >= 4)
	{
		downsampleshader.Use();
		framebuffers[framebuffer_current_draw].DownsampleTo(downscale_framebuffer.Handle(), 0, 0,
			downscale_framebuffer.Width(), downscale_framebuffer.Height(), downsampleshader_gamma, display_gamma,
			downsampleshader_dest_origin);
		downsampleshader.Use();
		downscale_framebuffer.DownsampleTo(resolved_framebuffer.Handle(), 0, 0,
			resolved_framebuffer.Width(), resolved_framebuffer.Height(), downsampleshader_gamma, display_gamma,
			downsampleshader_dest_origin);
		resolved_framebuffer.BindForRead();
	}
	else if (supersampling_factor >= 2)
	{
		downsampleshader.Use();
		framebuffers[framebuffer_current_draw].DownsampleTo(resolved_framebuffer.Handle(), 0, 0,
			resolved_framebuffer.Width(), resolved_framebuffer.Height(), downsampleshader_gamma, display_gamma,
			downsampleshader_dest_origin);
		resolved_framebuffer.BindForRead();
	}
	else
	{
		framebuffers[framebuffer_current_draw].BindForRead();
	}
	if (supersampling_factor >= 2)
		ShaderProgram::ClearBinding();
	glReadPixels(0, 0, OpenGL_state.screen_width, OpenGL_state.screen_height, GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid*)temp_data);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[framebuffer_current_draw].Handle());

	for (i = 0; i < h; i++)
	{
		for (t = 0; t < w; t++)
		{
			uint spix = temp_data[i * w + t];

			int r = spix & 0xff;
			int g = (spix >> 8) & 0xff;
			int b = (spix >> 16) & 0xff;

			dest_data[(((h - 1) - i) * w) + t] = GR_RGB16(r, g, b);
		}
	}

	mem_free(temp_data);
}

int GL4Renderer::SaveScreenshotPNG(const char* filename)
{
	int w = OpenGL_state.screen_width;
	int h = OpenGL_state.screen_height;
	int total = w * h;

	uint* temp_data = (uint*)mem_malloc(total * 4);
	if (!temp_data)
		return 0;

	ubyte* rgba_data = (ubyte*)mem_malloc(total * 4);
	if (!rgba_data)
	{
		mem_free(temp_data);
		return 0;
	}

	if (post_present_pending_swap)
	{
		post_present_framebuffer.BindForRead();
	}
	else
	{
		int supersampling_factor = SupersamplingFactor();
		float display_gamma = OpenGL_preferred_state.gamma != 0.0f ? 1.f / OpenGL_preferred_state.gamma : 1.f;
		if (supersampling_factor >= 4)
		{
			downsampleshader.Use();
			framebuffers[framebuffer_current_draw].DownsampleTo(downscale_framebuffer.Handle(), 0, 0,
				downscale_framebuffer.Width(), downscale_framebuffer.Height(), downsampleshader_gamma, display_gamma,
				downsampleshader_dest_origin);
			downsampleshader.Use();
			downscale_framebuffer.DownsampleTo(resolved_framebuffer.Handle(), 0, 0,
				resolved_framebuffer.Width(), resolved_framebuffer.Height(), downsampleshader_gamma, display_gamma,
				downsampleshader_dest_origin);
			resolved_framebuffer.BindForRead();
		}
		else if (supersampling_factor >= 2)
		{
			downsampleshader.Use();
			framebuffers[framebuffer_current_draw].DownsampleTo(resolved_framebuffer.Handle(), 0, 0,
				resolved_framebuffer.Width(), resolved_framebuffer.Height(), downsampleshader_gamma, display_gamma,
				downsampleshader_dest_origin);
			resolved_framebuffer.BindForRead();
		}
		else
		{
			framebuffers[framebuffer_current_draw].BindForRead();
		}
		if (supersampling_factor >= 2)
			ShaderProgram::ClearBinding();
	}
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid*)temp_data);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[framebuffer_current_draw].Handle());

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			uint spix = temp_data[((h - 1) - y) * w + x];
			ubyte* dest = &rgba_data[(y * w + x) * 4];
			dest[0] = (ubyte)(spix & 0xff);
			dest[1] = (ubyte)((spix >> 8) & 0xff);
			dest[2] = (ubyte)((spix >> 16) & 0xff);
			dest[3] = 255;
		}
	}

	int saved = bm_SaveRawRGBA32PNG(filename, w, h, rgba_data);
	mem_free(rgba_data);
	mem_free(temp_data);
	return saved;
}

void GL4Renderer::UpdateFramebuffer(void)
{
	int preferred_samples = GL_GetSupportedMsaaSamples(RendererMsaaSamples(OpenGL_preferred_state));
	int target_samples = preferred_samples;
	uint32_t current_samples = framebuffers[0].RequestedSamples();
	if (preferred_samples == 0)
	{
		if (!msaa_deferred_preferred_state_valid)
		{
			msaa_downshift_release_frames = 0;
			msaa_forced_off_target_samples = 0;
			msaa_forced_off_scene_presented = false;
		}
	}
	else if (msaa_forced_off_target_samples != 0 || msaa_downshift_release_frames != 0)
	{
		msaa_downshift_release_frames = 0;
		msaa_forced_off_target_samples = 0;
		msaa_forced_off_scene_presented = false;
	}
	const int supersampling_factor = SupersamplingFactor();
	const int overscan_percent = GL4OverscanPercent(OpenGL_preferred_state);
	framebuffer_logical_width = OpenGL_state.screen_width;
	framebuffer_logical_height = OpenGL_state.screen_height;
	if (overscan_percent > 100 && framebuffer_logical_width > 0 && framebuffer_logical_height > 0)
	{
		framebuffer_logical_width = (framebuffer_logical_width * overscan_percent + 99) / 100;
		framebuffer_logical_height = (framebuffer_logical_height * overscan_percent + 99) / 100;
	}
	framebuffer_logical_offset_x = (framebuffer_logical_width - OpenGL_state.screen_width + 1) / 2;
	framebuffer_logical_offset_y = (framebuffer_logical_height - OpenGL_state.screen_height + 1) / 2;

	int target_width = framebuffer_logical_width * supersampling_factor;
	int target_height = framebuffer_logical_height * supersampling_factor;
	mprintf((0, "GL4 UpdateFramebuffer begin: preferred=%d target=%d current=%u stage=%d size=%dx%d screen=%dx%d ssaa=%d bloom=%d gtao=%d postmask=%u/%u motion=%u/%u.\n",
		preferred_samples, target_samples, (unsigned)current_samples, msaa_downshift_release_frames,
		target_width, target_height, OpenGL_state.screen_width, OpenGL_state.screen_height,
		SupersamplingFactor(), OpenGL_preferred_state.bloom_enabled ? 1 : 0,
		OpenGL_preferred_state.gtao_enabled ? 1 : 0,
		(unsigned)post_protection_mask.mask_texture, (unsigned)post_protection_mask.samples,
		(unsigned)motion_vectors.velocity_texture, (unsigned)motion_vectors.samples));
	bool framebuffer_state_changed = framebuffers[0].Handle() != 0 &&
		(framebuffers[0].RequestedSamples() != (uint32_t)target_samples ||
		 framebuffers[0].Width() != (uint32_t)target_width ||
		 framebuffers[0].Height() != (uint32_t)target_height);
	if (framebuffer_state_changed)
	{
		// Drain current rendering, unbind old targets, then delete every old
		// target before allocating any replacements. Direct 8x->2x transitions
		// should behave like 8x->off->2x instead of interleaving old 8x storage
		// with new 2x allocations.
		glDisable(GL_MULTISAMPLE);
		glFinish();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		GL_UnbindFramebufferTextures();
		for (int i = 0; i < NUM_GL4_FBOS; i++)
			framebuffers[i].Destroy();
		resolved_framebuffer.Destroy();
		downscale_framebuffer.Destroy();
		bloom.DestroyFramebuffers();
		bloom_source_framebuffer.Destroy();
		bloom_source_resolved_framebuffer.Destroy();
		bloom_source_downscale_framebuffer.Destroy();
		ao_scene_framebuffer.Destroy();
		ao_composite_framebuffer.Destroy();
		post_present_framebuffer.Destroy();
		motion_vectors.Destroy();
		post_protection_mask.Destroy();
		GL_UnbindFramebufferTextures();
		glFinish();
	}
	gtao.DestroyFramebuffers();
	motion_vectors.Destroy();

	for (int i = 0; i < NUM_GL4_FBOS; i++)
	{
		framebuffers[i].Update(target_width, target_height, target_samples);
		post_protection_mask.Update(target_width, target_height, framebuffers[i].Samples());
		post_protection_mask.AttachToFramebuffer(framebuffers[i].Handle());
		if (framebuffer_state_changed)
		{
			framebuffers[i].ClearAll();
			post_protection_mask.ClearAttached(framebuffers[i].Handle());
		}
		post_protection_mask.UseSceneDrawBuffers(framebuffers[i].Handle());
	}
	if (supersampling_factor >= 2 || ((OpenGL_preferred_state.bloom_enabled || OpenGL_preferred_state.gtao_enabled) && target_samples >= 2))
		resolved_framebuffer.Update(framebuffer_logical_width, framebuffer_logical_height, 0);
	else
		resolved_framebuffer.Destroy();

	if (supersampling_factor >= 4)
		downscale_framebuffer.Update(framebuffer_logical_width * 2, framebuffer_logical_height * 2, 0);
	else
		downscale_framebuffer.Destroy();
	post_present_framebuffer.Update(OpenGL_state.screen_width, OpenGL_state.screen_height, 0);

	bloom_source_valid = false;
	ao_scene_valid = false;
	legacy_draw_uniforms_dirty = true;
	post_protection_mask_dirty = false;
	post_protection_mask_cleared_this_frame = false;

	framebuffer_current_draw = 0;
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[0].Handle());
	post_protection_mask.UseSceneDrawBuffers(framebuffers[0].Handle());
	if (framebuffers[0].Samples() >= 2)
		glEnable(GL_MULTISAMPLE);
	else
		glDisable(GL_MULTISAMPLE);
	if (framebuffer_state_changed)
		glFinish();
	//Unbind the read framebuffer so that OBS can capture the window properly
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	mprintf((0, "GL4 UpdateFramebuffer end: scene fbo=%u req=%u actual=%u resolved=%u downscale=%u post=%u postmask=%u/%u motion=%u/%u.\n",
		(unsigned)framebuffers[0].Handle(), (unsigned)framebuffers[0].RequestedSamples(), (unsigned)framebuffers[0].Samples(),
		(unsigned)resolved_framebuffer.Handle(), (unsigned)downscale_framebuffer.Handle(),
		(unsigned)post_present_framebuffer.Handle(),
		(unsigned)post_protection_mask.mask_texture, (unsigned)post_protection_mask.samples,
		(unsigned)motion_vectors.velocity_texture, (unsigned)motion_vectors.samples));
	GL4PerfFramebufferResourceState("UpdateEnd", framebuffers[0], preferred_samples,
		target_samples, supersampling_factor, post_protection_mask.mask_texture,
		post_protection_mask.samples, post_protection_mask.msaa_renderbuffer_storage,
		resolved_framebuffer.Handle(), downscale_framebuffer.Handle(), post_present_framebuffer.Handle());
	GL4PerfRenderState("UpdateEnd");
	framebuffer_state_snapshot_pending = true;

	GL_InitFramebufferVAO();
}

void GL4Renderer::CloseFramebuffer(void)
{
	GL4PerfGpuSplitShutdown();
	GL4PerfGpuFrameShutdown();
	for (int i = 0; i < NUM_GL4_FBOS; i++)
	{
		framebuffers[i].Destroy();
	}
	resolved_framebuffer.Destroy();
	downscale_framebuffer.Destroy();
	bloom.DestroyFramebuffers();
	gtao.DestroyFramebuffers();
	bloom_source_framebuffer.Destroy();
	bloom_source_resolved_framebuffer.Destroy();
	bloom_source_downscale_framebuffer.Destroy();
	ao_scene_framebuffer.Destroy();
	ao_composite_framebuffer.Destroy();
	post_present_framebuffer.Destroy();
	bloom_source_valid = false;
	ao_scene_valid = false;
	motion_vectors.Destroy();
	post_protection_mask.Destroy();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	GL_DestroyFramebufferVAO();
}

//shader test
void GL4Renderer::UseShaderTest(void)
{
	testshader.Use();
}

void GL4Renderer::EndShaderTest(void)
{
	//glUseProgram(0);
}

void GL4Renderer::SetCullFace(bool state)
{
	if (state)
		glEnable(GL_CULL_FACE);
	else
		glDisable(GL_CULL_FACE);
}

void GL4Renderer::GetScreenSize(int& screen_width, int& screen_height)
{
	screen_width = OpenGL_state.screen_width;
	screen_height = OpenGL_state.screen_height;
}

double GL4Renderer::GetDisplayRefreshRate()
{
#if defined(SDL3)
	if (GLWindow)
	{
		SDL_DisplayID display = SDL_GetDisplayForWindow(GLWindow);
		const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
		if (mode && mode->refresh_rate > 1.0f)
			return mode->refresh_rate;
	}
#elif defined(WIN32)
	if (hOpenGLWnd)
	{
		HMONITOR monitor = MonitorFromWindow(hOpenGLWnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFOEX monitor_info = {};
		monitor_info.cbSize = sizeof(monitor_info);
		if (monitor && GetMonitorInfo(monitor, &monitor_info))
		{
			DEVMODE device_mode = {};
			device_mode.dmSize = sizeof(device_mode);
			if (EnumDisplaySettings(monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &device_mode) &&
				device_mode.dmDisplayFrequency > 1)
			{
				return (double)device_mode.dmDisplayFrequency;
			}
		}
	}
#endif

	return 0.0;
}

GL4Renderer::GL4Renderer()
{
}

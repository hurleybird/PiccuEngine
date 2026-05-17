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

#pragma once

#include "grdefs.h"
#include "object.h"

//Do all game functions for one frame: render, move objects, etc.
void GameFrame(void);

//Render the world into a game window
//Parameters:	viewer - if not null, this object disabled from rendering.  Not used otherwise.
//					viewer_eye - where we're rendering from
//					viewer_roomnum - the roomnum viewer_eye is in
//					viewer_orient - the oriention for this view
//					zoom - the zoom for this view
//					rear_view - if true, we're looking out the rear of this object
void GameRenderWorld(object *viewer,vector *viewer_eye,int viewer_roomnum,matrix *viewer_orient,float zoom,bool rear_view,
					 int anchor_x = 0,int anchor_y = 0,int anchor_w = 0,int anchor_h = 0);

//[ISB] User's desired FOV. 
extern float Render_FOV_desired;

extern float Render_zoom;
extern float Render_FOV;

extern bool Game_paused;							//	determines if game is paused.
extern double Min_allowed_frametime;				// minimum seconds per frame, used for frame limiting

extern bool Rendering_main_view;					// determines if we're rendering the main view
extern bool Skip_render_game_frame;				// skips rendering the game frame if set.

void SetFrameLimitFps(int fps);
int GetFrameLimitFps();
void SetFrameLimitCommandLineOverride(bool enabled);
bool FrameLimitHasCommandLineOverride();

extern bool Perf_markers_enabled;
void PerfMarkersSetEnabled(bool enabled);
void PerfMarkersCaptureNextFrames(int frame_count, const char* reason);
void PerfMarkersBeginFrame();
void PerfMarkersEndFrame();
void PerfMarkersBegin(const char* marker_name);
void PerfMarkersEnd(const char* marker_name);
double PerfMarkersNow();
void PerfMarkersRecordDuration(const char* marker_name, double start_time, double duration);
bool RendererShouldCountMsaaTransitionFrame();

class PerfMarkerScope
{
public:
	explicit PerfMarkerScope(const char* marker_name) : m_marker_name(marker_name), m_active(Perf_markers_enabled)
	{
		if (m_active)
			PerfMarkersBegin(m_marker_name);
	}

	~PerfMarkerScope()
	{
		if (m_active)
			PerfMarkersEnd(m_marker_name);
	}

private:
	const char* m_marker_name;
	bool m_active;
};

#define PERF_MARKER_JOIN_INNER(a, b) a##b
#define PERF_MARKER_JOIN(a, b) PERF_MARKER_JOIN_INNER(a, b)
#define PERF_MARKER_SCOPE(name) PerfMarkerScope PERF_MARKER_JOIN(perf_marker_scope_, __LINE__)(name)


//Turn off all camera views
//If total reset is true, set all views to none, otherwise kill object view but keep rear views.
void InitCameraViews(bool total_reset);

//Pauses game
void PauseGame();

//	resumes game operation.
void ResumeGame();

#ifdef _DEBUG
//	initializes test systems.
void InitTestSystems();
#endif

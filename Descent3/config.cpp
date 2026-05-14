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

#include "ConfigItem.h"
#include "player.h"
#include "config.h"
#include "ddio.h"
#include "newui.h"
#include "3d.h"
#include "polymodel.h"
#include "application.h"
#include "descent.h"
#include "mono.h"
#include "Mission.h"
#include "ddio.h"
#include "gamefont.h"
#include "multi_ui.h"
#include "cinematics.h"
#include "hlsoundlib.h"
#include "terrain.h"
#include "CFILE.H"
#include "mem.h"
#include "lighting.h"
#include "PHYSICS.H"
#include "pilot.h"
#include "hud.h"
#include "bitmap.h"
#include "game.h"
#include "render.h"
#include "stringtable.h"
#include "SmallViews.h"
#include "D3ForceFeedback.h"
#include "descent.h"
#include "appdatabase.h"
#include "hlsoundlib.h"
#include "soundload.h"
#include "sounds.h"
#include "ctlconfig.h"
#include "d3music.h"
#include "gameloop.h"

#if defined(SDL3)
#include <SDL3/SDL_video.h>
#elif defined(WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#define STAT_SCORE STAT_TIMER

static const int Config_base_vertical_resolutions[] = {
	384, 480, 540, 600, 720, 768, 800, 960, 1050, 1080, 1200, 1440, 1600, 2160
};

struct ConfigAspectRatio
{
	int width;
	int height;
	const char* name;
};

enum
{
	CONFIG_ASPECT_5_4,
	CONFIG_ASPECT_4_3,
	CONFIG_ASPECT_16_10,
	CONFIG_ASPECT_16_9,
	CONFIG_ASPECT_COUNT
};

static const ConfigAspectRatio Config_aspect_ratios[CONFIG_ASPECT_COUNT] = {
	{5, 4, "5:4"},
	{4, 3, "4:3"},
	{16, 10, "16:10"},
	{16, 9, "16:9"}
};

int Game_video_resolution = 1;
int Game_window_res_width = 1920, Game_window_res_height = 1080;
int Game_window_aspect = CONFIG_ASPECT_16_9;
bool Game_fullscreen = false;
float Hud_text_scale = 1.0f;
float Render_FOV_desired = 72;

tDetailSettings Detail_settings;
int Default_detail_level = DETAIL_LEVEL_MED;

// toggles specified in general settings.
tGameToggles Game_toggles =
{
	true,
	false,
	true
};

int DesiredOpenGLProfile = GLPROFILE_CORE; //[ISB] yeah it shouldn't be an int but I don't want to deal with include order or include renderer.h in config so..
bool DesiredOpenGLProfileExplicit = false;

static bool ConfigCanUsePerPixelLighting()
{
	return OpenGLProfile == GLPROFILE_CORE;
}

static bool ConfigCanUseHBAO()
{
	return OpenGLProfile == GLPROFILE_CORE;
}

static bool ConfigShowsLegacyTerrainControls()
{
	return OpenGLProfile != GLPROFILE_CORE;
}

int ConfigNormalizeSupersamplingFactor(int factor)
{
	if (factor >= 4)
		return 4;
	if (factor >= 2)
		return 2;
	return 1;
}

float ConfigNormalizeHudTextScale(float scale)
{
	if (scale < 0.75f)
		return 0.75f;
	if (scale > 1.5f)
		return 1.5f;
	return scale;
}

float ConfigNormalizeBloomThreshold(float threshold)
{
	if (threshold < 0.0f)
		return 0.0f;
	if (threshold > 1.0f)
		return 1.0f;
	return threshold;
}

float ConfigNormalizeBloomIntensity(float intensity)
{
	if (intensity < 0.0f)
		return 0.0f;
	if (intensity > 1.0f)
		return 1.0f;
	return intensity;
}

static int ConfigNormalizeHBAOResolution(int resolution)
{
	if (resolution < HBAO_RESOLUTION_AUTO)
		return HBAO_RESOLUTION_AUTO;
	if (resolution > HBAO_RESOLUTION_QUARTER)
		return HBAO_RESOLUTION_QUARTER;
	return resolution;
}

static int SupersamplingFactorToIndex(int factor)
{
	switch (ConfigNormalizeSupersamplingFactor(factor))
	{
	case 2:
		return 1;
	case 4:
		return 2;
	default:
		return 0;
	}
}

static int SupersamplingIndexToFactor(int index)
{
	switch (index)
	{
	case 1:
		return 2;
	case 2:
		return 4;
	default:
		return 1;
	}
}

static int NormalizeMsaaSamples(int samples)
{
	if (samples >= 8)
		return 8;
	if (samples >= 4)
		return 4;
	if (samples >= 2)
		return 2;
	return 0;
}

static int MsaaSamplesToIndex(int samples)
{
	switch (NormalizeMsaaSamples(samples))
	{
	case 2:
		return 1;
	case 4:
		return 2;
	case 8:
		return 3;
	default:
		return 0;
	}
}

static int MsaaIndexToSamples(int index)
{
	switch (index)
	{
	case 1:
		return 2;
	case 2:
		return 4;
	case 3:
		return 8;
	default:
		return 0;
	}
}

#define IDV_VCONFIG			12	//video config
#define IDV_GCONFIG			13	//general config
#define IDV_SCONFIG			14	//audio config
#define IDV_DCONFIG			15	//detail level config
#define IDV_HCONFIG			16 //hud config
#define IDV_CCONFIG			17 //controller config
#define IDV_QUIT				0xff

#define UID_GAMMASLIDER		0x1000


/////////////////////////////////////////////////////////////////////////////
// Defines
#define IDV_OK			1
#define IDV_CANCEL	2

//video defines
#define VID_D3D			0
#define VID_GLIDE		1
#define VID_OPENGL		2

//small view settings
#define SMVS_NONE		0
#define SMVS_MISSLE	1

//these are the default settings for each detail level
#define DL_LOW_TERRAIN_DISTANCE		(MINIMUM_RENDER_DIST*TERRAIN_SIZE)
#define DL_LOW_PIXEL_ERROR			25.0
#define DL_LOW_SPECULAR_LIGHT		false
#define DL_LOW_DYNAMIC_LIGHTING		false
#define DL_LOW_FAST_HEADLIGHT		true
#define DL_LOW_MIRRORED_SURFACES	false
#define DL_LOW_FOG_ENABLED			false
#define DL_LOW_CORONAS_ENABLES		false
#define DL_LOW_PROCEDURALS			false
#define DL_LOW_POWERUP_HALOS		false
#define DL_LOW_SCORCH_MARKS			false
#define DL_LOW_WEAPON_CORONAS		false
#define DL_LOW_SPEC_MAPPING_TYPE	1
#define DL_LOW_OBJECT_COMPLEXITY	OBJECT_COMPLEXITY_LOW

#define DL_MED_TERRAIN_DISTANCE		(90*TERRAIN_SIZE)
#define DL_MED_PIXEL_ERROR			18.0
#define DL_MED_SPECULAR_LIGHT		false
#define DL_MED_DYNAMIC_LIGHTING		false
#define DL_MED_FAST_HEADLIGHT		true
#define DL_MED_MIRRORED_SURFACES	false
#define DL_MED_FOG_ENABLED			true
#define DL_MED_CORONAS_ENABLES		true
#define DL_MED_PROCEDURALS			false
#define DL_MED_POWERUP_HALOS		true
#define DL_MED_SCORCH_MARKS			true
#define DL_MED_WEAPON_CORONAS		false
#define DL_MED_SPEC_MAPPING_TYPE	1
#define DL_MED_OBJECT_COMPLEXITY	OBJECT_COMPLEXITY_MEDIUM

#define DL_HIGH_TERRAIN_DISTANCE	(100*TERRAIN_SIZE)
#define DL_HIGH_PIXEL_ERROR			12.0
#define DL_HIGH_SPECULAR_LIGHT		false
#define DL_HIGH_DYNAMIC_LIGHTING	true
#define DL_HIGH_FAST_HEADLIGHT		true
#define DL_HIGH_MIRRORED_SURFACES	true
#define DL_HIGH_FOG_ENABLED			true
#define DL_HIGH_CORONAS_ENABLES		true
#define DL_HIGH_PROCEDURALS			true
#define DL_HIGH_POWERUP_HALOS		true
#define DL_HIGH_SCORCH_MARKS		true
#define DL_HIGH_WEAPON_CORONAS		true
#define DL_HIGH_SPEC_MAPPING_TYPE	1
#define DL_HIGH_OBJECT_COMPLEXITY	OBJECT_COMPLEXITY_HIGH

#define DL_VHI_TERRAIN_DISTANCE		(120.0*TERRAIN_SIZE)
#define DL_VHI_PIXEL_ERROR			10.0
#define DL_VHI_SPECULAR_LIGHT		true
#define DL_VHI_DYNAMIC_LIGHTING		true
#define DL_VHI_FAST_HEADLIGHT		true
#define DL_VHI_MIRRORED_SURFACES	true
#define DL_VHI_FOG_ENABLED			true
#define DL_VHI_CORONAS_ENABLES		true
#define DL_VHI_PROCEDURALS			true
#define DL_VHI_POWERUP_HALOS		true
#define DL_VHI_SCORCH_MARKS			true
#define DL_VHI_WEAPON_CORONAS		true
#define DL_VHI_SPEC_MAPPING_TYPE	1
#define DL_VHI_OBJECT_COMPLEXITY	OBJECT_COMPLEXITY_HIGH

#define MINIMUM_TERRAIN_DETAIL		4
#define MAXIMUM_TERRAIN_DETAIL		28
#define MINIMUM_RENDER_DIST			80
#define MAXIMUM_RENDER_DIST			200

tDetailSettings DetailPresetLow =
{
	DL_LOW_TERRAIN_DISTANCE,
	DL_LOW_PIXEL_ERROR,
	DL_LOW_SPECULAR_LIGHT,
	DL_LOW_DYNAMIC_LIGHTING,
	DL_LOW_FAST_HEADLIGHT,
	DL_LOW_MIRRORED_SURFACES,
	DL_LOW_FOG_ENABLED,
	DL_LOW_CORONAS_ENABLES,
	DL_LOW_PROCEDURALS,
	DL_LOW_POWERUP_HALOS,
	DL_LOW_SCORCH_MARKS,
	DL_LOW_WEAPON_CORONAS,
	DL_LOW_SPEC_MAPPING_TYPE,
	DL_LOW_OBJECT_COMPLEXITY
};
tDetailSettings DetailPresetMed =
{
	DL_MED_TERRAIN_DISTANCE,
	DL_MED_PIXEL_ERROR,
	DL_MED_SPECULAR_LIGHT,
	DL_MED_DYNAMIC_LIGHTING,
	DL_MED_FAST_HEADLIGHT,
	DL_MED_MIRRORED_SURFACES,
	DL_MED_FOG_ENABLED,
	DL_MED_CORONAS_ENABLES,
	DL_MED_PROCEDURALS,
	DL_MED_POWERUP_HALOS,
	DL_MED_SCORCH_MARKS,
	DL_MED_WEAPON_CORONAS,
	DL_MED_SPEC_MAPPING_TYPE,
	DL_MED_OBJECT_COMPLEXITY
};
tDetailSettings DetailPresetHigh =
{
	DL_HIGH_TERRAIN_DISTANCE,
	DL_HIGH_PIXEL_ERROR,
	DL_HIGH_SPECULAR_LIGHT,
	DL_HIGH_DYNAMIC_LIGHTING,
	DL_HIGH_FAST_HEADLIGHT,
	DL_HIGH_MIRRORED_SURFACES,
	DL_HIGH_FOG_ENABLED,
	DL_HIGH_CORONAS_ENABLES,
	DL_HIGH_PROCEDURALS,
	DL_HIGH_POWERUP_HALOS,
	DL_HIGH_SCORCH_MARKS,
	DL_HIGH_WEAPON_CORONAS,
	DL_HIGH_SPEC_MAPPING_TYPE,
	DL_HIGH_OBJECT_COMPLEXITY
};
tDetailSettings DetailPresetVHi =
{
	DL_VHI_TERRAIN_DISTANCE,
	DL_VHI_PIXEL_ERROR,
	DL_VHI_SPECULAR_LIGHT,
	DL_VHI_DYNAMIC_LIGHTING,
	DL_VHI_FAST_HEADLIGHT,
	DL_VHI_MIRRORED_SURFACES,
	DL_VHI_FOG_ENABLED,
	DL_VHI_CORONAS_ENABLES,
	DL_VHI_PROCEDURALS,
	DL_VHI_POWERUP_HALOS,
	DL_VHI_SCORCH_MARKS,
	DL_VHI_WEAPON_CORONAS,
	DL_VHI_SPEC_MAPPING_TYPE,
	DL_VHI_OBJECT_COMPLEXITY
};

void ConfigSetDetailLevel(int level)
{
	switch (level)
	{
	case DETAIL_LEVEL_LOW:
		memcpy(&Detail_settings, &DetailPresetLow, sizeof(tDetailSettings));
		break;
	case DETAIL_LEVEL_MED:
		memcpy(&Detail_settings, &DetailPresetMed, sizeof(tDetailSettings));
		break;
	case DETAIL_LEVEL_HIGH:
		memcpy(&Detail_settings, &DetailPresetHigh, sizeof(tDetailSettings));
		break;
	case DETAIL_LEVEL_VERY_HIGH:
		memcpy(&Detail_settings, &DetailPresetVHi, sizeof(tDetailSettings));
		break;
	};

	Default_detail_level = level;
}


//////////////////////////////////////////////////////////////////////////////
// Gamma settings

//gamma configuration, window placement
#define	GAMMA_MENU_H	320
#define	GAMMA_MENU_W	416

#define GAMMA_SLICES			32
#define GAMMA_SLICE_WIDTH	256
#define GAMMA_SLICE_HEIGHT 128
#define GAMMA_SLICE_X		((GAMMA_MENU_W - GAMMA_SLICE_WIDTH)/2);
#define GAMMA_SLICE_Y		64
#define GAMMA_SLIDER_UNITS	145 //[ISB] Allow gamma all the way down to 0.1

#define IDV_AUTOGAMMA	6

void gamma_callback(newuiTiledWindow* wnd, void* data)
{
	int bm_handle = *((int*)data);

	g3Point pnts[4], * pntlist[4];
	rend_SetColorModel(CM_RGB);
	rend_SetAlphaType(AT_ALWAYS);
	rend_SetTextureType(TT_LINEAR);
	rend_SetLighting(LS_NONE);
	rend_SetOverlayType(OT_NONE);
	rend_SetWrapType(WT_WRAP);
	rend_SetZBufferState(0);

	// First draw checkboard
	int startx = GAMMA_SLICE_X;

	for (int i = 0; i < 4; i++)
	{
		pntlist[i] = &pnts[i];
		pnts[i].p3_z = 0;
		pnts[i].p3_r = 1;
		pnts[i].p3_g = 1;
		pnts[i].p3_b = 1;
		pnts[i].p3_flags = PF_PROJECTED;
	}

	pnts[0].p3_sx = startx;
	pnts[0].p3_sy = GAMMA_SLICE_Y;
	pnts[0].p3_u = 0;
	pnts[0].p3_v = 0;

	pnts[1].p3_sx = startx + GAMMA_SLICE_WIDTH;
	pnts[1].p3_sy = GAMMA_SLICE_Y;
	pnts[1].p3_u = 2;
	pnts[1].p3_v = 0;

	pnts[2].p3_sx = startx + GAMMA_SLICE_WIDTH;
	pnts[2].p3_sy = GAMMA_SLICE_Y + GAMMA_SLICE_HEIGHT;
	pnts[2].p3_u = 2;
	pnts[2].p3_v = 1;

	pnts[3].p3_sx = startx;
	pnts[3].p3_sy = GAMMA_SLICE_Y + GAMMA_SLICE_HEIGHT;
	pnts[3].p3_u = 0;
	pnts[3].p3_v = 1;

	rend_DrawPolygon2D(bm_handle, pntlist, 4);

	// Now draw grey in the center
	int int_val = 8;
	rend_SetFlatColor(GR_RGB(int_val, int_val, int_val));
	rend_SetTextureType(TT_FLAT);

	pnts[0].p3_sx += 64;
	pnts[0].p3_sy += 32;

	pnts[1].p3_sx -= 64;
	pnts[1].p3_sy += 32;

	pnts[2].p3_sx -= 64;
	pnts[2].p3_sy -= 32;

	pnts[3].p3_sx += 64;
	pnts[3].p3_sy -= 32;

	rend_DrawPolygon2D(0, pntlist, 4);

	rend_SetWrapType(WT_CLAMP);
	rend_SetZBufferState(1);
}


void config_gamma()
{
	newuiTiledWindow gamma_wnd;
	newuiSheet* sheet;
	tSliderSettings slider_set;
	short* gamma_slider;
	short curpos;
	int res, gamma_bitmap = -1;
	float init_gamma;

	// Make gamma bitmap
	gamma_bitmap = bm_AllocBitmap(128, 128, 0);
	if (gamma_bitmap <= 0) {
		gamma_bitmap = 0;
	}
	else {
		ushort* dest_data = (ushort*)bm_data(gamma_bitmap, 0);
		int addval = 0;
		int i, t;

		// This loop makes the checkerboard
		for (i = 0; i < 128; i++)
		{
			addval = (i & 1) ? 1 : 0;

			for (t = 0; t < 128; t++)
			{
				if (((t + addval) % 2) == 0) {
					dest_data[i * 128 + t] = OPAQUE_FLAG | (1 << 10) | (1 << 5) | (1);
				}
				else {
					dest_data[i * 128 + t] = OPAQUE_FLAG;
				}
			}
		}
	}

	// create ui.
	gamma_wnd.Create(TXT_AUTO_GAMMA, 0, 0, GAMMA_MENU_W, GAMMA_MENU_H);
	gamma_wnd.SetData(&gamma_bitmap);
	gamma_wnd.SetOnDrawCB(gamma_callback);

	sheet = gamma_wnd.GetSheet();

	// ok, cancel buttons.
	sheet->NewGroup(NULL, 130, 224, NEWUI_ALIGN_HORIZ);
	sheet->AddButton(TXT_OK, UID_OK);
	sheet->AddButton(TXT_CANCEL, UID_CANCEL);

	sheet->NewGroup(NULL, 0, 145);
	sheet->AddText(TXT_GAMMAADJUSTA);
	sheet->AddText(TXT_GAMMAADJUSTB);

	// add slider!
	sheet->NewGroup(NULL, 0, 175);
	slider_set.min_val.f = 0.1f;
	slider_set.max_val.f = 3.0f;
	slider_set.type = SLIDER_UNITS_FLOAT;

	init_gamma = Render_preferred_state.gamma;
	curpos = CALC_SLIDER_POS_FLOAT(init_gamma, &slider_set, GAMMA_SLIDER_UNITS);
	gamma_slider = sheet->AddSlider(TXT_GAMMA, GAMMA_SLIDER_UNITS, curpos, &slider_set, UID_GAMMASLIDER);
	sheet->SetInitialFocusedGadget(UID_GAMMASLIDER);

	// do ui.
	gamma_wnd.Open();

	do
	{
		res = gamma_wnd.DoUI();
		if (res == NEWUIRES_FORCEQUIT) {
			break;
		}
		if (sheet->HasChanged(gamma_slider)) {
			Render_preferred_state.gamma = CALC_SLIDER_FLOAT_VALUE(*gamma_slider, slider_set.min_val.f, slider_set.max_val.f, GAMMA_SLIDER_UNITS);
			rend_SetPreferredState(&Render_preferred_state);
		}
	} while (res != UID_OK && res != UID_CANCEL);

	// handle results.
	if (res == UID_OK) {
		Render_preferred_state.gamma = CALC_SLIDER_FLOAT_VALUE(*gamma_slider, slider_set.min_val.f, slider_set.max_val.f, GAMMA_SLIDER_UNITS);
	}
	else {
		Render_preferred_state.gamma = init_gamma;
	}

	rend_SetPreferredState(&Render_preferred_state);

	// cleanup
	gamma_wnd.Close();
	gamma_wnd.Destroy();

	if (gamma_bitmap != 0) {
		bm_FreeBitmap(gamma_bitmap);
	}
}

//////////////////////////////////////////////////////////////////
// VIDEO MENU
//
#define RESBUFFER_SIZE 50
#define ASPECTBUFFER_SIZE 32
#define IDV_CHANGEWINDOW 10
#define IDV_CHANGEASPECT 11
#define UID_RESOLUTION 110
#define UID_ASPECT 111

static int ConfigRoundAspectWidth(int height, int aspect)
{
	if (aspect < 0 || aspect >= CONFIG_ASPECT_COUNT)
		aspect = CONFIG_ASPECT_4_3;

	int width = (height * Config_aspect_ratios[aspect].width +
		Config_aspect_ratios[aspect].height / 2) / Config_aspect_ratios[aspect].height;
	if (width & 1)
		width++;
	return width;
}

static bool ConfigAspectFitsDisplay(int aspect, int height, int display_width, int display_height)
{
	if (aspect < 0 || aspect >= CONFIG_ASPECT_COUNT || height <= 0)
		return false;
	if (display_width > 0 && ConfigRoundAspectWidth(height, aspect) > display_width)
		return false;
	if (display_height > 0 && height > display_height)
		return false;
	return true;
}

static int ConfigAspectFromSize(int width, int height)
{
	if (width <= 0 || height <= 0)
		return CONFIG_ASPECT_4_3;

	int best = CONFIG_ASPECT_4_3;
	int best_error = 0x7fffffff;
	for (int i = 0; i < CONFIG_ASPECT_COUNT; i++)
	{
		int error = abs(width * Config_aspect_ratios[i].height -
			height * Config_aspect_ratios[i].width);
		if (error < best_error)
		{
			best_error = error;
			best = i;
		}
	}
	return best;
}

static int ConfigBestAspectForDisplay(int desired_aspect, int height, int display_width, int display_height)
{
	if (ConfigAspectFitsDisplay(desired_aspect, height, display_width, display_height))
		return desired_aspect;

	for (int i = CONFIG_ASPECT_COUNT - 1; i >= 0; i--)
	{
		if (ConfigAspectFitsDisplay(i, height, display_width, display_height))
			return i;
	}

	return CONFIG_ASPECT_5_4;
}

static int ConfigAspectOrderIndex(int aspect)
{
	switch (aspect)
	{
	case CONFIG_ASPECT_16_9:
		return 0;
	case CONFIG_ASPECT_16_10:
		return 1;
	case CONFIG_ASPECT_4_3:
		return 2;
	case CONFIG_ASPECT_5_4:
	default:
		return 3;
	}
}

static int ConfigAspectFromOrderIndex(int index)
{
	switch (index)
	{
	case 0:
		return CONFIG_ASPECT_16_9;
	case 1:
		return CONFIG_ASPECT_16_10;
	case 2:
		return CONFIG_ASPECT_4_3;
	default:
		return CONFIG_ASPECT_5_4;
	}
}

static int ConfigBuildVerticalResolutionList(int* resolutions, int max_resolutions,
	int display_width, int display_height)
{
	int count = 0;
	for (int i = 0; i < (int)(sizeof(Config_base_vertical_resolutions) / sizeof(Config_base_vertical_resolutions[0])); i++)
	{
		int height = Config_base_vertical_resolutions[i];
		if (!ConfigAspectFitsDisplay(CONFIG_ASPECT_5_4, height, display_width, display_height))
			continue;

		bool duplicate = false;
		for (int j = 0; j < count; j++)
		{
			if (resolutions[j] == height)
			{
				duplicate = true;
				break;
			}
		}
		if (!duplicate && count < max_resolutions)
			resolutions[count++] = height;
	}

	if (display_height > 0 &&
		ConfigAspectFitsDisplay(CONFIG_ASPECT_5_4, display_height, display_width, display_height))
	{
		bool duplicate = false;
		for (int i = 0; i < count; i++)
		{
			if (resolutions[i] == display_height)
			{
				duplicate = true;
				break;
			}
		}
		if (!duplicate && count < max_resolutions)
			resolutions[count++] = display_height;
	}

	for (int i = 1; i < count; i++)
	{
		int value = resolutions[i];
		int j = i - 1;
		while (j >= 0 && resolutions[j] > value)
		{
			resolutions[j + 1] = resolutions[j];
			j--;
		}
		resolutions[j + 1] = value;
	}

	return count;
}

static int ConfigHighestVerticalResolutionAtOrBelow(int desired_height, const int* resolutions, int count)
{
	if (count <= 0)
		return desired_height > 0 ? desired_height : 480;

	int best = resolutions[0];
	for (int i = 0; i < count; i++)
	{
		if (resolutions[i] <= desired_height)
			best = resolutions[i];
	}
	return best;
}

static void ConfigGetDesktopDisplaySize(int* display_width, int* display_height)
{
	*display_width = *display_height = 0;
#if defined(SDL3)
	SDL_DisplayID display = SDL_GetPrimaryDisplay();
	const SDL_DisplayMode* mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
	if (mode)
	{
		*display_width = mode->w;
		*display_height = mode->h;
	}
#elif defined(WIN32)
	DEVMODE device_mode = {};
	device_mode.dmSize = sizeof(device_mode);
	if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &device_mode))
	{
		*display_width = (int)device_mode.dmPelsWidth;
		*display_height = (int)device_mode.dmPelsHeight;
	}
	if (*display_width <= 0 || *display_height <= 0)
	{
		*display_width = GetSystemMetrics(SM_CXSCREEN);
		*display_height = GetSystemMetrics(SM_CYSCREEN);
	}
#endif
}

static void ConfigResolveDisplayMode(int display_width, int display_height, int desired_height,
	int desired_aspect, int* resolved_width, int* resolved_height, int* resolved_aspect)
{
	int resolutions[32];
	int count = ConfigBuildVerticalResolutionList(resolutions, 32, display_width, display_height);
	if (count <= 0)
	{
		*resolved_height = display_height > 0 ? display_height : 480;
		*resolved_aspect = CONFIG_ASPECT_5_4;
		*resolved_width = ConfigRoundAspectWidth(*resolved_height, *resolved_aspect);
		return;
	}

	if (desired_aspect < 0 || desired_aspect >= CONFIG_ASPECT_COUNT)
		desired_aspect = CONFIG_ASPECT_16_9;

	int start_height = ConfigHighestVerticalResolutionAtOrBelow(desired_height, resolutions, count);
	int start_index = 0;
	for (int i = 0; i < count; i++)
	{
		if (resolutions[i] == start_height)
		{
			start_index = i;
			break;
		}
	}

	for (int height_index = start_index; height_index >= 0; height_index--)
	{
		int height = resolutions[height_index];
		int start_aspect_order = (height == start_height && start_height == desired_height) ?
			ConfigAspectOrderIndex(desired_aspect) : ConfigAspectOrderIndex(CONFIG_ASPECT_16_9);

		for (int aspect_order = start_aspect_order; aspect_order < CONFIG_ASPECT_COUNT; aspect_order++)
		{
			int aspect = ConfigAspectFromOrderIndex(aspect_order);
			if (!ConfigAspectFitsDisplay(aspect, height, display_width, display_height))
				continue;

			*resolved_height = height;
			*resolved_aspect = aspect;
			*resolved_width = ConfigRoundAspectWidth(height, aspect);
			return;
		}
	}

	*resolved_height = resolutions[0];
	*resolved_aspect = CONFIG_ASPECT_5_4;
	*resolved_width = ConfigRoundAspectWidth(*resolved_height, *resolved_aspect);
}

void ConfigValidateGameWindowSize()
{
	int display_width, display_height;
	ConfigGetDesktopDisplaySize(&display_width, &display_height);
	if (display_width <= 0)
		display_width = Game_window_res_width > 0 ? Game_window_res_width : 1920;
	if (display_height <= 0)
		display_height = Game_window_res_height > 0 ? Game_window_res_height : 1080;

	int desired_aspect = Game_window_aspect;
	if (desired_aspect < 0 || desired_aspect >= CONFIG_ASPECT_COUNT ||
		abs(ConfigRoundAspectWidth(Game_window_res_height, desired_aspect) - Game_window_res_width) > 2)
	{
		desired_aspect = ConfigAspectFromSize(Game_window_res_width, Game_window_res_height);
	}

	ConfigResolveDisplayMode(display_width, display_height, Game_window_res_height, desired_aspect,
		&Game_window_res_width, &Game_window_res_height, &Game_window_aspect);
}

static void ConfigApplyFixedHBAOSettings()
{
	Render_preferred_state.hbao_temporal = true;
	Render_preferred_state.hbao_quality = HBAO_QUALITY_HIGH;
	Render_preferred_state.hbao_blur = HBAO_BLUR_WIDE;
	Render_preferred_state.hbao_radius = 3.0f;
	Render_preferred_state.hbao_intensity = 1.25f;
	Render_preferred_state.hbao_bias = 0.2f;
}

static int HBAOPresetToIndex(bool enabled, int resolution)
{
	if (!enabled)
		return 0;

	switch (ConfigNormalizeHBAOResolution(resolution))
	{
	case HBAO_RESOLUTION_QUARTER:
		return 1;
	case HBAO_RESOLUTION_FULL:
		return 3;
	case HBAO_RESOLUTION_HALF:
	case HBAO_RESOLUTION_AUTO:
	default:
		return 2;
	}
}

static void ApplyHBAOPresetFromIndex(int index)
{
	ConfigApplyFixedHBAOSettings();
	switch (index)
	{
	case 1:
		Render_preferred_state.hbao_enabled = true;
		Render_preferred_state.hbao_resolution = HBAO_RESOLUTION_QUARTER;
		break;
	case 2:
		Render_preferred_state.hbao_enabled = true;
		Render_preferred_state.hbao_resolution = HBAO_RESOLUTION_HALF;
		break;
	case 3:
		Render_preferred_state.hbao_enabled = true;
		Render_preferred_state.hbao_resolution = HBAO_RESOLUTION_FULL;
		break;
	case 0:
	default:
		Render_preferred_state.hbao_enabled = false;
		Render_preferred_state.hbao_resolution = HBAO_RESOLUTION_HALF;
		break;
	}
}

struct video_menu
{
	newuiSheet* sheet;
	newuiMenu* parent_menu;

	bool* filtering;									// settings
	bool* mipmapping;
	bool* per_pixel_lighting;
	bool* bloom_enabled;
	bool* vsync;

	short* fov;
	char* buffer;
	char* aspect_buffer;
	bool* fullscreen;
	int* antialiasing;
	int* supersampling;
	int* hbao;
	int* backend;

	int window_width, window_height;
	int window_aspect;
	int display_width, display_height;

	void update_display_text()
	{
		if (buffer)
			snprintf(buffer, RESBUFFER_SIZE, "%d x %d", window_width, window_height);
		if (aspect_buffer)
			snprintf(aspect_buffer, ASPECTBUFFER_SIZE, "%5s", Config_aspect_ratios[window_aspect].name);
	}

	void init_display_bounds()
	{
		ConfigGetDesktopDisplaySize(&display_width, &display_height);
		if (display_width <= 0 || display_height <= 0)
		{
			display_width = Max_window_w > 0 ? Max_window_w : Game_window_res_width;
			display_height = Max_window_h > 0 ? Max_window_h : Game_window_res_height;
		}
		if (display_width <= 0)
			display_width = 640;
		if (display_height <= 0)
			display_height = 480;
	}

	void normalize_display_choice()
	{
		ConfigResolveDisplayMode(display_width, display_height, window_height, window_aspect,
			&window_width, &window_height, &window_aspect);
		update_display_text();
	}

	void recenter_parent_menu()
	{
		if (!parent_menu)
			return;

		parent_menu->Move((Max_window_w - parent_menu->W()) / 2,
			(Max_window_h - parent_menu->H()) / 2,
			parent_menu->W(),
			parent_menu->H());
	}

	bool can_apply_display_settings_live()
	{
		return GetFunctionMode() == GAME_MODE || GetFunctionMode() == EDITOR_GAME_MODE;
	}

	bool apply_display_settings(bool allow_menu)
	{
		if (!fullscreen)
			return false;

		if (window_width == Game_window_res_width &&
			window_height == Game_window_res_height &&
			window_aspect == Game_window_aspect &&
			*fullscreen == Game_fullscreen)
		{
			return false;
		}

		if (!allow_menu && !can_apply_display_settings_live())
			return false;

		void (*old_callback)() = GetUICallback();
		bool old_cursor_visible = ui_IsCursorVisible();

		Game_fullscreen = *fullscreen;
		Game_window_res_width = window_width;
		Game_window_res_height = window_height;
		Game_window_aspect = window_aspect;

		ForceFullGameWindowOnNextGameMode();
		SetScreenMode(GetScreenMode(), true);
		if (old_callback)
			SetUICallback(old_callback);
		if (old_cursor_visible)
			ui_ShowCursor();
		if (GetScreenMode() == SM_GAME)
			PersistCurrentPilotGameWindowSize(true);
		recenter_parent_menu();

		return true;
	}

	void apply_live_settings()
	{
		bool changed = false;
		bool display_changed = fullscreen && sheet->HasChanged(fullscreen);
		bool fov_changed = false;

		if (filtering && sheet->HasChanged(filtering))
		{
			Render_preferred_state.filtering = (*filtering) ? 1 : 0;
			changed = true;
		}
		if (mipmapping && sheet->HasChanged(mipmapping))
		{
			Render_preferred_state.mipping = (*mipmapping) ? 1 : 0;
			changed = true;
		}
		if (per_pixel_lighting && sheet->HasChanged(per_pixel_lighting))
		{
			Render_preferred_state.per_pixel_lighting = ConfigCanUsePerPixelLighting() && *per_pixel_lighting;
			if (*per_pixel_lighting != Render_preferred_state.per_pixel_lighting)
			{
				*per_pixel_lighting = Render_preferred_state.per_pixel_lighting;
				sheet->UpdateChanges();
			}
			changed = true;
		}
		if (bloom_enabled && sheet->HasChanged(bloom_enabled))
		{
			Render_preferred_state.bloom_enabled = *bloom_enabled;
			changed = true;
		}
		if (hbao && sheet->HasChanged(hbao))
		{
			if (ConfigCanUseHBAO())
			{
				ApplyHBAOPresetFromIndex(*hbao);
			}
			else
			{
				ApplyHBAOPresetFromIndex(0);
				if (*hbao != 0)
				{
					*hbao = 0;
					sheet->UpdateChanges();
				}
			}
			changed = true;
		}
		if (vsync && sheet->HasChanged(vsync))
		{
			Render_preferred_state.vsync_on = (*vsync) ? 1 : 0;
			changed = true;
		}
		if (antialiasing && sheet->HasChanged(antialiasing))
		{
			Render_preferred_state.msaa_samples = (ubyte)MsaaIndexToSamples(*antialiasing);
			Render_preferred_state.antialised = Render_preferred_state.msaa_samples > 0;
			changed = true;
		}
		if (supersampling && sheet->HasChanged(supersampling))
		{
			Render_preferred_state.supersampling_factor = (ubyte)SupersamplingIndexToFactor(*supersampling);
			changed = true;
		}
		if (fov && sheet->HasChanged(fov))
		{
			Render_FOV_desired = fov[0] + D3_DEFAULT_FOV;
			Render_FOV = Render_FOV_desired;
			fov_changed = true;
		}

		if (changed)
			rend_SetPreferredState(&Render_preferred_state);

		bool display_applied = apply_display_settings(display_changed);
		if (changed || fov_changed || display_applied)
			sheet->UpdateChanges();
	}

	// sets the menu up.
	newuiSheet* setup(newuiMenu* menu)
	{
		sheet = NULL;
		parent_menu = NULL;
		filtering = NULL;
		mipmapping = NULL;
		per_pixel_lighting = NULL;
		bloom_enabled = NULL;
		vsync = NULL;
		fov = NULL;
		buffer = NULL;
		aspect_buffer = NULL;
		fullscreen = NULL;
		antialiasing = NULL;
		supersampling = NULL;
		hbao = NULL;
		backend = NULL;
		window_width = window_height = 0;
		window_aspect = CONFIG_ASPECT_16_9;
		display_width = display_height = 0;

		parent_menu = menu;
		sheet = menu->AddOption(IDV_VCONFIG, TXT_OPTVIDEO, NEWUIMENU_LARGE);

		sheet->NewGroup(NULL, 0, 0);
		init_display_bounds();
		window_width = Game_window_res_width;
		window_height = Game_window_res_height;
		window_aspect = Game_window_aspect;
		if (window_aspect < 0 || window_aspect >= CONFIG_ASPECT_COUNT)
			window_aspect = ConfigAspectFromSize(window_width, window_height);
		else if (abs(ConfigRoundAspectWidth(window_height, window_aspect) - window_width) > 2)
			window_aspect = ConfigAspectFromSize(window_width, window_height);
		normalize_display_choice();
		buffer = sheet->AddChangeableText(RESBUFFER_SIZE);
		sheet->NewGroup(NULL, 122, 0);
		aspect_buffer = sheet->AddChangeableText(ASPECTBUFFER_SIZE);
		sheet->NewGroup(NULL, 0, 12);
		update_display_text();
		sheet->AddLongButton("Resolution...", IDV_CHANGEWINDOW);
		sheet->AddLongButton("Aspect...", IDV_CHANGEASPECT);
		fullscreen = sheet->AddLongCheckBox("Fullscreen", Game_fullscreen);
		tSliderSettings settings = {};
		settings.min_val.f = D3_DEFAULT_FOV;
		settings.max_val.f = 90.f;
		settings.type = SLIDER_UNITS_FLOAT;
		fov = sheet->AddSlider("FOV", settings.max_val.f - settings.min_val.f, Render_FOV_desired - D3_DEFAULT_FOV, &settings);

		sheet->NewGroup("OpenGL profile", 0, 80);
		backend = sheet->AddFirstLongRadioButton("Compat (for NV)");
		sheet->AddLongRadioButton("Core (for AMD)");
		*backend = DesiredOpenGLProfile;

		// video settings
		sheet->NewGroup(TXT_TOGGLES, 0, 120);
		filtering = sheet->AddLongCheckBox(TXT_BILINEAR, (Render_preferred_state.filtering != 0));
		mipmapping = sheet->AddLongCheckBox(TXT_MIPMAPPING, (Render_preferred_state.mipping != 0));
		per_pixel_lighting = sheet->AddLongCheckBox("Per-pixel lighting",
			ConfigCanUsePerPixelLighting() && Render_preferred_state.per_pixel_lighting);
		bloom_enabled = sheet->AddLongCheckBox("Bloom", Render_preferred_state.bloom_enabled);

		sheet->NewGroup(TXT_MONITOR, 0, 188);
		vsync = sheet->AddLongCheckBox(TXT_CFG_VSYNCENABLED, (Render_preferred_state.vsync_on != 0));

		sheet->AddLongButton(TXT_AUTO_GAMMA, IDV_AUTOGAMMA);

		sheet->NewGroup("MSAA", 184, 0);
		int iTemp = MsaaSamplesToIndex(Render_preferred_state.msaa_samples);
		antialiasing = sheet->AddFirstRadioButton(TXT_OFF);
		sheet->AddRadioButton("2x");
		sheet->AddRadioButton("4x");
		sheet->AddRadioButton("8x");
		*antialiasing = iTemp;

		sheet->NewGroup("SSAA", 184, 94);
		supersampling = sheet->AddFirstRadioButton(TXT_OFF);
		sheet->AddRadioButton("2x");
		sheet->AddRadioButton("4x");
		*supersampling = SupersamplingFactorToIndex(Render_preferred_state.supersampling_factor);

		sheet->NewGroup("HBAO", 184, 162);
		hbao = sheet->AddFirstRadioButton(TXT_OFF);
		sheet->AddRadioButton(TXT_LOW);
		sheet->AddRadioButton(TXT_CFG_MEDIUM);
		sheet->AddRadioButton(TXT_CFG_HIGH);
		*hbao = ConfigCanUseHBAO() ?
			HBAOPresetToIndex(Render_preferred_state.hbao_enabled, Render_preferred_state.hbao_resolution) : 0;

		return sheet;
	};

	// retreive values from property sheet here.
	void finish()
	{
		if (filtering)
			Render_preferred_state.filtering = (*filtering) ? 1 : 0;
		if (mipmapping)
			Render_preferred_state.mipping = (*mipmapping) ? 1 : 0;
		if (per_pixel_lighting)
			Render_preferred_state.per_pixel_lighting = ConfigCanUsePerPixelLighting() && *per_pixel_lighting;
		if (bloom_enabled)
			Render_preferred_state.bloom_enabled = *bloom_enabled;
		if (hbao)
		{
			if (ConfigCanUseHBAO())
				ApplyHBAOPresetFromIndex(*hbao);
			else
				ApplyHBAOPresetFromIndex(0);
		}
		if (vsync)
			Render_preferred_state.vsync_on = (*vsync) ? 1 : 0;
		if (antialiasing)
		{
			Render_preferred_state.msaa_samples = (ubyte)MsaaIndexToSamples(*antialiasing);
			Render_preferred_state.antialised = Render_preferred_state.msaa_samples > 0;
		}
		if (supersampling)
			Render_preferred_state.supersampling_factor = (ubyte)SupersamplingIndexToFactor(*supersampling);

		Render_FOV_desired = fov[0] + D3_DEFAULT_FOV;
		if (Render_FOV != Render_FOV_desired)
			Render_FOV = Render_FOV_desired; //this may cause discontinuities if FOV is changed while zoomed. hmm.

		if (!apply_display_settings(true))
		{
			//Hopefully this doesn't do anything cursed..
			rend_SetPreferredState(&Render_preferred_state);
		}

		if (backend)
		{
			int olddesired = DesiredOpenGLProfile;
			DesiredOpenGLProfile = (opengl_profile)*backend;
			if (olddesired != DesiredOpenGLProfile)
				DesiredOpenGLProfileExplicit = true;
			if (olddesired != DesiredOpenGLProfile && DesiredOpenGLProfile != OpenGLProfile)
			{
				DoMessageBox(TXT_WARNING, "Changing the OpenGL profile will apply the next time you start Piccu Engine.", MSGBOX_OK);
			}
			if (DesiredOpenGLProfile != GLPROFILE_CORE)
				Render_preferred_state.per_pixel_lighting = false;
			if (DesiredOpenGLProfile != GLPROFILE_CORE)
				ApplyHBAOPresetFromIndex(0);
		}

		sheet = NULL;
	};

	int populate_resolution_list(newuiListBox* resolution_list, int* resolutions, int max_resolutions)
	{
		resolution_list->RemoveAll();
		int count = ConfigBuildVerticalResolutionList(resolutions, max_resolutions, display_width, display_height);
		char text[32];
		for (int i = 0; i < count; i++)
		{
			snprintf(text, sizeof(text), "%d", resolutions[i]);
			resolution_list->AddItem(text);
			if (resolutions[i] == window_height)
				resolution_list->SetCurrentIndex(i);
		}
		return count;
	}

	int populate_aspect_list(newuiListBox* aspect_list, int* aspects, int max_aspects)
	{
		aspect_list->RemoveAll();
		int count = 0;
		for (int i = 0; i < CONFIG_ASPECT_COUNT && count < max_aspects; i++)
		{
			if (!ConfigAspectFitsDisplay(i, window_height, display_width, display_height))
				continue;

			aspects[count] = i;
			aspect_list->AddItem(Config_aspect_ratios[i].name);
			if (i == window_aspect)
				aspect_list->SetCurrentIndex(count);
			count++;
		}
		return count;
	}

	// process
	void process(int res)
	{
		apply_live_settings();

		switch (res)
		{
		case IDV_CHANGEWINDOW:
		{
			newuiTiledWindow menu;
			newuiSheet* select_sheet;
			newuiListBox* resolution_list;
			int resolutions[32];
			int resolution_count;
			bool display_settings_changed = false;

			menu.Create("Resolution", 0, 0, 300, 384);
			select_sheet = menu.GetSheet();
			select_sheet->NewGroup(NULL, 10, 0);
			resolution_list = select_sheet->AddListBox(208, 256, UID_RESOLUTION, UILB_NOSORT);
			select_sheet->NewGroup(NULL, 100, 280, NEWUI_ALIGN_HORIZ);
			select_sheet->AddButton(TXT_OK, UID_OK);
			select_sheet->AddButton(TXT_CANCEL, UID_CANCEL);

			resolution_count = populate_resolution_list(resolution_list, resolutions, 32);

			menu.Open();

			int res;
			do
			{
				res = menu.DoUI();
			} while (res != UID_OK && res != UID_CANCEL);

			if (res == UID_OK)
			{
				int newindex = resolution_list->GetCurrentIndex();
				if (newindex >= 0 && newindex < resolution_count)
				{
					window_height = resolutions[newindex];
					window_aspect = ConfigBestAspectForDisplay(window_aspect, window_height, display_width, display_height);
					window_width = ConfigRoundAspectWidth(window_height, window_aspect);
					update_display_text();
					display_settings_changed = true;
				}
			}

			menu.Close();
			menu.Destroy();
			if (display_settings_changed)
				apply_display_settings(true);

		}
		break;
		case IDV_CHANGEASPECT:
		{
			newuiTiledWindow menu;
			newuiSheet* select_sheet;
			newuiListBox* aspect_list;
			int aspects[CONFIG_ASPECT_COUNT];
			int aspect_count;
			bool display_settings_changed = false;

			menu.Create("Aspect", 0, 0, 260, 256);
			select_sheet = menu.GetSheet();
			select_sheet->NewGroup(NULL, 10, 0);
			aspect_list = select_sheet->AddListBox(168, 128, UID_ASPECT, UILB_NOSORT);
			select_sheet->NewGroup(NULL, 80, 160, NEWUI_ALIGN_HORIZ);
			select_sheet->AddButton(TXT_OK, UID_OK);
			select_sheet->AddButton(TXT_CANCEL, UID_CANCEL);

			aspect_count = populate_aspect_list(aspect_list, aspects, CONFIG_ASPECT_COUNT);

			menu.Open();

			int res;
			do
			{
				res = menu.DoUI();
			} while (res != UID_OK && res != UID_CANCEL);

			if (res == UID_OK)
			{
				int newindex = aspect_list->GetCurrentIndex();
				if (newindex >= 0 && newindex < aspect_count)
				{
					window_aspect = aspects[newindex];
					window_width = ConfigRoundAspectWidth(window_height, window_aspect);
					update_display_text();
					display_settings_changed = true;
				}
			}

			menu.Close();
			menu.Destroy();
			if (display_settings_changed)
				apply_display_settings(true);
		}
		break;
		case IDV_AUTOGAMMA:
			config_gamma();
			break;
		}
	};
};

//////////////////////////////////////////////////////////////////
// SOUND MENU
//
struct sound_menu
{
	newuiSheet* sheet;
	newuiMenu* parent_menu;
	int ls_sound_id, sound_id;

	short* fxvolume, * musicvolume;			// volume sliders
	short* fxquantity;							// sound fx quantity limit
	int* fxquality;								// sfx quality low/high

	short old_fxquantity;

	short* doppler_level;
	short* reverb_level;
	bool* hrtf;

	short* add_float_slider(newuiSheet* target_sheet, const char* title, float value, float min_value, float max_value)
	{
		tSliderSettings slider_set;
		slider_set.type = SLIDER_UNITS_FLOAT;
		slider_set.min_val.f = min_value;
		slider_set.max_val.f = max_value;
		return target_sheet->AddSlider(title, 100, CALC_SLIDER_POS_FLOAT(value, &slider_set, 100), &slider_set);
	}

	float slider_float_value(short* slider, float min_value, float max_value)
	{
		return CALC_SLIDER_FLOAT_VALUE(*slider, min_value, max_value, 100);
	}

	void apply_effect_levels(int flags)
	{
		Sound_doppler_level = slider_float_value(doppler_level, 0.0f, 1.0f);
		Sound_reverb_level = slider_float_value(reverb_level, 0.0f, 1.0f);
		Sound_doppler = Sound_doppler_level > 0.0f;
		Sound_reverb = Sound_reverb_level > 0.0f;
		Sound_hrtf = *hrtf;
		Sound_system.UpdateEnvironmentToggles(flags);
	}

	// sets the menu up.
	newuiSheet* setup(newuiMenu* menu)
	{
		tSliderSettings slider_set;

		sheet = menu->AddOption(IDV_SCONFIG, TXT_OPTSOUND, NEWUIMENU_MEDIUM);
		parent_menu = menu;
		ls_sound_id = -1;
		sound_id = FindSoundName("Menu Slider Click");
		ASSERT(sound_id != -1);	//DAJ -1FIX

		// volume sliders
		sheet->NewGroup(NULL, 0, 0);

		slider_set.type = SLIDER_UNITS_PERCENT;
		fxvolume = sheet->AddSlider(TXT_SOUNDVOL, 10, (short)(Sound_system.GetMasterVolume() * 10), &slider_set);

		slider_set.type = SLIDER_UNITS_PERCENT;
		musicvolume = sheet->AddSlider(TXT_SNDMUSVOL, 10, (short)(D3MusicGetVolume() * 10), &slider_set);

		slider_set.min_val.i = MIN_SOUNDS_MIXED;
		slider_set.max_val.i = MAX_SOUNDS_MIXED;
		slider_set.type = SLIDER_UNITS_INT;
		fxquantity = sheet->AddSlider(TXT_SNDCFG_SFXQUANTITY, (slider_set.max_val.i - slider_set.min_val.i),
			Sound_system.GetLLSoundQuantity() - MIN_SOUNDS_MIXED, &slider_set);
		old_fxquantity = (Sound_system.GetLLSoundQuantity() - MIN_SOUNDS_MIXED);

		tSliderSettings effect_slider_set;
		effect_slider_set.type = SLIDER_UNITS_FLOAT;
		effect_slider_set.min_val.f = 0.0f;
		effect_slider_set.max_val.f = 1.0f;
		sheet->NewGroup(NULL, 0, 152);
		hrtf = sheet->AddLongCheckBox("Headphone HRTF", Sound_hrtf);
		doppler_level = sheet->AddSlider("Doppler", 100, CALC_SLIDER_POS_FLOAT(Sound_doppler_level, &effect_slider_set, 100), &effect_slider_set);
		reverb_level = sheet->AddSlider("Reverb", 100, CALC_SLIDER_POS_FLOAT(Sound_reverb_level, &effect_slider_set, 100), &effect_slider_set);

		// sound fx quality radio list.
		if (GetFunctionMode() != GAME_MODE && GetFunctionMode() != EDITOR_GAME_MODE)
		{
			sheet->NewGroup("Quality", 184, 0);

			fxquality = sheet->AddFirstRadioButton(TXT_SNDNORMAL);
			sheet->AddRadioButton(TXT_SNDHIGH);
			*fxquality = Sound_system.GetSoundQuality() == SQT_HIGH ? 1 : 0;
		}
		else
		{
			fxquality = NULL;
		}

		return sheet;
	};

	// retreive values from property sheet here.
	void finish()
	{
		Sound_system.SetMasterVolume((*fxvolume) / 10.0f);
		D3MusicSetVolume((*musicvolume) / 10.0f);

		if (fxquantity)
		{
			mprintf((1, "oldquant %d newquant %d\n", old_fxquantity, *fxquantity));
			if (old_fxquantity != (*fxquantity))
			{
				Sound_system.SetLLSoundQuantity((*fxquantity) + MIN_SOUNDS_MIXED);
			}
		}

		if (fxquality)
		{
			Sound_system.SetSoundQuality((*fxquality == 1) ? SQT_HIGH : SQT_NORMAL);
		}

		if (doppler_level && reverb_level && hrtf)
		{
			apply_effect_levels(ENV3DVALF_DOPPLER | ENV3dVALF_REVERBS | ENV3DVALF_HRTF);
		}
	};

	// process output and do stuff accordintly
	void process(int res)
	{
		if (parent_menu->GetCurrentOption() == IDV_SCONFIG && sheet->HasChanged(fxvolume))
		{
			Sound_system.SetMasterVolume((*fxvolume) / 10.0f);
			Sound_system.BeginSoundFrame(false);
			Sound_system.StopSoundImmediate(ls_sound_id);
			ls_sound_id = Sound_system.Play2dSound(sound_id);
			Sound_system.EndSoundFrame();
		}
		if (parent_menu->GetCurrentOption() == IDV_SCONFIG && sheet->HasChanged(musicvolume))
		{
			D3MusicSetVolume((*musicvolume) / 10.0f);
		}
		if (parent_menu->GetCurrentOption() == IDV_SCONFIG && fxquantity && sheet->HasChanged(fxquantity))
		{
			Sound_system.SetLLSoundQuantity((*fxquantity) + MIN_SOUNDS_MIXED);
			old_fxquantity = *fxquantity;
		}
		if (parent_menu->GetCurrentOption() == IDV_SCONFIG && fxquality && sheet->HasChanged(fxquality))
		{
			Sound_system.SetSoundQuality((*fxquality == 1) ? SQT_HIGH : SQT_NORMAL);
		}
		if (parent_menu->GetCurrentOption() == IDV_SCONFIG)
		{
			int effect_flags = 0;
			if (sheet->HasChanged(doppler_level))
				effect_flags |= ENV3DVALF_DOPPLER;
			if (sheet->HasChanged(reverb_level))
				effect_flags |= ENV3dVALF_REVERBS;
			if (sheet->HasChanged(hrtf))
				effect_flags |= ENV3DVALF_HRTF;
			if (effect_flags)
				apply_effect_levels(effect_flags);
		}
	};
};

//////////////////////////////////////////////////////////////////
// GENERAL SETTINGS (TOGGLES) MENU
//

#define UID_SHORTCUT_JOYSETTINGS			0x1000
#define UID_SHORTCUT_KEYSETTINGS			0x1001
#define UID_SHORTCUT_FORCEFEED				0x1002

struct toggles_menu
{
	newuiSheet* sheet;
	newuiMenu* parent_menu;

	int* terrain_autolevel;				// auto leveling radios
	int* mine_autolevel;
	int* missile_view;					// missile view radio
	bool* joy_enabled, * mse_enabled;
	bool* reticle_toggle, * guided_toggle;
	bool* shipsnd_toggle;

	// sets the menu up.
	newuiSheet* setup(newuiMenu* menu)
	{
		sheet = menu->AddOption(IDV_GCONFIG, TXT_OPTGENERAL, NEWUIMENU_MEDIUM);
		parent_menu = menu;
		bool ff_found = false;

		sheet->NewGroup(TXT_TERRAUTOLEV, 0, 0);
		terrain_autolevel = sheet->AddFirstRadioButton(TXT_NONE);
		sheet->AddRadioButton(TXT_CFG_MEDIUM);
		sheet->AddRadioButton(TXT_CFG_HIGH);
		*terrain_autolevel = Default_player_terrain_leveling;

		sheet->NewGroup(TXT_MINEAUTOLEV, 0, 70);
		mine_autolevel = sheet->AddFirstRadioButton(TXT_NONE);
		sheet->AddRadioButton(TXT_CFG_MEDIUM);
		sheet->AddRadioButton(TXT_CFG_HIGH);
		*mine_autolevel = Default_player_room_leveling;

		sheet->NewGroup(TXT_MISSILEVIEW, 0, 140);
		missile_view = sheet->AddFirstRadioButton(TXT_NONE);
		sheet->AddRadioButton(TXT_LEFT);
		sheet->AddRadioButton(TXT_RIGHT);

		*missile_view = (Missile_camera_window == SVW_LEFT) ? 1 : (Missile_camera_window == SVW_RIGHT) ? 2 : 0;

		sheet->NewGroup(TXT_CONTROL_TOGGLES, 110, 0);
		joy_enabled = sheet->AddLongCheckBox(TXT_JOYENABLED);
		mse_enabled = sheet->AddLongCheckBox(TXT_CFG_MOUSEENABLED);
		*joy_enabled = CHECK_FLAG(Current_pilot.read_controller, READF_JOY) ? true : false;
		*mse_enabled = CHECK_FLAG(Current_pilot.read_controller, READF_MOUSE) ? true : false;

		sheet->NewGroup(TXT_TOGGLES, 110, 70);
		reticle_toggle = sheet->AddLongCheckBox(TXT_TOG_SHOWRETICLE);
		guided_toggle = sheet->AddLongCheckBox(TXT_TOG_GUIDEDMISSILE);
		shipsnd_toggle = sheet->AddLongCheckBox(TXT_TOG_SHIPSOUNDS);
		*reticle_toggle = Game_toggles.show_reticle;
		*guided_toggle = Game_toggles.guided_mainview;
		*shipsnd_toggle = Game_toggles.ship_noises;

		ddio_ff_GetInfo(&ff_found, NULL);
		sheet->NewGroup(TXT_ADJUSTCONTROLSETTINGS, 110, 140);
		sheet->AddLongButton(TXT_JOYSENSBTN, UID_SHORTCUT_JOYSETTINGS);
		sheet->AddLongButton(TXT_KEYRAMPING, UID_SHORTCUT_KEYSETTINGS);

		if (ff_found)
		{
			sheet->AddLongButton(TXT_CFG_CONFIGFORCEFEEDBACK, UID_SHORTCUT_FORCEFEED);
		}
		return sheet;
	};

	// retreive values from property sheet here.
	void finish()
	{
		int iTemp;

		Default_player_terrain_leveling = *terrain_autolevel;
		Default_player_room_leveling = *mine_autolevel;

		iTemp = (*missile_view);
		Missile_camera_window = (iTemp == 1) ? SVW_LEFT : (iTemp == 2) ? SVW_RIGHT : -1;
#ifndef MACINTOSH
		Current_pilot.read_controller = (*joy_enabled) ? READF_JOY : 0;
		Current_pilot.read_controller |= (*mse_enabled) ? READF_MOUSE : 0;
#endif
		Game_toggles.show_reticle = (*reticle_toggle);
		Game_toggles.guided_mainview = (*guided_toggle);
		Game_toggles.ship_noises = (*shipsnd_toggle);
	};

	// process
	void process(int res)
	{
		switch (res)
		{
		case UID_SHORTCUT_JOYSETTINGS:
		case UID_SHORTCUT_FORCEFEED:
			LoadControlConfig();
			joystick_settings_dialog();
			SaveControlConfig();
			break;
		case UID_SHORTCUT_KEYSETTINGS:
			LoadControlConfig();
			key_settings_dialog();
			SaveControlConfig();
			break;
		}
	};
};

//////////////////////////////////////////////////////////////////
//  HUD CONFIG MENU
//
struct hud_menu
{
	newuiSheet* sheet;
	newuiMenu* parent_menu;

	int* ship_status;
	int* shield_energy;
	int* weapon_loads;
	int* afterburner;
	int* inventory;
	int* warnings;
	int* countermeasures;
	//	int *goals;

	int add_hud_option(const char* title, int** ptr, int y, int sel, bool graphical)
	{
		const int y_line = 29;

		sheet->NewGroup(title, 0, y, NEWUI_ALIGN_HORIZ);
		*ptr = sheet->AddFirstRadioButton(TXT_OFF);
		if (graphical)
		{
			sheet->AddRadioButton(TXT_TEXT);
			sheet->AddRadioButton(TXT_GRAPHICAL);
		}
		else
		{
			sheet->AddRadioButton(TXT_ON);
		}
		*(*ptr) = sel;
		return y + y_line;
	};

	// sets the menu up.
	newuiSheet* setup(newuiMenu* menu)
	{
		int y, sel;
		ushort stat, grstat;

		sheet = menu->AddOption(IDV_HCONFIG, TXT_OPTHUD, NEWUIMENU_MEDIUM);
		parent_menu = menu;

		Current_pilot.get_hud_data(NULL, &stat, &grstat);

		sel = (stat & STAT_SHIP) ? 1 : (grstat & STAT_SHIP) ? 2 : 0;
		y = add_hud_option(TXT_HUDSHIPSTATUS, &ship_status, 0, sel, true);

		sel = (stat & (STAT_SHIELDS + STAT_ENERGY)) ? 1 : (grstat & (STAT_SHIELDS + STAT_ENERGY)) ? 2 : 0;
		y = add_hud_option(TXT_HUDSHIELDENERGY, &shield_energy, y, sel, true);

		sel = (stat & (STAT_PRIMARYLOAD + STAT_SECONDARYLOAD)) ? 1 : (grstat & (STAT_PRIMARYLOAD + STAT_SECONDARYLOAD)) ? 2 : 0;
		y = add_hud_option(TXT_HUDWEAPONS, &weapon_loads, y, sel, true);

		sel = (stat & STAT_AFTERBURN) ? 1 : (grstat & STAT_AFTERBURN) ? 2 : 0;
		y = add_hud_option(TXT_HUDAFTERBURN, &afterburner, y, sel, true);

		sel = (stat & STAT_WARNING) ? 1 : (grstat & STAT_WARNING) ? 2 : 0;
		y = add_hud_option(TXT_HUDWARNINGS, &warnings, y, sel, true);

		sel = (stat & STAT_CNTRMEASURE) ? 1 : (grstat & STAT_CNTRMEASURE) ? 2 : 0;
		y = add_hud_option(TXT_HUDCNTRMEASURE, &countermeasures, y, sel, true);

		sel = (stat & STAT_INVENTORY) ? 1 : 0;
		y = add_hud_option(TXT_HUDINVENTORY, &inventory, y, sel, false);

		return sheet;
	};

	// retreive values from property sheet here.
	void finish()
	{
		int sel;
		ushort hud_new_stat = STAT_MESSAGES + STAT_CUSTOM;
		ushort hud_new_grstat = 0;

		sel = *ship_status;
		if (sel == 1) hud_new_stat |= STAT_SHIP;
		else if (sel == 2) hud_new_grstat |= STAT_SHIP;
		sel = *shield_energy;
		if (sel == 1) hud_new_stat |= (STAT_SHIELDS + STAT_ENERGY);
		else if (sel == 2) hud_new_grstat |= (STAT_SHIELDS + STAT_ENERGY);
		sel = *weapon_loads;
		if (sel == 1) hud_new_stat |= (STAT_PRIMARYLOAD + STAT_SECONDARYLOAD);
		else if (sel == 2) hud_new_grstat |= (STAT_PRIMARYLOAD + STAT_SECONDARYLOAD);
		sel = *afterburner;
		if (sel == 1) hud_new_stat |= STAT_AFTERBURN;
		else if (sel == 2) hud_new_grstat |= STAT_AFTERBURN;
		sel = *warnings;
		if (sel == 1) hud_new_stat |= STAT_WARNING;
		else if (sel == 2) hud_new_grstat |= STAT_WARNING;
		sel = *countermeasures;
		if (sel == 1) hud_new_stat |= STAT_CNTRMEASURE;
		else if (sel == 2) hud_new_grstat |= STAT_CNTRMEASURE;
		//		sel = *goals;
		//		if (sel==1) hud_new_stat |= STAT_GOALS;
		sel = *inventory;
		if (sel == 1) hud_new_stat |= STAT_INVENTORY;

		Current_pilot.set_hud_data(NULL, &hud_new_stat, &hud_new_grstat);

		// modify current hud stats if in game.
		if ((GetFunctionMode() == EDITOR_GAME_MODE || GetFunctionMode() == GAME_MODE) && GetHUDMode() == HUD_FULLSCREEN)
		{
			SetHUDState(hud_new_stat | STAT_SCORE | (Hud_stat_mask & STAT_FPS), hud_new_grstat);
		}
	};

	void process(int res)
	{
	};
};

#ifdef MACINTOSH
#pragma mark details --
#endif


//////////////////////////////////////////////////////////////////
// DETAILS MENU
//
struct details_menu
{
	newuiSheet* sheet;
	newuiMenu* parent_menu;

	int* detail_level;									// detail level radio
	int* objcomp;											// object complexity radio
	bool* specmap, * headlight, * mirror,				// check boxes
		* dynamic, * fog, * coronas, * procedurals,
		* powerup_halo, * scorches, * weapon_coronas;
	short* pixel_err,										// 0-27 (1-28)
		* rend_dist;											// 0-120 (80-200)

	int* texture_quality;

	// sets the menu up.
	newuiSheet* setup(newuiMenu* menu)
	{
		int iTemp;
		sheet = menu->AddOption(IDV_DCONFIG, TXT_OPTDETAIL, NEWUIMENU_MEDIUM);
		parent_menu = menu;
		const bool show_legacy_terrain_controls = ConfigShowsLegacyTerrainControls();

		// detail level radio
		Database->read_int("PredefDetailSetting", &Default_detail_level);
		iTemp = Default_detail_level;
		sheet->NewGroup(TXT_CFG_PRESETDETAILS, 0, 0);
		detail_level = sheet->AddFirstRadioButton(TXT_LOW);
		sheet->AddRadioButton(TXT_CFG_MEDIUM);
		sheet->AddRadioButton(TXT_CFG_HIGH);
		sheet->AddRadioButton(TXT_CFG_VERYHIGH);
		sheet->AddRadioButton(TXT_CFG_CUSTOM);
		*detail_level = iTemp;

		// toggles
		sheet->NewGroup(TXT_TOGGLES, 0, 87);
		specmap = sheet->AddLongCheckBox(TXT_SPECMAPPING, Detail_settings.Specular_lighting);
		headlight = sheet->AddLongCheckBox(TXT_FASTHEADLIGHT, Detail_settings.Fast_headlight_on);
		mirror = sheet->AddLongCheckBox(TXT_MIRRORSURF, Detail_settings.Mirrored_surfaces);
		dynamic = sheet->AddLongCheckBox(TXT_DYNLIGHTING, Detail_settings.Dynamic_lighting);
		fog = sheet->AddLongCheckBox(TXT_CFG_ENABLEFOG, Detail_settings.Fog_enabled);
		coronas = sheet->AddLongCheckBox(TXT_CFG_ENABLELIGHTCORONA, Detail_settings.Coronas_enabled);
		procedurals = sheet->AddLongCheckBox(TXT_CFG_PROCEDURALS, Detail_settings.Procedurals_enabled);
		powerup_halo = sheet->AddLongCheckBox(TXT_CFG_POWERUPHALOS, Detail_settings.Powerup_halos);
		scorches = sheet->AddLongCheckBox(TXT_CFG_SCORCHMARKS, Detail_settings.Scorches_enabled);
		weapon_coronas = sheet->AddLongCheckBox(TXT_CFG_WEAPONEFFECTS, Detail_settings.Weapon_coronas_enabled);

		// sliders
		if (show_legacy_terrain_controls)
		{
			tSliderSettings slider_set;
			sheet->NewGroup(TXT_GEOMETRY, 90, 0);
			iTemp = MAXIMUM_TERRAIN_DETAIL - Detail_settings.Pixel_error - MINIMUM_TERRAIN_DETAIL;
			if (iTemp < 0) iTemp = 0;
			slider_set.min_val.i = MINIMUM_TERRAIN_DETAIL;
			slider_set.max_val.i = MAXIMUM_TERRAIN_DETAIL;
			slider_set.type = SLIDER_UNITS_INT;
			pixel_err = sheet->AddSlider(TXT_TERRDETAIL, MAXIMUM_TERRAIN_DETAIL - MINIMUM_TERRAIN_DETAIL, iTemp, &slider_set);

			slider_set.min_val.i = MINIMUM_RENDER_DIST / 2;
			slider_set.max_val.i = MAXIMUM_RENDER_DIST / 2;
			slider_set.type = SLIDER_UNITS_INT;
			iTemp = (int)(Detail_settings.Terrain_render_distance / ((float)TERRAIN_SIZE)) - MINIMUM_RENDER_DIST;
			if (iTemp < 0) iTemp = 0;
			rend_dist = sheet->AddSlider(TXT_RENDDIST, (MAXIMUM_RENDER_DIST - MINIMUM_RENDER_DIST) / 2, iTemp / 2, &slider_set);
		}
		else
		{
			pixel_err = NULL;
			rend_dist = NULL;
		}

		// object complexity radio
		sheet->NewGroup(TXT_CFG_OBJECTCOMPLEXITY, show_legacy_terrain_controls ? 174 : 90, show_legacy_terrain_controls ? 87 : 0);
		objcomp = sheet->AddFirstRadioButton(TXT_LOW);
		sheet->AddRadioButton(TXT_CFG_MEDIUM);
		sheet->AddRadioButton(TXT_CFG_HIGH);
		sheet->AddRadioButton(TXT_CFG_MAX);
		*objcomp = Detail_settings.Object_complexity;

		return sheet;
	};

	// retreive values from property sheet here.
	void finish()
	{
		Detail_settings.Coronas_enabled = *coronas;
		Detail_settings.Dynamic_lighting = *dynamic;
		Detail_settings.Fast_headlight_on = *headlight;
		Detail_settings.Fog_enabled = *fog;
		Detail_settings.Mirrored_surfaces = *mirror;
		Detail_settings.Object_complexity = *objcomp;
		if (pixel_err)
			Detail_settings.Pixel_error = MAXIMUM_TERRAIN_DETAIL - ((*pixel_err) + MINIMUM_TERRAIN_DETAIL);
		Detail_settings.Powerup_halos = *powerup_halo;
		Detail_settings.Procedurals_enabled = *procedurals;
		Detail_settings.Scorches_enabled = *scorches;
		Detail_settings.Specular_lighting = *specmap;
		if (rend_dist)
			Detail_settings.Terrain_render_distance = (((*rend_dist) * 2) + MINIMUM_RENDER_DIST) * ((float)TERRAIN_SIZE);
		Detail_settings.Weapon_coronas_enabled = *weapon_coronas;

		Default_detail_level = *detail_level;
		Database->write("PredefDetailSetting", Default_detail_level);

		sheet = NULL;
	};

	// process output and do stuff accordintly
	void process(int res)
	{
		// check here if the detail level currently set should be custom
		bool changed = sheet->HasChanged(specmap) ||
			sheet->HasChanged(headlight) ||
			sheet->HasChanged(mirror) ||
			sheet->HasChanged(dynamic) ||
			sheet->HasChanged(fog) ||
			sheet->HasChanged(coronas) ||
			sheet->HasChanged(procedurals) ||
			sheet->HasChanged(powerup_halo) ||
			sheet->HasChanged(scorches) ||
			sheet->HasChanged(weapon_coronas) ||
			sheet->HasChanged(objcomp) ||
			(pixel_err && sheet->HasChanged(pixel_err)) ||
			(rend_dist && sheet->HasChanged(rend_dist));

		if (changed)
		{
			// enable custom radio button
			*detail_level = DETAIL_LEVEL_CUSTOM;
		}
		else
		{
			// check if any preset detail has been selected.
			if (sheet->HasChanged(detail_level))
			{
				set_preset_details(*detail_level);
			}
		}

	};

	//	sets detail presets
	void set_preset_details(int setting);
};


//	sets detail presets
void details_menu::set_preset_details(int setting)
{
	tDetailSettings ds;

	switch (setting)
	{
	case DETAIL_LEVEL_LOW:			memcpy(&ds, &DetailPresetLow, sizeof(tDetailSettings)); break;
	case DETAIL_LEVEL_MED:			memcpy(&ds, &DetailPresetMed, sizeof(tDetailSettings)); break;
	case DETAIL_LEVEL_HIGH:			memcpy(&ds, &DetailPresetHigh, sizeof(tDetailSettings)); break;
	case DETAIL_LEVEL_VERY_HIGH:	memcpy(&ds, &DetailPresetVHi, sizeof(tDetailSettings)); break;
	default:
		return;
	};

	//now go through all the config items and set to the new values
	if (pixel_err)
	{
		int iTemp = MAXIMUM_TERRAIN_DETAIL - ds.Pixel_error - MINIMUM_TERRAIN_DETAIL;
		if (iTemp < 0)
			iTemp = 0;
		*pixel_err = (short)(iTemp);
	}
	else
	{
		Detail_settings.Pixel_error = ds.Pixel_error;
	}
	if (rend_dist)
	{
		int iTemp = (int)((ds.Terrain_render_distance / ((float)TERRAIN_SIZE)) - MINIMUM_RENDER_DIST);
		if (iTemp < 0) iTemp = 0;
		iTemp = iTemp / 2;
		*rend_dist = (short)(iTemp);
	}
	else
	{
		Detail_settings.Terrain_render_distance = ds.Terrain_render_distance;
	}
	*objcomp = ds.Object_complexity;
	*specmap = ds.Specular_lighting;
	*headlight = ds.Fast_headlight_on;
	*mirror = ds.Mirrored_surfaces;
	*dynamic = ds.Dynamic_lighting;
	*fog = ds.Fog_enabled;
	*coronas = ds.Coronas_enabled;
	*procedurals = ds.Procedurals_enabled;
	*powerup_halo = ds.Powerup_halos;
	*scorches = ds.Scorches_enabled;
	*weapon_coronas = ds.Weapon_coronas_enabled;
}

//////////////////////////////////////////////////////////////////
//	new Options menu
//

// externed from init.cpp
extern void SaveGameSettings();

void OptionsMenu()
{
	newuiMenu menu;
	video_menu video;
	details_menu details;
	sound_menu sound;
	toggles_menu toggles;
	hud_menu hud;

	int res = -1, state = 0;								// state = 0, options menu, 1 = controller config, 2 = quitting.

	while (state != 2)
	{
		if (state == 1)
		{
			// enter controller config menu
			mprintf((0, "CONTROLLER CONFIG MENU HERE!\n"));
			CtlConfig(CTLCONFIG_KEYBOARD);
			state = 0;									// goto options menu.
		}
		else
		{
			// open menu

			DoWaitMessage(true);

			menu.Create();
			menu.Open();

			video.setup(&menu);						// setup video menu IDV_VCONFIG
			details.setup(&menu);					// setup details menu.  IDV_DCONFIG
			sound.setup(&menu);						// setup sound menu. IDV_SCONFIG
			toggles.setup(&menu);				 	// setup general menu. IDV_GCONFIG
			hud.setup(&menu);							// setup hud menu. IDV_HCONFIG

			menu.AddSimpleOption(IDV_CCONFIG, TXT_OPTCONFIG);
			menu.AddSimpleOption(UID_CANCEL, TXT_DONE);

			menu.SetCurrentOption(IDV_VCONFIG);

			DoWaitMessage(false);
			ui_SuppressLeftMouseUntilRelease();

			// run menu
			do
			{
				res = menu.DoUI();

				// immediate checking of any option ids.
				if (res == UID_CANCEL || res == NEWUIRES_FORCEQUIT) {
					state = 2;	break;				// next pass will quit
				}
				else if (res == IDV_CCONFIG) {
					state = 1;	break;				// next pass will enter controller menu
				}

				// handle any processing needed by current option.
				switch (menu.GetCurrentOption())
				{
				case IDV_DCONFIG:
					details.process(res);
					break;
				case IDV_SCONFIG:
					sound.process(res);
					break;
				case IDV_VCONFIG:
					video.process(res);
					break;
				case IDV_GCONFIG:
					toggles.process(res);
					break;
				case IDV_HCONFIG:
					hud.process(res);
					break;
				}
			} while (1);

			// get settings
			hud.finish();
			toggles.finish();
			sound.finish();
			details.finish();
			video.finish();

			// write them out and close.
			PltWriteFile(&Current_pilot);

			menu.Close();
			menu.Destroy();
		}
	}

	SaveGameSettings();
}

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

//	D3 Cockpit system
//		needs a model with textures.
//		major functions:
//			activate cockpit
//			render cockpit
//			deactivate cockpit.
#include "cockpit.h"
#include "game.h"
#include "polymodel.h"
#include "hud.h"
#include "gauges.h"
#include "ship.h"
#include "player.h"
#include "room.h"
#include "hlsoundlib.h"
#include "soundload.h"
#include "sounds.h"
#include "config.h"
#include "gametexture.h"
#include "renderer.h"
#include <string.h>
#include <math.h>
#include <algorithm>

#define COCKPIT_ANIM_TIME				2.0f
#define COCKPIT_DORMANT_FRAME			0.0
#define COCKPIT_START_FRAME			3.0
#define COCKPIT_MIDWAY_FRAME			7.0
#define COCKPIT_COMPLETE_FRAME		((float)Cockpit_info.model->frame_max)
//	cockpit FX modifiers
#define MAX_BUFFET_STRENGTH 0.75f
#define BUFFET_PERIOD 0.25f
#define COCKPIT_SHIFT_DELTA 0.02f
#define COCKPIT_DISPLAY_CENTER_THRESHOLD 0.25f
#define COCKPIT_DISPLAY_WIDTH_THRESHOLD 0.20f
struct tCockpitCfgInfo
{
	char modelname[PSFILENAME_LEN];
	char shieldrings[NUM_SHIELD_GAUGE_FRAMES][PSFILENAME_LEN];
	char shipimg[PSFILENAME_LEN];
	char burnimg[PSFILENAME_LEN];
	char energyimg[PSFILENAME_LEN];
	char invpulseimg[PSFILENAME_LEN];
};
struct tCockpitInfo
{
	int state;									// current state of cockpit on screen.
	int ship_index;							// index into ships page.
	int model_num;								// this should be retreived from the ship info.
	int snd_open, snd_close;				// sound open and close.
	float frame_time;							// current time of animation.
	float next_keyframe, this_keyframe;	// current animation of cockpit.
	poly_model* model;						// model of cockpit.
	unsigned nonlayered_mask;				// non layered submodel mask
	unsigned layered_mask;					// layered submodel mask
	unsigned post_post_mask;					// transparent canopy/glass rendered after post
	bool animating;							// is cockpit moving?
	bool resized;								// if cockpit is being resized....
	vector buffet_vec;						// buffet direction
	float buffet_amp;							// used to calculate real magnitude
	float buffet_wave;						// current sin angle of buffet 
	float buffet_time;						// current position in buffet wave along time axis.
	vector display_adjust_unit[MAX_SUBOBJECTS];	// cockpit-space offset per 1.0 display spread unit.
	vector display_adjust_scaled[MAX_SUBOBJECTS];
	int display_adjust_count;
	bool display_adjust_available;

	matrix orient;								// orientation of cockpit
};

static tCockpitInfo Cockpit_info;
struct tCockpitPostPostSnapshot
{
	bool valid;
	int frame_count;
	vector view_pos;
	matrix view_tmat;
	vector light_vec;
	float light_scalar_r;
	float light_scalar_g;
	float light_scalar_b;
	float normalized_time[MAX_SUBOBJECTS];
	bool display_adjust_active;
	float display_spread;
};
static tCockpitPostPostSnapshot Cockpit_post_post_snapshot = {};
float KeyframeAnimateCockpit();
//	loads cockpit. model_name = NULL, then will not load in model name.
void LoadCockpitInfo(const char* ckt_file, tCockpitCfgInfo* info);
static void BuildCockpitDisplayAdjustments();
static void ApplyCockpitDisplayAdjustments(float display_spread);
static float GetCockpitDisplaySpread();
static vector CockpitRootVectorToSubmodelParent(poly_model* pm, int submodel, vector root_vec);

static float CockpitSubmodelCenterX(poly_model* pm, int submodel)
{
	float x = pm->submodel[submodel].geometric_center.x;
	for (int mn = submodel; mn >= 0; mn = pm->submodel[mn].parent)
		x += pm->submodel[mn].offset.x;
	return x;
}

static int CockpitSideFromCenter(float x, float threshold)
{
	if (x > threshold)
		return 1;
	if (x < -threshold)
		return -1;
	return 0;
}

static bool CockpitSubmodelUsesTransparentMaterial(poly_model* pm, int submodel)
{
	bsp_info* sm = &pm->submodel[submodel];

	if (sm->alpha)
	{
		for (int i = 0; i < sm->nverts; i++)
		{
			if (sm->alpha[i] < 0.99f)
				return true;
		}
	}

	for (int i = 0; i < sm->num_faces; i++)
	{
		int texnum = sm->faces[i].texnum;
		if (texnum < 0 || texnum >= pm->n_textures)
			continue;

		int texture_index = pm->textures[texnum];
		if (texture_index < 0 || texture_index >= MAX_TEXTURES)
			continue;

		if ((GameTextures[texture_index].flags & (TF_ALPHA | TF_BREAKABLE)) || GameTextures[texture_index].alpha < 0.99f)
			return true;
	}

	return false;
}

static bool CockpitSubmodelShouldRenderPostPost(poly_model* pm, int submodel)
{
	if (pm->submodel[submodel].flags & (SOF_VIEWER | SOF_MONITOR_MASK))
		return false;
	if (pm->submodel[submodel].flags & SOF_LAYER)
		return true;
	return CockpitSubmodelUsesTransparentMaterial(pm, submodel);
}

static void BuildCockpitDisplayAdjustments()
{
	poly_model* pm = Cockpit_info.model;
	Cockpit_info.display_adjust_count = 0;
	Cockpit_info.display_adjust_available = false;

	for (int i = 0; i < MAX_SUBOBJECTS; i++)
	{
		vm_MakeZero(&Cockpit_info.display_adjust_unit[i]);
		vm_MakeZero(&Cockpit_info.display_adjust_scaled[i]);
	}

	if (!pm || pm->n_models <= 0)
		return;

	int count = std::min(pm->n_models, MAX_SUBOBJECTS);
	int display_sign[MAX_SUBOBJECTS] = {};
	float center_x[MAX_SUBOBJECTS] = {};

	float half_width = std::max(fabsf(pm->mins.x), fabsf(pm->maxs.x));
	float side_threshold = std::max(COCKPIT_DISPLAY_CENTER_THRESHOLD, half_width * COCKPIT_DISPLAY_WIDTH_THRESHOLD);

	for (int i = 0; i < count; i++)
		center_x[i] = CockpitSubmodelCenterX(pm, i);

	for (int i = 0; i < count; i++)
	{
		if (pm->submodel[i].parent < 0 || (pm->submodel[i].flags & (SOF_VIEWER | SOF_LAYER)))
			continue;
		if (!(pm->submodel[i].flags & SOF_MONITOR_MASK) && CockpitSubmodelUsesTransparentMaterial(pm, i))
			continue;

		display_sign[i] = CockpitSideFromCenter(center_x[i], side_threshold);
	}

	for (int i = 0; i < count; i++)
	{
		float desired = (float)display_sign[i];
		float parent_desired = 0.0f;
		if (pm->submodel[i].parent >= 0 && pm->submodel[i].parent < count)
			parent_desired = (float)display_sign[pm->submodel[i].parent];

		Cockpit_info.display_adjust_unit[i].x = desired - parent_desired;
		if (display_sign[i])
			Cockpit_info.display_adjust_available = true;
	}

	Cockpit_info.display_adjust_count = count;
}

static vector CockpitRootVectorToSubmodelParent(poly_model* pm, int submodel, vector root_vec)
{
	int ancestry[MAX_SUBOBJECTS];
	int ancestry_count = 0;
	int parent = pm->submodel[submodel].parent;

	for (int mn = parent; mn >= 0 && ancestry_count < MAX_SUBOBJECTS; mn = pm->submodel[mn].parent)
		ancestry[ancestry_count++] = mn;

	for (int i = ancestry_count - 1; i >= 0; i--)
	{
		matrix m;
		angvec* angs = &pm->submodel[ancestry[i]].angs;
		vm_AnglesToMatrix(&m, angs->p, angs->h, angs->b);
		root_vec = root_vec * m;
	}

	return root_vec;
}

static void ApplyCockpitDisplayAdjustments(float display_spread)
{
	for (int i = 0; i < Cockpit_info.display_adjust_count; i++)
	{
		vector cockpit_delta = Cockpit_info.display_adjust_unit[i] * display_spread;
		Cockpit_info.display_adjust_scaled[i] =
			CockpitRootVectorToSubmodelParent(Cockpit_info.model, i, cockpit_delta);
	}
	PolymodelSetSubmodelOffsetAdjustments(Cockpit_info.model_num, Cockpit_info.display_adjust_scaled,
		Cockpit_info.display_adjust_count);
}

static float GetCockpitDisplaySpread()
{
	const float aspect_5_4 = 5.0f / 4.0f;
	const float aspect_16_9 = 16.0f / 9.0f;
	const float aspect_21_9 = 21.0f / 9.0f;
	const float max_spread = 0.25f;
	const float phoenix_extra_wide_scale = 1.225f;
	const float magnum_extra_wide_scale = 0.85f;
	const float pyro_extra_wide_scale = 1.12f;

	if (Game_window_res_width <= 0 || Game_window_res_height <= 0)
		return 0.0f;

	float aspect = (float)Game_window_res_width / (float)Game_window_res_height;
	float extra_wide_t = (aspect - aspect_16_9) / (aspect_21_9 - aspect_16_9);
	if (extra_wide_t < 0.0f)
		extra_wide_t = 0.0f;
	else if (extra_wide_t > 1.0f)
		extra_wide_t = 1.0f;
	float extra_wide_spread = max_spread * ((aspect_21_9 - aspect_16_9) / (aspect_16_9 - aspect_5_4)) * extra_wide_t;

	if (Cockpit_info.ship_index == SHIP_PHOENIX_ID)
	{
		float t = (aspect - aspect_5_4) / (aspect_16_9 - aspect_5_4);
		if (t < 0.0f)
			t = 0.0f;
		else if (t > 1.0f)
			t = 1.0f;

		return max_spread * t + extra_wide_spread * phoenix_extra_wide_scale;
	}
	if (Cockpit_info.ship_index == SHIP_MAGNUM_ID)
		return extra_wide_spread * magnum_extra_wide_scale;
	if (Cockpit_info.ship_index == SHIP_PYRO_ID ||
		(Cockpit_info.ship_index >= 0 && Cockpit_info.ship_index < MAX_SHIPS &&
		 !stricmp(Ships[Cockpit_info.ship_index].name, "Black Pyro")))
	{
		return extra_wide_spread * pyro_extra_wide_scale;
	}

	return 0.0f;
}

//////////////////////////////////////////////////////////////////////////////
//	Initializes the cockpit by loading it in and initializing all it's gauges.
//	initialization of cockpit.
void InitCockpit(int ship_index)
{
	extern void FreeReticle();
	tCockpitCfgInfo cfginfo;
	int i;
	mprintf((0, "Initializing cockpit.\n"));
	LoadCockpitInfo(Ships[ship_index].cockpit_name, &cfginfo);
	// initialize special hud/cockpit images unique to this ship
	for (i = 0; i < NUM_SHIELD_GAUGE_FRAMES; i++)
	{
		if (cfginfo.shieldrings[i][0])
			HUD_resources.shield_bmp[i] = bm_AllocLoadFileBitmap(IGNORE_TABLE(cfginfo.shieldrings[i]), 0);
	}
	HUD_resources.ship_bmp = bm_AllocLoadFileBitmap(IGNORE_TABLE(cfginfo.shipimg), 0);
	HUD_resources.energy_bmp = bm_AllocLoadFileBitmap(IGNORE_TABLE(cfginfo.energyimg), 0);
	HUD_resources.afterburn_bmp = bm_AllocLoadFileBitmap(IGNORE_TABLE(cfginfo.burnimg), 0);
	HUD_resources.invpulse_bmp = bm_AllocLoadFileBitmap(IGNORE_TABLE(cfginfo.invpulseimg), 0);
	if (cfginfo.modelname[0] == 0) 
	{
		Cockpit_info.model_num = -1;
		mprintf((0, "No cockpit found for ship.\n"));
		return;
	}
	// initialize cockpit.
	Cockpit_info.ship_index = ship_index;
	Cockpit_info.model_num = LoadPolyModel(IGNORE_TABLE(cfginfo.modelname), 0);
	Cockpit_info.model = GetPolymodelPointer(Cockpit_info.model_num);
	Cockpit_info.frame_time = 0.0f;
	Cockpit_info.state = COCKPIT_STATE_DORMANT;
	Cockpit_info.this_keyframe = COCKPIT_DORMANT_FRAME;
	Cockpit_info.next_keyframe = COCKPIT_DORMANT_FRAME;
	Cockpit_info.resized = false;
	Cockpit_info.buffet_amp = 0.0f;
	Cockpit_info.snd_open = SOUND_COCKPIT;
	vm_MakeZero(&Cockpit_info.buffet_vec);
	//	find layered submodel mask
	Cockpit_info.layered_mask = 0x00000000;
	Cockpit_info.nonlayered_mask = 0x00000000;
	Cockpit_info.post_post_mask = 0x00000000;
	Cockpit_post_post_snapshot.valid = false;
	for (i = 0; i < Cockpit_info.model->n_models; i++)
	{
		if ((Cockpit_info.model->submodel[i].flags & SOF_VIEWER) || (Cockpit_info.model->submodel[i].flags & SOF_MONITOR_MASK))
			continue;
		if (CockpitSubmodelShouldRenderPostPost(Cockpit_info.model, i))
		{
			Cockpit_info.post_post_mask |= (1 << i);
			continue;
		}
		if (Cockpit_info.model->submodel[i].flags & SOF_LAYER)
			Cockpit_info.layered_mask |= (1 << i);
		else
			Cockpit_info.nonlayered_mask |= (1 << i);
	}
	BuildCockpitDisplayAdjustments();
	InitGauges(STAT_SHIELDS | STAT_SHIP | STAT_ENERGY | STAT_PRIMARYLOAD | STAT_SECONDARYLOAD | STAT_AFTERBURN);
	//	initialize reticle
	FreeReticle();
	InitReticle(-1, -1);
}
//	Forces freeing of cockpit
void FreeCockpit()
{
	int i;
	CloseGauges();

	if (Cockpit_info.model_num > -1) 
	{
		FreePolyModel(Cockpit_info.model_num);
		Cockpit_info.model = NULL;
		Cockpit_info.model_num = -1;
	}
	Cockpit_info.ship_index = -1;
	Cockpit_info.frame_time = 0.0f;
	Cockpit_info.state = COCKPIT_STATE_DORMANT;
	Cockpit_info.display_adjust_count = 0;
	Cockpit_info.display_adjust_available = false;
	Cockpit_info.post_post_mask = 0;
	Cockpit_post_post_snapshot.valid = false;
	//	free ship specific stuff for hud-cockpit shared.
	bm_FreeBitmap(HUD_resources.invpulse_bmp);
	bm_FreeBitmap(HUD_resources.afterburn_bmp);
	bm_FreeBitmap(HUD_resources.energy_bmp);
	for (i = 0; i < NUM_SHIELD_GAUGE_FRAMES; i++)
		bm_FreeBitmap(HUD_resources.shield_bmp[i]);
	bm_FreeBitmap(HUD_resources.ship_bmp);
}
//	check if cockpit exists
bool IsValidCockpit()
{
	return (Cockpit_info.model_num > -1) ? true : false;
}
bool CockpitFileParse(const char* command, const char* operand, void* data)
{
	tCockpitCfgInfo* cfginf = (tCockpitCfgInfo*)data;
	if (!strcmp(command, "ckptmodel")) 
	{
		// cockpit model name
		if (cfginf)
			strcpy(cfginf->modelname, operand);
	}
	else if (!strncmp(command, "shieldimg", strlen("shieldimg"))) 
	{
		//	cockpit shield ring names
		char buf[16];
		int i;
		for (i = 0; i < NUM_SHIELD_GAUGE_FRAMES; i++)
		{
			sprintf(buf, "shieldimg%d", i);
			if (!strcmpi(command, buf)) 
			{
				if (cfginf)
					strcpy(cfginf->shieldrings[i], operand);
				break;
			}
		}
	}
	else if (!strcmp(command, "shipimg")) 
	{
		// ship image name
		if (cfginf)
			strcpy(cfginf->shipimg, operand);
	}
	else if (!strcmp(command, "afterburnimg")) 
	{
		// ship image name
		if (cfginf)
			strcpy(cfginf->burnimg, operand);
	}
	else if (!strcmp(command, "energyimg")) 
	{
		// ship image name
		if (cfginf)
			strcpy(cfginf->energyimg, operand);
	}
	else if (!strcmp(command, "invpulseimg")) 
	{
		// ship image name
		if (cfginf)
			strcpy(cfginf->invpulseimg, operand);
	}
	else if (!strcmp(command, "fullhudinf")) 
	{
		if (cfginf)
			strcpy(HUD_resources.hud_inf_name, operand);
	}
	else 
	{
		return false;
	}
	return true;
}
//	loads pertinent information about cockpit.
void LoadCockpitInfo(const char* ckt_file, tCockpitCfgInfo* cfginfo)
{
	//	clear out return values.
	if (cfginfo) 
	{
		memset(cfginfo, 0, sizeof(tCockpitCfgInfo));
		ASSERT(NUM_SHIELD_GAUGE_FRAMES == 5);
		strcpy(cfginfo->shieldrings[0], TBL_GAMEFILE("shieldring01.ogf"));
		strcpy(cfginfo->shieldrings[1], TBL_GAMEFILE("shieldring02.ogf"));
		strcpy(cfginfo->shieldrings[2], TBL_GAMEFILE("shieldring03.ogf"));
		strcpy(cfginfo->shieldrings[3], TBL_GAMEFILE("shieldring04.ogf"));
		strcpy(cfginfo->shieldrings[4], TBL_GAMEFILE("shieldring05.ogf"));
		strcpy(cfginfo->shipimg, TBL_GAMEFILE("hudship.ogf"));
		strcpy(cfginfo->burnimg, TBL_GAMEFILE("hudburn.ogf"));
		strcpy(cfginfo->energyimg, TBL_GAMEFILE("hudenergy.ogf"));
		strcpy(cfginfo->invpulseimg, TBL_GAMEFILE("shieldinv.ogf"));
	}
	if (ckt_file[0] == 0)
		return;

	LoadHUDConfig(ckt_file, CockpitFileParse, cfginfo);
}
//	forces opening of cockpit.
void OpenCockpit()
{
	//	this should allow for opening cockpit while in closing mode.
	//	do this to insure that an immediate call to CloseCockpit will force it to close again.
	if (Cockpit_info.this_keyframe <= COCKPIT_COMPLETE_FRAME) 
	{
		Cockpit_info.state = COCKPIT_STATE_QUASI;
		Cockpit_info.this_keyframe = COCKPIT_DORMANT_FRAME;
		Cockpit_info.next_keyframe = COCKPIT_COMPLETE_FRAME;
		if (Cockpit_info.frame_time > 0.0f)
			Cockpit_info.frame_time = COCKPIT_ANIM_TIME - Cockpit_info.frame_time;
	}
	Sound_system.Play2dSound(Cockpit_info.snd_open);
	//	load hud information for cockpit.
	LoadCockpitInfo(Ships[Cockpit_info.ship_index].cockpit_name, NULL);
}

//	forces complete closing of cockpit
void CloseCockpit()
{
	//	this should allow for closing cockpit while in opening mode.
	//	do this to insure that an immediate call to OpenCockpit will force it to open again.
	if (Cockpit_info.this_keyframe >= COCKPIT_DORMANT_FRAME) 
	{
		Cockpit_info.next_keyframe = COCKPIT_DORMANT_FRAME;
		Cockpit_info.this_keyframe = COCKPIT_COMPLETE_FRAME;
		if (Cockpit_info.frame_time > 0.0f)
			Cockpit_info.frame_time = COCKPIT_ANIM_TIME - Cockpit_info.frame_time;
	}
	FlagGaugesNonfunctional(STAT_ALL);
	Sound_system.Play2dSound(Cockpit_info.snd_open);
}

//	forces quick opening of cockpit
void QuickOpenCockpit()
{
	Cockpit_info.frame_time = 0.0f;
	Cockpit_info.state = COCKPIT_STATE_FUNCTIONAL;
	Cockpit_info.this_keyframe = Cockpit_info.next_keyframe = COCKPIT_COMPLETE_FRAME;
	FlagGaugesFunctional(STAT_ALL);
	//	load hud information for cockpit.
	LoadCockpitInfo(Ships[Cockpit_info.ship_index].cockpit_name, NULL);
}
//	forces quick closing of cockpit
void QuickCloseCockpit()
{
	Cockpit_info.frame_time = 0.0f;
	Cockpit_info.state = COCKPIT_STATE_DORMANT;
	Cockpit_info.this_keyframe = Cockpit_info.next_keyframe = COCKPIT_DORMANT_FRAME;
	FlagGaugesNonfunctional(STAT_ALL);
}

//	resizes cockpit.
void ResizeCockpit()
{
	Cockpit_info.resized = true;
}

//	cockpit orientation.
void StartCockpitShake(float mag, vector* vec)
{
	ASSERT(vec);
	if (mag > MAX_BUFFET_STRENGTH)
		mag = MAX_BUFFET_STRENGTH;
	Cockpit_info.buffet_amp = mag;
	Cockpit_info.buffet_vec = (*vec);
	Cockpit_info.buffet_wave = FixSin(0);
	Cockpit_info.buffet_time = 0.0f;
}

//////////////////////////////////////////////////////////////////////////////
//	renders the cockpit.
extern float GetTerrainDynamicScalar(vector* pos, int seg);
extern void GetRoomDynamicScalar(vector* pos, room* rp, float* r, float* g, float* b);
void RenderCockpit()
{
	object* player_obj = &Objects[Players[Player_num].objnum];
	physics_info* player_phys = &player_obj->mtype.phys_info;
	vector view_pos, light_vec;
	matrix view_tmat;
	float view_z, view_y, view_x, keyframe;
	float light_scalar_r, light_scalar_g, light_scalar_b;
	float normalized_time[MAX_SUBOBJECTS];
	bool gauge_reset = false;
	//	draw cockpit depending on current state
	if (Cockpit_info.state == COCKPIT_STATE_DORMANT || Cockpit_info.model_num == -1)
		return;
	//	position cockpit correctly.
	bsp_info* viewer_subobj = CockpitGetMonitorSubmodel(SOF_VIEWER);
	if (!viewer_subobj) 
	{
		mprintf((0, "Cockpit missing viewer!\n"));
		return;
	}
	view_z = viewer_subobj->offset.z - Cockpit_info.buffet_vec.z * Cockpit_info.buffet_amp * Cockpit_info.buffet_wave * 1.1f;
	view_y = viewer_subobj->offset.y + Cockpit_info.buffet_vec.y * Cockpit_info.buffet_amp * Cockpit_info.buffet_wave;
	view_x = viewer_subobj->offset.x + Cockpit_info.buffet_vec.x * Cockpit_info.buffet_amp * Cockpit_info.buffet_wave;
	//@@	if (player_phys->rotthrust.x !=0.0f || player_phys->rotthrust.y != 0.0f) {
	//@@		gauge_reset = true;
	//@@		view_x += (player_phys->rotthrust.y*COCKPIT_SHIFT_DELTA/player_phys->full_rotthrust);
	//@@		view_y -= (player_phys->rotthrust.x*COCKPIT_SHIFT_DELTA/player_phys->full_rotthrust);
	//@@	}
	//	create new orientation, so the cockpit should be facing the viewer
	view_tmat = Viewer_object->orient;
	view_pos = (view_tmat.fvec * view_z) + (view_tmat.uvec * view_y) + (view_tmat.rvec * view_x) + Viewer_object->pos;
	view_tmat.fvec = -view_tmat.fvec;
	view_tmat.rvec = -view_tmat.rvec;
	//	lighting.
	light_vec = -Viewer_object->orient.uvec;
	if (OBJECT_OUTSIDE(player_obj))
	{
		float light_scalar = GetTerrainDynamicScalar(&player_obj->pos, CELLNUM(player_obj->roomnum));
		light_scalar_r = light_scalar;
		light_scalar_g = light_scalar;
		light_scalar_b = light_scalar;
	}
	else
	{
		GetRoomDynamicScalar(&player_obj->pos, &Rooms[player_obj->roomnum], &light_scalar_r, &light_scalar_g, &light_scalar_b);
	}
	if (light_scalar_r < 0.1f)
		light_scalar_r = 0.1f;
	if (light_scalar_g < 0.1f)
		light_scalar_g = 0.1f;
	if (light_scalar_b < 0.1f)
		light_scalar_b = 0.1f;
	// Samir, I moved this from the DrawPolygonModel call below -JL
	light_scalar_r *= .8f;
	light_scalar_g *= .8f;
	light_scalar_b *= .8f;

	if (player_obj->effect_info)
	{
		light_scalar_r = std::min(1.0f, light_scalar_r + (player_obj->effect_info->dynamic_red));
		light_scalar_g = std::min(1.0f, light_scalar_g + (player_obj->effect_info->dynamic_green));
		light_scalar_b = std::min(1.0f, light_scalar_b + (player_obj->effect_info->dynamic_blue));
	}
	if (Players[player_obj->id].flags & PLAYER_FLAGS_HEADLIGHT)
	{
		light_scalar_r = 1.0;
		light_scalar_g = 1.0;
		light_scalar_b = 1.0;
	}

	// animate and draw
	keyframe = KeyframeAnimateCockpit();
	SetNormalizedTimeAnim(keyframe, normalized_time, Cockpit_info.model);

	bool display_adjust_active = false;
	float display_spread = GetCockpitDisplaySpread();
	if (display_spread > 0.0001f && Cockpit_info.display_adjust_available)
	{
		SetModelAnglesAndPos(Cockpit_info.model, normalized_time);
		ApplyCockpitDisplayAdjustments(display_spread);
		display_adjust_active = true;
	}

	//	must put after animation.
	if (Cockpit_info.buffet_amp > 0.04f) 
	{
		angle buffet_angle;
		Cockpit_info.buffet_time += Frametime;
		if (Cockpit_info.buffet_time > BUFFET_PERIOD) 
		{
			Cockpit_info.buffet_time = 0.0f;
			Cockpit_info.buffet_amp *= 0.5f;
		}
		buffet_angle = (angle)(65536.0 * Cockpit_info.buffet_time / (BUFFET_PERIOD - ((BUFFET_PERIOD - Cockpit_info.buffet_time) * 0.5f)));
		Cockpit_info.buffet_wave = FixSin(buffet_angle);
		if (Cockpit_info.buffet_wave > 0.5f)
			Cockpit_info.buffet_wave = 1.0f;
		else if (Cockpit_info.buffet_wave < -0.5f)
			Cockpit_info.buffet_wave = -1.0f;
		else
			Cockpit_info.buffet_wave = 0.0f;
		Cockpit_info.animating = true;
	}
	else if (Cockpit_info.buffet_amp > 0.0f) 
	{
		Cockpit_info.animating = true;
		Cockpit_info.buffet_amp = 0.0f;
	}

	Cockpit_post_post_snapshot.valid = true;
	Cockpit_post_post_snapshot.frame_count = FrameCount;
	Cockpit_post_post_snapshot.view_pos = view_pos;
	Cockpit_post_post_snapshot.view_tmat = view_tmat;
	Cockpit_post_post_snapshot.light_vec = light_vec;
	Cockpit_post_post_snapshot.light_scalar_r = light_scalar_r;
	Cockpit_post_post_snapshot.light_scalar_g = light_scalar_g;
	Cockpit_post_post_snapshot.light_scalar_b = light_scalar_b;
	memcpy(Cockpit_post_post_snapshot.normalized_time, normalized_time, sizeof(normalized_time));
	Cockpit_post_post_snapshot.display_adjust_active = display_adjust_active;
	Cockpit_post_post_snapshot.display_spread = display_spread;

	//	draws lower z cockpit, and monitor glares after gauge renderering
	rend_SetAOSuppression(1.0f);
	rend_SetZBufferWriteMask(1);
	rend_SetZBufferState(1);
	DrawPolygonModel(&view_pos, &view_tmat, Cockpit_info.model_num, normalized_time, 0, &light_vec, light_scalar_r, light_scalar_g, light_scalar_b, Cockpit_info.nonlayered_mask, 0, 1);
	rend_SetBloomSuppression(1.0f);
	rend_SetZBufferState(0);
	RenderGauges(&view_pos, &view_tmat, normalized_time, (Cockpit_info.animating || Cockpit_info.resized), gauge_reset);
	rend_SetBloomSuppression(0.0f);
	rend_SetZBufferWriteMask(1);
	rend_SetZBufferState(1);
	DrawPolygonModel(&view_pos, &view_tmat, Cockpit_info.model_num, normalized_time, 0, &light_vec, light_scalar_r, light_scalar_g, light_scalar_b, Cockpit_info.layered_mask, 0, 1);
	rend_SetAOSuppression(0.0f);
	rend_SetZBufferState(0);

	if (display_adjust_active)
		PolymodelClearSubmodelOffsetAdjustments();

	Cockpit_info.resized = false;
}

bool CockpitHasPostPostElements()
{
	return Cockpit_info.model_num > -1 && Cockpit_info.post_post_mask != 0;
}

void RenderCockpitPostPost()
{
	if (!CockpitHasPostPostElements() || !Cockpit_post_post_snapshot.valid ||
		(Cockpit_post_post_snapshot.frame_count != FrameCount &&
		 Cockpit_post_post_snapshot.frame_count + 1 != FrameCount))
	{
		return;
	}

	if (Cockpit_post_post_snapshot.display_adjust_active)
		ApplyCockpitDisplayAdjustments(Cockpit_post_post_snapshot.display_spread);

	rend_SetZBufferWriteMask(0);
	rend_SetZBufferState(0);
	DrawPolygonModel(&Cockpit_post_post_snapshot.view_pos, &Cockpit_post_post_snapshot.view_tmat,
		Cockpit_info.model_num, Cockpit_post_post_snapshot.normalized_time, 0,
		&Cockpit_post_post_snapshot.light_vec,
		Cockpit_post_post_snapshot.light_scalar_r,
		Cockpit_post_post_snapshot.light_scalar_g,
		Cockpit_post_post_snapshot.light_scalar_b,
		Cockpit_info.post_post_mask, 0, 1);
	rend_SetZBufferWriteMask(1);

	if (Cockpit_post_post_snapshot.display_adjust_active)
		PolymodelClearSubmodelOffsetAdjustments();
}

//////////////////////////////////////////////////////////////////////////////
//	
//	adjusts current keyframe and dormancy
float KeyframeAnimateCockpit()
{
	float newkeyframe;

	newkeyframe = Cockpit_info.this_keyframe + (Cockpit_info.next_keyframe - Cockpit_info.this_keyframe) * (Cockpit_info.frame_time / COCKPIT_ANIM_TIME);
	//	mprintf((0, "this=%.1f next=%.1f ft=%.1f\n", Cockpit_info.this_keyframe, Cockpit_info.next_keyframe, Cockpit_info.frame_time));
	//	going up in keyframes
	if (Cockpit_info.this_keyframe < Cockpit_info.next_keyframe) 
	{
		if (newkeyframe >= Cockpit_info.next_keyframe) 
		{
			Cockpit_info.frame_time = 0.0f;
			Cockpit_info.this_keyframe = Cockpit_info.next_keyframe;
		}
	}
	//	going down in keyframes
	else if (Cockpit_info.this_keyframe > Cockpit_info.next_keyframe) 
	{
		if (newkeyframe <= Cockpit_info.next_keyframe) 
		{
			Cockpit_info.frame_time = 0.0f;
			Cockpit_info.this_keyframe = Cockpit_info.next_keyframe;
		}
	}
	else 
	{
		Cockpit_info.animating = false;
		return newkeyframe;
	}

	//	decide if we can process cockpit
	Cockpit_info.animating = true;
	if (Cockpit_info.this_keyframe != Cockpit_info.next_keyframe) 
	{
		Cockpit_info.frame_time += Frametime;
	}
	if (Cockpit_info.this_keyframe == COCKPIT_COMPLETE_FRAME && Cockpit_info.next_keyframe == Cockpit_info.this_keyframe) 
	{
		FlagGaugesFunctional(STAT_ALL);
		Cockpit_info.state = COCKPIT_STATE_FUNCTIONAL;
	}
	else if (Cockpit_info.this_keyframe == COCKPIT_DORMANT_FRAME && Cockpit_info.next_keyframe == COCKPIT_DORMANT_FRAME) 
	{
		Cockpit_info.state = COCKPIT_STATE_DORMANT;
	}
	else 
	{
		Cockpit_info.state = COCKPIT_STATE_QUASI;
	}
	return newkeyframe;
}

//////////////////////////////////////////////////////////////////////////////
//	
//	returns the submodel number of the monitor requested
bsp_info* CockpitGetMonitorSubmodel(ushort monitor_flag)
{
	int i;
	ASSERT(Cockpit_info.model_num > -1);
	for (i = 0; i < Poly_models[Cockpit_info.model_num].n_models; i++)
	{
		if (Poly_models[Cockpit_info.model_num].submodel[i].flags & monitor_flag)
			return &Poly_models[Cockpit_info.model_num].submodel[i];
	}
	return NULL;
}

//	returns the polymodel for the hud
poly_model* CockpitGetPolyModel()
{
	ASSERT(Cockpit_info.model_num > -1);

	return &Poly_models[Cockpit_info.model_num];
}

//Tell whether the cockpit is animating
int CockpitState()
{
	return Cockpit_info.state;
}

#version 450 core

uniform sampler2DArray colortexture;
uniform sampler2DArray lightmaptexture;
uniform int terrain_dynamic_light_count[4];
uniform vec3 terrain_dynamic_light_positions[32];
uniform vec3 terrain_dynamic_light_colors[32];
uniform float terrain_dynamic_light_radii[32];
uniform vec3 terrain_dynamic_light_directions[32];
uniform float terrain_dynamic_light_dot_ranges[32];
uniform int terrain_dynamic_light_directional[32];
uniform float ao_weight_value;
uniform int ao_capture_weight_mode;

uniform int motion_vector_mode;
uniform mat4 motion_vector_current_view_projection;
uniform mat4 motion_vector_previous_view_projection;
uniform vec2 motion_vector_screen_size;
uniform int motion_vector_has_previous;

vec2 ComputeMotionVector(vec3 world_position)
{
	if (motion_vector_mode != 2 || motion_vector_has_previous == 0)
		return vec2(0.0);

	vec4 current_clip = motion_vector_current_view_projection * vec4(world_position, 1.0);
	vec4 previous_clip = motion_vector_previous_view_projection * vec4(world_position, 1.0);
	if (current_clip.w <= 0.00001 || previous_clip.w <= 0.00001)
		return vec2(0.0);

	vec2 current_ndc = current_clip.xy / current_clip.w;
	vec2 previous_ndc = previous_clip.xy / previous_clip.w;
	return (current_ndc - previous_ndc) * 0.5;
}

layout(std140) uniform TerrainFogBlock
{
	vec4 color;
	float start_dist;
	float end_dist;
} fog;

in vec4 outcolor;
in vec3 outuv;
in vec3 outuv2;
in vec3 outworld;
in float outdepth;
flat in int outlmpage;
flat in int outtexpage;

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 velocity;
layout(location = 2) out vec4 post_mask;
layout(location = 3) out float ao_class;

vec3 ApplyDynamicLightmapLighting(vec3 lightmap_color)
{
	vec3 face_normal = vec3(0.0, 1.0, 0.0);
	vec3 dynamic_color = vec3(0.0);
	int lightmap_page = clamp(outlmpage, 0, 3);
	int dynamic_light_count = terrain_dynamic_light_count[lightmap_page];
	int light_base = lightmap_page * 8;
	if (dynamic_light_count == 0)
		return lightmap_color;

	for (int i = 0; i < 8; i++)
	{
		if (i >= dynamic_light_count)
			break;

		int light_index = light_base + i;
		vec3 light_delta = outworld - terrain_dynamic_light_positions[light_index];
		float radius = max(terrain_dynamic_light_radii[light_index], 0.0001);
		float distance = length(light_delta);
		vec3 light_vector = (distance > 0.0001) ? light_delta / distance : face_normal;
		float scalar = 1.0 - (distance / radius);
		if (scalar <= 0.0)
			continue;

		if (terrain_dynamic_light_directional[light_index] != 0)
		{
			vec3 light_direction = normalize(terrain_dynamic_light_directions[light_index]);
			float direction_dot = dot(light_vector, light_direction);
			float dot_range = terrain_dynamic_light_dot_ranges[light_index];
			if (direction_dot < dot_range)
				continue;

			scalar *= (direction_dot - dot_range) / max(1.0 - dot_range, 0.0001);
		}

		dynamic_color += terrain_dynamic_light_colors[light_index] * scalar;
	}

	return clamp(lightmap_color + dynamic_color, vec3(0.0), vec3(1.0));
}

vec4 SampleTerrainBaseTexture(vec2 uv, int layer)
{
	vec2 tile_origin = floor(uv);
	vec2 tile_uv = uv - tile_origin;
	bvec2 upper_edge = bvec2(tile_uv.x <= 0.000001 && uv.x > 0.0, tile_uv.y <= 0.000001 && uv.y > 0.0);
	tile_uv = mix(tile_uv, vec2(1.0), upper_edge);

	vec2 texel_inset = 0.5 / vec2(textureSize(colortexture, 0).xy);
	tile_uv = clamp(tile_uv, texel_inset, vec2(1.0) - texel_inset);

	return textureGrad(colortexture, vec3(tile_origin + tile_uv, float(max(layer, 0))), dFdx(uv), dFdy(uv));
}

void main()
{
	velocity = ComputeMotionVector(outworld);
	if (ao_capture_weight_mode != 0)
	{
		color = vec4(ao_weight_value, ao_weight_value, ao_weight_value, 1.0);
		post_mask = vec4(0.0, 0.0, 0.0, 1.0);
		ao_class = ao_weight_value;
		return;
	}

	vec4 basecolor = SampleTerrainBaseTexture(outuv.xy / outuv.z, outtexpage);
	vec4 lmcolor = texture(lightmaptexture, vec3(outuv2.xy / outuv2.z, float(clamp(outlmpage, 0, 3))));
	lmcolor.rgb = ApplyDynamicLightmapLighting(lmcolor.rgb);
	vec4 litcolor = basecolor * lmcolor * outcolor;
	float fog_start = clamp(1.0 - (1.0 / max(fog.start_dist, 0.0001)), 0.0, 1.0);
	float fog_end = clamp(1.0 - (1.0 / max(fog.end_dist, 0.0001)), 0.0, 1.0);
	float fog_amount = clamp((outdepth - fog_start) / max(fog_end - fog_start, 0.0001), 0.0, 1.0);
	color = vec4(mix(litcolor.rgb, fog.color.rgb, fog_amount), litcolor.a);
	post_mask = vec4(0.0, fog_amount, 0.0, 1.0);
	ao_class = 1.0 / 255.0;
}

#version 450 core

uniform sampler2D colortexture;
uniform sampler2D lightmaptexture;
uniform int dynamic_light_count;
uniform vec3 dynamic_light_positions[8];
uniform vec3 dynamic_light_colors[8];
uniform float dynamic_light_radii[8];
uniform vec3 dynamic_light_directions[8];
uniform float dynamic_light_dot_ranges[8];
uniform int dynamic_light_directional[8];
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

in vec2 outuv;
in vec2 outuv2;
in vec3 outworld;
in vec3 outnormal;
in vec3 outpt;

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 velocity;
layout(location = 2) out vec4 post_mask;
layout(location = 3) out float ao_class;

vec3 ApplyDynamicLightmapLighting(vec3 lightmap_color)
{
	if (dynamic_light_count == 0)
		return lightmap_color;

	vec3 normal = normalize(outnormal);
	vec3 dynamic_color = vec3(0.0);

	for (int i = 0; i < 8; i++)
	{
		if (i >= dynamic_light_count)
			break;

		vec3 light_delta = outworld - dynamic_light_positions[i];
		float radius = max(dynamic_light_radii[i], 0.0001);
		float distance = length(light_delta);
		vec3 light_vector = (distance > 0.0001) ? light_delta / distance : normal;
		float scalar = 1.0 - (distance / radius);
		if (scalar <= 0.0)
			continue;

		if (dynamic_light_directional[i] != 0)
		{
			vec3 light_direction = normalize(dynamic_light_directions[i]);
			float direction_dot = dot(light_vector, light_direction);
			float dot_range = dynamic_light_dot_ranges[i];
			if (direction_dot < dot_range)
				continue;

			scalar *= (direction_dot - dot_range) / max(1.0 - dot_range, 0.0001);
		}

		dynamic_color += dynamic_light_colors[i] * scalar;
	}

	return clamp(lightmap_color + dynamic_color, vec3(0.0), vec3(1.0));
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

	vec4 basecolor = texture(colortexture, outuv);
	vec4 lmcolor = texture(lightmaptexture, outuv2);
	lmcolor.rgb = ApplyDynamicLightmapLighting(lmcolor.rgb);
	vec4 litcolor = vec4(basecolor.rgb * lmcolor.rgb, 1.0);
	float eye_dist = max(-outpt.z, 0.0001);
	float fog_start = clamp(1.0 - (1.0 / max(fog.start_dist, 0.0001)), 0.0, 1.0);
	float fog_end = clamp(1.0 - (1.0 / max(fog.end_dist, 0.0001)), 0.0, 1.0);
	float fog_depth = clamp(1.0 - (1.0 / eye_dist), 0.0, 1.0);
	float fog_amount = clamp((fog_depth - fog_start) / max(fog_end - fog_start, 0.0001), 0.0, 1.0);
	color = vec4(mix(litcolor.rgb, fog.color.rgb, fog_amount), basecolor.a);
	post_mask = vec4(0.0, fog_amount, 0.0, 1.0);
	ao_class = 1.0 / 255.0;
}

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

in vec2 outuv;
in vec2 outuv2;
in vec3 outworld;
in vec3 outnormal;

layout(location = 0) out vec4 color;
layout(location = 2) out vec4 hbao_mask;

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
	vec4 basecolor = texture(colortexture, outuv);
	vec4 lmcolor = texture(lightmaptexture, outuv2);
	lmcolor.rgb = ApplyDynamicLightmapLighting(lmcolor.rgb);
	color = vec4(basecolor.rgb * lmcolor.rgb, basecolor.a);
	hbao_mask = vec4(0.0, 0.0, 0.0, 1.0);
}

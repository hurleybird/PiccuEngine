//the preprocessor definitions, including #version, are automatically applied
//at compile time for the generic shader

layout(std140) uniform TerrainFogBlock
{
	vec4 color;
	float start_dist;
	float end_dist;
} fog;

#if defined(USE_TEXTURING)
uniform sampler2D colortexture;
#if defined(USE_LIGHTMAP)
uniform sampler2D lightmaptexture;
#endif
#endif

uniform int phong_enabled;
uniform vec3 phong_light_direction;
uniform int dynamic_light_count;
uniform vec3 dynamic_face_normal;
uniform vec3 dynamic_light_positions[8];
uniform vec3 dynamic_light_colors[8];
uniform float dynamic_light_radii[8];
uniform vec3 dynamic_light_directions[8];
uniform float dynamic_light_dot_ranges[8];
uniform int dynamic_light_directional[8];
uniform float hbao_suppression;
uniform float bloom_suppression;

in vec4 outcolor;
in vec4 outnormal;
#if defined(USE_TEXTURING)
in vec3 outuv;
#if defined(USE_LIGHTMAP)
in vec3 outuv2;
#endif
#endif
#if defined(USE_FOG)
in vec3 outpt;
#endif

layout(location = 0) out vec4 color;
layout(location = 2) out vec4 hbao_mask;

vec4 ApplyPhongLighting(vec4 source_color)
{
	if (phong_enabled == 0)
		return source_color;

	vec3 normal = normalize(outnormal.xyz / max(outnormal.w, 0.0001));
	vec3 light_direction = normalize(phong_light_direction);
	float light = clamp((-dot(light_direction, normal) + 1.0) * 0.5, 0.0, 1.0);
	return vec4(source_color.rgb * light, source_color.a);
}

vec3 ApplyDynamicLightmapLighting(vec3 lightmap_color)
{
	if (dynamic_light_count == 0)
		return lightmap_color;

	vec3 world_position = outnormal.xyz / max(outnormal.w, 0.0001);
	vec3 face_normal = normalize(dynamic_face_normal);
	vec3 dynamic_color = vec3(0.0);

	for (int i = 0; i < 8; i++)
	{
		if (i >= dynamic_light_count)
			break;

		vec3 light_delta = world_position - dynamic_light_positions[i];
		float radius = max(dynamic_light_radii[i], 0.0001);
		float distance = length(light_delta);
		vec3 light_vector = (distance > 0.0001) ? light_delta / distance : face_normal;
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
	vec4 vertex_color = ApplyPhongLighting(outcolor);
	float suppression = 0.0;

	#if defined(USE_SPECULAR)
		color = vec4(vertex_color.rgb, texture(colortexture, outuv.xy / outuv.z).a * vertex_color.a);
	#elif defined(USE_TEXTURING) && defined(USE_LIGHTMAP)
		vec4 lightmap_color = texture(lightmaptexture, outuv2.xy / outuv2.z);
		lightmap_color.rgb = ApplyDynamicLightmapLighting(lightmap_color.rgb);
		color = texture(colortexture, outuv.xy / outuv.z) * lightmap_color * vertex_color;
	#elif defined(USE_TEXTURING)
		color = texture(colortexture, outuv.xy / outuv.z) * vertex_color;
	#else
		color = vertex_color;
	#endif
	float suppression_alpha = clamp(color.a, 0.0, 1.0);
	suppression = clamp(hbao_suppression * (1.0 - pow(1.0 - suppression_alpha, 3.0)), 0.0, 1.0);
	float bloom_mask = clamp(bloom_suppression * (1.0 - pow(1.0 - suppression_alpha, 3.0)), 0.0, 1.0);
	
	#if defined(USE_FOG)
		float fog_start = clamp(1.0 - (1.0 / max(fog.start_dist, 0.0001)), 0.0, 1.0);
		float fog_end = clamp(1.0 - (1.0 / max(fog.end_dist, 0.0001)), 0.0, 1.0);
		float fog_depth = clamp(-outpt.z, 0.0, 1.0);
		float mag = clamp((fog_depth - fog_start) / max(fog_end - fog_start, 0.0001), 0.0, 1.0);
		color = vec4(mix(color.rgb, fog.color.rgb, mag), color.a);
		suppression = max(suppression, 1.0 - pow(1.0 - mag, 3.0));
	#endif
	hbao_mask = vec4(max(suppression, bloom_mask), 0.0, 0.0, 1.0);
}

#version 450 core

uniform sampler2D colortexture;
uniform sampler2D lightmaptexture;
uniform int ao_class_value;
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

in vec2 outuv;
in vec2 outuv2;
in float outlight;
in float outalpha;
in vec3 outworld;

layout(location = 0) out vec4 color;
layout(location = 1) out vec2 velocity;
layout(location = 2) out vec4 post_mask;
layout(location = 3) out float ao_class;

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
	color = vec4(basecolor.rgb * lmcolor.rgb * outlight, basecolor.a * outalpha);
	post_mask = vec4(0.0, 0.0, 0.0, 1.0);
	ao_class = float(clamp(ao_class_value, 0, 255)) / 255.0;
}

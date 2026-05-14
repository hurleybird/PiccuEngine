#version 430 core

uniform sampler2D colortexture;
uniform sampler2D lightmaptexture;

layout(std140) uniform RoomBlock
{
	vec4 fog_color;
	float fog_distance;
	float brightness;
	int not_in_room;
	vec4 fog_plane;
} room;

in vec2 outuv;
in vec3 outpt;
in float outlight;
in float outalpha;
flat in vec4 outplane;

layout(location = 0) out vec4 color;
layout(location = 2) out vec4 hbao_mask;

void main()
{
	vec4 basecolor = texture(colortexture, outuv);
	
	float mag = 0;
	if (room.not_in_room != 0)
	{
		//alternate way of doing this suggested by Jeff Graw
		float dist = dot(outpt, outplane.xyz) + outplane.w;
		float t = outplane.w / (outplane.w - dist);
		vec3 portal_point = outpt * t; 
		mag = step(0, dist) * max(0, -(outpt.z - portal_point.z));
	}
	else
	{
		mag = -outpt.z;
	}
	
	float fog_amount = clamp(mag / room.fog_distance, 0, 1);
	float hbao_fog = 1.0 - pow(1.0 - fog_amount, 3.0);
	color = mix(vec4(basecolor.rgb, 1.0), vec4(room.fog_color.rgb, 1.0f), fog_amount) * vec4(outlight, outlight, outlight, 1.0);
	color.a *= outalpha;
	hbao_mask = vec4(hbao_fog, 0.0, 0.0, 1.0);
}

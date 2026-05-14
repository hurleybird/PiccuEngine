#version 430 core

uniform sampler2D colortexture;
uniform sampler2D lightmaptexture;

in vec2 outuv;
in vec2 outuv2;
in float outlight;
in float outalpha;

layout(location = 0) out vec4 color;
layout(location = 2) out vec4 hbao_mask;

void main()
{
	vec4 basecolor = texture(colortexture, outuv);
	color = vec4(basecolor.rgb * outlight, basecolor.a * outalpha);
	hbao_mask = vec4(0.0, 0.0, 0.0, 1.0);
}

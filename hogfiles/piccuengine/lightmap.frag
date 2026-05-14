#version 430 core

uniform sampler2D colortexture;
uniform sampler2D lightmaptexture;

in vec2 outuv;
in vec2 outuv2;

layout(location = 0) out vec4 color;
layout(location = 2) out vec4 hbao_mask;

void main()
{
	vec4 basecolor = texture(colortexture, outuv);
	vec4 lmcolor = texture(lightmaptexture, outuv2);
	color = vec4(basecolor.rgb * lmcolor.rgb, 1.0);
	hbao_mask = vec4(0.0, 0.0, 0.0, 1.0);
}

#version 330 core

layout(std140) uniform CommonBlock
{
	mat4 projection;
	mat4 modelview;
} commons;

layout(location = 0) in vec3 position;
layout(location = 2) in vec3 normal;
layout(location = 4) in vec2 uv;
layout(location = 5) in vec2 uv2;

out vec2 outuv;
out vec2 outuv2;
out vec3 outworld;
out vec3 outnormal;
out vec3 outpt;

void main()
{
	vec4 temp = commons.modelview * vec4(position, 1.0);
	gl_Position = commons.projection * temp;
	float depth = clamp(1.0 - (1.0 / max(-temp.z, 0.0001)), 0.0, 1.0);
	gl_Position.z = (depth * 2.0 - 1.0) * gl_Position.w;
	outuv = uv;
	outuv2 = uv2;
	outworld = position;
	outnormal = normal;
	outpt = temp.xyz;
}

#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 avViewSpaceNormal;
layout(location = 2) in vec2 aTexCoords;

out vec3 vViewSpacePosition;
out vec2 vTexCoords;
out vec3 vViewSpaceNormal;

uniform mat4 uModelViewProjMatrix;
uniform mat4 uModelViewMatrix;
uniform mat4 uNormalMatrix;

void main() {
    vTexCoords = aTexCoords;
    gl_Position = uModelViewProjMatrix * vec4(aPos, 1.0);
    vViewSpaceNormal = (uModelViewMatrix * vec4(avViewSpaceNormal, 0.0)).xyz;
    vViewSpacePosition = (uModelViewMatrix * vec4(aPos, 0.0)).xyz;
}

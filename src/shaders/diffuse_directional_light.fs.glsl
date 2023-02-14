#version 330 core
in vec3 vViewSpaceNormal;

uniform vec3 lightDirection;
uniform float lightIntensity;

void main() {
    float ctheta = dot(normalize(lightDirection), normalize(vViewSpaceNormal));
    vec3 BRDF = vec3(1.0f / 3.1415);
    vec3 result = BRDF * lightIntensity * ctheta;
    gl_FragColor = vec4(result, 1.0f);
}
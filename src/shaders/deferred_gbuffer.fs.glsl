#version 330 core
layout(location = 0) out vec3 gPosition;
layout(location = 1) out vec3 gNormal;
layout(location = 2) out vec4 gDiffuse;
layout(location = 3) out vec4 gMetallic;
layout(location = 4) out vec3 gEmissive;
layout(location = 5) out vec4 gOcclusion;

in vec2 vTexCoords;
in vec3 vSpacePosition;
in vec3 vViewSpaceNormal;

uniform vec4 uBaseColorFactor;
uniform float uMetallicFactor;
uniform float uRoughnessFactor;
uniform vec3 uEmissiveFactor;
uniform float uOcclusionStrength;

uniform sampler2D uBaseColorTexture;
uniform sampler2D uMetallicRoughnessTexture;
uniform sampler2D uEmissiveTexture;
uniform sampler2D uOcclusionTexture;

// Constants
const float GAMMA = 2.2;
const float INV_GAMMA = 1. / GAMMA;

vec3 LINEARtoSRGB(vec3 color) {
    return pow(color, vec3(INV_GAMMA));
}

vec4 SRGBtoLINEAR(vec4 srgbIn) {
    return vec4(pow(srgbIn.xyz, vec3(GAMMA)), srgbIn.w);
}

void main() {    
    // store the fragment position vector in the first gbuffer texture
    gPosition = vSpacePosition;
    // also store the per-fragment normals into the gbuffer
    gNormal = normalize(vViewSpaceNormal);
    // and also all the different textures used for pdr
    vec4 baseColorFromTexture = SRGBtoLINEAR(texture(uBaseColorTexture, vTexCoords));
    gDiffuse = baseColorFromTexture * uBaseColorFactor;

    vec4 metallicRougnessFromTexture = texture(uMetallicRoughnessTexture, vTexCoords);
    gMetallic.xyz = vec3(uMetallicFactor * metallicRougnessFromTexture.b);
    gMetallic.w = uRoughnessFactor * metallicRougnessFromTexture.g;

    gEmissive = SRGBtoLINEAR(texture2D(uEmissiveTexture, vTexCoords)).rgb *
        uEmissiveFactor;

    gOcclusion = texture(uOcclusionTexture, vTexCoords) * uOcclusionStrength;
}
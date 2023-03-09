#version 330 core
layout(location = 0) out vec3 gPosition;
layout(location = 1) out vec3 gNormal;
layout(location = 2) out vec3 gDiffuse;
layout(location = 3) out vec3 gMetallic;
layout(location = 4) out vec3 gEmissive;
layout(location = 5) out vec3 gOcclusion;

in vec2 vTexCoords;
in vec3 vViewSpacePosition;
in vec3 vViewSpaceNormal;

uniform sampler2D uBaseColorTexture;
uniform sampler2D uMetallicRoughnessTexture;
uniform sampler2D uEmissiveTexture;
uniform sampler2D uOcclusionTexture;

void main() {    
    // store the fragment position vector in the first gbuffer texture
    gPosition = vViewSpacePosition;
    // also store the per-fragment normals into the gbuffer
    gNormal = normalize(vViewSpaceNormal);
    // and also all the different textures used for pdr
    gDiffuse = texture(uBaseColorTexture, vTexCoords).rgb;
    gMetallic = texture(uMetallicRoughnessTexture, vTexCoords).rgb;
    gEmissive = texture(uEmissiveTexture, vTexCoords).rgb;
    gOcclusion = texture(uOcclusionTexture, vTexCoords).rgb;
}
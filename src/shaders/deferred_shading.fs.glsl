#version 330
out vec3 fColor;

in vec2 vTexCoords;

uniform int uApplyOcclusion;

uniform vec3 uLightDirection;
uniform vec3 uLightIntensity;

uniform float uOcclusionStrength;

uniform sampler2D gPosition;
uniform sampler2D gNormal;
uniform sampler2D gDiffuse;
uniform sampler2D gMetallic;
uniform sampler2D gEmissive;
uniform sampler2D gOcclusion;

// Constants
const float GAMMA = 2.2;
const float INV_GAMMA = 1. / GAMMA;
const float M_PI = 3.141592653589793;
const float M_1_PI = 1.0 / M_PI;

vec3 LINEARtoSRGB(vec3 color) {
    return pow(color, vec3(INV_GAMMA));
}

void main() {
    vec3 vViewSpaceNormal = texture(gNormal, vTexCoords).rgb;
    vec3 vViewSpacePosition = texture(gPosition, vTexCoords).rgb;
    vec3 N = normalize(vViewSpaceNormal);
    vec3 V = normalize(-vViewSpacePosition);
    vec3 L = uLightDirection;
    vec3 H = normalize(L + V);

    vec4 baseColor = texture(gDiffuse, vTexCoords);
    vec3 metallic = texture(gMetallic, vTexCoords).xyz;
    float roughness = texture(gMetallic, vTexCoords).w;

    vec3 dielectricSpecular = vec3(0.04);
    vec3 black = vec3(0.);

    vec3 c_diff = mix(baseColor.rgb * (1 - dielectricSpecular.r), black, metallic);
    vec3 F_0 = mix(vec3(dielectricSpecular), baseColor.rgb, metallic);
    float alpha = roughness * roughness;

    float VdotH = clamp(dot(V, H), 0., 1.);
    float baseShlickFactor = 1 - VdotH;
    float shlickFactor = baseShlickFactor * baseShlickFactor; // power 2
    shlickFactor *= shlickFactor;                             // power 4
    shlickFactor *= baseShlickFactor;                         // power 5
    vec3 F = F_0 + (vec3(1) - F_0) * shlickFactor;

    float sqrAlpha = alpha * alpha;
    float NdotL = clamp(dot(N, L), 0., 1.);
    float NdotV = clamp(dot(N, V), 0., 1.);
    float visDenominator = NdotL * sqrt(NdotV * NdotV * (1 - sqrAlpha) + sqrAlpha) +
        NdotV * sqrt(NdotL * NdotL * (1 - sqrAlpha) + sqrAlpha);
    float Vis = visDenominator > 0. ? 0.5 / visDenominator : 0.0;

    float NdotH = clamp(dot(N, H), 0., 1.);
    float baseDenomD = (NdotH * NdotH * (sqrAlpha - 1.) + 1.);
    float D = M_1_PI * sqrAlpha / (baseDenomD * baseDenomD);

    vec3 f_specular = F * Vis * D;

    vec3 diffuse = c_diff * M_1_PI;

    vec3 f_diffuse = (1. - F) * diffuse;
    vec3 emissive = texture(gEmissive, vTexCoords).xyz;

    vec3 color = (f_diffuse + f_specular) * uLightIntensity * NdotL;
    color += emissive;

    float ao;
    if(1 == uApplyOcclusion) {
        ao = texture2D(gOcclusion, vTexCoords).r;
        float occlusionStrength = texture2D(gOcclusion, vTexCoords).w;
        color = mix(color, color * ao, occlusionStrength);
    }

    fColor = LINEARtoSRGB(color);
}

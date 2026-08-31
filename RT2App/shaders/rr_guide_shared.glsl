// One pinned specular guide approximation shared by the RR guide resolve and
// the renderer's BRDF include. The helper returns linear view-dependent Fenv.
#ifndef RR_GUIDE_SHARED_GLSL
#define RR_GUIDE_SHARED_GLSL
vec3 EnvBRDFApprox2(vec3 specularColor, float roughness, float NoV)
{
    vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    vec4 c1 = vec4( 1.0,  0.0425,  1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return max(specularColor * ab.x + vec3(ab.y), vec3(0.0));
}

// Single material authority shared by path tracing and RR guide production.
// Keeping metallic removal/F0/view dependence here prevents the guide pass from
// drifting from the renderer's resolved surface semantics.
vec3 RRDiffuseReflectance(vec3 baseColor, float metallic)
{
    return max(baseColor, vec3(0.0)) * (1.0 - clamp(metallic, 0.0, 1.0));
}

vec3 RRComputeF0(vec3 baseColor, float metallic)
{
    return mix(vec3(0.04), max(baseColor, vec3(0.0)), clamp(metallic, 0.0, 1.0));
}

vec3 RRSpecularAlbedo(vec3 baseColor, float metallic, float roughness, float NoV)
{
    return EnvBRDFApprox2(RRComputeF0(baseColor, metallic),
                          clamp(roughness, 0.0, 1.0), clamp(abs(NoV), 0.0, 1.0));
}
#endif

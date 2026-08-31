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
#endif

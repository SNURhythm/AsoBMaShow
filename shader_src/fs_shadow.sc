$input v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_shadowColor;
uniform vec4 u_shadowParams; // x=width, y=height, z=radius, w=spread

float roundedBoxDistance(vec2 samplePoint, vec2 halfSize, float radius)
{
    vec2 q = abs(samplePoint) - halfSize + vec2_splat(radius);
    return length(max(q, vec2_splat(0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

void main()
{
    vec2 boxSize = max(u_shadowParams.xy, vec2_splat(1.0));
    float radius = clamp(
        u_shadowParams.z, 0.0, min(boxSize.x, boxSize.y) * 0.5);
    float spread = max(u_shadowParams.w, 1.0);
    vec2 shadowSize = boxSize + vec2_splat(spread * 2.0);

    vec2 local = v_texcoord0 * shadowSize - vec2_splat(spread);
    vec2 samplePoint = local - boxSize * 0.5;
    float distance = roundedBoxDistance(samplePoint, boxSize * 0.5, radius);
    float normalizedDistance = saturate(max(distance, 0.0) / spread);

    float gaussian = exp2(-5.0 * normalizedDistance * normalizedDistance);
    float edgeFade = 1.0 - smoothstep(0.82, 1.0, normalizedDistance);
    float alpha = u_shadowColor.a * 0.55 * gaussian * edgeFade;

    gl_FragColor = vec4(u_shadowColor.rgb, alpha);
}

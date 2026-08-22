$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

// x: outline distance, y: shadow smoothing, zw: normalized shadow offset.
uniform vec4 u_distanceParameters;
uniform vec4 u_outlineColor;
uniform vec4 u_shadowColor;

void main()
{
    const float smoothing = 1.0 / 16.0;
    float distance = texture2D(s_texColor, v_texcoord0).a;
    float outlineFactor = smoothstep(0.5 - smoothing, 0.5 + smoothing, distance);
    vec4 color = mix(u_outlineColor, v_color0, outlineFactor);
    float alpha = smoothstep(u_distanceParameters.x - smoothing,
                             u_distanceParameters.x + smoothing, distance);
    vec4 mainColor = vec4(color.rgb, color.a * alpha);

    float shadowDistance = texture2D(
        s_texColor, v_texcoord0 - u_distanceParameters.zw).a;
    float shadowAlpha = smoothstep(0.5 - u_distanceParameters.y,
                                   0.5 + u_distanceParameters.y,
                                   shadowDistance);
    vec4 shadow = vec4(u_shadowColor.rgb, u_shadowColor.a * shadowAlpha);
    gl_FragColor = mix(shadow, mainColor, mainColor.a);
}

$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_skinSampling;

void main()
{
    vec4 center = texture2D(s_texColor, v_texcoord0);
    if (u_skinSampling.x > 0.5)
    {
        // Pinned Beatoraja filtering preserves the nearest texel's alpha.
        if (center.a <= 0.0)
        {
            gl_FragColor = vec4(0.0);
            return;
        }
        vec2 size = vec2(textureSize(s_texColor, 0));
        vec2 halfTexel = vec2(0.5) / size;
        vec4 p00 = texture2D(s_texColor, v_texcoord0 - halfTexel);
        vec4 p10 = texture2D(s_texColor, v_texcoord0 + vec2(halfTexel.x, -halfTexel.y));
        vec4 p01 = texture2D(s_texColor, v_texcoord0 + vec2(-halfTexel.x, halfTexel.y));
        vec4 p11 = texture2D(s_texColor, v_texcoord0 + halfTexel);
        vec2 weight = fract(v_texcoord0 * size + vec2(0.5));
        vec4 top = mix(p00 * p00.a, p10 * p10.a, weight.x);
        vec4 bottom = mix(p01 * p01.a, p11 * p11.a, weight.x);
        vec4 filtered = mix(top, bottom, weight.y) / center.a;
        filtered.a = center.a;
        gl_FragColor = filtered * v_color0;
    }
    else
    {
        gl_FragColor = center * v_color0;
    }
}

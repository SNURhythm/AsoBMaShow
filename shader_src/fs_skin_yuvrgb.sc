$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_texY, 0);
SAMPLER2D(s_texU, 1);
SAMPLER2D(s_texV, 2);

void main()
{
    float y = texture2D(s_texY, v_texcoord0).r;
    float u = texture2D(s_texU, v_texcoord0).r - 0.5;
    float v = texture2D(s_texV, v_texcoord0).r - 0.5;

    vec3 rgb = vec3(
        y + 1.402 * v,
        y - 0.344 * u - 0.714 * v,
        y + 1.772 * u
    );
    gl_FragColor = vec4(rgb, 1.0) * v_color0;
}

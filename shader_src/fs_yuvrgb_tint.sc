$input v_texcoord0
#include <bgfx_shader.sh>

SAMPLER2D(s_texY, 0);
SAMPLER2D(s_texU, 1);
SAMPLER2D(s_texV, 2);
uniform vec4 u_tintColor;

void main()
{
    float y = texture2D(s_texY, v_texcoord0).r;
    float u = texture2D(s_texU, v_texcoord0).r - 0.5;
    float v = texture2D(s_texV, v_texcoord0).r - 0.5;
    vec3 rgb = vec3(y + 1.402 * v, y - 0.344 * u - 0.714 * v, y + 1.772 * u);
    // The previous RGBA8 intermediate clamped conversion before brightness.
    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0) * u_tintColor.rgb, 1.0);
}

$input v_texcoord0
#include <bgfx_shader.sh>

SAMPLER2D(s_texY, 0);
SAMPLER2D(s_texU, 1);
uniform vec4 u_videoColor0;
uniform vec4 u_videoColor1;
uniform vec4 u_videoColor2;
uniform vec4 u_tintColor;

void main()
{
    vec4 yuv = vec4(texture2D(s_texY, v_texcoord0).r,
                    texture2D(s_texU, v_texcoord0).rg, 1.0);
    vec3 rgb = vec3(dot(u_videoColor0, yuv), dot(u_videoColor1, yuv),
                    dot(u_videoColor2, yuv));
    // Match the intermediate's range before multiplying its sampled RGB.
    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0) * u_tintColor.rgb, 1.0);
}

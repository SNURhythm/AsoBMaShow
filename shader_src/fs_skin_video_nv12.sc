$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_texY, 0);
SAMPLER2D(s_texU, 1);
uniform vec4 u_videoColor0;
uniform vec4 u_videoColor1;
uniform vec4 u_videoColor2;

void main()
{
    vec4 yuv = vec4(texture2D(s_texY, v_texcoord0).r,
                    texture2D(s_texU, v_texcoord0).rg, 1.0);
    vec3 rgb = vec3(dot(u_videoColor0, yuv), dot(u_videoColor1, yuv),
                    dot(u_videoColor2, yuv));
    gl_FragColor = vec4(rgb, 1.0) * v_color0;
}

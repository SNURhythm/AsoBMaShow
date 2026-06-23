// fs_blurH.sc
$input v_texcoord0
#include "../bgfx_shader.sh"

SAMPLER2D(s_texColor, 0);
uniform vec4 u_texelSize; // x=1/width, y=1/height
uniform vec4 u_blurScale; // x=scale factor

void main()
{
    vec2 uv = v_texcoord0;
    vec2 step = vec2(u_texelSize.x * u_blurScale.x, 0.0);
    vec3 c  = vec3(0.0, 0.0, 0.0);

    c += texture2D(s_texColor, uv + step * -4.0).rgb * 0.05;
    c += texture2D(s_texColor, uv + step * -3.0).rgb * 0.09;
    c += texture2D(s_texColor, uv + step * -2.0).rgb * 0.12;
    c += texture2D(s_texColor, uv + step * -1.0).rgb * 0.15;
    c += texture2D(s_texColor, uv).rgb * 0.18;
    c += texture2D(s_texColor, uv + step * 1.0).rgb * 0.15;
    c += texture2D(s_texColor, uv + step * 2.0).rgb * 0.12;
    c += texture2D(s_texColor, uv + step * 3.0).rgb * 0.09;
    c += texture2D(s_texColor, uv + step * 4.0).rgb * 0.05;

    gl_FragColor = vec4(c, 1.0);
}

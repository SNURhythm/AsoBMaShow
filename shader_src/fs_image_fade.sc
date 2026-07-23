$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_imageFadeParams;
uniform vec4 u_imageScrimColor;

void main() {
    vec4 color = texture2D(s_texColor, v_texcoord0);
    float scrimAlpha = saturate(u_imageScrimColor.a);
    color.rgb = mix(color.rgb, u_imageScrimColor.rgb, scrimAlpha);
    float progress = saturate(
        dot(v_texcoord0, u_imageFadeParams.xy) + u_imageFadeParams.z);
    float strength = saturate(u_imageFadeParams.w);
    float alphaMultiplier = mix(1.0 - strength, 1.0, progress);
    color.a *= alphaMultiplier;
    gl_FragColor = color;
}

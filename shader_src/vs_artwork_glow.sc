$input a_position, a_texcoord0, a_texcoord1, a_color0
$output v_color0

#include <bgfx_shader.sh>

uniform vec4 u_artworkGlowRect;
uniform vec4 u_artworkGlowParams;
uniform vec4 u_artworkGlowSpectrum0;
uniform vec4 u_artworkGlowSpectrum1;
uniform vec4 u_artworkGlowSpectrum2;
uniform vec4 u_artworkGlowSpectrum3;

float artworkGlowBand(float band)
{
    if (band < 4.0) return u_artworkGlowSpectrum0[int(band)];
    if (band < 8.0) return u_artworkGlowSpectrum1[int(band) - 4];
    if (band < 12.0) return u_artworkGlowSpectrum2[int(band) - 8];
    return u_artworkGlowSpectrum3[int(band) - 12];
}

void main()
{
    float band = a_texcoord1.x;
    float wave = clamp(mix(artworkGlowBand(band),
        artworkGlowBand(min(band + 1.0, 15.0)), a_texcoord1.y), 0.0, 1.0);
    float opacity = wave * wave;
    float bass = clamp(u_artworkGlowParams.z, 0.0, 1.0);
    float extent = u_artworkGlowParams.y *
        (0.02 + 0.08 * bass + 0.90 * opacity * wave) * (0.75 + 0.25 * bass);
    float offset = mix(-1.0, extent, a_position.z);
    vec2 position = u_artworkGlowRect.xy + a_position.xy * u_artworkGlowRect.zw +
        a_texcoord0.xy * offset;
    gl_Position = mul(u_modelViewProj, vec4(position, 0.0, 1.0));

    float alpha = 0.87 * opacity * a_texcoord0.z +
        (0.10 * opacity + 0.025 * bass) * a_texcoord0.w;
    // Match the CPU mesh's rounded alpha, then UiBatchRenderer's truncation
    // when applying inherited opacity. The fragment shader remains fs_simple.
    float meshAlpha = floor(a_color0.a * u_artworkGlowParams.x * alpha * 255.0 + 0.5);
    v_color0 = vec4(a_color0.rgb,
        floor(meshAlpha * u_artworkGlowParams.w) / 255.0);
}

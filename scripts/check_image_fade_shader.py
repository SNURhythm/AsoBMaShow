#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


root = (
    pathlib.Path(sys.argv[1])
    if len(sys.argv) > 1
    else pathlib.Path(__file__).parent.parent
).resolve()
shader_path = root / "shader_src/fs_image_fade.sc"
shader = shader_path.read_text(encoding="utf-8") if shader_path.is_file() else ""

required = {
    "fade uniform": "uniform vec4 u_imageFadeParams",
    "scrim uniform": "uniform vec4 u_imageScrimColor",
    "image sampler": "SAMPLER2D(s_texColor, 0)",
    "scrim alpha clamp": "saturate(u_imageScrimColor.a)",
    "scrim rgb blend": "mix(color.rgb, u_imageScrimColor.rgb, scrimAlpha)",
    "direction progress": "dot(v_texcoord0, u_imageFadeParams.xy)",
    "offset progress": "+ u_imageFadeParams.z",
    "strength clamp": "saturate(u_imageFadeParams.w)",
    "alpha-only fade": "color.a *= alphaMultiplier",
    "preserved color output": "gl_FragColor = color",
}

failures = [label for label, fragment in required.items() if fragment not in shader]
if failures:
    for failure in failures:
        print(f"FAIL: image fade shader is missing {failure}", file=sys.stderr)
    raise SystemExit(1)

print("image fade shader audit passed")

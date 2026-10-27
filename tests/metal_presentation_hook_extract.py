#!/usr/bin/env python3
"""Extract the actual generated acquisition hook; never edit the submodule."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def extract(output):
    upstream = ROOT / 'bgfx/bgfx/src/renderer_mtl.cpp'
    original = upstream.read_text()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        source, patched, script = root / 'source.cpp', root / 'patched.cpp', root / 'patch.cmake'
        source.write_text(original)
        script.write_text(
            f'include("{ROOT}/cmake/BgfxMetalPresentation.cmake")\n'
            f'file(READ "{source}" content)\n'
            'asobmashow_patch_bgfx_metal_presentation(content)\n'
            f'file(WRITE "{patched}" "${{content}}")\n')
        command = ['cmake', '-P', str(script)]
        subprocess.run(command, check=True, capture_output=True)
        generated = patched.read_text()
        start = generated.index('const bool occluded =', generated.index('MTL::Texture* SwapChainMtl::currentDrawableTexture()'))
        end = generated.index('\n\t\t\tif (m_drawable != NULL)', start)
        hook = generated[start:end]
        assert 'addPresentedHandler' in hook, 'acquisition hook missing'
        # Outside the hook, all native acquire ownership/presentation code is
        # byte-for-byte unchanged. Init/destruction only gain reset prefixes.
        tail = '\n\t\t\tif (m_drawable != NULL)'
        assert generated[end:] == original[original.index(tail, original.index('MTL::Texture* SwapChainMtl::currentDrawableTexture()')):]
        present = 'presentCommandBuffer->presentDrawable( (MTL::Drawable*)frameBuffer.m_swapChain->m_drawable);'
        assert present in generated
        for anchor in ('#include "renderer_mtl.h"', 'm_drawable = occluded ? NULL : m_metalLayer->nextDrawable();',
                       'void SwapChainMtl::init(void* _nwh)', 'SwapChainMtl::~SwapChainMtl()',
                       'm_metalLayer->setMaximumDrawableCount(bx::clamp<uint32_t>('):
            source.write_text(original.replace(anchor, 'changed upstream anchor', 1))
            result = subprocess.run(command, capture_output=True, text=True)
            assert result.returncode != 0 and 'changed' in result.stderr, anchor
        assert upstream.read_text() == original, 'upstream changed'
        Path(output).write_text(hook)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    extract(parser.parse_args().output)

#!/usr/bin/env python3
"""Run common's real quad writers against bounded, guarded bgfx allocations.

The allocator reports short buffers but retains guard storage, so the old
unchecked writers fail assertions without corrupting the test process.
"""
from pathlib import Path
import subprocess
import tempfile
from support.fixture_compiler import FixtureCompiler

root = Path(__file__).resolve().parents[1]
source = (root / 'src/rendering/common.cpp').read_text()
header = (root / 'src/rendering/common.h').read_text()
fixture = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "rendering/UnitCircleSamples.h"
#define BGFX_INVALID_HANDLE {UINT16_MAX}
#define BGFX_STATE_MSAA 8
#define SDL_LOG_CATEGORY_RENDER 0
#define SHADER_SIMPLE "vs_simple.bin", "fs_simple.bin"
template<class... T> void SDL_LogWarn(int, const char *, T...) {}
#define BGFX_STATE_WRITE_RGB 1
#define BGFX_STATE_WRITE_A 2
#define BGFX_STATE_BLEND_ALPHA 4
#define SHADER_TEXT "vs_text.bin", "fs_text.bin"
namespace bgfx {
using ViewId = uint16_t;
struct TextureHandle { uint16_t idx; };
struct UniformHandle { uint16_t idx; };
struct ProgramHandle { uint16_t idx; };
template<class T> bool isValid(T h) { return h.idx != UINT16_MAX; }
struct VertexLayout { uint16_t stride = 20; uint16_t getStride() const { return stride; } };
struct TransientVertexBuffer { uint8_t *data = nullptr; uint32_t size = 0; };
struct TransientIndexBuffer { uint8_t *data = nullptr; uint32_t size = 0; };
alignas(16) std::array<uint8_t, 128> vb;
alignas(16) std::array<uint8_t, 32> ib;
uint32_t availableV = 4, availableI = 6, returnedV = 4, returnedI = 6;
bool nullData = false;
int allocations = 0, bindings = 0, submissions = 0;
uint32_t getAvailTransientVertexBuffer(uint32_t n, const VertexLayout &) { return std::min(n, availableV); }
uint32_t getAvailTransientIndexBuffer(uint32_t n) { return std::min(n, availableI); }
void allocTransientVertexBuffer(TransientVertexBuffer *out, uint32_t n, const VertexLayout &layout) {
  ++allocations; *out = {nullData ? nullptr : vb.data(), std::min(n, returnedV) * layout.stride};
}
void allocTransientIndexBuffer(TransientIndexBuffer *out, uint32_t n) {
  ++allocations; *out = {nullData ? nullptr : ib.data(), std::min(n, returnedI) * 2};
}
void setVertexBuffer(int, const TransientVertexBuffer *) { ++bindings; }
void setIndexBuffer(const TransientIndexBuffer *) { ++bindings; }
void setTexture(int, UniformHandle, TextureHandle) {}
void setState(uint64_t) {}
void setUniform(UniformHandle, const void *) {}
void setScissor() {}
void submit(ViewId, ProgramHandle, uint32_t = 0) { ++submissions; }
void setTransform(const float *) {}
}
namespace bx { void memCopy(void *to, const void *from, size_t n) { std::memcpy(to, from, n); } }
namespace rendering {
constexpr bgfx::ViewId main_view = 128;
struct PosTexCoord0Vertex { static bgfx::VertexLayout ms_decl; float x,y,z,u,v; };
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
struct PosColorVertex { static bgfx::VertexLayout ms_decl; float x,y,z; uint32_t abgr; };
bgfx::VertexLayout PosColorVertex::ms_decl{16};
int window_width = 100, window_height = 50;
void setScissorUI(int,int,int,int) {}
struct UniformCache {
  static UniformCache &getInstance() { static UniformCache v; return v; }
  bgfx::UniformHandle getSampler(const char *) { return {1}; }
  bgfx::UniformHandle getVec4(const char *) { return {2}; }
};
struct ShaderManager {
  static ShaderManager &getInstance() { static ShaderManager v; return v; }
  bgfx::ProgramHandle getProgram(const char *, const char *) { return {1}; }
};
'''
# Preserve declarations as well as function bodies, including newly shared
# allocation guards; discard unrelated SDL texture upload and UI geometry.
fixture += header[header.index('bool createRect(') if 'bool createRect(' in header else header.index('void createRect('):]
fixture += '\nnamespace rendering {\n'
fixture += source[source.index('bool createRect(') if 'bool createRect(' in source else source.index('void createRect('):source.index('bgfx::TextureHandle sdlSurfaceToBgfxTexture(')]
fixture += source[source.index('bool renderFullscreenTexture(') if 'bool renderFullscreenTexture(' in source else source.index('void renderFullscreenTexture('):]
submission_header = (root / 'src/rendering/FrameSubmission.h').read_text()
fixture += submission_header.replace('#pragma once', '')
fixture += '\nnamespace rendering::detail { extern "C" bool asobmashow_bgfx_frame_draws_accepted() { return true; } }\n'
if 'class FrameSubmission' not in submission_header:
    fixture += '\nnamespace rendering { struct FrameSubmission { bool accepted() const { return frameDrawsAccepted(); } }; }\n'
for batch in ('SimpleBatchRenderer', 'TexBatchRenderer'):
    for suffix in ('h', 'cpp'):
        batch_source = (root / f'src/rendering/{batch}.{suffix}').read_text()
        fixture += '\n'.join(line for line in batch_source.splitlines()
                             if not line.startswith(('#include', '#pragma once')))
        # Separate the two implementation-local constants in this single TU.
        fixture = fixture.replace('kMaxBatchVertices', f'{batch}MaxVertices')
        fixture = fixture.replace('kMaxBatchIndices', f'{batch}MaxIndices')
        fixture += '\n'
fixture += r'''
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template<class F> bool invoke(F f) {
  if constexpr (std::is_void_v<decltype(f())>) { f(); return true; }
  else return f();
}
int main(int argc, char **argv) {
  const std::string scenario = argv[1];
  if (scenario == "legacy-batches") {
    rendering::SimpleBatchRenderer simple;
    rendering::TexBatchRenderer textured;
    for (int kind = 0; kind < 2; ++kind) {
      for (int failure = 0; failure < 5; ++failure) {
        bgfx::availableV = failure == 0 ? 2 : 4;
        bgfx::availableI = failure == 1 ? 3 : 6;
        bgfx::returnedV = failure == 2 ? 2 : 4;
        bgfx::returnedI = failure == 3 ? 3 : 6;
        bgfx::nullData = failure == 4;
        bgfx::vb.fill(0xcd); bgfx::ib.fill(0xcd);
        bgfx::submissions = 0;
        const rendering::FrameSubmission failedFrame;
        if (kind == 0) {
          simple.begin(); simple.addRect(0, 0, 1, 1, 0xffffffff); simple.end();
          simple.begin(); simple.end();
        } else {
          textured.begin(); textured.addRect(0, 0, 1, 1, 1, 1, {1}); textured.end();
          textured.begin(); textured.end();
        }
        require(bgfx::submissions == 0, "failed batch submitted geometry");
        require(std::all_of(bgfx::vb.begin(), bgfx::vb.end(), [](auto b) { return b == 0xcd; }) &&
                std::all_of(bgfx::ib.begin(), bgfx::ib.end(), [](auto b) { return b == 0xcd; }),
                "failed legacy batch wrote incomplete storage");
        require(!failedFrame.accepted(), "discarded legacy batch acknowledged as complete");
        bgfx::availableV = 4; bgfx::availableI = 6;
        bgfx::returnedV = 4; bgfx::returnedI = 6; bgfx::nullData = false;
        const rendering::FrameSubmission recoveredFrame;
        if (kind == 0) {
          simple.begin(); simple.addRect(0, 0, 1, 1, 0xffffffff); simple.end();
        } else {
          textured.begin(); textured.addRect(0, 0, 1, 1, 1, 1, {1}); textured.end();
        }
        require(recoveredFrame.accepted() && bgfx::submissions == 1,
                "legacy batch did not recover on a fresh frame");
        require(!failedFrame.accepted(), "later success erased an earlier frame failure");
      }
    }
    return 0;
  }
  for (int helper = 0; helper != 7; ++helper) {
    bgfx::vb.fill(0xcd); bgfx::ib.fill(0xcd);
    bgfx::allocations = bgfx::bindings = bgfx::submissions = 0;
    bgfx::availableV = bgfx::returnedV = 4;
    bgfx::availableI = bgfx::returnedI = 6;
    bgfx::nullData = scenario == "null";
    if (scenario == "vertex-preflight") bgfx::availableV = bgfx::returnedV = 2;
    if (scenario == "index-preflight") bgfx::availableI = bgfx::returnedI = 3;
    if (scenario == "vertex-short") bgfx::returnedV = 2;
    if (scenario == "index-short") bgfx::returnedI = 3;
    bgfx::TransientVertexBuffer vertices{}; bgfx::TransientIndexBuffer indices{};
    bool result = true;
    switch (helper) {
    case 0: result = invoke([&] { return rendering::createRect(vertices, indices, 0, 0, 100, 50, 0xff00ff00); }); break;
    case 1: result = invoke([&] { return rendering::screenSpaceQuad(); }); break;
    case 2: result = invoke([&] { return rendering::renderTextureRegion({1}, 0, 0, 0, 100, 50); }); break;
    case 3: result = invoke([&] { return rendering::renderTextureRegionTint({1}, 0, 0, 0, 100, 50, 0.5f); }); break;
    case 4: result = invoke([&] { return rendering::renderFullscreenTexture({1}, 0); }); break;
    case 5: result = invoke([&] { return rendering::renderFullscreenTextureTint({1}, 0, 0.5f); }); break;
    case 6: result = invoke([&] { return rendering::renderTextureRegionScissor({1}, 0, 0, 0, 100, 50); }); break;
    }
    if (scenario == "success") {
      require(result && bgfx::allocations == 2, "complete quad rejected");
      require(bgfx::vb[0] != 0xcd && bgfx::ib[0] == 0, "complete quad not written");
      require(bgfx::ib[12] == 0xcd && bgfx::vb[80] == 0xcd, "complete quad crossed guard");
      require(bgfx::submissions == (helper >= 2 ? 1 : 0), "complete quad submission changed");
    } else {
      require(std::all_of(bgfx::vb.begin(), bgfx::vb.end(), [](auto b) { return b == 0xcd; }) &&
              std::all_of(bgfx::ib.begin(), bgfx::ib.end(), [](auto b) { return b == 0xcd; }),
              "incomplete allocation was written");
      require(!result && bgfx::bindings == 0 && bgfx::submissions == 0, "incomplete quad accepted");
      if (scenario.find("preflight") != std::string::npos)
        require(bgfx::allocations == 0, "preflight failure consumed transient storage");
    }
  }
}
'''
compiler = FixtureCompiler.from_environment()
with tempfile.TemporaryDirectory(prefix='rendering-transient-boundary-') as directory:
    cpp = Path(directory) / 'boundary.cpp'
    binary = Path(directory) / ('boundary' + compiler.executable_suffix)
    cpp.write_text(fixture)
    try:
        compiler.build([cpp], binary, directory, includes=[root / 'src'])
    except subprocess.CalledProcessError as error:
        print(error.stderr)
        raise
    # null-data case is safe only once the returned-size guards exist.
    for scenario in ('vertex-preflight', 'index-preflight', 'vertex-short', 'index-short', 'null', 'success', 'legacy-batches'):
        subprocess.run([str(binary), scenario], check=True)
print('Common transient allocation boundaries passed')

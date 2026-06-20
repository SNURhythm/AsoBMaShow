#pragma once
#include "BgfxRAII.h"
#include "../RAII.h"
#include <bgfx/bgfx.h>
#include <stdexcept>
#include <string>
#include <SDL2/SDL.h>
#include <unordered_map>
#include <filesystem>
#include <cstdint>
#include <limits>
namespace rendering {
// singleton
class ShaderManager {
private:
  std::unordered_map<std::string, bgfx::ProgramHandle> programMap;

  ShaderManager() {} // Private constructor
  bgfx::ShaderHandle loadShader(const std::string &FILENAME) {
    std::filesystem::path shaderPath;
    switch (bgfx::getRendererType()) {
    case bgfx::RendererType::Noop:
    case bgfx::RendererType::Direct3D11:
    case bgfx::RendererType::Direct3D12:
      shaderPath = "./shaders/dx11/";
      break;
    case bgfx::RendererType::Gnm:
      shaderPath = "./shaders/pssl/";
      break;
    case bgfx::RendererType::Metal:
      shaderPath = "./shaders/metal/";
      break;
    case bgfx::RendererType::OpenGL:
      shaderPath = "./shaders/glsl/";
      break;
    case bgfx::RendererType::OpenGLES:
      shaderPath = "./shaders/essl/";
      break;
    case bgfx::RendererType::Vulkan:
      shaderPath = "./shaders/spirv/";
      break;
    default:
      throw std::runtime_error("Unknown renderer type");
    }

    std::string path = (shaderPath / FILENAME).string();
    UniqueResource<SDL_RWops, SDL_RWclose> rw(
        SDL_RWFromFile(path.c_str(), "rb"));
    if (rw == nullptr) {
      throw std::runtime_error("Failed to open shader file: " + path);
    }
    const Sint64 fileSize = SDL_RWsize(rw.get());
    if (fileSize <= 0 ||
        fileSize > static_cast<Sint64>(std::numeric_limits<uint32_t>::max())) {
      throw std::runtime_error("Invalid shader file size: " + path);
    }
    const auto size = static_cast<uint32_t>(fileSize);
    UniqueResource<void, SDL_free> data(SDL_malloc(size));
    if (data == nullptr) {
      throw std::runtime_error("Failed to allocate shader buffer: " + path);
    }
    if (SDL_RWread(rw.get(), data.get(), 1, size) != size) {
      throw std::runtime_error("Failed to read shader file: " + path);
    }
    auto shader = bgfx::createShader(bgfx::copy(data.get(), size));
    if (!bgfx::isValid(shader)) {
      throw std::runtime_error("Failed to create shader: " + path);
    }
    return shader;
  }

public:
  static ShaderManager &getInstance() {
    static ShaderManager instance;
    return instance;
  }

  ShaderManager(const ShaderManager &) = delete;
  void operator=(const ShaderManager &) = delete;

  bgfx::ProgramHandle getProgram(const std::string &vs, const std::string &fs) {
    std::string name;
    name.reserve(vs.size() + fs.size() + 1);
    name.append(vs);
    name.push_back('_');
    name.append(fs);

    if (const auto it = programMap.find(name); it != programMap.end()) {
      return it->second;
    }

    BgfxHandleGuard<bgfx::ShaderHandle> vsh(loadShader(vs));
    BgfxHandleGuard<bgfx::ShaderHandle> fsh(loadShader(fs));
    BgfxHandleGuard<bgfx::ProgramHandle> program(
        bgfx::createProgram(vsh.get(), fsh.get(), true));
    if (!bgfx::isValid(program.get())) {
      throw std::runtime_error("Failed to create shader program: " + name);
    }

    vsh.release();
    fsh.release();
    const bgfx::ProgramHandle result = program.get();
    programMap.emplace(std::move(name), result);
    program.release();
    return result;
  }

  void preloadProgram(const std::string &vs, const std::string &fs) {
    getProgram(vs, fs);
  }

  void release() {
    for (auto &program : programMap) {
      bgfx::destroy(program.second);
    }
    programMap.clear();
  }
};
} // namespace rendering

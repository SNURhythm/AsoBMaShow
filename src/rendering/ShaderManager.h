#pragma once
#include <bgfx/bgfx.h>
#include <stdexcept>
#include <string>
#include <SDL2/SDL.h>
#include <unordered_map>
#include <filesystem>
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
    SDL_RWops *rw = SDL_RWFromFile(path.c_str(), "rb");
    if (rw == nullptr) {
      throw std::runtime_error("Failed to open shader file: " + path);
    }
    size_t size = SDL_RWsize(rw);
    void *data = SDL_malloc(size);
    if (SDL_RWread(rw, data, 1, size) != size) {
      SDL_free(data);
      SDL_RWclose(rw);
      throw std::runtime_error("Failed to read shader file: " + path);
    }
    SDL_RWclose(rw);
    return bgfx::createShader(bgfx::copy(data, size));
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

    bgfx::ShaderHandle vsh = loadShader(vs);
    bgfx::ShaderHandle fsh = loadShader(fs);
    const bgfx::ProgramHandle program = bgfx::createProgram(vsh, fsh, true);
    programMap.emplace(std::move(name), program);
    return program;
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

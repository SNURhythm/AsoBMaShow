#pragma once
#include "BgfxRAII.h"
#include "../RAII.h"
#include <bgfx/bgfx.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <SDL3/SDL.h>
#include <unordered_map>
#include <filesystem>
#include <cstdint>
#include <limits>
namespace rendering {
// singleton
class ShaderManager {
private:
  struct ProgramKeyView {
    std::string_view vs;
    std::string_view fs;
    bool operator==(const ProgramKeyView &) const = default;
  };
  struct ProgramKey {
    std::string vs;
    std::string fs;
    operator ProgramKeyView() const noexcept { return {vs, fs}; }
  };
  struct ProgramKeyHash {
    using is_transparent = void;
    std::size_t operator()(ProgramKeyView key) const noexcept {
      const auto vertex = std::hash<std::string_view>{}(key.vs);
      const auto fragment = std::hash<std::string_view>{}(key.fs);
      return vertex ^ (fragment + 0x9e3779b9U + (vertex << 6U) + (vertex >> 2U));
    }
  };
  struct ProgramKeyEqual {
    using is_transparent = void;
    bool operator()(ProgramKeyView left, ProgramKeyView right) const noexcept {
      return left == right;
    }
  };
  std::unordered_map<ProgramKey, bgfx::ProgramHandle, ProgramKeyHash,
                     ProgramKeyEqual> programMap;

  ShaderManager() {} // Private constructor
  bgfx::ShaderHandle loadShader(std::string_view FILENAME) {
    std::filesystem::path shaderPath;
    switch (bgfx::getRendererType()) {
    case bgfx::RendererType::Noop:
    case bgfx::RendererType::Direct3D11:
    case bgfx::RendererType::Direct3D12:
      shaderPath = "shaders/dx11/";
      break;
    case bgfx::RendererType::Gnm:
      shaderPath = "shaders/pssl/";
      break;
    case bgfx::RendererType::Metal:
      shaderPath = "shaders/metal/";
      break;
    case bgfx::RendererType::OpenGL:
      shaderPath = "shaders/glsl/";
      break;
    case bgfx::RendererType::OpenGLES:
      shaderPath = "shaders/essl/";
      break;
    case bgfx::RendererType::Vulkan:
      shaderPath = "shaders/spirv/";
      break;
    default:
      throw std::runtime_error("Unknown renderer type");
    }

    std::string path = (shaderPath / FILENAME).string();
    UniqueResource<SDL_IOStream, SDL_CloseIO> rw(
        SDL_IOFromFile(path.c_str(), "rb"));
    if (rw == nullptr) {
      throw std::runtime_error("Failed to open shader file: " + path);
    }
    const Sint64 fileSize = SDL_GetIOSize(rw.get());
    if (fileSize <= 0 ||
        fileSize > static_cast<Sint64>(std::numeric_limits<uint32_t>::max())) {
      throw std::runtime_error("Invalid shader file size: " + path);
    }
    const auto size = static_cast<uint32_t>(fileSize);
    UniqueResource<void, SDL_free> data(SDL_malloc(size));
    if (data == nullptr) {
      throw std::runtime_error("Failed to allocate shader buffer: " + path);
    }
    if (SDL_ReadIO(rw.get(), data.get(), size) != size) {
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

  bgfx::ProgramHandle getProgram(std::string_view vs, std::string_view fs) {
    if (const auto it = programMap.find(ProgramKeyView{vs, fs});
        it != programMap.end()) {
      return it->second;
    }

    BgfxHandleGuard<bgfx::ShaderHandle> vsh(loadShader(vs));
    BgfxHandleGuard<bgfx::ShaderHandle> fsh(loadShader(fs));
    BgfxHandleGuard<bgfx::ProgramHandle> program(
        bgfx::createProgram(vsh.get(), fsh.get(), true));
    if (!bgfx::isValid(program.get())) {
      throw std::runtime_error("Failed to create shader program: " +
                               std::string(vs) + " / " + std::string(fs));
    }

    vsh.release();
    fsh.release();
    const bgfx::ProgramHandle result = program.get();
    programMap.emplace(ProgramKey{std::string(vs), std::string(fs)}, result);
    program.release();
    return result;
  }

  void preloadProgram(std::string_view vs, std::string_view fs) {
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

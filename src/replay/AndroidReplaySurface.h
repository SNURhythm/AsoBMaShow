#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>
}

#include <cstdint>
#include <string>

namespace replay_video_export {

// Bridges the existing BGRA readback queue to MediaCodec's Surface input.
// EGL handles RGB -> encoder color conversion; no CPU YUV buffer is needed.
// The private context is restored after each operation, allowing setup, frame
// submission and cleanup on different threads without disturbing bgfx's EGL.
class AndroidReplaySurface {
public:
  AndroidReplaySurface() = default;
  ~AndroidReplaySurface() { reset(); }
  AndroidReplaySurface(const AndroidReplaySurface &) = delete;
  AndroidReplaySurface &operator=(const AndroidReplaySurface &) = delete;

  bool prepare(AVCodecContext *codec, std::string &error) {
    reset();
    AVDictionary *options = nullptr;
    av_dict_set(&options, "create_window", "1", 0);
    const int result = av_hwdevice_ctx_create(
        &device_, AV_HWDEVICE_TYPE_MEDIACODEC, nullptr, options, 0);
    av_dict_free(&options);
    if (result < 0 || !device_) return fail(error, "create MediaCodec device");
    auto *deviceContext = reinterpret_cast<AVHWDeviceContext *>(device_->data);
    auto *mediaContext = static_cast<AVMediaCodecDeviceContext *>(deviceContext->hwctx);
    if (!mediaContext->native_window)
      return fail(error, "create MediaCodec input Surface");
    codec->hw_device_ctx = av_buffer_ref(device_);
    return codec->hw_device_ctx != nullptr || fail(error, "retain MediaCodec device");
  }

  bool initialize(const AVCodecContext *codec, std::string &error) {
    if (!device_ || codec->width <= 0 || codec->height <= 0)
      return fail(error, "initialize encoder Surface dimensions");
    width_ = codec->width;
    height_ = codec->height;
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr))
      return fail(error, "initialize EGL display");
    presentationTime_ = reinterpret_cast<PFNEGLPRESENTATIONTIMEANDROIDPROC>(
        eglGetProcAddress("eglPresentationTimeANDROID"));
    if (!presentationTime_) return fail(error, "find EGL presentation timestamps");
    constexpr EGLint recordableAndroid = 0x3142;
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        recordableAndroid, EGL_TRUE, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display_, attributes, &config, 1, &count) || count < 1)
      return fail(error, "choose recordable EGL configuration");
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttributes);
    if (context_ == EGL_NO_CONTEXT) return fail(error, "create encoder EGL context");
    auto *deviceContext = reinterpret_cast<AVHWDeviceContext *>(device_->data);
    auto *mediaContext = static_cast<AVMediaCodecDeviceContext *>(deviceContext->hwctx);
    surface_ = eglCreateWindowSurface(display_, config,
        static_cast<ANativeWindow *>(mediaContext->native_window), nullptr);
    if (surface_ == EGL_NO_SURFACE) return fail(error, "create encoder EGL Surface");
    CurrentContext current(display_, surface_, context_);
    if (!current.valid) return fail(error, "bind encoder EGL context");
    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (width_ > maxTextureSize || height_ > maxTextureSize)
      return fail(error, "fit replay frame in an encoder texture");
    GLint precisionRange[2] = {};
    GLint precisionBits = 0;
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT,
                                precisionRange, &precisionBits);
    if (precisionBits < 16)
      return fail(error, "provide precise encoder texture coordinates");
    constexpr const char *vertexSource =
        "attribute vec2 position; attribute vec2 texcoord; varying highp vec2 uv;"
        "void main(){gl_Position=vec4(position,0.0,1.0);uv=texcoord;}";
    constexpr const char *fragmentSource =
        "precision highp float; varying highp vec2 uv; uniform sampler2D pixels;"
        "void main(){gl_FragColor=vec4(texture2D(pixels,uv).bgr,1.0);}";
    const GLuint vertex = compile(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
      glDeleteShader(vertex);
      glDeleteShader(fragment);
      return fail(error, "compile encoder texture shaders");
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glBindAttribLocation(program_, 0, "position");
    glBindAttribLocation(program_, 1, "texcoord");
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) return fail(error, "link encoder texture shaders");
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    return glGetError() == GL_NO_ERROR || fail(error, "allocate encoder texture");
  }

  bool submit(const uint8_t *bgra, int64_t timestampNanos, std::string &error) {
    if (!bgra || !texture_ || timestampNanos < 0)
      return fail(error, "submit encoder frame");
    CurrentContext current(display_, surface_, context_);
    if (!current.valid) return fail(error, "bind encoder EGL context");
    glViewport(0, 0, width_, height_);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_,
                    GL_RGBA, GL_UNSIGNED_BYTE, bgra);
    // Input rows start at the top; GL's framebuffer origin is bottom-left.
    constexpr GLfloat vertices[] = {
        -1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, 1, 1, 1, 0};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (glGetError() != GL_NO_ERROR) return fail(error, "draw encoder frame");
    if (!presentationTime_(display_, surface_, timestampNanos))
      return fail(error, "set encoder frame timestamp");
    return eglSwapBuffers(display_, surface_) || fail(error, "submit encoder Surface");
  }

  void reset() {
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
      {
        CurrentContext current(display_, surface_, context_);
        if (current.valid) {
          glDeleteTextures(1, &texture_);
          glDeleteProgram(program_);
        }
      }
      eglDestroyContext(display_, context_);
    }
    if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE)
      eglDestroySurface(display_, surface_);
    // Do not terminate the default display: bgfx may be sharing it.
    display_ = EGL_NO_DISPLAY;
    context_ = EGL_NO_CONTEXT;
    surface_ = EGL_NO_SURFACE;
    program_ = texture_ = 0;
    presentationTime_ = nullptr;
    av_buffer_unref(&device_);
  }

private:
  struct CurrentContext {
    EGLDisplay previousDisplay = eglGetCurrentDisplay();
    EGLSurface previousDraw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface previousRead = eglGetCurrentSurface(EGL_READ);
    EGLContext previousContext = eglGetCurrentContext();
    EGLDisplay display;
    bool valid;
    CurrentContext(EGLDisplay targetDisplay, EGLSurface surface, EGLContext context)
        : display(targetDisplay),
          valid(eglMakeCurrent(targetDisplay, surface, surface, context)) {}
    ~CurrentContext() {
      if (!valid) return;
      if (previousDisplay != EGL_NO_DISPLAY)
        eglMakeCurrent(previousDisplay, previousDraw, previousRead, previousContext);
      else
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
  };

  static bool fail(std::string &error, const char *operation) {
    error = std::string("Failed to ") + operation;
    return false;
  }

  static GLuint compile(GLenum type, const char *source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled) return shader;
    glDeleteShader(shader);
    return 0;
  }

  AVBufferRef *device_ = nullptr;
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
  PFNEGLPRESENTATIONTIMEANDROIDPROC presentationTime_ = nullptr;
  GLuint program_ = 0;
  GLuint texture_ = 0;
  int width_ = 0;
  int height_ = 0;
};

} // namespace replay_video_export

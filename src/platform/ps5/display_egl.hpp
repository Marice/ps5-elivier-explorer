// ps5-homebrew-ui - EGL display and OpenGL 4.6 Core context.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <EGL/egl.h>

#include <string>

namespace hui::ps5
{

// What the window surface should be, for the console probes that ask whether the driver offers a
// 10-bit or half-float surface in an HDR colour space (docs/decision-log.md, D44). The default is
// what the app has always used: 8 bits per channel, no colour space attribute (the PS5 backend
// owns the window and takes a NULL attribute list).
struct SurfaceRequest
{
    int red = 8;
    int green = 8;
    int blue = 8;
    int alpha = 8;
    bool float_pixels = false; // EGL_EXT_pixel_format_float
    EGLint colour_space = 0;   // EGL_GL_COLORSPACE_KHR value; 0 leaves the attribute out

    bool is_default() const
    {
        return red == 8 && green == 8 && blue == 8 && alpha == 8 && !float_pixels &&
               colour_space == 0;
    }
};

// Owns the EGL display, window surface and GL 4.6 Core context. ps5-opengl
// presents fullscreen; with runtime display modes the size is chosen before
// EGL starts, so changing it means close() and open() again. Every GL object
// dies with the context and must be recreated by its owner.
class Display
{
  public:
    Display() = default;
    Display(const Display &) = delete;
    Display &operator=(const Display &) = delete;
    ~Display();

    // Opens at width x height (1920x1080, 2560x1440 or 3840x2160) when the
    // SDK supports runtime display modes, else at the SDK's fixed profile.
    bool open(int width = 1920, int height = 1080);
    // The same, with a surface other than the default. A request that is not the default has no
    // depth or stencil buffer (the probes only clear and swap), which lets more configs match.
    bool open(int width, int height, const SurfaceRequest &request);
    bool swap();
    void close();
    // True when the SDK can change the display size at runtime.
    static bool supports_display_modes();

    int width() const
    {
        return width_;
    }
    int height() const
    {
        return height_;
    }
    EGLint last_error() const
    {
        return last_error_;
    }
    // After open(): the config that was chosen ("rgba=10/10/10/2 float=0") and the colour space
    // the surface reports (0 when the driver does not say).
    const std::string &config_info() const
    {
        return config_info_;
    }
    EGLint surface_colour_space() const
    {
        return surface_colour_space_;
    }

  private:
    void fail(const char *operation);

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    int width_ = 0;
    int height_ = 0;
    EGLint last_error_ = EGL_SUCCESS;
    std::string config_info_;
    EGLint surface_colour_space_ = 0;
};

const char *egl_error_name(EGLint error);

} // namespace hui::ps5

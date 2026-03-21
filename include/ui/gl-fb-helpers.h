/*
 * QEMU OpenGL framebuffer helpers (EGL-free, for macOS CGL path).
 * Derived from egl-helpers.h.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef QEMU_GL_FB_HELPERS_H
#define QEMU_GL_FB_HELPERS_H

#include <epoxy/gl.h>

typedef struct gl_fb {
    int width;
    int height;
    int x;
    int y;
    GLuint texture;
    GLuint framebuffer;
    bool delete_texture;
} gl_fb;

#define GL_FB_INIT { 0, }

void gl_fb_destroy(gl_fb *fb);
void gl_fb_setup_default(gl_fb *fb, int width, int height, int x, int y);
void gl_fb_setup_for_tex(gl_fb *fb, int width, int height,
                         GLuint texture, bool delete_texture);
void gl_fb_setup_new_tex(gl_fb *fb, int width, int height);
void gl_fb_blit(gl_fb *dst, gl_fb *src, bool flip);

#endif /* QEMU_GL_FB_HELPERS_H */

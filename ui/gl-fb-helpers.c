/*
 * QEMU OpenGL framebuffer helpers (EGL-free, for macOS CGL path).
 * Derived from egl-helpers.c.
 *
 * Copyright (C) 2015-2016 Gerd Hoffmann <kraxel@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "ui/gl-fb-helpers.h"

static void gl_fb_delete_texture(gl_fb *fb)
{
    if (!fb->delete_texture) {
        return;
    }

    glDeleteTextures(1, &fb->texture);
    fb->delete_texture = false;
}

void gl_fb_destroy(gl_fb *fb)
{
    if (!fb->framebuffer) {
        return;
    }

    gl_fb_delete_texture(fb);
    glDeleteFramebuffers(1, &fb->framebuffer);

    fb->width = 0;
    fb->height = 0;
    fb->x = 0;
    fb->y = 0;
    fb->texture = 0;
    fb->framebuffer = 0;
}

void gl_fb_setup_default(gl_fb *fb, int width, int height, int x, int y)
{
    fb->width = width;
    fb->height = height;
    fb->x = x;
    fb->y = y;
    fb->framebuffer = 0;
}

void gl_fb_setup_for_tex(gl_fb *fb, int width, int height,
                         GLuint texture, bool delete_texture)
{
    gl_fb_delete_texture(fb);

    fb->width = width;
    fb->height = height;
    fb->texture = texture;
    fb->delete_texture = delete_texture;
    if (!fb->framebuffer) {
        glGenFramebuffers(1, &fb->framebuffer);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fb->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, fb->texture, 0);
}

void gl_fb_setup_new_tex(gl_fb *fb, int width, int height)
{
    GLuint texture;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height,
                 0, GL_BGRA, GL_UNSIGNED_BYTE, 0);

    gl_fb_setup_for_tex(fb, width, height, texture, true);
}

void gl_fb_blit(gl_fb *dst, gl_fb *src, bool flip)
{
    GLuint x1 = 0;
    GLuint y1 = flip ? src->height : 0;
    GLuint x2 = src->width;
    GLuint y2 = flip ? 0 : src->height;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->framebuffer);
    glViewport(0, 0, dst->width, dst->height);
    glClear(GL_COLOR_BUFFER_BIT);

    glBlitFramebuffer(x1, y1, x2, y2,
                      dst->x, dst->y,
                      dst->x + dst->width, dst->y + dst->height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "trace.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "hw/virtio/virtio-iommu.h"
#include "ui/rect.h"

#include <glib/gmem.h>
#include <rutabaga_gfx/rutabaga_gfx_ffi.h>

#define CHECK(condition, cmd)                                                 \
    do {                                                                      \
        if (!(condition)) {                                                   \
            error_report("CHECK failed in %s() %s:" "%d", __func__,           \
                         __FILE__, __LINE__);                                 \
            (cmd)->error = VIRTIO_GPU_RESP_ERR_UNSPEC;                        \
            return;                                                           \
       }                                                                      \
    } while (0)

struct rutabaga_aio_data {
    struct VirtIOGPURutabaga *vr;
    struct rutabaga_fence fence;
};

static int rutabaga_find_mapping_slot(VirtIOGPURutabaga *vr, uint32_t resource_id)
{
    uint32_t slot;

    for (slot = 0; slot < MAX_SLOTS; slot++) {
        if (vr->memory_regions[slot].used &&
            vr->memory_regions[slot].resource_id == resource_id) {
            return slot;
        }
    }

    return -1;
}

static void rutabaga_release_mapping_slot(VirtIOGPUBase *vb,
                                          VirtIOGPURutabaga *vr,
                                          uint32_t slot)
{
    MemoryRegion *mr = &vr->memory_regions[slot].mr;

    memory_region_del_subregion(&vb->hostmem, mr);
    vr->memory_regions[slot].resource_id = 0;
    vr->memory_regions[slot].used = 0;
}

static void
virtio_gpu_rutabaga_update_cursor(VirtIOGPU *g, struct virtio_gpu_scanout *s,
                                  uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;
    struct rutabaga_transfer transfer = { 0 };
    struct iovec transfer_iovec;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    res = virtio_gpu_find_resource(g, resource_id);
    if (!res) {
        return;
    }

    if (res->width != s->current_cursor->width ||
        res->height != s->current_cursor->height) {
        return;
    }

    transfer.x = 0;
    transfer.y = 0;
    transfer.z = 0;
    transfer.w = res->width;
    transfer.h = res->height;
    transfer.d = 1;

    transfer_iovec.iov_base = s->current_cursor->data;
    transfer_iovec.iov_len = res->width * res->height * 4;

    rutabaga_resource_transfer_read(vr->rutabaga, 0,
                                    resource_id, &transfer,
                                    &transfer_iovec);
}

static void
virtio_gpu_rutabaga_gl_flushed(VirtIOGPUBase *b)
{
    VirtIOGPU *g = VIRTIO_GPU(b);
    virtio_gpu_process_cmdq(g);
}

static void
rutabaga_cmd_create_resource_2d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct rutabaga_create_3d rc_3d = { 0 };
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_create_2d c2d;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(c2d);
    trace_virtio_gpu_cmd_res_create_2d(c2d.resource_id, c2d.format,
                                       c2d.width, c2d.height);

    rc_3d.target = 2;
    rc_3d.format = c2d.format;
    rc_3d.bind = (1 << 1);
    rc_3d.width = c2d.width;
    rc_3d.height = c2d.height;
    rc_3d.depth = 1;
    rc_3d.array_size = 1;
    rc_3d.last_level = 0;
    rc_3d.nr_samples = 0;
    rc_3d.flags = VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP;

    result = rutabaga_resource_create_3d(vr->rutabaga, c2d.resource_id, &rc_3d);
    CHECK(!result, cmd);

    res = g_new0(struct virtio_gpu_simple_resource, 1);
    res->dmabuf_fd = -1;
    res->width = c2d.width;
    res->height = c2d.height;
    res->format = c2d.format;
    res->resource_id = c2d.resource_id;

    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
}

static void
rutabaga_cmd_create_resource_3d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct rutabaga_create_3d rc_3d = { 0 };
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_create_3d c3d;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(c3d);

    trace_virtio_gpu_cmd_res_create_3d(c3d.resource_id, c3d.format,
                                       c3d.width, c3d.height, c3d.depth);

    rc_3d.target = c3d.target;
    rc_3d.format = c3d.format;
    rc_3d.bind = c3d.bind;
    rc_3d.width = c3d.width;
    rc_3d.height = c3d.height;
    rc_3d.depth = c3d.depth;
    rc_3d.array_size = c3d.array_size;
    rc_3d.last_level = c3d.last_level;
    rc_3d.nr_samples = c3d.nr_samples;
    rc_3d.flags = c3d.flags;

    result = rutabaga_resource_create_3d(vr->rutabaga, c3d.resource_id, &rc_3d);
    CHECK(!result, cmd);

    res = g_new0(struct virtio_gpu_simple_resource, 1);
    res->dmabuf_fd = -1;
    res->width = c3d.width;
    res->height = c3d.height;
    res->format = c3d.format;
    res->resource_id = c3d.resource_id;

    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
}

static void
virtio_gpu_rutabaga_resource_unref(VirtIOGPU *g,
                                   struct virtio_gpu_simple_resource *res,
                                   Error **errp)
{
    int i;
    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    int32_t result;
    Error *local_err = NULL;
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);
    int slot = rutabaga_find_mapping_slot(vr, res->resource_id);

    if (res->scanout_bitmask) {
        for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
            if (res->scanout_bitmask & (1 << i)) {
                virtio_gpu_disable_scanout(g, i);
            }
        }
    }

    if (slot >= 0) {
        rutabaga_release_mapping_slot(vb, vr, slot);
        res->blob = NULL;

        result = rutabaga_resource_unmap(vr->rutabaga, res->resource_id);
        if (result) {
            error_setg_errno(&local_err,
                             (int)result,
                             "%s: rutabaga_resource_unmap returned %"PRIi32
                             " for resource_id = %"PRIu32,
                             __func__, result, res->resource_id);
        }
    }

    result = rutabaga_resource_unref(vr->rutabaga, res->resource_id);
    if (result) {
        if (!local_err) {
            error_setg_errno(&local_err,
                             (int)result,
                             "%s: rutabaga_resource_unref returned %"PRIi32
                             " for resource_id = %"PRIu32,
                             __func__, result, res->resource_id);
        }
    }

    if (res->image) {
        pixman_image_unref(res->image);
    }

    virtio_gpu_cleanup_mapping(g, res);
    QTAILQ_REMOVE(&g->reslist, res, next);
    g_free(res);

    error_propagate(errp, local_err);
}

static void
rutabaga_cmd_resource_unref(VirtIOGPU *g,
                            struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result = 0;
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_unref unref;
    Error *local_err = NULL;

    VIRTIO_GPU_FILL_CMD(unref);

    trace_virtio_gpu_cmd_res_unref(unref.resource_id);

    res = virtio_gpu_find_resource(g, unref.resource_id);
    CHECK(res, cmd);

    virtio_gpu_rutabaga_resource_unref(g, res, &local_err);
    if (local_err) {
        error_report_err(local_err);
        /* local_err was freed, do not reuse it. */
        local_err = NULL;
        result = 1;
    }
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_context_create(VirtIOGPU *g,
                            struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_ctx_create cc;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(cc);
    trace_virtio_gpu_cmd_ctx_create(cc.hdr.ctx_id,
                                    cc.debug_name);

    result = rutabaga_context_create(vr->rutabaga, cc.hdr.ctx_id,
                                     cc.context_init, cc.debug_name, cc.nlen);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_context_destroy(VirtIOGPU *g,
                             struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_ctx_destroy cd;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(cd);
    trace_virtio_gpu_cmd_ctx_destroy(cd.hdr.ctx_id);

    result = rutabaga_context_destroy(vr->rutabaga, cd.hdr.ctx_id);
    CHECK(!result, cmd);
}

static void rutabaga_sync_native_surface(VirtIOGPURutabaga *vr,
                                         uint32_t scanout_id,
                                         QemuConsole *con)
{
    void *handle = NULL;
    int width_pt = 0, height_pt = 0, width_px = 0, height_px = 0;
    float dpr = 1.0f;
    bool has_surface = qemu_console_get_native_surface(
        con, &handle, &width_pt, &height_pt, &width_px, &height_px, &dpr);

    if (!has_surface && vr->native_surface_active[scanout_id]) {
        rutabaga_teardown_native_surface(vr->rutabaga, scanout_id);
        vr->native_surface_active[scanout_id] = false;
        return;
    }

    if (has_surface && !vr->native_surface_active[scanout_id]) {
        int ret = rutabaga_setup_native_surface(
            vr->rutabaga, scanout_id, handle,
            width_pt, height_pt, width_px, height_px, dpr);
        if (ret == 0) {
            vr->native_surface_active[scanout_id] = true;
            vr->native_surface_width_pt[scanout_id] = width_pt;
            vr->native_surface_height_pt[scanout_id] = height_pt;
            vr->native_surface_width_px[scanout_id] = width_px;
            vr->native_surface_height_px[scanout_id] = height_px;
            vr->native_surface_dpr[scanout_id] = dpr;
        }
        return;
    }

    if (has_surface && vr->native_surface_active[scanout_id]) {
        if (width_pt != vr->native_surface_width_pt[scanout_id] ||
            height_pt != vr->native_surface_height_pt[scanout_id] ||
            width_px != vr->native_surface_width_px[scanout_id] ||
            height_px != vr->native_surface_height_px[scanout_id] ||
            dpr != vr->native_surface_dpr[scanout_id]) {
            int ret = rutabaga_resize_native_surface(
                vr->rutabaga, scanout_id,
                width_pt, height_pt, width_px, height_px, dpr);
            if (ret == 0) {
                vr->native_surface_width_pt[scanout_id] = width_pt;
                vr->native_surface_height_pt[scanout_id] = height_pt;
                vr->native_surface_width_px[scanout_id] = width_px;
                vr->native_surface_height_px[scanout_id] = height_px;
                vr->native_surface_dpr[scanout_id] = dpr;
            }
        }
    }
}

static void
rutabaga_cmd_resource_flush(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result, i;
    struct virtio_gpu_scanout *scanout = NULL;
    struct virtio_gpu_simple_resource *res;
    struct rutabaga_transfer transfer = { 0 };
    struct iovec transfer_iovec;
    struct virtio_gpu_resource_flush rf;
    QemuRect flush_rect;
    bool within_bounds = false;
    bool update_submitted = false;

    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);
    if (vr->headless) {
        return;
    }

    VIRTIO_GPU_FILL_CMD(rf);
    trace_virtio_gpu_cmd_res_flush(rf.resource_id,
                                   rf.r.width, rf.r.height, rf.r.x, rf.r.y);

    res = virtio_gpu_find_resource(g, rf.resource_id);
    CHECK(res, cmd);

    if (res->blob) {
        for (i = 0; i < vb->conf.max_outputs; i++) {
            scanout = &vb->scanout[i];
            if (scanout->resource_id == res->resource_id &&
                rf.r.x < scanout->x + scanout->width &&
                rf.r.x + rf.r.width >= scanout->x &&
                rf.r.y < scanout->y + scanout->height &&
                rf.r.y + rf.r.height >= scanout->y) {
                within_bounds = true;

                rutabaga_sync_native_surface(vr, i, scanout->con);

                {
                    int present_ret = rutabaga_present_flushed_resource(
                        vr->rutabaga, rf.resource_id,
                        rf.r.x, rf.r.y, rf.r.width, rf.r.height);
                    if (present_ret > 0) {
                        return;  /* native surface handled the present */
                    }
                }

                if (console_has_gl(scanout->con)) {
                    dpy_gl_update(scanout->con, 0, 0, scanout->width,
                                  scanout->height);
                    update_submitted = true;
                }
            }
        }

        if (update_submitted) {
            return;
        }

        if (!within_bounds) {
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            return;
        }
    } else {
        for (i = 0; i < vb->conf.max_outputs; i++) {
            if (res->scanout_bitmask & (1 << i)) {
                within_bounds = true;
                break;
            }
        }

        if (!within_bounds) {
            return;
        }

        transfer.x = 0;
        transfer.y = 0;
        transfer.z = 0;
        transfer.w = res->width;
        transfer.h = res->height;
        transfer.d = 1;

        transfer_iovec.iov_base = pixman_image_get_data(res->image);
        transfer_iovec.iov_len = pixman_image_get_stride(res->image) *
                                 res->height;

        result = rutabaga_resource_transfer_read(vr->rutabaga, 0,
                                                 rf.resource_id, &transfer,
                                                 &transfer_iovec);
        CHECK(!result, cmd);
    }

    qemu_rect_init(&flush_rect, rf.r.x, rf.r.y, rf.r.width, rf.r.height);
    for (i = 0; i < vb->conf.max_outputs; i++) {
        QemuRect rect;

        if (!(res->scanout_bitmask & (1 << i))) {
            continue;
        }

        scanout = &vb->scanout[i];
        qemu_rect_init(&rect, scanout->x, scanout->y,
                       scanout->width, scanout->height);
        if (qemu_rect_intersect(&flush_rect, &rect, &rect)) {
            qemu_rect_translate(&rect, -scanout->x, -scanout->y);
            dpy_gfx_update(scanout->con,
                           rect.x, rect.y, rect.width, rect.height);
        }
    }
}

static void
rutabaga_cmd_set_scanout(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_framebuffer fb = { 0 };
    struct virtio_gpu_scanout *scanout = NULL;
    struct virtio_gpu_set_scanout ss;

    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);
    if (vr->headless) {
        return;
    }

    VIRTIO_GPU_FILL_CMD(ss);
    trace_virtio_gpu_cmd_set_scanout(ss.scanout_id, ss.resource_id,
                                     ss.r.width, ss.r.height, ss.r.x, ss.r.y);

    if (ss.scanout_id >= g->parent_obj.conf.max_outputs) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }
    scanout = &vb->scanout[ss.scanout_id];

    if (ss.resource_id == 0) {
        rutabaga_set_scanout_resource(vr->rutabaga, ss.scanout_id, 0, 0, 0);
        virtio_gpu_disable_scanout(g, ss.scanout_id);
        return;
    }

    res = virtio_gpu_find_resource(g, ss.resource_id);
    CHECK(res, cmd);

    if (!res->image) {
        pixman_format_code_t pformat;
        pformat = virtio_gpu_get_pixman_format(res->format);
        CHECK(pformat, cmd);

        res->image = pixman_image_create_bits(pformat,
                                              res->width,
                                              res->height,
                                              NULL, 0);
        CHECK(res->image, cmd);
        pixman_image_ref(res->image);
    }

    vb->enable = 1;

    /* realloc the surface ptr */
    scanout->ds = qemu_create_displaysurface_pixman(res->image);
    fb.format = pixman_image_get_format(res->image);
    fb.bytes_pp = DIV_ROUND_UP(PIXMAN_FORMAT_BPP(fb.format), 8);
    fb.width = pixman_image_get_width(res->image);
    fb.height = pixman_image_get_height(res->image);
    fb.stride = pixman_image_get_stride(res->image);
    fb.offset = ss.r.x * fb.bytes_pp + ss.r.y * fb.stride;
    dpy_gfx_replace_surface(scanout->con, NULL);
    dpy_gfx_replace_surface(scanout->con, scanout->ds);
    virtio_gpu_update_scanout(g, ss.scanout_id, res, &fb, &ss.r);
    rutabaga_set_scanout_resource(vr->rutabaga, ss.scanout_id,
                                  ss.resource_id, fb.width, fb.height);
}

static void
rutabaga_cmd_set_scanout_blob(VirtIOGPU *g,
                              struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_framebuffer fb = { 0 };
    struct virtio_gpu_set_scanout_blob ss;
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_scanout *scanout;
    pixman_image_t *rect;

    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);
    if (vr->headless) {
        return;
    }

    VIRTIO_GPU_FILL_CMD(ss);
    virtio_gpu_scanout_blob_bswap(&ss);
    trace_virtio_gpu_cmd_set_scanout_blob(ss.scanout_id, ss.resource_id,
                                          ss.r.width, ss.r.height, ss.r.x,
                                          ss.r.y);

    if (ss.scanout_id >= g->parent_obj.conf.max_outputs) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }

    if (ss.resource_id == 0) {
        rutabaga_set_scanout_resource(vr->rutabaga, ss.scanout_id, 0, 0, 0);
        virtio_gpu_disable_scanout(g, ss.scanout_id);
        return;
    }

    res = virtio_gpu_find_resource(g, ss.resource_id);
    CHECK(res, cmd);
    CHECK(res->blob, cmd);

    if (!virtio_gpu_scanout_blob_to_fb(&fb, &ss, res->blob_size)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    res->width = ss.width;
    res->height = ss.height;
    res->format = ss.format;

    vb->enable = 1;
    scanout = &vb->scanout[ss.scanout_id];
    rect = pixman_image_create_bits(fb.format, ss.r.width, ss.r.height,
                                    (uint32_t *)((uint8_t *)res->blob +
                                                 fb.offset),
                                    fb.stride);
    CHECK(rect, cmd);

    scanout->ds = qemu_create_displaysurface_pixman(rect);
    pixman_image_unref(rect);
    dpy_gfx_replace_surface(scanout->con, NULL);
    dpy_gl_scanout_disable(scanout->con);
    dpy_gfx_replace_surface(scanout->con, scanout->ds);
    virtio_gpu_update_scanout(g, ss.scanout_id, res, &fb, &ss.r);
    rutabaga_set_scanout_resource(vr->rutabaga, ss.scanout_id,
                                  ss.resource_id, ss.width, ss.height);
    rutabaga_sync_native_surface(vr, ss.scanout_id, scanout->con);
}

static void
rutabaga_cmd_submit_3d(VirtIOGPU *g,
                       struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_cmd_submit cs;
    struct rutabaga_command rutabaga_cmd = { 0 };
    g_autofree uint8_t *buf = NULL;
    size_t s;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(cs);
    trace_virtio_gpu_cmd_ctx_submit(cs.hdr.ctx_id, cs.size);

    buf = g_new0(uint8_t, cs.size);
    s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num,
                   sizeof(cs), buf, cs.size);
    CHECK(s == cs.size, cmd);

    rutabaga_cmd.ctx_id = cs.hdr.ctx_id;
    rutabaga_cmd.cmd = buf;
    rutabaga_cmd.cmd_size = cs.size;

    result = rutabaga_submit_command(vr->rutabaga, &rutabaga_cmd);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_transfer_to_host_2d(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct rutabaga_transfer transfer = { 0 };
    struct virtio_gpu_transfer_to_host_2d t2d;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(t2d);
    trace_virtio_gpu_cmd_res_xfer_toh_2d(t2d.resource_id);

    transfer.x = t2d.r.x;
    transfer.y = t2d.r.y;
    transfer.z = 0;
    transfer.w = t2d.r.width;
    transfer.h = t2d.r.height;
    transfer.d = 1;

    result = rutabaga_resource_transfer_write(vr->rutabaga, 0, t2d.resource_id,
                                              &transfer);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_transfer_to_host_3d(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct rutabaga_transfer transfer = { 0 };
    struct virtio_gpu_transfer_host_3d t3d;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(t3d);
    trace_virtio_gpu_cmd_res_xfer_toh_3d(t3d.resource_id);

    transfer.x = t3d.box.x;
    transfer.y = t3d.box.y;
    transfer.z = t3d.box.z;
    transfer.w = t3d.box.w;
    transfer.h = t3d.box.h;
    transfer.d = t3d.box.d;
    transfer.level = t3d.level;
    transfer.stride = t3d.stride;
    transfer.layer_stride = t3d.layer_stride;
    transfer.offset = t3d.offset;

    result = rutabaga_resource_transfer_write(vr->rutabaga, t3d.hdr.ctx_id,
                                              t3d.resource_id, &transfer);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_transfer_from_host_3d(VirtIOGPU *g,
                                   struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct rutabaga_transfer transfer = { 0 };
    struct virtio_gpu_transfer_host_3d t3d;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(t3d);
    trace_virtio_gpu_cmd_res_xfer_fromh_3d(t3d.resource_id);

    transfer.x = t3d.box.x;
    transfer.y = t3d.box.y;
    transfer.z = t3d.box.z;
    transfer.w = t3d.box.w;
    transfer.h = t3d.box.h;
    transfer.d = t3d.box.d;
    transfer.level = t3d.level;
    transfer.stride = t3d.stride;
    transfer.layer_stride = t3d.layer_stride;
    transfer.offset = t3d.offset;

    result = rutabaga_resource_transfer_read(vr->rutabaga, t3d.hdr.ctx_id,
                                             t3d.resource_id, &transfer, NULL);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_attach_backing(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct rutabaga_iovecs vecs = { 0 };
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_attach_backing att_rb;
    int ret;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(att_rb);
    trace_virtio_gpu_cmd_res_back_attach(att_rb.resource_id);

    res = virtio_gpu_find_resource(g, att_rb.resource_id);
    CHECK(res, cmd);
    CHECK(!res->iov, cmd);

    ret = virtio_gpu_create_mapping_iov(g, att_rb.nr_entries, sizeof(att_rb),
                                        cmd, NULL, &res->iov, &res->iov_cnt);
    CHECK(!ret, cmd);

    vecs.iovecs = res->iov;
    vecs.num_iovecs = res->iov_cnt;

    ret = rutabaga_resource_attach_backing(vr->rutabaga, att_rb.resource_id,
                                           &vecs);
    if (ret != 0) {
        virtio_gpu_cleanup_mapping(g, res);
    }

    CHECK(!ret, cmd);
}

static void
rutabaga_cmd_detach_backing(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_detach_backing detach_rb;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(detach_rb);
    trace_virtio_gpu_cmd_res_back_detach(detach_rb.resource_id);

    res = virtio_gpu_find_resource(g, detach_rb.resource_id);
    CHECK(res, cmd);

    rutabaga_resource_detach_backing(vr->rutabaga,
                                     detach_rb.resource_id);

    virtio_gpu_cleanup_mapping(g, res);
}

static void
rutabaga_cmd_ctx_attach_resource(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_ctx_resource att_res;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(att_res);
    trace_virtio_gpu_cmd_ctx_res_attach(att_res.hdr.ctx_id,
                                        att_res.resource_id);

    result = rutabaga_context_attach_resource(vr->rutabaga, att_res.hdr.ctx_id,
                                              att_res.resource_id);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_ctx_detach_resource(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_ctx_resource det_res;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(det_res);
    trace_virtio_gpu_cmd_ctx_res_detach(det_res.hdr.ctx_id,
                                        det_res.resource_id);

    result = rutabaga_context_detach_resource(vr->rutabaga, det_res.hdr.ctx_id,
                                              det_res.resource_id);
    CHECK(!result, cmd);
}

static void
rutabaga_cmd_get_capset_info(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_get_capset_info info;
    struct virtio_gpu_resp_capset_info resp;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(info);

    result = rutabaga_get_capset_info(vr->rutabaga, info.capset_index,
                                      &resp.capset_id, &resp.capset_max_version,
                                      &resp.capset_max_size);
    CHECK(!result, cmd);

    resp.hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void
rutabaga_cmd_get_capset(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    struct virtio_gpu_get_capset gc;
    struct virtio_gpu_resp_capset *resp;
    uint32_t capset_size, capset_version;
    uint32_t current_id, i;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(gc);
    for (i = 0; i < vr->num_capsets; i++) {
        result = rutabaga_get_capset_info(vr->rutabaga, i,
                                          &current_id, &capset_version,
                                          &capset_size);
        CHECK(!result, cmd);

        if (current_id == gc.capset_id) {
            break;
        }
    }

    CHECK(i < vr->num_capsets, cmd);

    resp = g_malloc0(sizeof(*resp) + capset_size);
    resp->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    rutabaga_get_capset(vr->rutabaga, gc.capset_id, gc.capset_version,
                        resp->capset_data, capset_size);

    virtio_gpu_ctrl_response(g, cmd, &resp->hdr, sizeof(*resp) + capset_size);
    g_free(resp);
}

static void
rutabaga_cmd_resource_create_blob(VirtIOGPU *g,
                                  struct virtio_gpu_ctrl_command *cmd)
{
    int result;
    struct rutabaga_iovecs vecs = { 0 };
    g_autofree struct virtio_gpu_simple_resource *res = NULL;
    struct virtio_gpu_resource_create_blob cblob;
    struct rutabaga_create_blob rc_blob = { 0 };

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(cblob);
    trace_virtio_gpu_cmd_res_create_blob(cblob.resource_id, cblob.size);

    CHECK(cblob.resource_id != 0, cmd);

    res = g_new0(struct virtio_gpu_simple_resource, 1);
    res->dmabuf_fd = -1;

    res->resource_id = cblob.resource_id;
    res->blob_size = cblob.size;

    if (cblob.blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D) {
        result = virtio_gpu_create_mapping_iov(g, cblob.nr_entries,
                                               sizeof(cblob), cmd, &res->addrs,
                                               &res->iov, &res->iov_cnt);
        CHECK(!result, cmd);
    }

    rc_blob.blob_id = cblob.blob_id;
    rc_blob.blob_mem = cblob.blob_mem;
    rc_blob.blob_flags = cblob.blob_flags;
    rc_blob.size = cblob.size;

    vecs.iovecs = res->iov;
    vecs.num_iovecs = res->iov_cnt;

    result = rutabaga_resource_create_blob(vr->rutabaga, cblob.hdr.ctx_id,
                                           cblob.resource_id, &rc_blob, &vecs,
                                           NULL);

    if (result && cblob.blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D) {
        virtio_gpu_cleanup_mapping(g, res);
    }

    CHECK(!result, cmd);

    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
    res = NULL;
}

static void
rutabaga_cmd_resource_map_blob(VirtIOGPU *g,
                               struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    uint32_t map_info = 0;
    int current_slot;
    uint32_t slot = 0;
    struct virtio_gpu_simple_resource *res;
    struct rutabaga_mapping mapping = { 0 };
    struct virtio_gpu_resource_map_blob mblob;
    struct virtio_gpu_resp_map_info resp = { 0 };

    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(mblob);
    virtio_gpu_map_blob_bswap(&mblob);

    CHECK(mblob.resource_id != 0, cmd);

    res = virtio_gpu_find_resource(g, mblob.resource_id);
    CHECK(res, cmd);

    result = rutabaga_resource_map_info(vr->rutabaga, mblob.resource_id,
                                        &map_info);
    CHECK(!result, cmd);

    /*
     * RUTABAGA_MAP_ACCESS_* flags are not part of the virtio-gpu spec, but do
     * exist to potentially allow the hypervisor to restrict write access to
     * memory. QEMU does not need to use this functionality at the moment.
     */
    resp.map_info = map_info & RUTABAGA_MAP_CACHE_MASK;

    current_slot = rutabaga_find_mapping_slot(vr, mblob.resource_id);
    if (current_slot >= 0) {
        rutabaga_release_mapping_slot(vb, vr, current_slot);
        res->blob = NULL;

        result = rutabaga_resource_unmap(vr->rutabaga, mblob.resource_id);
        CHECK(!result, cmd);
    }

    result = rutabaga_resource_map(vr->rutabaga, mblob.resource_id, &mapping);
    CHECK(!result, cmd);

    /*
     * There is small risk of the MemoryRegion dereferencing the pointer after
     * rutabaga unmaps it. Please see discussion here:
     *
     * https://lists.gnu.org/archive/html/qemu-devel/2023-09/msg05141.html
     *
     * It is highly unlikely to happen in practice and doesn't affect known
     * use cases. However, it should be fixed and is noted here for posterity.
     */
    for (slot = 0; slot < MAX_SLOTS; slot++) {
        if (vr->memory_regions[slot].used) {
            continue;
        }

        MemoryRegion *mr = &(vr->memory_regions[slot].mr);
        memory_region_init_ram_ptr(mr, OBJECT(vr), "blob", mapping.size,
                                   mapping.ptr);
        memory_region_add_subregion(&vb->hostmem, mblob.offset, mr);
        vr->memory_regions[slot].resource_id = mblob.resource_id;
        vr->memory_regions[slot].used = 1;
        break;
    }

    if (slot >= MAX_SLOTS) {
        result = rutabaga_resource_unmap(vr->rutabaga, mblob.resource_id);
        CHECK(!result, cmd);
    }

    CHECK(slot < MAX_SLOTS, cmd);
    res->blob = mapping.ptr;

    resp.hdr.type = VIRTIO_GPU_RESP_OK_MAP_INFO;
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void
rutabaga_cmd_resource_unmap_blob(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    int32_t result;
    int slot;
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_unmap_blob ublob;

    VirtIOGPUBase *vb = VIRTIO_GPU_BASE(g);
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(ublob);
    virtio_gpu_unmap_blob_bswap(&ublob);

    CHECK(ublob.resource_id != 0, cmd);

    res = virtio_gpu_find_resource(g, ublob.resource_id);
    CHECK(res, cmd);

    slot = rutabaga_find_mapping_slot(vr, ublob.resource_id);
    CHECK(slot >= 0, cmd);

    rutabaga_release_mapping_slot(vb, vr, slot);
    res->blob = NULL;
    result = rutabaga_resource_unmap(vr->rutabaga, res->resource_id);
    CHECK(!result, cmd);
}

static void
virtio_gpu_rutabaga_process_cmd(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    struct rutabaga_fence fence = { 0 };
    int32_t result;

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    VIRTIO_GPU_FILL_CMD(cmd->cmd_hdr);

    switch (cmd->cmd_hdr.type) {
    case VIRTIO_GPU_CMD_CTX_CREATE:
        rutabaga_cmd_context_create(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        rutabaga_cmd_context_destroy(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
        rutabaga_cmd_create_resource_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        rutabaga_cmd_create_resource_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        rutabaga_cmd_submit_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        rutabaga_cmd_transfer_to_host_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        rutabaga_cmd_transfer_to_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
        rutabaga_cmd_transfer_from_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        rutabaga_cmd_attach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        rutabaga_cmd_detach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        rutabaga_cmd_set_scanout(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
        rutabaga_cmd_set_scanout_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        rutabaga_cmd_resource_flush(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        rutabaga_cmd_resource_unref(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        rutabaga_cmd_ctx_attach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        rutabaga_cmd_ctx_detach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        rutabaga_cmd_get_capset_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET:
        rutabaga_cmd_get_capset(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
        virtio_gpu_get_display_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_EDID:
        virtio_gpu_get_edid(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        rutabaga_cmd_resource_create_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        rutabaga_cmd_resource_map_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        rutabaga_cmd_resource_unmap_blob(g, cmd);
        break;
    default:
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }

    if (cmd->finished) {
        return;
    }
    if (cmd->error) {
        error_report("%s: ctrl 0x%x, error 0x%x", __func__,
                     cmd->cmd_hdr.type, cmd->error);
        virtio_gpu_ctrl_response_nodata(g, cmd, cmd->error);
        return;
    }
    if (!(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE)) {
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        return;
    }

    fence.flags = cmd->cmd_hdr.flags;
    fence.ctx_id = cmd->cmd_hdr.ctx_id;
    fence.fence_id = cmd->cmd_hdr.fence_id;
    fence.ring_idx = cmd->cmd_hdr.ring_idx;

    trace_virtio_gpu_fence_ctrl(cmd->cmd_hdr.fence_id, cmd->cmd_hdr.type);

    result = rutabaga_create_fence(vr->rutabaga, &fence);
    CHECK(!result, cmd);
}

static void
virtio_gpu_rutabaga_aio_cb(void *opaque)
{
    struct rutabaga_aio_data *data = opaque;
    VirtIOGPU *g = VIRTIO_GPU(data->vr);
    struct rutabaga_fence fence_data = data->fence;
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    uint32_t signaled_ctx_specific = fence_data.flags &
                                     RUTABAGA_FLAG_INFO_RING_IDX;

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        /*
         * Due to context specific timelines.
         */
        uint32_t target_ctx_specific = cmd->cmd_hdr.flags &
                                       RUTABAGA_FLAG_INFO_RING_IDX;

        if (signaled_ctx_specific != target_ctx_specific) {
            continue;
        }

        if (signaled_ctx_specific &&
           (cmd->cmd_hdr.ring_idx != fence_data.ring_idx)) {
            continue;
        }

        if (cmd->cmd_hdr.fence_id > fence_data.fence_id) {
            continue;
        }

        trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        QTAILQ_REMOVE(&g->fenceq, cmd, next);
        g_free(cmd);
    }

    g_free(data);
}

static void
virtio_gpu_rutabaga_fence_cb(uint64_t user_data,
                             const struct rutabaga_fence *fence)
{
    struct rutabaga_aio_data *data;
    VirtIOGPU *g = (VirtIOGPU *)user_data;
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    /*
     * gfxstream and both cross-domain (and even newer versions virglrenderer:
     * see VIRGL_RENDERER_ASYNC_FENCE_CB) like to signal fence completion on
     * threads ("callback threads") that are different from the thread that
     * processes the command queue ("main thread").
     *
     * crosvm and other virtio-gpu 1.1 implementations enable callback threads
     * via locking.  However, on QEMU a deadlock is observed if
     * virtio_gpu_ctrl_response_nodata(..) [used in the fence callback] is used
     * from a thread that is not the main thread.
     *
     * The reason is QEMU's internal locking is designed to work with QEMU
     * threads (see rcu_register_thread()) and not generic C/C++/Rust threads.
     * For now, we can workaround this by scheduling the return of the
     * fence descriptors on the main thread.
     */

    data = g_new0(struct rutabaga_aio_data, 1);
    data->vr = vr;
    data->fence = *fence;
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            virtio_gpu_rutabaga_aio_cb,
                            data);
}

static void
virtio_gpu_rutabaga_debug_cb(uint64_t user_data,
                             const struct rutabaga_debug *debug)
{
    switch (debug->debug_type) {
    case RUTABAGA_DEBUG_ERROR:
        error_report("%s", debug->message);
        break;
    case RUTABAGA_DEBUG_WARN:
        warn_report("%s", debug->message);
        break;
    case RUTABAGA_DEBUG_INFO:
        info_report("%s", debug->message);
        break;
    default:
        error_report("unknown debug type: %u", debug->debug_type);
    }
}

static bool virtio_gpu_rutabaga_init(VirtIOGPU *g, Error **errp)
{
    int result;
    const char *env_renderer_features;
    struct rutabaga_builder builder = { 0 };
    struct rutabaga_channel channel = { 0 };
    struct rutabaga_channels channels = { 0 };

    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);
    vr->rutabaga = NULL;

    builder.wsi = RUTABAGA_WSI_SURFACELESS;
    /*
     * Currently, if WSI is specified, the only valid strings are "surfaceless"
     * or "headless".  Surfaceless doesn't create a native window surface, but
     * does copy from the render target to the Pixman buffer if a virtio-gpu
     * 2D hypercall is issued.  Surfacless is the default.
     *
     * Headless is like surfaceless, but doesn't copy to the Pixman buffer. The
     * use case is automated testing environments where there is no need to view
     * results.
     *
     * In the future, more performant virtio-gpu 2D UI integration may be added.
     */
    if (vr->wsi) {
        if (g_str_equal(vr->wsi, "surfaceless")) {
            vr->headless = false;
        } else if (g_str_equal(vr->wsi, "headless")) {
            vr->headless = true;
        } else if (g_str_equal(vr->wsi, "vulkan-swapchain")) {
            builder.wsi = RUTABAGA_WSI_VULKAN_SWAPCHAIN;
            vr->headless = false;
        } else {
            error_setg(errp, "invalid wsi option selected");
            return false;
        }
    }

    builder.fence_cb = virtio_gpu_rutabaga_fence_cb;
    builder.debug_cb = virtio_gpu_rutabaga_debug_cb;
    builder.capset_mask = vr->capset_mask;
    builder.user_data = (uint64_t)g;

    /*
     * If the user doesn't specify the wayland socket path, we try to infer
     * the socket via a process similar to the one used by libwayland.
     * libwayland does the following:
     *
     * 1) If $WAYLAND_DISPLAY is set, attempt to connect to
     *    $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY
     * 2) Otherwise, attempt to connect to $XDG_RUNTIME_DIR/wayland-0
     * 3) Otherwise, don't pass a wayland socket to rutabaga. If a guest
     *    wayland proxy is launched, it will fail to work.
     */
    channel.channel_type = RUTABAGA_CHANNEL_TYPE_WAYLAND;
    g_autofree gchar *path = NULL;
    if (!vr->wayland_socket_path) {
        const gchar *runtime_dir = g_get_user_runtime_dir();
        const gchar *display = g_getenv("WAYLAND_DISPLAY");
        if (!display) {
            display = "wayland-0";
        }

        if (runtime_dir) {
            path = g_build_filename(runtime_dir, display, NULL);
            channel.channel_name = path;
        }
    } else {
        channel.channel_name = vr->wayland_socket_path;
    }

    if ((builder.capset_mask & (1 << RUTABAGA_CAPSET_CROSS_DOMAIN))) {
        if (channel.channel_name) {
            channels.channels = &channel;
            channels.num_channels = 1;
            builder.channels = &channels;
        }
    }

    env_renderer_features = g_getenv("QEMU_RUTABAGA_GFXSTREAM_FEATURES");
    if (vr->gfxstream_features && vr->gfxstream_features[0]) {
        builder.renderer_features = vr->gfxstream_features;
    } else if (env_renderer_features && env_renderer_features[0]) {
        builder.renderer_features = env_renderer_features;
    }

    result = rutabaga_init(&builder, &vr->rutabaga);
    if (result) {
        error_setg_errno(errp, -result, "Failed to init rutabaga");
        return false;
    }

    if (g->parent_obj.conf.refresh_rate) {
        uint32_t vsync_hz = g->parent_obj.conf.refresh_rate / 1000;
        if (vsync_hz > 0) {
            rutabaga_set_vsync_hz(vr->rutabaga, vsync_hz);
        }
    }

    return true;
}

static int virtio_gpu_rutabaga_get_num_capsets(VirtIOGPU *g)
{
    int result;
    uint32_t num_capsets;
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);

    result = rutabaga_get_num_capsets(vr->rutabaga, &num_capsets);
    if (result) {
        error_report("Failed to get capsets");
        return 0;
    }
    vr->num_capsets = num_capsets;
    return num_capsets;
}

static void virtio_gpu_rutabaga_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    struct virtio_gpu_ctrl_command *cmd;

    if (!virtio_queue_ready(vq)) {
        return;
    }

    cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    while (cmd) {
        cmd->vq = vq;
        cmd->error = 0;
        cmd->finished = false;
        QTAILQ_INSERT_TAIL(&g->cmdq, cmd, next);
        cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    }

    virtio_gpu_process_cmdq(g);
}

static void virtio_gpu_rutabaga_ui_info(void *opaque, uint32_t idx,
                                        QemuUIInfo *info)
{
    VirtIOGPUBase *g = opaque;
    VirtIOGPURutabaga *vr = VIRTIO_GPU_RUTABAGA(g);
    bool refresh_changed = false;

    if (idx >= g->conf.max_outputs) {
        return;
    }

    /*
     * SDL/DBus UI geometry reflects the host presentation window, not the
     * guest scanout mode. Feeding that back into req_state makes the guest
     * believe its physical display changed to the host window size
     * (for example 640x480), which breaks Android layout and density.
     *
     * Rutabaga only needs host ui_info here to track display refresh changes
     * for gfxstream's vsync thread. Keep the guest mode derived from the
     * configured virtio-gpu output / guest scanout instead of the host window.
     */
    g->req_state[idx].x = info->xoff;
    g->req_state[idx].y = info->yoff;

    if (info->refresh_rate &&
        info->refresh_rate != g->req_state[idx].refresh_rate) {
        g->req_state[idx].refresh_rate = info->refresh_rate;
        refresh_changed = true;
        uint32_t vsync_hz = info->refresh_rate / 1000;
        if (vsync_hz > 0 && vr->rutabaga) {
            rutabaga_set_vsync_hz(vr->rutabaga, vsync_hz);
        }
    } else if (!info->refresh_rate) {
        g->req_state[idx].refresh_rate = info->refresh_rate;
    }

    if (refresh_changed) {
        g->virtio_config.events_read |= VIRTIO_GPU_EVENT_DISPLAY;
        virtio_notify_config(&g->parent_obj);
    }
}

static void virtio_gpu_rutabaga_realize(DeviceState *qdev, Error **errp)
{
    int num_capsets;
    VirtIOGPUBase *bdev = VIRTIO_GPU_BASE(qdev);
    VirtIOGPU *gpudev = VIRTIO_GPU(qdev);

#if HOST_BIG_ENDIAN
    error_setg(errp, "rutabaga is not supported on bigendian platforms");
    return;
#endif

    if (!virtio_gpu_rutabaga_init(gpudev, errp)) {
        return;
    }

    num_capsets = virtio_gpu_rutabaga_get_num_capsets(gpudev);
    if (!num_capsets) {
        return;
    }

    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_RUTABAGA_ENABLED);
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_BLOB_ENABLED);
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_CONTEXT_INIT_ENABLED);

    bdev->virtio_config.num_capsets = num_capsets;
    virtio_gpu_device_realize(qdev, errp);

    /*
     * Replace the base ui_info callback with a rutabaga-specific one
     * that forwards host display refresh_rate into gfxstream's VsyncThread.
     */
    {
        static GraphicHwOps rutabaga_ops;
        static bool ops_initialized;
        if (!ops_initialized) {
            rutabaga_ops = *bdev->hw_ops;
            rutabaga_ops.ui_info = virtio_gpu_rutabaga_ui_info;
            ops_initialized = true;
        }
        bdev->hw_ops = &rutabaga_ops;
        for (int i = 0; i < bdev->conf.max_outputs; i++) {
            if (bdev->scanout[i].con) {
                graphic_console_set_hwops(bdev->scanout[i].con,
                                          &rutabaga_ops, bdev);
            }
        }
    }
}

static const Property virtio_gpu_rutabaga_properties[] = {
    DEFINE_PROP_BIT64("gfxstream-vulkan", VirtIOGPURutabaga, capset_mask,
                      RUTABAGA_CAPSET_GFXSTREAM_VULKAN, false),
    DEFINE_PROP_BIT64("cross-domain", VirtIOGPURutabaga, capset_mask,
                      RUTABAGA_CAPSET_CROSS_DOMAIN, false),
    DEFINE_PROP_BIT64("x-gfxstream-gles", VirtIOGPURutabaga, capset_mask,
                      RUTABAGA_CAPSET_GFXSTREAM_GLES, false),
    DEFINE_PROP_BIT64("x-gfxstream-composer", VirtIOGPURutabaga, capset_mask,
                      RUTABAGA_CAPSET_GFXSTREAM_COMPOSER, false),
    DEFINE_PROP_STRING("x-gfxstream-features", VirtIOGPURutabaga,
                       gfxstream_features),
    DEFINE_PROP_STRING("wayland-socket-path", VirtIOGPURutabaga,
                       wayland_socket_path),
    DEFINE_PROP_STRING("wsi", VirtIOGPURutabaga, wsi),
};

static void virtio_gpu_rutabaga_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    VirtIOGPUBaseClass *vbc = VIRTIO_GPU_BASE_CLASS(klass);
    VirtIOGPUClass *vgc = VIRTIO_GPU_CLASS(klass);

    vbc->gl_flushed = virtio_gpu_rutabaga_gl_flushed;
    vgc->handle_ctrl = virtio_gpu_rutabaga_handle_ctrl;
    vgc->process_cmd = virtio_gpu_rutabaga_process_cmd;
    vgc->update_cursor_data = virtio_gpu_rutabaga_update_cursor;
    vgc->resource_destroy = virtio_gpu_rutabaga_resource_unref;
    vdc->realize = virtio_gpu_rutabaga_realize;
    device_class_set_props(dc, virtio_gpu_rutabaga_properties);
}

static const TypeInfo virtio_gpu_rutabaga_info[] = {
    {
        .name = TYPE_VIRTIO_GPU_RUTABAGA,
        .parent = TYPE_VIRTIO_GPU,
        .instance_size = sizeof(VirtIOGPURutabaga),
        .class_init = virtio_gpu_rutabaga_class_init,
    },
};

DEFINE_TYPES(virtio_gpu_rutabaga_info)

module_obj(TYPE_VIRTIO_GPU_RUTABAGA);
module_kconfig(VIRTIO_GPU);
module_dep("hw-display-virtio-gpu");

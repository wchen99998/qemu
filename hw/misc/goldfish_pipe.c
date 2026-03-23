/*
 * Goldfish virtual pipe device (V2 protocol).
 *
 * This device implements the goldfish-pipe MMIO interface expected by
 * the guest kernel driver (drivers/platform/goldfish/goldfish_pipe.c).
 * It routes pipe service operations to gfxstream through rutabaga FFI.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/misc/goldfish_pipe.h"
#include "hw/acpi/goldfish_defs.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* --------------------------------------------------------------------------
 * ABI types (from the owned gfxstream goldfish_pipe.h, forward-declared here
 * to keep QEMU compilable without gfxstream includes on the build path).
 * -------------------------------------------------------------------------- */

typedef struct GoldfishPipeBuffer {
    void  *data;
    size_t size;
} GoldfishPipeBuffer;

typedef enum {
    GP_POLL_IN  = (1 << 0),
    GP_POLL_OUT = (1 << 1),
    GP_POLL_HUP = (1 << 2),
} GoldfishPipePollFlags;

typedef enum {
    GP_WAKE_CLOSED = (1 << 0),
    GP_WAKE_READ   = (1 << 1),
    GP_WAKE_WRITE  = (1 << 2),
} GoldfishPipeWakeFlags;

typedef enum {
    GP_ERROR_INVAL = -1,
    GP_ERROR_AGAIN = -2,
    GP_ERROR_NOMEM = -3,
    GP_ERROR_IO    = -4,
} GoldfishPipeError;

typedef enum {
    GP_CLOSE_GRACEFUL       = 0,
    GP_CLOSE_REBOOT         = 1,
    GP_CLOSE_LOAD_SNAPSHOT  = 2,
    GP_CLOSE_ERROR          = 3,
} GoldfishPipeCloseReason;

typedef struct GoldfishHwPipe   GoldfishHwPipe;
typedef struct GoldfishHostPipe GoldfishHostPipe;

typedef struct GoldfishPipeServiceOps {
    GoldfishHostPipe* (*guest_open)(GoldfishHwPipe *hw_pipe);
    GoldfishHostPipe* (*guest_open_with_flags)(GoldfishHwPipe *hw_pipe,
                                               uint32_t flags);
    void (*guest_close)(GoldfishHostPipe *host_pipe,
                        GoldfishPipeCloseReason reason);
    void (*guest_pre_load)(void *stream);
    void (*guest_post_load)(void *stream);
    void (*guest_pre_save)(void *stream);
    void (*guest_post_save)(void *stream);
    GoldfishHostPipe* (*guest_load)(void *stream, GoldfishHwPipe *hw_pipe,
                                    char *force_close);
    void (*guest_save)(GoldfishHostPipe *host_pipe, void *stream);
    GoldfishPipePollFlags (*guest_poll)(GoldfishHostPipe *host_pipe);
    int (*guest_recv)(GoldfishHostPipe *host_pipe,
                      GoldfishPipeBuffer *buffers, int num_buffers);
    void (*wait_guest_recv)(GoldfishHostPipe *host_pipe);
    int (*guest_send)(GoldfishHostPipe **host_pipe,
                      const GoldfishPipeBuffer *buffers, int num_buffers);
    void (*wait_guest_send)(GoldfishHostPipe *host_pipe);
    void (*guest_wake_on)(GoldfishHostPipe *host_pipe,
                          GoldfishPipeWakeFlags wake_flags);
    void (*dma_add_buffer)(void *, uint64_t, uint64_t);
    void (*dma_remove_buffer)(uint64_t);
    void (*dma_invalidate_host_mappings)(void);
    void (*dma_reset_host_mappings)(void);
    void (*dma_save_mappings)(void *);
    void (*dma_load_mappings)(void *);
} GoldfishPipeServiceOps;

typedef struct GoldfishPipeHwFuncs {
    void (*signal_wake)(GoldfishHwPipe *hw_pipe, GoldfishPipeWakeFlags flags);
    void (*close_from_host)(GoldfishHwPipe *hw_pipe);
    void (*reset_pipe)(GoldfishHwPipe *hw_pipe, GoldfishHostPipe *new_host);
    int  (*get_id)(GoldfishHwPipe *hw_pipe);
    GoldfishHwPipe *(*lookup_by_id)(int id);
} GoldfishPipeHwFuncs;

/* Rutabaga FFI entry points (resolved at link time). */
extern const void *rutabaga_gfxstream_get_service_ops(void);
extern const void *rutabaga_gfxstream_set_service_hw_funcs(const void *hw_funcs);

/* --------------------------------------------------------------------------
 * Register layout (must match the guest driver goldfish_pipe_qemu.h).
 * -------------------------------------------------------------------------- */

enum PipeRegs {
    PIPE_REG_CMD                 = 0x00,
    PIPE_REG_SIGNAL_BUFFER_HIGH  = 0x04,
    PIPE_REG_SIGNAL_BUFFER       = 0x08,
    PIPE_REG_SIGNAL_BUFFER_COUNT = 0x0C,
    PIPE_REG_OPEN_BUFFER_HIGH    = 0x14,
    PIPE_REG_OPEN_BUFFER         = 0x18,
    PIPE_REG_VERSION             = 0x24,
    PIPE_REG_GET_SIGNALLED       = 0x30,
};

enum PipeCmdCode {
    PIPE_CMD_OPEN           = 1,
    PIPE_CMD_CLOSE          = 2,
    PIPE_CMD_POLL           = 3,
    PIPE_CMD_WRITE          = 4,
    PIPE_CMD_WAKE_ON_WRITE  = 5,
    PIPE_CMD_READ           = 6,
    PIPE_CMD_WAKE_ON_READ   = 7,
};

enum {
    PIPE_DEVICE_VERSION   = 2,
    MAX_SIGNALLED_PIPES   = 64,
    MAX_BUFFERS           = 336,
    MAX_PIPES             = 512,
};

/* Shared structures (DMA'd from guest – host reads via address_space). */

typedef struct __attribute__((packed)) GuestPipeCommand {
    int32_t  cmd;
    int32_t  id;
    int32_t  status;
    int32_t  reserved;
    /* rw_params */
    uint32_t buffers_count;
    int32_t  consumed_size;
    uint64_t ptrs[MAX_BUFFERS];
    uint32_t sizes[MAX_BUFFERS];
} GuestPipeCommand;

typedef struct __attribute__((packed)) SignalledPipeBuffer {
    uint32_t id;
    uint32_t flags;
} SignalledPipeBuffer;

typedef struct __attribute__((packed)) OpenCommandParam {
    uint64_t command_buffer_ptr;
    uint32_t rw_params_max_count;
} OpenCommandParam;

/* --------------------------------------------------------------------------
 * Per-pipe state.
 * -------------------------------------------------------------------------- */

struct GoldfishPipeState;

typedef struct HwPipe {
    uint32_t                id;
    uint32_t                gen;           /* generation – prevents stale wakes */
    uint64_t                cmd_phys;      /* guest-physical address of command buf */
    GoldfishHostPipe       *host_pipe;
    bool                    wanted_wake_read;
    bool                    wanted_wake_write;
    struct GoldfishPipeState *dev;         /* back-pointer for hw_funcs callbacks */
} HwPipe;

/* Internal wake-queue entry (not guest-visible). Carries a generation tag
 * so the BH can discard wakes for pipes that have been closed and whose
 * slot has been reused since the wake was queued. */
typedef struct WakeEntry {
    uint32_t id;
    uint32_t flags;
    uint32_t gen;
} WakeEntry;

/* --------------------------------------------------------------------------
 * Device state.
 * -------------------------------------------------------------------------- */

struct GoldfishPipeState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq     irq;

    /* Service ops obtained from gfxstream through rutabaga FFI. */
    const GoldfishPipeServiceOps *ops;

    /* MMIO register state */
    uint32_t version;
    uint64_t signal_buf_phys;
    uint32_t signal_buf_count;
    uint64_t open_buf_phys;

    /* Pipe table */
    HwPipe  *pipes[MAX_PIPES];
    uint32_t pipe_count;

    /* Signalled-pipe staging area (written under BQL on main loop) */
    uint32_t sig_pending;
    SignalledPipeBuffer sig_staging[MAX_SIGNALLED_PIPES];

    /* Per-slot generation counter: incremented each time a pipe is opened
     * in that slot.  Stale wakes (from a previous incarnation) are detected
     * and discarded in the BH by comparing the queued gen against the
     * current pipe's gen. */
    uint32_t pipe_gen[MAX_PIPES];

    /* Thread-safe wake queue: populated by hw_signal_wake from any thread,
     * drained by the BH on the QEMU main loop. */
    QemuMutex wake_lock;
    uint32_t  wake_count;
    WakeEntry wake_queue[MAX_SIGNALLED_PIPES];
    QEMUBH   *wake_bh;
};

/* --------------------------------------------------------------------------
 * Helpers.
 * -------------------------------------------------------------------------- */

static const GoldfishPipeServiceOps *get_ops(GoldfishPipeState *s)
{
    if (!s->ops) {
        s->ops = (const GoldfishPipeServiceOps *)
            rutabaga_gfxstream_get_service_ops();
    }
    return s->ops;
}

static uint32_t goldfish_pipe_signal_capacity(const GoldfishPipeState *s)
{
    return MIN((uint32_t)MAX_SIGNALLED_PIPES, s->signal_buf_count);
}

/*
 * BH callback: drains the thread-safe wake queue on the QEMU main loop,
 * writes the signals into guest memory, and raises the IRQ.  Runs under
 * the BQL so address_space_write / qemu_irq_raise are safe.
 */
static void goldfish_pipe_wake_bh(void *opaque)
{
    GoldfishPipeState *s = opaque;
    WakeEntry local[MAX_SIGNALLED_PIPES];
    uint32_t count;
    uint32_t signal_capacity = goldfish_pipe_signal_capacity(s);

    qemu_mutex_lock(&s->wake_lock);
    count = s->wake_count;
    if (count > MAX_SIGNALLED_PIPES) count = MAX_SIGNALLED_PIPES;
    memcpy(local, s->wake_queue, count * sizeof(local[0]));
    s->wake_count = 0;
    qemu_mutex_unlock(&s->wake_lock);

    for (uint32_t i = 0; i < count; i++) {
        if (s->sig_pending >= signal_capacity) {
            break;
        }

        uint32_t id    = local[i].id;
        uint32_t flags = local[i].flags;
        uint32_t gen   = local[i].gen;

        /* Discard stale wakes: pipe closed, or slot reused (generation mismatch). */
        if (id >= MAX_PIPES || !s->pipes[id]) continue;
        if (s->pipes[id]->gen != gen) continue;

        if (s->signal_buf_phys) {
            hwaddr addr = s->signal_buf_phys +
                s->sig_pending * sizeof(SignalledPipeBuffer);
            SignalledPipeBuffer entry = { .id = id, .flags = flags };
            address_space_write(&address_space_memory, addr,
                                MEMTXATTRS_UNSPECIFIED,
                                &entry, sizeof(entry));
        }

        s->sig_staging[s->sig_pending].id = id;
        s->sig_staging[s->sig_pending].flags = flags;
        s->sig_pending++;
    }

    if (s->sig_pending > 0) {
        qemu_irq_raise(s->irq);
    }
}

/*
 * pipe_signal_wake – safe to call from ANY thread (host RenderThread, etc.).
 * Appends to the lock-protected wake queue and schedules the BH.
 */
static void pipe_signal_wake(GoldfishPipeState *s, uint32_t id,
                             uint32_t flags, uint32_t gen)
{
    qemu_mutex_lock(&s->wake_lock);
    if (s->wake_count < MAX_SIGNALLED_PIPES) {
        s->wake_queue[s->wake_count].id    = id;
        s->wake_queue[s->wake_count].flags = flags;
        s->wake_queue[s->wake_count].gen   = gen;
        s->wake_count++;
    }
    qemu_mutex_unlock(&s->wake_lock);

    qemu_bh_schedule(s->wake_bh);
}

/* Read guest memory at a guest-physical address into a host buffer. */
static void guest_read(hwaddr gpa, void *buf, size_t len)
{
    address_space_read(&address_space_memory, gpa,
                       MEMTXATTRS_UNSPECIFIED, buf, len);
}

/* Write host buffer to guest-physical address. */
static void guest_write(hwaddr gpa, const void *buf, size_t len)
{
    address_space_write(&address_space_memory, gpa,
                        MEMTXATTRS_UNSPECIFIED, buf, len);
}

/* Write back the status field to the guest command buffer. */
static void write_cmd_status(HwPipe *p, int32_t status)
{
    hwaddr status_addr = p->cmd_phys + offsetof(GuestPipeCommand, status);
    guest_write(status_addr, &status, sizeof(status));
}

/* Write back consumed_size to the guest command buffer. */
static void write_cmd_consumed(HwPipe *p, int32_t consumed)
{
    hwaddr addr = p->cmd_phys + offsetof(GuestPipeCommand, consumed_size);
    guest_write(addr, &consumed, sizeof(consumed));
}

/* --------------------------------------------------------------------------
 * Command handlers.
 * -------------------------------------------------------------------------- */

static void cmd_open(GoldfishPipeState *s, uint32_t pipe_id)
{
    const GoldfishPipeServiceOps *ops = get_ops(s);
    if (!ops || !ops->guest_open) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "goldfish_pipe: no service ops for OPEN\n");
        if (pipe_id < MAX_PIPES && s->pipes[pipe_id]) {
            write_cmd_status(s->pipes[pipe_id], GP_ERROR_IO);
        }
        return;
    }

    /* Read the open params from guest memory. */
    OpenCommandParam open_params;
    guest_read(s->open_buf_phys, &open_params, sizeof(open_params));

    /* Allocate pipe if needed. */
    if (pipe_id >= MAX_PIPES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "goldfish_pipe: pipe id %u out of range\n", pipe_id);
        return;
    }

    HwPipe *p = s->pipes[pipe_id];
    if (!p) {
        p = g_new0(HwPipe, 1);
        p->id = pipe_id;
        p->dev = s;
        s->pipes[pipe_id] = p;
        if (pipe_id >= s->pipe_count) {
            s->pipe_count = pipe_id + 1;
        }
    }
    /* Bump generation so any in-flight wakes from a prior pipe in this
     * slot are detected as stale and discarded by the BH. */
    p->gen = ++s->pipe_gen[pipe_id];
    p->cmd_phys = open_params.command_buffer_ptr;

    /* Create the host-side pipe. */
    p->host_pipe = ops->guest_open((GoldfishHwPipe *)p);
    write_cmd_status(p, p->host_pipe ? 0 : GP_ERROR_IO);
}

static void cmd_close(GoldfishPipeState *s, uint32_t pipe_id)
{
    if (pipe_id >= MAX_PIPES || !s->pipes[pipe_id]) return;

    HwPipe *p = s->pipes[pipe_id];
    const GoldfishPipeServiceOps *ops = get_ops(s);

    if (ops && ops->guest_close && p->host_pipe) {
        ops->guest_close(p->host_pipe, GP_CLOSE_GRACEFUL);
    }
    write_cmd_status(p, 0);

    s->pipes[pipe_id] = NULL;
    g_free(p);
}

static void cmd_poll(GoldfishPipeState *s, uint32_t pipe_id)
{
    if (pipe_id >= MAX_PIPES || !s->pipes[pipe_id]) return;

    HwPipe *p = s->pipes[pipe_id];
    const GoldfishPipeServiceOps *ops = get_ops(s);

    if (!ops || !ops->guest_poll || !p->host_pipe) {
        write_cmd_status(p, GP_ERROR_IO);
        return;
    }

    int status = ops->guest_poll(p->host_pipe);
    write_cmd_status(p, status);
}

static void cmd_read_write(GoldfishPipeState *s, uint32_t pipe_id,
                           bool is_write)
{
    if (pipe_id >= MAX_PIPES || !s->pipes[pipe_id]) return;

    HwPipe *p = s->pipes[pipe_id];
    const GoldfishPipeServiceOps *ops = get_ops(s);

    if (!ops || !p->host_pipe) {
        write_cmd_status(p, GP_ERROR_IO);
        return;
    }

    /* Read rw_params from guest command buffer. */
    uint32_t buffers_count = 0;
    guest_read(p->cmd_phys + offsetof(GuestPipeCommand, buffers_count),
               &buffers_count, sizeof(buffers_count));

    if (buffers_count == 0 || buffers_count > MAX_BUFFERS) {
        write_cmd_status(p, GP_ERROR_INVAL);
        return;
    }

    /* Read the pointer and size arrays from guest memory. */
    uint64_t *ptrs = g_new(uint64_t, buffers_count);
    uint32_t *sizes = g_new(uint32_t, buffers_count);

    guest_read(p->cmd_phys + offsetof(GuestPipeCommand, ptrs),
               ptrs, buffers_count * sizeof(uint64_t));
    guest_read(p->cmd_phys + offsetof(GuestPipeCommand, sizes),
               sizes, buffers_count * sizeof(uint32_t));

    /* Build host-side buffers by mapping guest physical pages. */
    GoldfishPipeBuffer *bufs = g_new0(GoldfishPipeBuffer, buffers_count);
    void **allocs = g_new(void *, buffers_count);

    for (uint32_t i = 0; i < buffers_count; i++) {
        bufs[i].size = sizes[i];
        bufs[i].data = g_malloc(sizes[i]);
        allocs[i] = bufs[i].data;

        if (!is_write) {
            /* For read (guest reads from host), no pre-copy needed. */
        } else {
            /* For write (guest writes to host), copy data from guest. */
            guest_read(ptrs[i], bufs[i].data, sizes[i]);
        }
    }

    int result;
    if (is_write) {
        result = ops->guest_send(&p->host_pipe, bufs, buffers_count);
    } else {
        result = ops->guest_recv(p->host_pipe, bufs, buffers_count);
    }

    if (!is_write && result > 0) {
        /* Copy data back to guest for reads. */
        int remaining = result;
        for (uint32_t i = 0; i < buffers_count && remaining > 0; i++) {
            int copy = remaining < (int)sizes[i] ? remaining : (int)sizes[i];
            guest_write(ptrs[i], bufs[i].data, copy);
            remaining -= copy;
        }
    }

    write_cmd_status(p, result < 0 ? result : 0);
    write_cmd_consumed(p, result > 0 ? result : 0);

    for (uint32_t i = 0; i < buffers_count; i++) {
        g_free(allocs[i]);
    }
    g_free(allocs);
    g_free(bufs);
    g_free(ptrs);
    g_free(sizes);
}

static void cmd_wake_on(GoldfishPipeState *s, uint32_t pipe_id, bool on_write)
{
    if (pipe_id >= MAX_PIPES || !s->pipes[pipe_id]) return;

    HwPipe *p = s->pipes[pipe_id];
    const GoldfishPipeServiceOps *ops = get_ops(s);

    if (on_write) {
        p->wanted_wake_write = true;
    } else {
        p->wanted_wake_read = true;
    }

    if (ops && ops->guest_wake_on && p->host_pipe) {
        GoldfishPipeWakeFlags flags = 0;
        if (p->wanted_wake_read)  flags |= GP_WAKE_READ;
        if (p->wanted_wake_write) flags |= GP_WAKE_WRITE;
        ops->guest_wake_on(p->host_pipe, flags);
    }

    /* Check if the pipe is already ready and signal immediately. */
    if (ops && ops->guest_poll && p->host_pipe) {
        int poll = ops->guest_poll(p->host_pipe);
        uint32_t wake = 0;
        if ((poll & GP_POLL_IN) && p->wanted_wake_read) {
            wake |= GP_WAKE_READ;
            p->wanted_wake_read = false;
        }
        if ((poll & GP_POLL_OUT) && p->wanted_wake_write) {
            wake |= GP_WAKE_WRITE;
            p->wanted_wake_write = false;
        }
        if (poll & GP_POLL_HUP) {
            wake |= GP_WAKE_CLOSED;
        }
        if (wake) {
            pipe_signal_wake(s, pipe_id, wake, p->gen);
        }
    }

    write_cmd_status(p, 0);
}

/* --------------------------------------------------------------------------
 * MMIO operations.
 * -------------------------------------------------------------------------- */

static uint64_t goldfish_pipe_read(void *opaque, hwaddr offset, unsigned size)
{
    GoldfishPipeState *s = GOLDFISH_PIPE(opaque);

    switch (offset) {
    case PIPE_REG_VERSION:
        return PIPE_DEVICE_VERSION;

    case PIPE_REG_GET_SIGNALLED: {
        uint32_t count = MIN(s->sig_pending, goldfish_pipe_signal_capacity(s));
        s->sig_pending = 0;
        qemu_irq_lower(s->irq);
        return count;
    }

    default:
        qemu_log_mask(LOG_UNIMP,
                      "goldfish_pipe: unhandled read at 0x%02" HWADDR_PRIx "\n",
                      offset);
        return 0;
    }
}

static void goldfish_pipe_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    GoldfishPipeState *s = GOLDFISH_PIPE(opaque);

    switch (offset) {
    case PIPE_REG_CMD: {
        uint32_t pipe_id = (uint32_t)value;
        if (pipe_id >= MAX_PIPES) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "goldfish_pipe: CMD with invalid pipe id %u\n",
                          pipe_id);
            return;
        }

        /* For OPEN, pipe may not exist yet – that's fine. */
        HwPipe *p = (pipe_id < MAX_PIPES) ? s->pipes[pipe_id] : NULL;
        int32_t cmd = -1;

        if (p && p->cmd_phys) {
            guest_read(p->cmd_phys, &cmd, sizeof(cmd));
        } else {
            /* For OPEN, read cmd from wherever the open flow expects. */
            cmd = PIPE_CMD_OPEN;
        }

        switch (cmd) {
        case PIPE_CMD_OPEN:
            cmd_open(s, pipe_id);
            break;
        case PIPE_CMD_CLOSE:
            cmd_close(s, pipe_id);
            break;
        case PIPE_CMD_POLL:
            cmd_poll(s, pipe_id);
            break;
        case PIPE_CMD_WRITE:
            cmd_read_write(s, pipe_id, true);
            break;
        case PIPE_CMD_READ:
            cmd_read_write(s, pipe_id, false);
            break;
        case PIPE_CMD_WAKE_ON_WRITE:
            cmd_wake_on(s, pipe_id, true);
            break;
        case PIPE_CMD_WAKE_ON_READ:
            cmd_wake_on(s, pipe_id, false);
            break;
        default:
            qemu_log_mask(LOG_UNIMP,
                          "goldfish_pipe: unknown cmd %d for pipe %u\n",
                          cmd, pipe_id);
            if (p) write_cmd_status(p, GP_ERROR_INVAL);
            break;
        }
        break;
    }

    case PIPE_REG_SIGNAL_BUFFER_HIGH:
        s->signal_buf_phys = (s->signal_buf_phys & 0xFFFFFFFFULL) |
                             ((uint64_t)value << 32);
        break;

    case PIPE_REG_SIGNAL_BUFFER:
        s->signal_buf_phys = (s->signal_buf_phys & 0xFFFFFFFF00000000ULL) |
                             (value & 0xFFFFFFFF);
        break;

    case PIPE_REG_SIGNAL_BUFFER_COUNT:
        s->signal_buf_count = (uint32_t)value;
        s->sig_pending = MIN(s->sig_pending, goldfish_pipe_signal_capacity(s));
        break;

    case PIPE_REG_OPEN_BUFFER_HIGH:
        s->open_buf_phys = (s->open_buf_phys & 0xFFFFFFFFULL) |
                           ((uint64_t)value << 32);
        break;

    case PIPE_REG_OPEN_BUFFER:
        s->open_buf_phys = (s->open_buf_phys & 0xFFFFFFFF00000000ULL) |
                           (value & 0xFFFFFFFF);
        break;

    case PIPE_REG_VERSION:
        /* Guest writes its version – we accept and ignore. */
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "goldfish_pipe: unhandled write 0x%" PRIx64
                      " at 0x%02" HWADDR_PRIx "\n",
                      value, offset);
        break;
    }
}

static const MemoryRegionOps goldfish_pipe_ops = {
    .read  = goldfish_pipe_read,
    .write = goldfish_pipe_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.max_access_size = 4,
    .impl.max_access_size  = 4,
    .impl.min_access_size  = 4,
};

/* --------------------------------------------------------------------------
 * HW funcs – callbacks from host service bridge into the QEMU device.
 * -------------------------------------------------------------------------- */

static void hw_signal_wake(GoldfishHwPipe *hw_pipe,
                           GoldfishPipeWakeFlags flags)
{
    HwPipe *p = (HwPipe *)hw_pipe;
    if (!p || !p->dev) return;

    GoldfishPipeState *s = p->dev;
    pipe_signal_wake(s, p->id, (uint32_t)flags, p->gen);
}

static void hw_close_from_host(GoldfishHwPipe *hw_pipe)
{
    /* Not needed for current services; stub. */
    (void)hw_pipe;
}

static void hw_reset_pipe(GoldfishHwPipe *hw_pipe,
                          GoldfishHostPipe *new_host)
{
    HwPipe *p = (HwPipe *)hw_pipe;
    if (p) p->host_pipe = new_host;
}

static int hw_get_id(GoldfishHwPipe *hw_pipe)
{
    HwPipe *p = (HwPipe *)hw_pipe;
    return p ? (int)p->id : -1;
}

static GoldfishHwPipe *hw_lookup_by_id(int id)
{
    /* Snapshot restore path – not yet wired. */
    return NULL;
}

static const GoldfishPipeHwFuncs s_hw_funcs = {
    .signal_wake    = hw_signal_wake,
    .close_from_host = hw_close_from_host,
    .reset_pipe     = hw_reset_pipe,
    .get_id         = hw_get_id,
    .lookup_by_id   = hw_lookup_by_id,
};

/* --------------------------------------------------------------------------
 * Device lifecycle.
 * -------------------------------------------------------------------------- */

static void goldfish_pipe_realize(DeviceState *dev, Error **errp)
{
    GoldfishPipeState *s = GOLDFISH_PIPE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &goldfish_pipe_ops, s,
                          "goldfish_pipe", GOLDFISH_PIPE_IOMEM_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->version = PIPE_DEVICE_VERSION;

    /* Thread-safe wake queue + BH for deferring signals to main loop. */
    qemu_mutex_init(&s->wake_lock);
    s->wake_bh = qemu_bh_new(goldfish_pipe_wake_bh, s);

    /* Install hw funcs so the host service bridge can signal wakes. */
    rutabaga_gfxstream_set_service_hw_funcs(&s_hw_funcs);
}

static void goldfish_pipe_unrealize(DeviceState *dev)
{
    GoldfishPipeState *s = GOLDFISH_PIPE(dev);
    const GoldfishPipeServiceOps *ops = get_ops(s);

    for (uint32_t i = 0; i < MAX_PIPES; i++) {
        if (s->pipes[i]) {
            if (ops && ops->guest_close && s->pipes[i]->host_pipe) {
                ops->guest_close(s->pipes[i]->host_pipe, GP_CLOSE_REBOOT);
            }
            g_free(s->pipes[i]);
            s->pipes[i] = NULL;
        }
    }

    if (s->wake_bh) {
        qemu_bh_delete(s->wake_bh);
        s->wake_bh = NULL;
    }
    qemu_mutex_destroy(&s->wake_lock);
}

/*
 * Snapshot: only global device registers are persisted.  Per-pipe host
 * state is NOT saved here; the host service bridge force-closes all
 * pipes on restore (see GoldfishPipeService.cpp svc_guest_load).
 * The guest kernel re-opens pipes after resume, which is the defined
 * first-pass snapshot policy per PLAN.md.
 */
static const VMStateDescription vmstate_goldfish_pipe = {
    .name = TYPE_GOLDFISH_PIPE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(version, GoldfishPipeState),
        VMSTATE_UINT64(signal_buf_phys, GoldfishPipeState),
        VMSTATE_UINT32(signal_buf_count, GoldfishPipeState),
        VMSTATE_UINT64(open_buf_phys, GoldfishPipeState),
        VMSTATE_UINT32(pipe_count, GoldfishPipeState),
        VMSTATE_END_OF_LIST()
    }
};

static void goldfish_pipe_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize   = goldfish_pipe_realize;
    dc->unrealize = goldfish_pipe_unrealize;
    dc->vmsd      = &vmstate_goldfish_pipe;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo goldfish_pipe_info = {
    .name          = TYPE_GOLDFISH_PIPE,
    .parent        = TYPE_DYNAMIC_SYS_BUS_DEVICE,
    .instance_size = sizeof(GoldfishPipeState),
    .class_init    = goldfish_pipe_class_init,
};

static void goldfish_pipe_register_types(void)
{
    type_register_static(&goldfish_pipe_info);
}

type_init(goldfish_pipe_register_types)

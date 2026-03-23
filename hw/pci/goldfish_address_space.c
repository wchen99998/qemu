/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"

#include <rutabaga_gfx/rutabaga_gfx_ffi.h>

#include "hw/acpi/goldfish_defs.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/pci/goldfish_address_space.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "qapi/error.h"
#include "system/memory.h"

typedef uint32_t (*address_space_device_gen_handle_t)(void);
typedef void (*address_space_device_destroy_handle_t)(uint32_t handle);
typedef void (*address_space_device_tell_ping_info_t)(uint32_t handle, uint64_t gpa);
typedef void (*address_space_device_ping_t)(uint32_t handle);
typedef int (*address_space_device_add_memory_mapping_t)(uint64_t gpa,
                                                         void *ptr,
                                                         uint64_t size);
typedef int (*address_space_device_remove_memory_mapping_t)(uint64_t gpa,
                                                            void *ptr,
                                                            uint64_t size);
typedef void *(*address_space_device_get_host_ptr_t)(uint64_t gpa);
typedef void *(*address_space_device_handle_to_context_t)(uint32_t handle);
typedef void (*address_space_device_clear_t)(void);
typedef uint64_t (*address_space_device_hostmem_register_t)(const void *entry);
typedef void (*address_space_device_hostmem_unregister_t)(uint64_t id);
typedef void (*address_space_device_ping_at_hva_t)(uint32_t handle, void *hva);
typedef void (*address_space_device_deallocation_callback_t)(void *context, uint64_t gpa);
typedef void (*address_space_device_register_deallocation_callback_t)(
    void *context,
    uint64_t gpa,
    address_space_device_deallocation_callback_t callback);
typedef void (*address_space_device_run_deallocation_callbacks_t)(uint64_t gpa);
typedef const void *(*address_space_device_control_get_hw_funcs_t)(void);
typedef void (*address_space_device_create_instance_t)(const void *create);

typedef struct qemu_address_space_device_control_ops {
    address_space_device_gen_handle_t gen_handle;
    address_space_device_destroy_handle_t destroy_handle;
    address_space_device_tell_ping_info_t tell_ping_info;
    address_space_device_ping_t ping;
    address_space_device_add_memory_mapping_t add_memory_mapping;
    address_space_device_remove_memory_mapping_t remove_memory_mapping;
    address_space_device_get_host_ptr_t get_host_ptr;
    address_space_device_handle_to_context_t handle_to_context;
    address_space_device_clear_t clear;
    address_space_device_hostmem_register_t hostmem_register;
    address_space_device_hostmem_unregister_t hostmem_unregister;
    address_space_device_ping_at_hva_t ping_at_hva;
    address_space_device_register_deallocation_callback_t register_deallocation_callback;
    address_space_device_run_deallocation_callbacks_t run_deallocation_callbacks;
    address_space_device_control_get_hw_funcs_t control_get_hw_funcs;
    address_space_device_create_instance_t create_instance;
} qemu_address_space_device_control_ops;

typedef struct AddressSpaceHwFuncs {
    int (*allocSharedHostRegion)(uint64_t page_aligned_size, uint64_t *offset);
    int (*freeSharedHostRegion)(uint64_t offset);
    int (*allocSharedHostRegionLocked)(uint64_t page_aligned_size, uint64_t *offset);
    int (*freeSharedHostRegionLocked)(uint64_t offset);
    uint64_t (*getPhysAddrStart)(void);
    uint64_t (*getPhysAddrStartLocked)(void);
    uint32_t (*getGuestPageSize)(void);
    int (*allocSharedHostRegionFixedLocked)(uint64_t page_aligned_size, uint64_t offset);
} AddressSpaceHwFuncs;

struct address_block {
    uint64_t offset;
    union {
        uint64_t size_available;
        struct {
            uint64_t size : 63;
            uint64_t available : 1;
        };
    };
    uint64_t map_size;
};

struct address_space_allocator {
    struct address_block *blocks;
    int32_t size;
    int32_t capacity;
    uint64_t total_bytes;
};

struct goldfish_address_space_ping {
    uint64_t offset;
    uint64_t size;
    uint64_t metadata;
    uint32_t version;
    uint32_t wait_fd;
    uint32_t wait_flags;
    uint32_t direction;
};

enum address_space_register_id {
    ADDRESS_SPACE_REGISTER_COMMAND = 0,
    ADDRESS_SPACE_REGISTER_STATUS = 4,
    ADDRESS_SPACE_REGISTER_GUEST_PAGE_SIZE = 8,
    ADDRESS_SPACE_REGISTER_BLOCK_SIZE_LOW = 12,
    ADDRESS_SPACE_REGISTER_BLOCK_SIZE_HIGH = 16,
    ADDRESS_SPACE_REGISTER_BLOCK_OFFSET_LOW = 20,
    ADDRESS_SPACE_REGISTER_BLOCK_OFFSET_HIGH = 24,
    ADDRESS_SPACE_REGISTER_PING = 28,
    ADDRESS_SPACE_REGISTER_PING_INFO_ADDR_LOW = 32,
    ADDRESS_SPACE_REGISTER_PING_INFO_ADDR_HIGH = 36,
    ADDRESS_SPACE_REGISTER_HANDLE = 40,
    ADDRESS_SPACE_REGISTER_PHYS_START_LOW = 44,
    ADDRESS_SPACE_REGISTER_PHYS_START_HIGH = 48,
    ADDRESS_SPACE_REGISTER_PING_WITH_DATA = 52,
};

enum address_space_command_id {
    ADDRESS_SPACE_COMMAND_ALLOCATE_BLOCK = 1,
    ADDRESS_SPACE_COMMAND_DEALLOCATE_BLOCK = 2,
    ADDRESS_SPACE_COMMAND_GEN_HANDLE = 3,
    ADDRESS_SPACE_COMMAND_DESTROY_HANDLE = 4,
    ADDRESS_SPACE_COMMAND_TELL_PING_INFO_ADDR = 5,
};

struct address_space_registers {
    uint32_t status;
    uint32_t guest_page_size;
    uint32_t size_low;
    uint32_t size_high;
    uint32_t offset_low;
    uint32_t offset_high;
    uint32_t ping;
    uint32_t ping_info_addr_low;
    uint32_t ping_info_addr_high;
    uint32_t handle;
    uint32_t phys_start_low;
    uint32_t phys_start_high;
};

static const char *address_space_register_name(enum address_space_register_id reg)
{
    switch (reg) {
    case ADDRESS_SPACE_REGISTER_COMMAND:
        return "COMMAND";
    case ADDRESS_SPACE_REGISTER_STATUS:
        return "STATUS";
    case ADDRESS_SPACE_REGISTER_GUEST_PAGE_SIZE:
        return "GUEST_PAGE_SIZE";
    case ADDRESS_SPACE_REGISTER_BLOCK_SIZE_LOW:
        return "BLOCK_SIZE_LOW";
    case ADDRESS_SPACE_REGISTER_BLOCK_SIZE_HIGH:
        return "BLOCK_SIZE_HIGH";
    case ADDRESS_SPACE_REGISTER_BLOCK_OFFSET_LOW:
        return "BLOCK_OFFSET_LOW";
    case ADDRESS_SPACE_REGISTER_BLOCK_OFFSET_HIGH:
        return "BLOCK_OFFSET_HIGH";
    case ADDRESS_SPACE_REGISTER_PING:
        return "PING";
    case ADDRESS_SPACE_REGISTER_PING_INFO_ADDR_LOW:
        return "PING_INFO_ADDR_LOW";
    case ADDRESS_SPACE_REGISTER_PING_INFO_ADDR_HIGH:
        return "PING_INFO_ADDR_HIGH";
    case ADDRESS_SPACE_REGISTER_HANDLE:
        return "HANDLE";
    case ADDRESS_SPACE_REGISTER_PHYS_START_LOW:
        return "PHYS_START_LOW";
    case ADDRESS_SPACE_REGISTER_PHYS_START_HIGH:
        return "PHYS_START_HIGH";
    case ADDRESS_SPACE_REGISTER_PING_WITH_DATA:
        return "PING_WITH_DATA";
    default:
        return "UNKNOWN";
    }
}

static const char *address_space_command_name(enum address_space_command_id cmd)
{
    switch (cmd) {
    case ADDRESS_SPACE_COMMAND_ALLOCATE_BLOCK:
        return "ALLOCATE_BLOCK";
    case ADDRESS_SPACE_COMMAND_DEALLOCATE_BLOCK:
        return "DEALLOCATE_BLOCK";
    case ADDRESS_SPACE_COMMAND_GEN_HANDLE:
        return "GEN_HANDLE";
    case ADDRESS_SPACE_COMMAND_DESTROY_HANDLE:
        return "DESTROY_HANDLE";
    case ADDRESS_SPACE_COMMAND_TELL_PING_INFO_ADDR:
        return "TELL_PING_INFO_ADDR";
    default:
        return "UNKNOWN";
    }
}

typedef struct GoldfishAddressSpaceState {
    PCIDevice parent_obj;
    MemoryRegion control_io;
    MemoryRegion area_mem;
    void *area_mem_ptr;
    qemu_irq irq;
    QemuMutex mutex;
    GHashTable *ping_info_gpas;

    struct address_space_registers registers;
    struct address_space_allocator allocator;

    const void *previous_hw_funcs;
    bool previous_hw_funcs_saved;
} GoldfishAddressSpaceState;

#define TYPE_GOLDFISH_ADDRESS_SPACE GOLDFISH_ADDRESS_SPACE_NAME
OBJECT_DECLARE_SIMPLE_TYPE(GoldfishAddressSpaceState, GOLDFISH_ADDRESS_SPACE)

#define ANDROID_EMU_ADDRESS_SPACE_BAD_OFFSET (~(uint64_t)0)
#if defined(__APPLE__) && defined(__arm64__)
#define DEFAULT_GUEST_PAGE_SIZE 16384
#else
#define DEFAULT_GUEST_PAGE_SIZE 4096
#endif

static int null_address_space_memory_state_load(QEMUFile *file)
{
    (void)file;
    return 0;
}

static int null_address_space_memory_state_save(QEMUFile *file)
{
    (void)file;
    return 0;
}

static const GoldfishAddressSpaceOps goldfish_address_space_null_ops = {
    .load = null_address_space_memory_state_load,
    .save = null_address_space_memory_state_save,
};

static const GoldfishAddressSpaceOps *s_goldfish_address_space_ops =
    &goldfish_address_space_null_ops;
static const qemu_address_space_device_control_ops *s_address_space_device_control_ops;
static GoldfishAddressSpaceState *s_current_state;
static int s_verbose_logging;
int goldfish_as_oob_access = 1;
static const AddressSpaceHwFuncs s_goldfish_address_space_hw_funcs = {
    .allocSharedHostRegion = goldfish_address_space_alloc_shared_host_region,
    .freeSharedHostRegion = goldfish_address_space_free_shared_host_region,
    .allocSharedHostRegionLocked = goldfish_address_space_alloc_shared_host_region_locked,
    .freeSharedHostRegionLocked = goldfish_address_space_free_shared_host_region_locked,
    .getPhysAddrStart = goldfish_address_space_get_phys_addr_start,
    .getPhysAddrStartLocked = goldfish_address_space_get_phys_addr_start_locked,
    .getGuestPageSize = goldfish_address_space_get_guest_page_size,
    .allocSharedHostRegionFixedLocked =
        goldfish_address_space_alloc_shared_host_region_fixed_locked,
};

static void address_space_assert(bool condition)
{
    g_assert(condition);
}

static void *address_space_malloc0(size_t size)
{
    return g_malloc0(size);
}

static void *address_space_realloc(void *ptr, size_t size)
{
    return g_realloc(ptr, size);
}

static void address_space_free(void *ptr)
{
    g_free(ptr);
}

static uint64_t merge_u64(uint64_t low, uint64_t high)
{
    return low | (high << 32);
}

static uint32_t lower_32_bits(uint64_t value)
{
    return (uint32_t)value;
}

static uint32_t upper_32_bits(uint64_t value)
{
    return value >> 32;
}

static uint64_t address_space_align_size(uint64_t page_size, uint64_t size)
{
    address_space_assert(page_size > 0);
    if (!size) {
        return 0;
    }
    return ((size + page_size - 1) / page_size) * page_size;
}

static int address_space_allocator_find_available_block(struct address_block *block,
                                                        int n_blocks,
                                                        uint64_t size_at_least)
{
    int index = -1;
    uint64_t size_at_index = 0;
    int i;

    address_space_assert(n_blocks >= 1);
    for (i = 0; i < n_blocks; ++i, ++block) {
        uint64_t this_size = block->size;
        address_space_assert(this_size > 0);
        if (this_size >= size_at_least && block->available &&
            (index < 0 || this_size < size_at_index)) {
            index = i;
            size_at_index = this_size;
        }
    }

    return index;
}

static int address_space_allocator_find_available_block_at_offset(
    struct address_block *block,
    int n_blocks,
    uint64_t size_at_least,
    uint64_t offset)
{
    int i;

    address_space_assert(n_blocks >= 1);
    for (i = 0; i < n_blocks; ++i, ++block) {
        uint64_t this_size = block->size;
        uint64_t block_offset;
        uint64_t offset_in_block;

        address_space_assert(this_size > 0);
        block_offset = block->offset;
        if (!block->available || offset < block_offset) {
            continue;
        }

        offset_in_block = offset - block_offset;
        if (offset_in_block < this_size &&
            size_at_least <= this_size - offset_in_block) {
            return i;
        }
    }

    return -1;
}

static int address_space_allocator_grow_capacity(int old_capacity)
{
    address_space_assert(old_capacity >= 1);
    return old_capacity + old_capacity;
}

static struct address_block *address_space_allocator_split_block(
    struct address_space_allocator *allocator,
    int index,
    uint64_t size)
{
    struct address_block *blocks;
    struct address_block *to_borrow_from;
    struct address_block *new_block;
    uint64_t new_size;

    address_space_assert(allocator->capacity >= 1);
    address_space_assert(allocator->size >= 1);
    address_space_assert(allocator->size <= allocator->capacity);
    address_space_assert(index >= 0);
    address_space_assert(index < allocator->size);
    address_space_assert(size < allocator->blocks[index].size);

    if (allocator->size == allocator->capacity) {
        int new_capacity = address_space_allocator_grow_capacity(allocator->capacity);
        allocator->blocks = address_space_realloc(
            allocator->blocks, sizeof(struct address_block) * new_capacity);
        address_space_assert(allocator->blocks);
        allocator->capacity = new_capacity;
    }

    blocks = allocator->blocks;
    memmove(&blocks[index + 2], &blocks[index + 1],
            sizeof(struct address_block) * (allocator->size - index - 1));

    to_borrow_from = &blocks[index];
    new_block = to_borrow_from + 1;
    new_size = to_borrow_from->size - size;

    to_borrow_from->size = new_size;
    new_block->offset = to_borrow_from->offset + new_size;
    new_block->size = size;
    new_block->available = 1;
    new_block->map_size = 0;

    ++allocator->size;
    return new_block;
}

static struct address_block *address_space_allocator_split_block_at_offset(
    struct address_space_allocator *allocator,
    int index,
    uint64_t size,
    uint64_t offset)
{
    struct address_block *blocks;
    struct address_block *to_borrow_from;
    struct address_block *new_block;
    struct address_block *extra_block;
    uint64_t old_block_size;
    bool need_extra_block;
    int new_block_index_offset;

    address_space_assert(allocator->capacity >= 1);
    address_space_assert(allocator->size >= 1);
    address_space_assert(allocator->size <= allocator->capacity);
    address_space_assert(index >= 0);
    address_space_assert(index < allocator->size);
    address_space_assert(size < allocator->blocks[index].size);

    need_extra_block =
        (allocator->blocks[index].size - size -
         (offset - allocator->blocks[index].offset)) != 0;
    new_block_index_offset = need_extra_block ? 1 : 0;

    if (allocator->size + new_block_index_offset >= allocator->capacity) {
        int new_capacity =
            address_space_allocator_grow_capacity(allocator->capacity +
                                                  new_block_index_offset);
        allocator->blocks = address_space_realloc(
            allocator->blocks, sizeof(struct address_block) * new_capacity);
        address_space_assert(allocator->blocks);
        allocator->capacity = new_capacity;
    }

    blocks = allocator->blocks;
    memmove(&blocks[index + 2 + new_block_index_offset], &blocks[index + 1],
            sizeof(struct address_block) * (allocator->size - index - 1));

    to_borrow_from = &blocks[index];
    new_block = to_borrow_from + 1;
    extra_block = to_borrow_from + 2;

    old_block_size = to_borrow_from->size;
    to_borrow_from->size = offset - to_borrow_from->offset;

    new_block->offset = offset;
    new_block->size = size;
    new_block->available = 1;
    new_block->map_size = 0;
    ++allocator->size;

    if (need_extra_block) {
        extra_block->offset = offset + size;
        extra_block->size = old_block_size - size - to_borrow_from->size;
        extra_block->available = 1;
        extra_block->map_size = 0;
        ++allocator->size;
    }

    return new_block;
}

static void address_space_allocator_release_block(struct address_space_allocator *allocator,
                                                  int index)
{
    struct address_block *blocks = allocator->blocks;
    int before = index - 1;
    int after = index + 1;
    int size = allocator->size;

    address_space_assert(index >= 0);
    address_space_assert(index < size);

    blocks[index].available = 1;

    if (before >= 0 && blocks[before].available) {
        if (after < size && blocks[after].available) {
            blocks[before].size += blocks[index].size + blocks[after].size;
            size -= 2;
            memmove(&blocks[index], &blocks[index + 2],
                    sizeof(struct address_block) * (size - index));
            allocator->size = size;
        } else {
            blocks[before].size += blocks[index].size;
            --size;
            memmove(&blocks[index], &blocks[index + 1],
                    sizeof(struct address_block) * (size - index));
            allocator->size = size;
        }
    } else if (after < size && blocks[after].available) {
        blocks[index].size += blocks[after].size;
        --size;
        memmove(&blocks[after], &blocks[after + 1],
                sizeof(struct address_block) * (size - after));
        allocator->size = size;
    }
}

static uint64_t address_space_allocator_allocate(struct address_space_allocator *allocator,
                                                 uint64_t size)
{
    int index = address_space_allocator_find_available_block(allocator->blocks,
                                                             allocator->size,
                                                             size);
    struct address_block *block;

    if (index < 0) {
        return ANDROID_EMU_ADDRESS_SPACE_BAD_OFFSET;
    }

    address_space_assert(index < allocator->size);
    block = &allocator->blocks[index];
    address_space_assert(block->size >= size);
    if (block->size > size) {
        block = address_space_allocator_split_block(allocator, index, size);
    }

    address_space_assert(block->size == size);
    block->available = 0;
    return block->offset;
}

static int address_space_allocator_allocate_fixed(struct address_space_allocator *allocator,
                                                  uint64_t size,
                                                  uint64_t offset)
{
    int index = address_space_allocator_find_available_block_at_offset(allocator->blocks,
                                                                       allocator->size,
                                                                       size,
                                                                       offset);
    struct address_block *block;

    if (index < 0) {
        return -ENOMEM;
    }

    address_space_assert(index < allocator->size);
    block = &allocator->blocks[index];
    address_space_assert(block->size >= size);
    if (block->size > size) {
        block = address_space_allocator_split_block_at_offset(allocator, index,
                                                              size, offset);
    }

    address_space_assert(block->size == size);
    block->available = 0;
    return 0;
}

static uint32_t address_space_allocator_deallocate(struct address_space_allocator *allocator,
                                                   uint64_t offset)
{
    struct address_block *block = allocator->blocks;
    int size = allocator->size;
    int index;

    address_space_assert(size >= 1);
    for (index = 0; index < size; ++index, ++block) {
        if (block->offset == offset) {
            if (block->available) {
                return EINVAL;
            }
            address_space_allocator_release_block(allocator, index);
            return 0;
        }
    }

    return EINVAL;
}

static void address_space_allocator_init(struct address_space_allocator *allocator,
                                         uint64_t size,
                                         int initial_capacity)
{
    struct address_block *block;

    address_space_assert(initial_capacity >= 1);
    allocator->blocks = address_space_malloc0(sizeof(struct address_block) *
                                              initial_capacity);
    address_space_assert(allocator->blocks);

    block = allocator->blocks;
    block->offset = 0;
    block->size = size;
    block->available = 1;

    allocator->size = 1;
    allocator->capacity = initial_capacity;
    allocator->total_bytes = size;
}

static void address_space_allocator_destroy(struct address_space_allocator *allocator)
{
    address_space_assert(allocator->size == 1);
    address_space_assert(allocator->capacity >= allocator->size);
    address_space_assert(allocator->blocks[0].available);
    address_space_free(allocator->blocks);
}

void goldfish_address_space_set_service_ops(const GoldfishAddressSpaceOps *ops)
{
    s_goldfish_address_space_ops = ops ? ops : &goldfish_address_space_null_ops;
}

static void goldfish_address_space_activate_hw_funcs(GoldfishAddressSpaceState *state)
{
    const void *previous;

    previous = rutabaga_gfxstream_set_address_space_hw_funcs(
        &s_goldfish_address_space_hw_funcs);

    if (!state->previous_hw_funcs_saved) {
        state->previous_hw_funcs = previous;
        state->previous_hw_funcs_saved = true;
    }
}

static uint32_t address_space_allocate_block(GoldfishAddressSpaceState *state)
{
    struct address_space_registers *regs = &state->registers;
    uint64_t size = merge_u64(regs->size_low, regs->size_high);
    uint64_t aligned_size = address_space_align_size(regs->guest_page_size, size);
    uint64_t offset;

    if (!aligned_size) {
        return EINVAL;
    }

    offset = address_space_allocator_allocate(&state->allocator, aligned_size);
    regs->offset_low = lower_32_bits(offset);
    regs->offset_high = upper_32_bits(offset);
    regs->size_low = lower_32_bits(aligned_size);
    regs->size_high = upper_32_bits(aligned_size);

    if (s_verbose_logging) {
        uint64_t phys_start = goldfish_address_space_get_phys_addr_start_locked();
        fprintf(stderr,
                "%s: allocate offset 0x%llx physStart 0x%llx gpa 0x%llx\n",
                __func__,
                (unsigned long long)offset,
                (unsigned long long)phys_start,
                (unsigned long long)(phys_start + offset));
    }

    return offset == ANDROID_EMU_ADDRESS_SPACE_BAD_OFFSET ? ENOMEM : 0;
}

static uint32_t address_space_deallocate_block(GoldfishAddressSpaceState *state)
{
    struct address_space_registers *regs = &state->registers;
    uint64_t offset = merge_u64(regs->offset_low, regs->offset_high);
    uint64_t phys_start = goldfish_address_space_get_phys_addr_start_locked();

    if (s_verbose_logging) {
        fprintf(stderr,
                "%s: deallocate offset 0x%llx physStart 0x%llx gpa 0x%llx\n",
                __func__,
                (unsigned long long)offset,
                (unsigned long long)phys_start,
                (unsigned long long)(phys_start + offset));
    }

    if (phys_start && s_address_space_device_control_ops &&
        s_address_space_device_control_ops->run_deallocation_callbacks) {
        s_address_space_device_control_ops->run_deallocation_callbacks(phys_start + offset);
    }

    return address_space_allocator_deallocate(&state->allocator, offset);
}

static void address_space_set_ping_info_gpa(GoldfishAddressSpaceState *state,
                                            uint32_t handle,
                                            uint64_t ping_info_gpa)
{
    uint64_t *stored_gpa = g_new(uint64_t, 1);

    *stored_gpa = ping_info_gpa;
    g_hash_table_replace(state->ping_info_gpas,
                         GUINT_TO_POINTER(handle), stored_gpa);
}

static bool address_space_get_ping_info_gpa(GoldfishAddressSpaceState *state,
                                            uint32_t handle,
                                            uint64_t *ping_info_gpa)
{
    uint64_t *stored_gpa =
        g_hash_table_lookup(state->ping_info_gpas, GUINT_TO_POINTER(handle));

    if (!stored_gpa) {
        return false;
    }

    *ping_info_gpa = *stored_gpa;
    return true;
}

static uint32_t address_space_run_command(GoldfishAddressSpaceState *state,
                                          enum address_space_command_id cmd)
{
    uint32_t result;
    uint64_t ping_info_gpa =
        merge_u64(state->registers.ping_info_addr_low,
                  state->registers.ping_info_addr_high);

    if (s_verbose_logging) {
        fprintf(stderr,
                "%s: %s handle=%u ping_info_gpa=0x%llx phys_start=0x%llx\n",
                __func__, address_space_command_name(cmd), state->registers.handle,
                (unsigned long long)ping_info_gpa,
                (unsigned long long)goldfish_address_space_get_phys_addr_start_locked());
    }

    switch (cmd) {
    case ADDRESS_SPACE_COMMAND_ALLOCATE_BLOCK:
        result = address_space_allocate_block(state);
        break;
    case ADDRESS_SPACE_COMMAND_DEALLOCATE_BLOCK:
        result = address_space_deallocate_block(state);
        break;
    case ADDRESS_SPACE_COMMAND_GEN_HANDLE:
        if (!s_address_space_device_control_ops ||
            !s_address_space_device_control_ops->gen_handle) {
            result = ENODEV;
            break;
        }
        state->registers.handle = s_address_space_device_control_ops->gen_handle();
        result = 0;
        break;
    case ADDRESS_SPACE_COMMAND_DESTROY_HANDLE:
        if (!s_address_space_device_control_ops ||
            !s_address_space_device_control_ops->destroy_handle) {
            result = ENODEV;
            break;
        }
        s_address_space_device_control_ops->destroy_handle(state->registers.handle);
        g_hash_table_remove(state->ping_info_gpas,
                            GUINT_TO_POINTER(state->registers.handle));
        result = 0;
        break;
    case ADDRESS_SPACE_COMMAND_TELL_PING_INFO_ADDR:
        if (!s_address_space_device_control_ops ||
            !s_address_space_device_control_ops->tell_ping_info) {
            result = ENODEV;
            break;
        }
        if (s_verbose_logging) {
            fprintf(stderr,
                    "ASG-TRACE TELL_PING_INFO handle=%u gpa=0x%llx\n",
                    state->registers.handle,
                    (unsigned long long)ping_info_gpa);
        }
        s_address_space_device_control_ops->tell_ping_info(
            state->registers.handle, ping_info_gpa);
        address_space_set_ping_info_gpa(state, state->registers.handle,
                                        ping_info_gpa);
        result = 0;
        break;
    default:
        result = ENOSYS;
        break;
    }

    if (s_verbose_logging) {
        fprintf(stderr, "%s: %s result=%u handle=%u ping_info_gpa=0x%llx\n",
                __func__, address_space_command_name(cmd), result,
                state->registers.handle, (unsigned long long)ping_info_gpa);
    }

    return result;
}

static uint64_t address_space_control_read_locked(GoldfishAddressSpaceState *state,
                                                  hwaddr offset,
                                                  unsigned size)
{
    uint64_t value = 0;

    if (size != 4) {
        return 0;
    }

    switch ((enum address_space_register_id)offset) {
    case ADDRESS_SPACE_REGISTER_STATUS:
        value = state->registers.status;
        break;
    case ADDRESS_SPACE_REGISTER_GUEST_PAGE_SIZE:
        value = state->registers.guest_page_size;
        break;
    case ADDRESS_SPACE_REGISTER_BLOCK_SIZE_LOW:
        value = state->registers.size_low;
        break;
    case ADDRESS_SPACE_REGISTER_BLOCK_SIZE_HIGH:
        value = state->registers.size_high;
        break;
    case ADDRESS_SPACE_REGISTER_BLOCK_OFFSET_LOW:
        value = state->registers.offset_low;
        break;
    case ADDRESS_SPACE_REGISTER_BLOCK_OFFSET_HIGH:
        value = state->registers.offset_high;
        break;
    case ADDRESS_SPACE_REGISTER_PING:
        value = state->registers.ping;
        break;
    case ADDRESS_SPACE_REGISTER_PING_INFO_ADDR_LOW:
        value = state->registers.ping_info_addr_low;
        break;
    case ADDRESS_SPACE_REGISTER_PING_INFO_ADDR_HIGH:
        value = state->registers.ping_info_addr_high;
        break;
    case ADDRESS_SPACE_REGISTER_HANDLE:
        value = state->registers.handle;
        break;
    case ADDRESS_SPACE_REGISTER_PHYS_START_LOW:
        value = state->registers.phys_start_low;
        break;
    case ADDRESS_SPACE_REGISTER_PHYS_START_HIGH:
        value = state->registers.phys_start_high;
        break;
    default:
        value = 0;
        break;
    }

    if (s_verbose_logging) {
        switch ((enum address_space_register_id)offset) {
        case ADDRESS_SPACE_REGISTER_STATUS:
        case ADDRESS_SPACE_REGISTER_PING_INFO_ADDR_LOW:
        case ADDRESS_SPACE_REGISTER_PING_INFO_ADDR_HIGH:
        case ADDRESS_SPACE_REGISTER_HANDLE:
            fprintf(stderr, "%s: read %s -> 0x%llx\n", __func__,
                    address_space_register_name((enum address_space_register_id)offset),
                    (unsigned long long)value);
            break;
        default:
            break;
        }
    }

    return value;
}

static void address_space_control_write_locked(void *opaque,
                                               hwaddr offset,
                                               uint64_t value,
                                               unsigned size)
{
    GoldfishAddressSpaceState *state = opaque;

    if (size != 4) {
        return;
    }

    if (s_verbose_logging) {
        fprintf(stderr, "%s: write %s <- 0x%llx\n", __func__,
                address_space_register_name((enum address_space_register_id)offset),
                (unsigned long long)value);
    }

    switch ((enum address_space_register_id)offset) {
    case ADDRESS_SPACE_REGISTER_COMMAND:
        goldfish_address_space_activate_hw_funcs(state);
        state->registers.status = address_space_run_command(state, value);
        qemu_irq_pulse(state->irq);
        break;
    case ADDRESS_SPACE_REGISTER_GUEST_PAGE_SIZE:
#if defined(__APPLE__) && defined(__arm64__)
        state->registers.guest_page_size = 16384;
#else
        state->registers.guest_page_size = value;
#endif
        break;
    case ADDRESS_SPACE_REGISTER_BLOCK_SIZE_LOW:
        state->registers.size_low = value;
        break;
    case ADDRESS_SPACE_REGISTER_BLOCK_SIZE_HIGH:
        state->registers.size_high = value;
        break;
    case ADDRESS_SPACE_REGISTER_BLOCK_OFFSET_LOW:
        state->registers.offset_low = value;
        break;
    case ADDRESS_SPACE_REGISTER_BLOCK_OFFSET_HIGH:
        state->registers.offset_high = value;
        break;
    case ADDRESS_SPACE_REGISTER_PING:
        goldfish_address_space_activate_hw_funcs(state);
        state->registers.ping = (uint32_t)value;
        if (s_verbose_logging) {
            fprintf(stderr, "ASG-TRACE PING handle=%u\n", (uint32_t)value);
        }
        if (s_address_space_device_control_ops &&
            s_address_space_device_control_ops->ping) {
            s_address_space_device_control_ops->ping(value);
        }
        break;
    case ADDRESS_SPACE_REGISTER_PING_WITH_DATA:
    {
        uint64_t ping_info_gpa = 0;
        void *ping_info_hva = NULL;

        goldfish_address_space_activate_hw_funcs(state);

        if (!s_address_space_device_control_ops ||
            !s_address_space_device_control_ops->ping_at_hva ||
            !s_address_space_device_control_ops->get_host_ptr) {
            hw_error("%s: PING_WITH_DATA requested but host ASG hva ops are unavailable",
                     __func__);
        }

        if (!address_space_get_ping_info_gpa(state, (uint32_t)value,
                                             &ping_info_gpa)) {
            hw_error("%s: PING_WITH_DATA for unknown handle %u",
                     __func__, (uint32_t)value);
        }

        ping_info_hva =
            s_address_space_device_control_ops->get_host_ptr(ping_info_gpa);

        if (s_verbose_logging) {
            fprintf(stderr,
                    "ASG-TRACE PING_WITH_DATA handle=%u gpa=0x%llx hva=%p\n",
                    (uint32_t)value,
                    (unsigned long long)ping_info_gpa, ping_info_hva);
        }

        if (!ping_info_hva) {
            hw_error("%s: PING_WITH_DATA could not resolve handle %u ping info GPA 0x%llx",
                     __func__, (uint32_t)value,
                     (unsigned long long)ping_info_gpa);
        }

        s_address_space_device_control_ops->ping_at_hva((uint32_t)value,
                                                        ping_info_hva);
        break;
    }
    case ADDRESS_SPACE_REGISTER_PING_INFO_ADDR_LOW:
        state->registers.ping_info_addr_low = (uint32_t)value;
        break;
    case ADDRESS_SPACE_REGISTER_PING_INFO_ADDR_HIGH:
        state->registers.ping_info_addr_high = (uint32_t)value;
        break;
    case ADDRESS_SPACE_REGISTER_HANDLE:
        state->registers.handle = (uint32_t)value;
        break;
    case ADDRESS_SPACE_REGISTER_PHYS_START_LOW:
        state->registers.phys_start_low = (uint32_t)value;
        break;
    case ADDRESS_SPACE_REGISTER_PHYS_START_HIGH:
        state->registers.phys_start_high = (uint32_t)value;
        break;
    default:
        break;
    }
}

static uint64_t address_space_control_read(void *opaque, hwaddr offset, unsigned size)
{
    GoldfishAddressSpaceState *state = opaque;
    uint64_t value;

    qemu_mutex_lock(&state->mutex);
    value = address_space_control_read_locked(state, offset, size);
    qemu_mutex_unlock(&state->mutex);
    return value;
}

static void address_space_control_write(void *opaque,
                                        hwaddr offset,
                                        uint64_t value,
                                        unsigned size)
{
    GoldfishAddressSpaceState *state = opaque;

    qemu_mutex_lock(&state->mutex);
    address_space_control_write_locked(state, offset, value, size);
    qemu_mutex_unlock(&state->mutex);
}

static const MemoryRegionOps address_space_control_ops = {
    .read = address_space_control_read,
    .write = address_space_control_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void address_space_pci_realize(PCIDevice *dev, Error **errp)
{
    GoldfishAddressSpaceState *state = GOLDFISH_ADDRESS_SPACE(dev);
    uint64_t align = 0;

    if (s_current_state) {
        error_setg(errp, "goldfish_address_space only supports one device instance");
        return;
    }

    s_address_space_device_control_ops =
        (const qemu_address_space_device_control_ops *)
            rutabaga_gfxstream_get_address_space_device_control_ops();
    if (!s_address_space_device_control_ops ||
        !s_address_space_device_control_ops->gen_handle ||
        !s_address_space_device_control_ops->tell_ping_info ||
        !s_address_space_device_control_ops->ping) {
        error_setg(errp, "gfxstream address-space control ops are unavailable");
        return;
    }

    /*
     * Darwin rejects QEMU_MAP_NORESERVE, so reserve the sparse BAR mapping
     * normally and let the host fault pages in on demand.
     */
    state->area_mem_ptr = qemu_anon_ram_alloc(GOLDFISH_ADDRESS_SPACE_AREA_SIZE,
                                              &align, false, false);
    if (!state->area_mem_ptr) {
        error_setg(errp, "failed to reserve goldfish address-space backing");
        return;
    }

    memory_region_init_io(&state->control_io, OBJECT(state), &address_space_control_ops,
                          state, GOLDFISH_ADDRESS_SPACE_CONTROL_NAME,
                          GOLDFISH_ADDRESS_SPACE_CONTROL_SIZE);
    pci_register_bar(dev, GOLDFISH_ADDRESS_SPACE_CONTROL_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &state->control_io);

    memory_region_init_ram_device_ptr(&state->area_mem, OBJECT(state),
                                      GOLDFISH_ADDRESS_SPACE_AREA_NAME,
                                      GOLDFISH_ADDRESS_SPACE_AREA_SIZE,
                                      state->area_mem_ptr);
    pci_register_bar(dev, GOLDFISH_ADDRESS_SPACE_AREA_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &state->area_mem);

    pci_set_byte(dev->config + PCI_INTERRUPT_LINE,
                 GOLDFISH_ADDRESS_SPACE_PCI_INTERRUPT_LINE);
    pci_config_set_interrupt_pin(dev->config,
                                 GOLDFISH_ADDRESS_SPACE_PCI_INTERRUPT_PIN);

    state->irq = pci_allocate_irq(dev);
    qemu_mutex_init(&state->mutex);
    state->ping_info_gpas = g_hash_table_new_full(g_direct_hash,
                                                  g_direct_equal,
                                                  NULL,
                                                  g_free);
    state->registers.guest_page_size = DEFAULT_GUEST_PAGE_SIZE;
    address_space_allocator_init(&state->allocator,
                                 GOLDFISH_ADDRESS_SPACE_AREA_SIZE, 32);

    if (getenv("ANDROID_EMUGL_VERBOSE") &&
        !strcmp(getenv("ANDROID_EMUGL_VERBOSE"), "1")) {
        s_verbose_logging = 1;
    }
    if (getenv("GOLDFISH_ADDRESS_SPACE_NO_OOB_ACCESS")) {
        goldfish_as_oob_access = 0;
    }

    goldfish_address_space_activate_hw_funcs(state);
    s_current_state = state;
}

static void address_space_pci_unrealize(PCIDevice *dev)
{
    GoldfishAddressSpaceState *state = GOLDFISH_ADDRESS_SPACE(dev);

    if (state->previous_hw_funcs_saved) {
        rutabaga_gfxstream_set_address_space_hw_funcs(state->previous_hw_funcs);
    }

    if (s_current_state == state) {
        s_current_state = NULL;
    }

    qemu_free_irq(state->irq);
    object_unparent(OBJECT(&state->area_mem));
    object_unparent(OBJECT(&state->control_io));
    if (state->area_mem_ptr) {
        qemu_anon_ram_free(state->area_mem_ptr,
                           GOLDFISH_ADDRESS_SPACE_AREA_SIZE);
        state->area_mem_ptr = NULL;
    }
    g_hash_table_destroy(state->ping_info_gpas);
    address_space_allocator_destroy(&state->allocator);
    qemu_mutex_destroy(&state->mutex);
}

static void address_space_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci = PCI_DEVICE_CLASS(klass);

    dc->desc = GOLDFISH_ADDRESS_SPACE_NAME;
    dc->hotpluggable = false;

    pci->realize = address_space_pci_realize;
    pci->exit = address_space_pci_unrealize;
    pci->vendor_id = GOLDFISH_ADDRESS_SPACE_PCI_VENDOR_ID;
    pci->device_id = GOLDFISH_ADDRESS_SPACE_PCI_DEVICE_ID;
    pci->revision = GOLDFISH_ADDRESS_SPACE_PCI_REVISION;
    pci->class_id = PCI_CLASS_OTHERS;
}

static const TypeInfo address_space_pci_info = {
    .name = TYPE_GOLDFISH_ADDRESS_SPACE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GoldfishAddressSpaceState),
    .class_init = address_space_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void address_space_register_types(void)
{
    type_register_static(&address_space_pci_info);
}

type_init(address_space_register_types);

void goldfish_address_space_map_hook(uint64_t gpa, uint64_t size)
{
    int64_t offset;
    int i;

    if (!s_current_state) {
        return;
    }

    offset = gpa - s_current_state->area_mem.addr;
    if (offset < 0) {
        return;
    }

    for (i = 0; i < s_current_state->allocator.size; ++i) {
        struct address_block *block = &s_current_state->allocator.blocks[i];
        if (block->available) {
            continue;
        }
        if (offset == block->offset) {
            block->map_size = size;
            return;
        }
    }
}

int goldfish_address_space_is_oob(uint64_t offset, uint64_t size)
{
    uint64_t bytes_found = 0;
    int i;

    if (!s_current_state) {
        return -EINVAL;
    }

    qemu_mutex_lock(&s_current_state->mutex);
    for (i = 0; i < s_current_state->allocator.size; ++i) {
        struct address_block *block = &s_current_state->allocator.blocks[i];
        if (block->available) {
            continue;
        }
        if (offset >= block->offset + block->map_size ||
            offset + size <= block->offset) {
            continue;
        }
        bytes_found += MIN(block->offset + block->map_size, offset + size) -
                       MAX(block->offset, offset);
    }
    qemu_mutex_unlock(&s_current_state->mutex);

    return bytes_found != size;
}

int goldfish_address_space_alloc_shared_host_region(uint64_t page_aligned_size,
                                                    uint64_t *offset)
{
    int result;

    if (!s_current_state) {
        return -EINVAL;
    }

    qemu_mutex_lock(&s_current_state->mutex);
    result = goldfish_address_space_alloc_shared_host_region_locked(page_aligned_size,
                                                                    offset);
    qemu_mutex_unlock(&s_current_state->mutex);
    return result;
}

int goldfish_address_space_free_shared_host_region(uint64_t offset)
{
    int result;

    if (!s_current_state) {
        return -EINVAL;
    }

    qemu_mutex_lock(&s_current_state->mutex);
    result = goldfish_address_space_free_shared_host_region_locked(offset);
    qemu_mutex_unlock(&s_current_state->mutex);
    return result;
}

int goldfish_address_space_alloc_shared_host_region_locked(uint64_t page_aligned_size,
                                                           uint64_t *offset)
{
    uint64_t offset_out;

    if (!s_current_state || !offset) {
        return -EINVAL;
    }

    offset_out = address_space_allocator_allocate(&s_current_state->allocator,
                                                  page_aligned_size);
    if (offset_out == ANDROID_EMU_ADDRESS_SPACE_BAD_OFFSET) {
        return -ENOMEM;
    }

    *offset = offset_out;
    return 0;
}

int goldfish_address_space_alloc_shared_host_region_fixed_locked(uint64_t page_aligned_size,
                                                                 uint64_t offset)
{
    if (!s_current_state) {
        return -EINVAL;
    }

    return address_space_allocator_allocate_fixed(&s_current_state->allocator,
                                                  page_aligned_size, offset);
}

int goldfish_address_space_free_shared_host_region_locked(uint64_t offset)
{
    if (!s_current_state) {
        return -EINVAL;
    }

    return address_space_allocator_deallocate(&s_current_state->allocator, offset) ?
           -EINVAL : 0;
}

uint64_t goldfish_address_space_get_phys_addr_start(void)
{
    uint64_t result;

    if (!s_current_state) {
        return ANDROID_EMU_ADDRESS_SPACE_BAD_OFFSET;
    }

    qemu_mutex_lock(&s_current_state->mutex);
    result = goldfish_address_space_get_phys_addr_start_locked();
    qemu_mutex_unlock(&s_current_state->mutex);
    return result;
}

uint64_t goldfish_address_space_get_phys_addr_start_locked(void)
{
    uint64_t result;

    if (!s_current_state) {
        return 0;
    }

    result = s_current_state->registers.phys_start_low;
    result |= ((uint64_t)s_current_state->registers.phys_start_high) << 32;
    if (!result) {
        result = object_property_get_uint(OBJECT(&s_current_state->area_mem),
                                          "addr", NULL);
    }
    return result;
}

uint32_t goldfish_address_space_get_guest_page_size(void)
{
    return s_current_state ? s_current_state->registers.guest_page_size :
                             DEFAULT_GUEST_PAGE_SIZE;
}

/*
 * MMIX virt framebuffer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "exec/hwaddr.h"
#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "hw/display/framebuffer.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "system/address-spaces.h"
#include "ui/pixel_ops.h"
#include "framebuffer.h"

static void mmix_framebuffer_draw_line(void *opaque, uint8_t *dest,
                                       const uint8_t *src, int width,
                                       int dest_step)
{
    MMIXFramebufferState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int bpp = surface_bits_per_pixel(surface);

    while (width--) {
        uint8_t r = src[1];
        uint8_t g = src[2];
        uint8_t b = src[3];
        uint32_t pixel;

        switch (bpp) {
        case 8:
            *dest = rgb_to_pixel8(r, g, b);
            break;
        case 15:
            *(uint16_t *)dest = rgb_to_pixel15(r, g, b);
            break;
        case 16:
            *(uint16_t *)dest = rgb_to_pixel16(r, g, b);
            break;
        case 24:
            pixel = rgb_to_pixel24(r, g, b);
            dest[0] = pixel;
            dest[1] = pixel >> 8;
            dest[2] = pixel >> 16;
            break;
        case 32:
            *(uint32_t *)dest = rgb_to_pixel32(r, g, b);
            break;
        default:
            g_assert_not_reached();
        }

        src += MMIX_VIRT_FRAMEBUFFER_BPP / 8;
        dest += dest_step;
    }
}

static bool mmix_framebuffer_update_display(void *opaque)
{
    MMIXFramebufferState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    int first = 0;
    int last = 0;

    if (!surface || !surface_bits_per_pixel(surface) ||
        (!s->invalidate && !s->refresh_pending)) {
        return true;
    }

    framebuffer_update_display(surface, &s->fbsection,
                               MMIX_VIRT_FRAMEBUFFER_WIDTH,
                               MMIX_VIRT_FRAMEBUFFER_HEIGHT,
                               MMIX_VIRT_FRAMEBUFFER_STRIDE,
                               surface_stride(surface),
                               surface_bytes_per_pixel(surface),
                               s->invalidate, mmix_framebuffer_draw_line, s,
                               &first, &last);
    if (first >= 0) {
        qemu_console_update(s->con, 0, first,
                            MMIX_VIRT_FRAMEBUFFER_WIDTH, last - first + 1);
    }

    s->invalidate = false;
    s->refresh_pending = false;
    return true;
}

static void mmix_framebuffer_invalidate_display(void *opaque)
{
    MMIXFramebufferState *s = opaque;

    s->invalidate = true;
}

static const GraphicHwOps mmix_framebuffer_graphic_ops = {
    .invalidate = mmix_framebuffer_invalidate_display,
    .gfx_update = mmix_framebuffer_update_display,
};

static uint64_t mmix_framebuffer_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    (void)opaque;
    (void)size;

    switch (addr) {
    case MMIX_VIRT_FRAMEBUFFER_REG_WIDTH:
        return MMIX_VIRT_FRAMEBUFFER_WIDTH;
    case MMIX_VIRT_FRAMEBUFFER_REG_HEIGHT:
        return MMIX_VIRT_FRAMEBUFFER_HEIGHT;
    case MMIX_VIRT_FRAMEBUFFER_REG_STRIDE:
        return MMIX_VIRT_FRAMEBUFFER_STRIDE;
    case MMIX_VIRT_FRAMEBUFFER_REG_FORMAT:
        return MMIX_VIRT_FRAMEBUFFER_FORMAT_XRGB8888;
    case MMIX_VIRT_FRAMEBUFFER_REG_BASE:
        return mmix_virt_memmap[MMIX_VIRT_FRAMEBUFFER].base;
    case MMIX_VIRT_FRAMEBUFFER_REG_SIZE:
        return mmix_virt_memmap[MMIX_VIRT_FRAMEBUFFER].size;
    case MMIX_VIRT_FRAMEBUFFER_REG_FLUSH:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented register read 0x%02" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }
}

static void mmix_framebuffer_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned size)
{
    MMIXFramebufferState *s = opaque;

    (void)value;
    (void)size;

    switch (addr) {
    case MMIX_VIRT_FRAMEBUFFER_REG_WIDTH:
    case MMIX_VIRT_FRAMEBUFFER_REG_HEIGHT:
    case MMIX_VIRT_FRAMEBUFFER_REG_STRIDE:
    case MMIX_VIRT_FRAMEBUFFER_REG_FORMAT:
    case MMIX_VIRT_FRAMEBUFFER_REG_BASE:
    case MMIX_VIRT_FRAMEBUFFER_REG_SIZE:
        return;
    case MMIX_VIRT_FRAMEBUFFER_REG_FLUSH:
        s->refresh_pending = true;
        qemu_console_hw_update(s->con);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented register write 0x%02" HWADDR_PRIx
                      "\n", __func__, addr);
    }
}

static const MemoryRegionOps mmix_framebuffer_ops = {
    .read = mmix_framebuffer_read,
    .write = mmix_framebuffer_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
};

static void mmix_framebuffer_reset(DeviceState *dev)
{
    MMIXFramebufferState *s = MMIX_FRAMEBUFFER(dev);

    s->invalidate = true;
    s->refresh_pending = false;
}

static void mmix_framebuffer_realize(DeviceState *dev, Error **errp)
{
    MMIXFramebufferState *s = MMIX_FRAMEBUFFER(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mmix_framebuffer_ops, s,
                          TYPE_MMIX_FRAMEBUFFER,
                          MMIX_VIRT_FRAMEBUFFER_CONTROL_MMIO_SIZE);

    framebuffer_update_memory_section(
        &s->fbsection, get_system_memory(),
        mmix_virt_memmap[MMIX_VIRT_FRAMEBUFFER].base,
        MMIX_VIRT_FRAMEBUFFER_HEIGHT, MMIX_VIRT_FRAMEBUFFER_STRIDE);
    if (!s->fbsection.mr) {
        error_setg(errp, "MMIX framebuffer RAM is not mapped");
        return;
    }

    s->invalidate = true;
    s->con = qemu_graphic_console_create(dev, 0,
                                         &mmix_framebuffer_graphic_ops, s);
    qemu_console_resize(s->con, MMIX_VIRT_FRAMEBUFFER_WIDTH,
                        MMIX_VIRT_FRAMEBUFFER_HEIGHT);
}

static void mmix_framebuffer_unrealize(DeviceState *dev)
{
    MMIXFramebufferState *s = MMIX_FRAMEBUFFER(dev);

    if (s->fbsection.mr) {
        memory_region_set_log(s->fbsection.mr, false, DIRTY_MEMORY_VGA);
        memory_region_unref(s->fbsection.mr);
        s->fbsection.mr = NULL;
    }
    if (s->con) {
        qemu_graphic_console_close(s->con);
        s->con = NULL;
    }
}

static const VMStateDescription vmstate_mmix_framebuffer = {
    .name = TYPE_MMIX_FRAMEBUFFER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(refresh_pending, MMIXFramebufferState),
        VMSTATE_END_OF_LIST()
    },
};

static void mmix_framebuffer_instance_init(Object *obj)
{
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);
    MMIXFramebufferState *s = MMIX_FRAMEBUFFER(obj);

    sysbus_init_mmio(dev, &s->iomem);
}

static void mmix_framebuffer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    (void)data;

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_legacy_reset(dc, mmix_framebuffer_reset);
    dc->realize = mmix_framebuffer_realize;
    dc->unrealize = mmix_framebuffer_unrealize;
    dc->vmsd = &vmstate_mmix_framebuffer;
}

static const TypeInfo mmix_framebuffer_info = {
    .name = TYPE_MMIX_FRAMEBUFFER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_init = mmix_framebuffer_instance_init,
    .instance_size = sizeof(MMIXFramebufferState),
    .class_init = mmix_framebuffer_class_init,
};

static void mmix_framebuffer_register_types(void)
{
    type_register_static(&mmix_framebuffer_info);
}

type_init(mmix_framebuffer_register_types)

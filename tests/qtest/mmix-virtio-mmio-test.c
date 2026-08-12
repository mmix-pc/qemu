/*
 * QTest testcase for MMIX virtio-mmio machine wiring.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "libqos/virtio-mmio.h"
#include "standard-headers/linux/virtio_blk.h"
#include "standard-headers/linux/virtio_ring.h"

#define MMIX_VIRT_VIRTIO_MMIO_BASE 0x10001000ULL
#define MMIX_VIRT_INTC_BASE 0x10004000ULL
#define MMIX_VIRT_INTC_PENDING 0x0000
#define MMIX_VIRT_INTC_CONTEXT_BASE 0x1000
#define MMIX_VIRT_INTC_CONTEXT_STRIDE 0x100
#define MMIX_VIRT_INTC_CONTEXT_ENABLE 0x00
#define MMIX_VIRT_INTC_CONTEXT_CLAIM 0x04
#define MMIX_VIRT_INTC_CONTEXT_COMPLETE 0x08

#define MMIX_VIRT_VIRTIO_BLOCK0_IRQ 2

#define MMIX_VIRTIO_MMIO_PAGE_SIZE 4096
#define MMIX_VIRTIO_BLK_TIMEOUT_US (30 * 1000 * 1000)

#define MMIX_INTC_QOM_PATH "/machine/intc"
#define MMIX_INTC_OUTPUT_IRQ "sysbus-irq"

typedef struct MMIXVirtioBlkReq {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
    uint8_t data[512];
    uint8_t status;
} MMIXVirtioBlkReq;

static uint64_t mmix_intc_context_reg(unsigned cpu, uint64_t reg)
{
    return MMIX_VIRT_INTC_BASE + MMIX_VIRT_INTC_CONTEXT_BASE +
           cpu * MMIX_VIRT_INTC_CONTEXT_STRIDE + reg;
}

static uint32_t mmix_intc_irq_mask(unsigned irq)
{
    return 1U << irq;
}

static uint32_t mmix_intc_read_pending(QTestState *qts)
{
    return qtest_readl(qts, MMIX_VIRT_INTC_BASE + MMIX_VIRT_INTC_PENDING);
}

static uint32_t mmix_intc_claim(QTestState *qts, unsigned cpu)
{
    return qtest_readl(qts, mmix_intc_context_reg(cpu,
                                                 MMIX_VIRT_INTC_CONTEXT_CLAIM));
}

static void mmix_intc_write_enable(QTestState *qts, unsigned cpu,
                                   uint32_t value)
{
    qtest_writel(qts, mmix_intc_context_reg(cpu,
                                            MMIX_VIRT_INTC_CONTEXT_ENABLE),
                 value);
}

static QTestState *mmix_virtio_mmio_start(void)
{
    return qtest_init("-machine virt");
}

static QTestState *mmix_virtio_blk_start(void)
{
    return qtest_init("-machine virt "
                      "-drive file=null-co://,if=none,format=raw,id=blk0 "
                      "-device virtio-blk-device,drive=blk0");
}

static void mmix_virtio_blk_init(QTestState *qts, QVirtioMMIODevice *dev,
                                 QGuestAllocator *alloc, QVirtQueue **vq)
{
    uint64_t features;

    qvirtio_mmio_init_device(dev, qts, MMIX_VIRT_VIRTIO_MMIO_BASE,
                             MMIX_VIRTIO_MMIO_PAGE_SIZE);
    g_assert_cmpuint(dev->vdev.device_type, ==, VIRTIO_ID_BLOCK);
    qvirtio_start_device(&dev->vdev);

    features = qvirtio_get_features(&dev->vdev);
    features &= ~(QVIRTIO_F_BAD_FEATURE |
                  (1ULL << VIRTIO_RING_F_INDIRECT_DESC) |
                  (1ULL << VIRTIO_RING_F_EVENT_IDX) |
                  (1ULL << VIRTIO_BLK_F_SCSI));
    qvirtio_set_features(&dev->vdev, features);

    alloc_init(alloc, ALLOC_NO_FLAGS, 0x00100000, 0x01000000,
               MMIX_VIRTIO_MMIO_PAGE_SIZE);
    *vq = qvirtqueue_setup(&dev->vdev, alloc, 0);
    qvirtio_set_driver_ok(&dev->vdev);
}

static uint32_t mmix_virtio_blk_submit_read(QTestState *qts,
                                            QGuestAllocator *alloc,
                                            QVirtioMMIODevice *dev,
                                            QVirtQueue *vq)
{
    MMIXVirtioBlkReq req = {
        .type = cpu_to_le32(VIRTIO_BLK_T_IN),
        .ioprio = 0,
        .sector = 0,
        .status = 0xff,
    };
    uint64_t req_addr;
    uint32_t free_head;

    req_addr = guest_alloc(alloc, sizeof(req));
    qtest_memwrite(qts, req_addr, &req, sizeof(req));

    free_head = qvirtqueue_add(qts, vq, req_addr, 16, false, true);
    qvirtqueue_add(qts, vq, req_addr + 16, sizeof(req.data), true, true);
    qvirtqueue_add(qts, vq, req_addr + 16 + sizeof(req.data), 1, true, false);
    qvirtqueue_kick(qts, &dev->vdev, vq, free_head);

    return free_head;
}

static void mmix_virtio_mmio_wait_interrupt(QTestState *qts)
{
    gint64 start_time = g_get_monotonic_time();

    while (!(qtest_readl(qts, MMIX_VIRT_VIRTIO_MMIO_BASE +
                         QVIRTIO_MMIO_INTERRUPT_STATUS) & 1)) {
        g_assert(g_get_monotonic_time() - start_time <=
                 MMIX_VIRTIO_BLK_TIMEOUT_US);
    }
}

static void test_mmix_virtio_mmio_no_child(void)
{
    QTestState *qts = mmix_virtio_mmio_start();

    g_assert_cmphex(qtest_readl(qts, MMIX_VIRT_VIRTIO_MMIO_BASE +
                                QVIRTIO_MMIO_MAGIC_VALUE), ==, 0x74726976);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRT_VIRTIO_MMIO_BASE +
                                QVIRTIO_MMIO_VERSION), ==, 1);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRT_VIRTIO_MMIO_BASE +
                                QVIRTIO_MMIO_DEVICE_ID), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRT_VIRTIO_MMIO_BASE +
                                QVIRTIO_MMIO_VENDOR_ID), ==, 0x554d4551);

    qtest_quit(qts);
}

static void test_mmix_virtio_blk_attach(void)
{
    QTestState *qts = mmix_virtio_blk_start();

    g_assert_cmphex(qtest_readl(qts, MMIX_VIRT_VIRTIO_MMIO_BASE +
                                QVIRTIO_MMIO_MAGIC_VALUE), ==, 0x74726976);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRT_VIRTIO_MMIO_BASE +
                                QVIRTIO_MMIO_VERSION), ==, 1);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRT_VIRTIO_MMIO_BASE +
                                QVIRTIO_MMIO_DEVICE_ID), ==, 2);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRT_VIRTIO_MMIO_BASE +
                                QVIRTIO_MMIO_VENDOR_ID), ==, 0x554d4551);

    qtest_quit(qts);
}

static void test_mmix_virtio_mmio_irq(void)
{
    QTestState *qts = mmix_virtio_blk_start();
    QVirtioMMIODevice dev;
    QGuestAllocator alloc;
    QVirtQueue *vq;
    uint32_t virtio_mask = mmix_intc_irq_mask(MMIX_VIRT_VIRTIO_BLOCK0_IRQ);
    uint32_t free_head;
    uint32_t used_head;

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);

    mmix_intc_write_enable(qts, 0, virtio_mask);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_virtio_blk_init(qts, &dev, &alloc, &vq);
    free_head = mmix_virtio_blk_submit_read(qts, &alloc, &dev, vq);

    mmix_virtio_mmio_wait_interrupt(qts);
    g_assert_cmphex(mmix_intc_read_pending(qts), ==, virtio_mask);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==,
                     MMIX_VIRT_VIRTIO_BLOCK0_IRQ);
    g_assert_true(qvirtqueue_get_buf(qts, vq, &used_head, NULL));
    g_assert_cmpuint(used_head, ==, free_head);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_writel(qts, MMIX_VIRT_VIRTIO_MMIO_BASE +
                 QVIRTIO_MMIO_INTERRUPT_ACK, 1);
    g_assert_cmphex(mmix_intc_read_pending(qts), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    qvirtqueue_cleanup(dev.vdev.bus, vq, &alloc);
    alloc_destroy(&alloc);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/virtio-mmio/no-child",
                   test_mmix_virtio_mmio_no_child);
    qtest_add_func("/mmix/virtio-mmio/virtio-blk-attach",
                   test_mmix_virtio_blk_attach);
    qtest_add_func("/mmix/virtio-mmio/irq",
                   test_mmix_virtio_mmio_irq);

    return g_test_run();
}

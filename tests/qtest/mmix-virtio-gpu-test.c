/*
 * QTest testcase for the MMIX VirtIO GPU platform path.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/virtio-mmio.h"
#include "qemu/bswap.h"
#include "standard-headers/linux/virtio_config.h"
#include "standard-headers/linux/virtio_gpu.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_ring.h"

#define MMIX_VIRTIO_BASE              UINT64_C(0x0001000040000000)
#define MMIX_VIRTIO_PAGE_SIZE         4096
#define MMIX_VIRTIO_IRQ_BASE          2048
#define MMIX_VIRTIO_TIMEOUT_US        (30 * G_USEC_PER_SEC)

#define MMIX_INTC_BASE                UINT64_C(0x0001000030000000)
#define MMIX_INTC_PENDING_BASE        0x1000
#define MMIX_INTC_CONTEXT_BASE        UINT64_C(0x0001000034000000)
#define MMIX_INTC_ENABLE_BASE         0x0000
#define MMIX_INTC_CLAIM               0x0800
#define MMIX_INTC_COMPLETE            0x0808
#define MMIX_INTC_QOM_PATH            "/machine/intc"
#define MMIX_INTC_OUTPUT_IRQ          "sysbus-irq"

typedef struct MMIXVirtioGPU {
    QTestState *qts;
    QVirtioMMIODevice device;
    QGuestAllocator allocator;
    QVirtQueue *control;
    QVirtQueue *cursor;
} MMIXVirtioGPU;

static uint64_t mmix_intc_source_bit(unsigned int source)
{
    return UINT64_C(1) << (source % 64);
}

static uint64_t mmix_intc_source_reg(uint64_t base, unsigned int source)
{
    return base + (source / 64) * sizeof(uint64_t);
}

static uint64_t mmix_intc_context_reg(uint64_t reg)
{
    return MMIX_INTC_CONTEXT_BASE + reg;
}

static void mmix_intc_enable_source(QTestState *qts, unsigned int source)
{
    uint64_t reg = mmix_intc_source_reg(
        mmix_intc_context_reg(MMIX_INTC_ENABLE_BASE), source);

    qtest_writeq(qts, reg, qtest_readq(qts, reg) |
                 mmix_intc_source_bit(source));
}

static uint64_t mmix_intc_pending(QTestState *qts, unsigned int source)
{
    return qtest_readq(qts, mmix_intc_source_reg(
                           MMIX_INTC_BASE + MMIX_INTC_PENDING_BASE, source));
}

static uint64_t mmix_intc_claim(QTestState *qts)
{
    return qtest_readq(qts, mmix_intc_context_reg(MMIX_INTC_CLAIM));
}

static void mmix_intc_complete(QTestState *qts, unsigned int source)
{
    qtest_writeq(qts, mmix_intc_context_reg(MMIX_INTC_COMPLETE), source);
}

static void mmix_virtio_gpu_wait_irq(MMIXVirtioGPU *gpu)
{
    gint64 deadline = g_get_monotonic_time() + MMIX_VIRTIO_TIMEOUT_US;

    while (!(qtest_readl(gpu->qts, MMIX_VIRTIO_BASE +
                         QVIRTIO_MMIO_INTERRUPT_STATUS) & 1)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    }
}

static void mmix_virtio_gpu_wait_used(MMIXVirtioGPU *gpu, QVirtQueue *vq,
                                      uint32_t expected_head)
{
    gint64 deadline = g_get_monotonic_time() + MMIX_VIRTIO_TIMEOUT_US;

    for (;;) {
        uint32_t actual_head;

        if (qvirtqueue_get_buf(gpu->qts, vq, &actual_head, NULL)) {
            g_assert_cmpuint(actual_head, ==, expected_head);
            return;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    }
}

static uint32_t mmix_virtio_gpu_config_readl(MMIXVirtioGPU *gpu,
                                             uint64_t offset)
{
    /* Legacy VirtIO-MMIO exposes device configuration in guest byte order. */
    return be32_to_cpu(qvirtio_config_readl(&gpu->device.vdev, offset));
}

static void mmix_virtio_gpu_start(MMIXVirtioGPU *gpu)
{
    uint64_t features;

    gpu->qts = qtest_init("-machine virt,graphics=off "
                          "-device virtio-gpu-device -display none");
    qtest_irq_intercept_out_named(gpu->qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    qvirtio_mmio_init_device(&gpu->device, gpu->qts, MMIX_VIRTIO_BASE,
                             MMIX_VIRTIO_PAGE_SIZE);
    g_assert_cmpuint(gpu->device.vdev.device_type, ==, VIRTIO_ID_GPU);

    qvirtio_start_device(&gpu->device.vdev);
    features = qvirtio_get_features(&gpu->device.vdev);
    g_assert_true(features & (UINT64_C(1) << VIRTIO_GPU_F_EDID));
    features &= ~(QVIRTIO_F_BAD_FEATURE |
                  (UINT64_C(1) << VIRTIO_RING_F_INDIRECT_DESC) |
                  (UINT64_C(1) << VIRTIO_RING_F_EVENT_IDX) |
                  (UINT64_C(1) << VIRTIO_GPU_F_VIRGL) |
                  (UINT64_C(1) << VIRTIO_GPU_F_RESOURCE_UUID) |
                  (UINT64_C(1) << VIRTIO_GPU_F_RESOURCE_BLOB) |
                  (UINT64_C(1) << VIRTIO_GPU_F_CONTEXT_INIT) |
                  (UINT64_C(1) << VIRTIO_GPU_F_BLOB_ALIGNMENT));
    qvirtio_set_features(&gpu->device.vdev, features);

    g_assert_cmpuint(mmix_virtio_gpu_config_readl(
                         gpu, offsetof(struct virtio_gpu_config,
                                       num_scanouts)), ==, 1);
    g_assert_cmpuint(mmix_virtio_gpu_config_readl(
                         gpu, offsetof(struct virtio_gpu_config,
                                       num_capsets)), ==, 0);

    alloc_init(&gpu->allocator, ALLOC_NO_FLAGS, 0x00100000, 0x02000000,
               MMIX_VIRTIO_PAGE_SIZE);
    gpu->control = qvirtqueue_setup(&gpu->device.vdev, &gpu->allocator, 0);
    gpu->cursor = qvirtqueue_setup(&gpu->device.vdev, &gpu->allocator, 1);
    qvirtio_set_driver_ok(&gpu->device.vdev);
    mmix_intc_enable_source(gpu->qts, MMIX_VIRTIO_IRQ_BASE);
}

static void mmix_virtio_gpu_stop(MMIXVirtioGPU *gpu)
{
    qvirtqueue_cleanup(gpu->device.vdev.bus, gpu->cursor, &gpu->allocator);
    qvirtqueue_cleanup(gpu->device.vdev.bus, gpu->control, &gpu->allocator);
    alloc_destroy(&gpu->allocator);
    qtest_quit(gpu->qts);
}

static void test_mmix_virtio_gpu_base_protocol(void)
{
    struct virtio_gpu_ctrl_hdr request = {
        .type = cpu_to_le32(VIRTIO_GPU_CMD_GET_DISPLAY_INFO),
    };
    struct virtio_gpu_resp_display_info response = { 0 };
    struct virtio_gpu_update_cursor cursor = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_MOVE_CURSOR),
        .pos.scanout_id = cpu_to_le32(0),
        .pos.x = cpu_to_le32(17),
        .pos.y = cpu_to_le32(19),
    };
    const unsigned int source = MMIX_VIRTIO_IRQ_BASE;
    const uint64_t source_bit = mmix_intc_source_bit(source);
    MMIXVirtioGPU gpu = { 0 };
    uint64_t request_addr;
    uint64_t response_addr;
    uint64_t cursor_addr;
    uint32_t control_head;
    uint32_t cursor_head;

    mmix_virtio_gpu_start(&gpu);

    request_addr = guest_alloc(&gpu.allocator, sizeof(request));
    response_addr = guest_alloc(&gpu.allocator, sizeof(response));
    qtest_memwrite(gpu.qts, request_addr, &request, sizeof(request));
    qtest_memwrite(gpu.qts, response_addr, &response, sizeof(response));
    control_head = qvirtqueue_add(gpu.qts, gpu.control, request_addr,
                                  sizeof(request), false, true);
    qvirtqueue_add(gpu.qts, gpu.control, response_addr, sizeof(response),
                   true, false);
    qvirtqueue_kick(gpu.qts, &gpu.device.vdev, gpu.control, control_head);

    mmix_virtio_gpu_wait_irq(&gpu);
    g_assert_cmphex(mmix_intc_pending(gpu.qts, source) & source_bit, ==,
                    source_bit);
    g_assert_true(qtest_get_irq(gpu.qts, 0));
    g_assert_cmpuint(mmix_intc_claim(gpu.qts), ==, source);
    mmix_virtio_gpu_wait_used(&gpu, gpu.control, control_head);
    qtest_memread(gpu.qts, response_addr, &response, sizeof(response));
    g_assert_cmpuint(le32_to_cpu(response.hdr.type), ==,
                     VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
    g_assert_cmpuint(le32_to_cpu(response.pmodes[0].enabled), ==, 1);
    g_assert_cmpuint(le32_to_cpu(response.pmodes[0].r.width), ==, 1280);
    g_assert_cmpuint(le32_to_cpu(response.pmodes[0].r.height), ==, 800);
    g_assert_cmpuint(le32_to_cpu(response.pmodes[1].enabled), ==, 0);

    cursor_addr = guest_alloc(&gpu.allocator, sizeof(cursor));
    qtest_memwrite(gpu.qts, cursor_addr, &cursor, sizeof(cursor));
    cursor_head = qvirtqueue_add(gpu.qts, gpu.cursor, cursor_addr,
                                 sizeof(cursor), false, false);
    qvirtqueue_kick(gpu.qts, &gpu.device.vdev, gpu.cursor, cursor_head);
    mmix_virtio_gpu_wait_used(&gpu, gpu.cursor, cursor_head);

    mmix_intc_complete(gpu.qts, source);
    g_assert_true(qtest_get_irq(gpu.qts, 0));
    g_assert_cmpuint(mmix_intc_claim(gpu.qts), ==, source);
    qtest_writel(gpu.qts, MMIX_VIRTIO_BASE +
                  QVIRTIO_MMIO_INTERRUPT_ACK, 1);
    mmix_intc_complete(gpu.qts, source);
    g_assert_cmphex(mmix_intc_pending(gpu.qts, source) & source_bit, ==, 0);
    g_assert_false(qtest_get_irq(gpu.qts, 0));

    guest_free(&gpu.allocator, cursor_addr);
    guest_free(&gpu.allocator, response_addr);
    guest_free(&gpu.allocator, request_addr);
    mmix_virtio_gpu_stop(&gpu);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/virtio-gpu/base-protocol",
                   test_mmix_virtio_gpu_base_protocol);
    return g_test_run();
}

/*
 * QTest testcase for the MMIX VirtIO GPU platform path.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "libqos/virtio-mmio.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
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

#define MMIX_GPU_RESOURCE_ID          1
#define MMIX_GPU_WIDTH                64
#define MMIX_GPU_HEIGHT               64
#define MMIX_GPU_STRIDE               (MMIX_GPU_WIDTH * 4)
#define MMIX_GPU_BACKING_SIZE         (MMIX_GPU_HEIGHT * MMIX_GPU_STRIDE)

typedef struct MMIXVirtioGPU {
    QTestState *qts;
    QVirtioMMIODevice device;
    QGuestAllocator allocator;
    QVirtQueue *control;
    QVirtQueue *cursor;
} MMIXVirtioGPU;

typedef struct MMIXPPMImage {
    uint8_t *data;
    size_t len;
    size_t pixels_offset;
    unsigned width;
    unsigned height;
} MMIXPPMImage;

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

static void mmix_virtio_gpu_initialize(MMIXVirtioGPU *gpu)
{
    uint64_t features;

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

    gpu->control = qvirtqueue_setup(&gpu->device.vdev, &gpu->allocator, 0);
    gpu->cursor = qvirtqueue_setup(&gpu->device.vdev, &gpu->allocator, 1);
    qvirtio_set_driver_ok(&gpu->device.vdev);
    mmix_intc_enable_source(gpu->qts, MMIX_VIRTIO_IRQ_BASE);
}

static void mmix_virtio_gpu_start(MMIXVirtioGPU *gpu)
{
    gpu->qts = qtest_init("-machine virt,graphics=off "
                          "-device virtio-gpu-device -display none");
    qtest_irq_intercept_out_named(gpu->qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    alloc_init(&gpu->allocator, ALLOC_NO_FLAGS, 0x00100000, 0x02000000,
               MMIX_VIRTIO_PAGE_SIZE);
    mmix_virtio_gpu_initialize(gpu);
}

static void mmix_virtio_gpu_stop(MMIXVirtioGPU *gpu)
{
    qvirtqueue_cleanup(gpu->device.vdev.bus, gpu->cursor, &gpu->allocator);
    qvirtqueue_cleanup(gpu->device.vdev.bus, gpu->control, &gpu->allocator);
    alloc_destroy(&gpu->allocator);
    qtest_quit(gpu->qts);
}

static void mmix_virtio_gpu_finish_irq(MMIXVirtioGPU *gpu)
{
    const unsigned int source = MMIX_VIRTIO_IRQ_BASE;
    const uint64_t source_bit = mmix_intc_source_bit(source);

    g_assert_cmphex(mmix_intc_pending(gpu->qts, source) & source_bit, ==,
                    source_bit);
    g_assert_cmpuint(mmix_intc_claim(gpu->qts), ==, source);
    qtest_writel(gpu->qts, MMIX_VIRTIO_BASE +
                 QVIRTIO_MMIO_INTERRUPT_ACK, 1);
    mmix_intc_complete(gpu->qts, source);
    g_assert_cmphex(mmix_intc_pending(gpu->qts, source) & source_bit, ==, 0);
    g_assert_false(qtest_get_irq(gpu->qts, 0));
}

static void mmix_virtio_gpu_control(MMIXVirtioGPU *gpu,
                                    const void *request,
                                    size_t request_size,
                                    uint32_t expected_response)
{
    struct virtio_gpu_ctrl_hdr response = { 0 };
    uint64_t request_addr = guest_alloc(&gpu->allocator, request_size);
    uint64_t response_addr = guest_alloc(&gpu->allocator, sizeof(response));
    uint32_t head;

    qtest_memwrite(gpu->qts, request_addr, request, request_size);
    qtest_memwrite(gpu->qts, response_addr, &response, sizeof(response));
    head = qvirtqueue_add(gpu->qts, gpu->control, request_addr,
                          request_size, false, true);
    qvirtqueue_add(gpu->qts, gpu->control, response_addr, sizeof(response),
                   true, false);
    qvirtqueue_kick(gpu->qts, &gpu->device.vdev, gpu->control, head);

    mmix_virtio_gpu_wait_irq(gpu);
    mmix_virtio_gpu_wait_used(gpu, gpu->control, head);
    qtest_memread(gpu->qts, response_addr, &response, sizeof(response));
    g_assert_cmpuint(le32_to_cpu(response.type), ==, expected_response);
    mmix_virtio_gpu_finish_irq(gpu);

    guest_free(&gpu->allocator, response_addr);
    guest_free(&gpu->allocator, request_addr);
}

static uint64_t mmix_virtio_gpu_submit_cursor(
    MMIXVirtioGPU *gpu, const struct virtio_gpu_update_cursor *cmd,
    uint32_t *head)
{
    uint64_t command_addr = guest_alloc(&gpu->allocator, sizeof(*cmd));

    qtest_memwrite(gpu->qts, command_addr, cmd, sizeof(*cmd));
    *head = qvirtqueue_add(gpu->qts, gpu->cursor, command_addr, sizeof(*cmd),
                           false, false);
    qvirtqueue_kick(gpu->qts, &gpu->device.vdev, gpu->cursor, *head);
    return command_addr;
}

static void mmix_virtio_gpu_cursor(MMIXVirtioGPU *gpu,
                                   const struct virtio_gpu_update_cursor *cmd)
{
    uint32_t head;
    uint64_t command_addr = mmix_virtio_gpu_submit_cursor(gpu, cmd, &head);

    mmix_virtio_gpu_wait_irq(gpu);
    mmix_virtio_gpu_wait_used(gpu, gpu->cursor, head);
    mmix_virtio_gpu_finish_irq(gpu);
    guest_free(&gpu->allocator, command_addr);
}

static const uint8_t *mmix_ppm_next_token(const uint8_t *cursor,
                                          const uint8_t *end,
                                          const uint8_t **token,
                                          size_t *token_len)
{
    while (cursor < end) {
        if (g_ascii_isspace(*cursor)) {
            cursor++;
        } else if (*cursor == '#') {
            while (cursor < end && *cursor != '\n') {
                cursor++;
            }
        } else {
            break;
        }
    }

    *token = cursor;
    while (cursor < end && !g_ascii_isspace(*cursor) && *cursor != '#') {
        cursor++;
    }
    *token_len = cursor - *token;
    return cursor;
}

static unsigned mmix_ppm_token_uint(const uint8_t *token, size_t token_len)
{
    g_autofree char *text = g_strndup((const char *)token, token_len);
    char *end = NULL;
    uint64_t value = g_ascii_strtoull(text, &end, 10);

    g_assert_nonnull(end);
    g_assert_true(*end == '\0');
    g_assert_cmpuint(value, <=, UINT_MAX);
    return value;
}

static MMIXPPMImage mmix_ppm_load(const char *path)
{
    MMIXPPMImage image = { 0 };
    const uint8_t *cursor;
    const uint8_t *end;
    const uint8_t *token;
    size_t token_len;
    unsigned maxval;
    GError *error = NULL;

    g_assert_true(g_file_get_contents(path, (char **)&image.data, &image.len,
                                      &error));
    g_assert_no_error(error);

    cursor = image.data;
    end = image.data + image.len;
    cursor = mmix_ppm_next_token(cursor, end, &token, &token_len);
    g_assert_cmpmem(token, token_len, "P6", 2);
    cursor = mmix_ppm_next_token(cursor, end, &token, &token_len);
    image.width = mmix_ppm_token_uint(token, token_len);
    cursor = mmix_ppm_next_token(cursor, end, &token, &token_len);
    image.height = mmix_ppm_token_uint(token, token_len);
    cursor = mmix_ppm_next_token(cursor, end, &token, &token_len);
    maxval = mmix_ppm_token_uint(token, token_len);

    g_assert_cmpuint(maxval, ==, 255);
    g_assert_true(cursor < end && g_ascii_isspace(*cursor));
    image.pixels_offset = ++cursor - image.data;
    g_assert_cmpuint(image.len - image.pixels_offset, ==,
                     (size_t)image.width * image.height * 3);
    return image;
}

static void mmix_ppm_assert_pixel(const MMIXPPMImage *image,
                                  unsigned int x, unsigned int y,
                                  uint8_t r, uint8_t g, uint8_t b)
{
    size_t offset;

    g_assert_cmpuint(x, <, image->width);
    g_assert_cmpuint(y, <, image->height);
    offset = image->pixels_offset + ((size_t)y * image->width + x) * 3;
    g_assert_cmphex(image->data[offset], ==, r);
    g_assert_cmphex(image->data[offset + 1], ==, g);
    g_assert_cmphex(image->data[offset + 2], ==, b);
}

static bool mmix_qmp_has_command(QTestState *qts, const char *command)
{
    g_autoptr(QDict) response = qtest_qmp(
        qts, "{'execute':'query-commands'}");
    QList *commands = qdict_get_qlist(response, "return");
    const QListEntry *entry;

    QLIST_FOREACH_ENTRY(commands, entry) {
        QDict *info = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(info, "name"), command)) {
            return true;
        }
    }
    return false;
}

static char *mmix_virtio_gpu_screendump(MMIXVirtioGPU *gpu)
{
    g_autofree char *tmpdir = g_canonicalize_filename(g_get_tmp_dir(), NULL);
    char *path = g_strdup_printf("%s/mmix-virtio-gpu-%u.ppm", tmpdir,
                                 (unsigned int)getpid());

    g_unlink(path);
    qtest_qmp_assert_success(
        gpu->qts,
        "{'execute':'screendump','arguments':{'filename':%s,'format':'ppm'}}",
        path);
    return path;
}

static void mmix_virtio_gpu_create_scanout(MMIXVirtioGPU *gpu,
                                           uint64_t backing_addr)
{
    struct virtio_gpu_resource_create_2d create = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
        .format = cpu_to_le32(VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM),
        .width = cpu_to_le32(MMIX_GPU_WIDTH),
        .height = cpu_to_le32(MMIX_GPU_HEIGHT),
    };
    struct {
        struct virtio_gpu_resource_attach_backing command;
        struct virtio_gpu_mem_entry entry;
    } attach = {
        .command.hdr.type =
            cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING),
        .command.resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
        .command.nr_entries = cpu_to_le32(1),
        .entry.addr = cpu_to_le64(backing_addr),
        .entry.length = cpu_to_le32(MMIX_GPU_BACKING_SIZE),
    };
    struct virtio_gpu_transfer_to_host_2d transfer = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D),
        .r.width = cpu_to_le32(MMIX_GPU_WIDTH),
        .r.height = cpu_to_le32(MMIX_GPU_HEIGHT),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    struct virtio_gpu_set_scanout scanout = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_SET_SCANOUT),
        .r.width = cpu_to_le32(MMIX_GPU_WIDTH),
        .r.height = cpu_to_le32(MMIX_GPU_HEIGHT),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    struct virtio_gpu_resource_flush flush = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_FLUSH),
        .r.width = cpu_to_le32(MMIX_GPU_WIDTH),
        .r.height = cpu_to_le32(MMIX_GPU_HEIGHT),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    struct virtio_gpu_update_cursor cursor = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_UPDATE_CURSOR),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };

    mmix_virtio_gpu_control(gpu, &create, sizeof(create),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(gpu, &attach, sizeof(attach),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(gpu, &transfer, sizeof(transfer),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(gpu, &scanout, sizeof(scanout),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(gpu, &flush, sizeof(flush),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_cursor(gpu, &cursor);
}

static void mmix_virtio_gpu_transfer_and_flush(MMIXVirtioGPU *gpu)
{
    struct virtio_gpu_transfer_to_host_2d transfer = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D),
        .r.width = cpu_to_le32(MMIX_GPU_WIDTH),
        .r.height = cpu_to_le32(MMIX_GPU_HEIGHT),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    struct virtio_gpu_resource_flush flush = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_FLUSH),
        .r.width = cpu_to_le32(MMIX_GPU_WIDTH),
        .r.height = cpu_to_le32(MMIX_GPU_HEIGHT),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };

    mmix_virtio_gpu_control(gpu, &transfer, sizeof(transfer),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(gpu, &flush, sizeof(flush),
                            VIRTIO_GPU_RESP_OK_NODATA);
}

static void mmix_virtio_gpu_release_scanout(MMIXVirtioGPU *gpu)
{
    struct virtio_gpu_update_cursor cursor = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_UPDATE_CURSOR),
    };
    struct virtio_gpu_set_scanout scanout = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_SET_SCANOUT),
    };
    struct virtio_gpu_resource_detach_backing detach = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    struct virtio_gpu_resource_unref unref = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_UNREF),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };

    mmix_virtio_gpu_cursor(gpu, &cursor);
    mmix_virtio_gpu_control(gpu, &scanout, sizeof(scanout),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(gpu, &detach, sizeof(detach),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(gpu, &unref, sizeof(unref),
                            VIRTIO_GPU_RESP_OK_NODATA);
}

static void mmix_virtio_gpu_assert_pixel(MMIXVirtioGPU *gpu,
                                         unsigned int x, unsigned int y,
                                         uint8_t r, uint8_t g, uint8_t b)
{
    g_autofree char *path = mmix_virtio_gpu_screendump(gpu);
    MMIXPPMImage image = mmix_ppm_load(path);

    g_assert_cmpuint(image.width, ==, MMIX_GPU_WIDTH);
    g_assert_cmpuint(image.height, ==, MMIX_GPU_HEIGHT);
    mmix_ppm_assert_pixel(&image, x, y, r, g, b);
    g_free(image.data);
    g_assert_cmpint(g_unlink(path), ==, 0);
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

static void test_mmix_virtio_gpu_2d_resource(void)
{
    struct virtio_gpu_resource_create_2d create = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
        .format = cpu_to_le32(VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM),
        .width = cpu_to_le32(MMIX_GPU_WIDTH),
        .height = cpu_to_le32(MMIX_GPU_HEIGHT),
    };
    struct {
        struct virtio_gpu_resource_attach_backing command;
        struct virtio_gpu_mem_entry entry;
    } attach = {
        .command.hdr.type =
            cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING),
        .command.resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
        .command.nr_entries = cpu_to_le32(1),
        .entry.length = cpu_to_le32(MMIX_GPU_BACKING_SIZE),
    };
    struct virtio_gpu_transfer_to_host_2d transfer = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D),
        .r.width = cpu_to_le32(MMIX_GPU_WIDTH),
        .r.height = cpu_to_le32(MMIX_GPU_HEIGHT),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    struct virtio_gpu_set_scanout scanout = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_SET_SCANOUT),
        .r.width = cpu_to_le32(MMIX_GPU_WIDTH),
        .r.height = cpu_to_le32(MMIX_GPU_HEIGHT),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    struct virtio_gpu_resource_flush flush = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_FLUSH),
        .r.width = cpu_to_le32(MMIX_GPU_WIDTH),
        .r.height = cpu_to_le32(MMIX_GPU_HEIGHT),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    struct virtio_gpu_update_cursor update_cursor = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_UPDATE_CURSOR),
        .pos.x = cpu_to_le32(2),
        .pos.y = cpu_to_le32(3),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
        .hot_x = cpu_to_le32(1),
        .hot_y = cpu_to_le32(1),
    };
    struct virtio_gpu_update_cursor move_cursor = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_MOVE_CURSOR),
        .pos.x = cpu_to_le32(11),
        .pos.y = cpu_to_le32(13),
    };
    struct virtio_gpu_resource_detach_backing detach = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    struct virtio_gpu_resource_unref unref = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_UNREF),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    g_autofree uint8_t *pixels = g_malloc0(MMIX_GPU_BACKING_SIZE);
    g_autofree char *screendump = NULL;
    MMIXVirtioGPU gpu = { 0 };
    MMIXPPMImage image;
    uint64_t backing_addr;
    size_t first = (9 * MMIX_GPU_WIDTH + 7) * 4;
    size_t second = (40 * MMIX_GPU_WIDTH + 50) * 4;

    pixels[first] = 0x56;
    pixels[first + 1] = 0x34;
    pixels[first + 2] = 0x12;
    pixels[second] = 0xef;
    pixels[second + 1] = 0xcd;
    pixels[second + 2] = 0xab;

    mmix_virtio_gpu_start(&gpu);
    backing_addr = guest_alloc(&gpu.allocator, MMIX_GPU_BACKING_SIZE);
    attach.entry.addr = cpu_to_le64(backing_addr);
    qtest_memwrite(gpu.qts, backing_addr, pixels, MMIX_GPU_BACKING_SIZE);

    mmix_virtio_gpu_control(&gpu, &create, sizeof(create),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(&gpu, &attach, sizeof(attach),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(&gpu, &transfer, sizeof(transfer),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(&gpu, &scanout, sizeof(scanout),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(&gpu, &flush, sizeof(flush),
                            VIRTIO_GPU_RESP_OK_NODATA);

    if (mmix_qmp_has_command(gpu.qts, "screendump")) {
        screendump = mmix_virtio_gpu_screendump(&gpu);
        image = mmix_ppm_load(screendump);
        g_assert_cmpuint(image.width, ==, MMIX_GPU_WIDTH);
        g_assert_cmpuint(image.height, ==, MMIX_GPU_HEIGHT);
        mmix_ppm_assert_pixel(&image, 7, 9, 0x12, 0x34, 0x56);
        mmix_ppm_assert_pixel(&image, 50, 40, 0xab, 0xcd, 0xef);
        g_free(image.data);
        g_assert_cmpint(g_unlink(screendump), ==, 0);
        g_clear_pointer(&screendump, g_free);
    } else {
        g_test_message("screendump unavailable; skipping pixel validation");
    }

    mmix_virtio_gpu_cursor(&gpu, &update_cursor);
    mmix_virtio_gpu_cursor(&gpu, &move_cursor);

    update_cursor.resource_id = 0;
    mmix_virtio_gpu_cursor(&gpu, &update_cursor);
    scanout.resource_id = 0;
    scanout.r.width = 0;
    scanout.r.height = 0;
    mmix_virtio_gpu_control(&gpu, &scanout, sizeof(scanout),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(&gpu, &detach, sizeof(detach),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(&gpu, &unref, sizeof(unref),
                            VIRTIO_GPU_RESP_OK_NODATA);

    if (mmix_qmp_has_command(gpu.qts, "screendump")) {
        screendump = mmix_virtio_gpu_screendump(&gpu);
        image = mmix_ppm_load(screendump);
        mmix_ppm_assert_pixel(&image, 7, 9, 0, 0, 0);
        mmix_ppm_assert_pixel(&image, 50, 40, 0, 0, 0);
        g_free(image.data);
        g_assert_cmpint(g_unlink(screendump), ==, 0);
        g_clear_pointer(&screendump, g_free);
    }

    mmix_virtio_gpu_control(&gpu, &create, sizeof(create),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(&gpu, &unref, sizeof(unref),
                            VIRTIO_GPU_RESP_OK_NODATA);

    g_assert_cmphex(qtest_readl(gpu.qts, MMIX_VIRTIO_BASE +
                               QVIRTIO_MMIO_INTERRUPT_STATUS), ==, 0);
    guest_free(&gpu.allocator, backing_addr);
    mmix_virtio_gpu_stop(&gpu);
}

static void test_mmix_virtio_gpu_reset(void)
{
    struct virtio_gpu_update_cursor cursor = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_MOVE_CURSOR),
        .pos.x = cpu_to_le32(23),
        .pos.y = cpu_to_le32(29),
    };
    struct virtio_gpu_resource_create_2d create = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
        .format = cpu_to_le32(VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM),
        .width = cpu_to_le32(MMIX_GPU_WIDTH),
        .height = cpu_to_le32(MMIX_GPU_HEIGHT),
    };
    struct virtio_gpu_resource_unref unref = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_RESOURCE_UNREF),
        .resource_id = cpu_to_le32(MMIX_GPU_RESOURCE_ID),
    };
    g_autofree uint8_t *pixels = g_malloc0(MMIX_GPU_BACKING_SIZE);
    MMIXVirtioGPU gpu = { 0 };
    uint64_t command_addr;
    uint64_t backing_addr;
    uint32_t head;

    mmix_virtio_gpu_start(&gpu);
    backing_addr = guest_alloc(&gpu.allocator, MMIX_GPU_BACKING_SIZE);
    qtest_memwrite(gpu.qts, backing_addr, pixels, MMIX_GPU_BACKING_SIZE);
    mmix_virtio_gpu_create_scanout(&gpu, backing_addr);
    command_addr = mmix_virtio_gpu_submit_cursor(&gpu, &cursor, &head);
    mmix_virtio_gpu_wait_irq(&gpu);

    qtest_system_reset(gpu.qts);
    g_assert_cmphex(qtest_readl(gpu.qts, MMIX_VIRTIO_BASE +
                               QVIRTIO_MMIO_DEVICE_ID), ==, VIRTIO_ID_GPU);
    g_assert_cmphex(qtest_readl(gpu.qts, MMIX_VIRTIO_BASE +
                               QVIRTIO_MMIO_DEVICE_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(gpu.qts, MMIX_VIRTIO_BASE +
                               QVIRTIO_MMIO_INTERRUPT_STATUS), ==, 0);
    g_assert_cmphex(mmix_intc_pending(gpu.qts, MMIX_VIRTIO_IRQ_BASE) &
                    mmix_intc_source_bit(MMIX_VIRTIO_IRQ_BASE), ==, 0);

    guest_free(&gpu.allocator, command_addr);
    guest_free(&gpu.allocator, backing_addr);
    qvirtqueue_cleanup(gpu.device.vdev.bus, gpu.cursor, &gpu.allocator);
    qvirtqueue_cleanup(gpu.device.vdev.bus, gpu.control, &gpu.allocator);
    mmix_virtio_gpu_initialize(&gpu);
    mmix_virtio_gpu_control(&gpu, &create, sizeof(create),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_control(&gpu, &unref, sizeof(unref),
                            VIRTIO_GPU_RESP_OK_NODATA);
    mmix_virtio_gpu_stop(&gpu);
}

static void mmix_virtio_gpu_adopt_migration(MMIXVirtioGPU *destination,
                                             MMIXVirtioGPU *source,
                                             QTestState *qts)
{
    destination->qts = qts;
    destination->device = source->device;
    destination->device.qts = qts;
    alloc_init(&destination->allocator, ALLOC_NO_FLAGS,
               source->allocator.start, source->allocator.end,
               source->allocator.page_size);
    migrate_allocator(&source->allocator, &destination->allocator);
    destination->control = source->control;
    destination->cursor = source->cursor;
    destination->control->vdev = &destination->device.vdev;
    destination->cursor->vdev = &destination->device.vdev;
    source->control = NULL;
    source->cursor = NULL;
}

static void test_mmix_virtio_gpu_migration(void)
{
    static const char *args =
        "-machine virt,graphics=off "
        "-device virtio-gpu-device -display none";
    struct virtio_gpu_update_cursor cursor = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_MOVE_CURSOR),
        .pos.x = cpu_to_le32(31),
        .pos.y = cpu_to_le32(37),
    };
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir =
        g_dir_make_tmp("mmix-virtio-gpu-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    g_autofree uint8_t *pixels = g_malloc0(MMIX_GPU_BACKING_SIZE);
    MMIXVirtioGPU source = { 0 };
    MMIXVirtioGPU destination = { 0 };
    uint64_t command_addr;
    uint64_t backing_addr;
    uint32_t head;
    size_t pixel = (21 * MMIX_GPU_WIDTH + 19) * 4;
    QTestState *to;

    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf("%s -incoming %s", args, uri);

    source.qts = qtest_init(args);
    qtest_irq_intercept_out_named(source.qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    alloc_init(&source.allocator, ALLOC_NO_FLAGS, 0x00100000, 0x02000000,
               MMIX_VIRTIO_PAGE_SIZE);
    mmix_virtio_gpu_initialize(&source);
    to = qtest_init(incoming);
    qtest_irq_intercept_out_named(to, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);

    pixels[pixel] = 0x33;
    pixels[pixel + 1] = 0x22;
    pixels[pixel + 2] = 0x11;
    backing_addr = guest_alloc(&source.allocator, MMIX_GPU_BACKING_SIZE);
    qtest_memwrite(source.qts, backing_addr, pixels, MMIX_GPU_BACKING_SIZE);
    mmix_virtio_gpu_create_scanout(&source, backing_addr);
    command_addr = mmix_virtio_gpu_submit_cursor(&source, &cursor, &head);
    mmix_virtio_gpu_wait_irq(&source);

    qtest_qmp_assert_success(
        source.qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_qmp_eventwait(source.qts, "STOP");
    qtest_qmp_eventwait(to, "RESUME");
    mmix_virtio_gpu_adopt_migration(&destination, &source, to);

    g_assert_cmphex(qtest_readl(to, MMIX_VIRTIO_BASE +
                               QVIRTIO_MMIO_DEVICE_STATUS), !=, 0);
    mmix_virtio_gpu_wait_used(&destination, destination.cursor, head);
    mmix_virtio_gpu_finish_irq(&destination);
    guest_free(&destination.allocator, command_addr);
    if (mmix_qmp_has_command(to, "screendump")) {
        mmix_virtio_gpu_assert_pixel(&destination, 19, 21,
                                     0x11, 0x22, 0x33);
    }

    pixels[pixel] = 0xcc;
    pixels[pixel + 1] = 0xbb;
    pixels[pixel + 2] = 0xaa;
    qtest_memwrite(to, backing_addr, pixels, MMIX_GPU_BACKING_SIZE);
    mmix_virtio_gpu_transfer_and_flush(&destination);
    cursor.pos.x = cpu_to_le32(41);
    cursor.pos.y = cpu_to_le32(43);
    mmix_virtio_gpu_cursor(&destination, &cursor);
    if (mmix_qmp_has_command(to, "screendump")) {
        mmix_virtio_gpu_assert_pixel(&destination, 19, 21,
                                     0xaa, 0xbb, 0xcc);
    }

    mmix_virtio_gpu_release_scanout(&destination);
    guest_free(&destination.allocator, backing_addr);
    mmix_virtio_gpu_stop(&destination);
    alloc_destroy(&source.allocator);
    qtest_quit(source.qts);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

static void test_mmix_virtio_gpu_migration_incompatible(void)
{
    static const char *source_args =
        "-machine virt,graphics=off "
        "-device virtio-gpu-device -display none";
    static const char *destination_args =
        "-machine virt,graphics=off "
        "-device virtio-gpu-device,max_outputs=2 -display none";
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir =
        g_dir_make_tmp("mmix-virtio-gpu-fail-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    QTestState *from;
    QTestState *to;
    gint64 deadline;

    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf("%s -incoming %s", destination_args, uri);
    from = qtest_init(source_args);
    to = qtest_init(incoming);
    qtest_set_expected_status(to, 1);

    qtest_qmp_assert_success(
        from, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + MMIX_VIRTIO_TIMEOUT_US;
    while (qtest_probe_child(to)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/virtio-gpu/base-protocol",
                   test_mmix_virtio_gpu_base_protocol);
    qtest_add_func("/mmix/virtio-gpu/2d-resource",
                   test_mmix_virtio_gpu_2d_resource);
    qtest_add_func("/mmix/virtio-gpu/reset",
                   test_mmix_virtio_gpu_reset);
    qtest_add_func("/mmix/virtio-gpu/migration",
                   test_mmix_virtio_gpu_migration);
    qtest_add_func("/mmix/virtio-gpu/migration-incompatible",
                   test_mmix_virtio_gpu_migration_incompatible);
    return g_test_run();
}

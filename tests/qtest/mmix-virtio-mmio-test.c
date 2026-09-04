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
#include "standard-headers/linux/virtio_config.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_ring.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define MMIX_VIRTIO_BASE              UINT64_C(0x0001000040000000)
#define MMIX_VIRTIO_STRIDE            UINT64_C(0x10000)
#define MMIX_VIRTIO_REGISTER_SIZE     UINT64_C(0x200)
#define MMIX_VIRTIO_ACTIVE_SLOTS      32
#define MMIX_VIRTIO_SLOT_CAPACITY     4096
#define MMIX_DISCOVERABLE_BASE        UINT64_C(0x0001000050000000)
#define MMIX_DISCOVERABLE_SLOT_CAPACITY 4096

#define MMIX_INTC_BASE                UINT64_C(0x0001000030000000)
#define MMIX_INTC_PENDING_BASE        0x1000
#define MMIX_INTC_CONTEXT_BASE        UINT64_C(0x0001000034000000)
#define MMIX_INTC_CONTEXT_STRIDE      UINT64_C(0x10000)
#define MMIX_INTC_ENABLE_BASE         0x0000
#define MMIX_INTC_CLAIM               0x0800
#define MMIX_INTC_COMPLETE            0x0808

#define MMIX_VIRTIO_IRQ_BASE          2048
#define MMIX_VIRTIO_PAGE_SIZE         4096
#define MMIX_VIRTIO_TIMEOUT_US        (30 * G_USEC_PER_SEC)

#define MMIX_INTC_QOM_PATH            "/machine/intc"
#define MMIX_INTC_OUTPUT_IRQ          "sysbus-irq"

typedef struct MMIXVirtioBlkReq {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
    uint8_t data[512];
    uint8_t status;
} MMIXVirtioBlkReq;

static uint64_t mmix_virtio_slot(unsigned int slot)
{
    return MMIX_VIRTIO_BASE + slot * MMIX_VIRTIO_STRIDE;
}

static char *mmix_qom_child_by_type(QTestState *qts, const char *path,
                                    const char *type)
{
    g_autoptr(QDict) response = qtest_qmp(
        qts,
        "{ 'execute': 'qom-list', 'arguments': { 'path': %s } }", path);
    QList *properties = qobject_to(QList, qdict_get(response, "return"));
    QListEntry *entry;

    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(property, "type"), type)) {
            return g_strdup(qdict_get_str(property, "name"));
        }
    }
    return NULL;
}

static void mmix_assert_transport_owner(QTestState *qts, unsigned int slot,
                                        const char *device_type)
{
    g_autofree char *transport = g_strdup_printf(
        "/machine/virtio-mmio[%u]", slot);
    g_autofree char *bus_name = mmix_qom_child_by_type(
        qts, transport, "child<virtio-mmio-bus>");
    g_autofree char *bus = NULL;
    g_autofree char *link_type = NULL;
    g_autofree char *device = NULL;

    g_assert_nonnull(bus_name);
    bus = g_strdup_printf("%s/%s", transport, bus_name);
    if (device_type) {
        link_type = g_strdup_printf("link<%s>", device_type);
        device = mmix_qom_child_by_type(qts, bus, link_type);
        g_assert_cmpstr(device, ==, "child[0]");
    } else {
        g_assert_null(mmix_qom_child_by_type(qts, bus,
                                             "link<virtio-blk-device>"));
        g_assert_null(mmix_qom_child_by_type(qts, bus,
                                             "link<virtio-rng-device>"));
    }
}

static uint64_t mmix_intc_source_bit(unsigned int source)
{
    return UINT64_C(1) << (source % 64);
}

static uint64_t mmix_intc_source_reg(uint64_t base, unsigned int source)
{
    return base + (source / 64) * sizeof(uint64_t);
}

static uint64_t mmix_intc_context_reg(unsigned int cpu, uint64_t reg)
{
    return MMIX_INTC_CONTEXT_BASE + cpu * MMIX_INTC_CONTEXT_STRIDE + reg;
}

static uint64_t mmix_intc_pending(QTestState *qts, unsigned int source)
{
    return qtest_readq(qts, mmix_intc_source_reg(
                           MMIX_INTC_BASE + MMIX_INTC_PENDING_BASE, source));
}

static void mmix_intc_write_enable(QTestState *qts, unsigned int cpu,
                                   unsigned int source, uint64_t value)
{
    qtest_writeq(qts, mmix_intc_source_reg(
                     mmix_intc_context_reg(cpu, MMIX_INTC_ENABLE_BASE),
                     source), value);
}

static uint64_t mmix_intc_claim(QTestState *qts, unsigned int cpu)
{
    return qtest_readq(qts, mmix_intc_context_reg(cpu, MMIX_INTC_CLAIM));
}

static void mmix_intc_complete(QTestState *qts, unsigned int cpu,
                               unsigned int source)
{
    qtest_writeq(qts, mmix_intc_context_reg(cpu, MMIX_INTC_COMPLETE), source);
}

static QTestState *mmix_virtio_blocks_start(unsigned int blocks,
                                            unsigned int cpus)
{
    g_autoptr(GString) args = g_string_new(NULL);
    unsigned int i;

    g_string_append_printf(args, "-machine virt -smp %u", cpus);
    for (i = 0; i < blocks; i++) {
        g_string_append_printf(
            args,
            " -drive file=null-co://,if=none,format=raw,id=blk%u"
            " -device virtio-blk-device,drive=blk%u",
            i, i);
    }

    return qtest_init(args->str);
}

static void mmix_virtio_blk_init(QTestState *qts, unsigned int slot,
                                 QVirtioMMIODevice *dev,
                                 QGuestAllocator *alloc, QVirtQueue **vq)
{
    uint64_t features;

    qvirtio_mmio_init_device(dev, qts, mmix_virtio_slot(slot),
                             MMIX_VIRTIO_PAGE_SIZE);
    g_assert_cmpuint(dev->vdev.device_type, ==, VIRTIO_ID_BLOCK);
    qvirtio_start_device(&dev->vdev);

    features = qvirtio_get_features(&dev->vdev);
    features &= ~(QVIRTIO_F_BAD_FEATURE |
                  (1ULL << VIRTIO_RING_F_INDIRECT_DESC) |
                  (1ULL << VIRTIO_RING_F_EVENT_IDX) |
                  (1ULL << VIRTIO_BLK_F_SCSI));
    qvirtio_set_features(&dev->vdev, features);

    alloc_init(alloc, ALLOC_NO_FLAGS, 0x00100000, 0x01000000,
               MMIX_VIRTIO_PAGE_SIZE);
    *vq = qvirtqueue_setup(&dev->vdev, alloc, 0);
    qvirtio_set_driver_ok(&dev->vdev);
}

static uint32_t mmix_virtio_blk_submit_read(QTestState *qts,
                                            QGuestAllocator *alloc,
                                            QVirtioMMIODevice *dev,
                                            QVirtQueue *vq,
                                            uint64_t *req_addr)
{
    MMIXVirtioBlkReq req = {
        .type = cpu_to_le32(VIRTIO_BLK_T_IN),
        .status = 0xff,
    };
    uint32_t free_head;

    *req_addr = guest_alloc(alloc, sizeof(req));
    qtest_memwrite(qts, *req_addr, &req, sizeof(req));

    free_head = qvirtqueue_add(qts, vq, *req_addr, 16, false, true);
    qvirtqueue_add(qts, vq, *req_addr + 16, sizeof(req.data), true, true);
    qvirtqueue_add(qts, vq, *req_addr + 16 + sizeof(req.data),
                   1, true, false);
    qvirtqueue_kick(qts, &dev->vdev, vq, free_head);

    return free_head;
}

static void mmix_virtio_wait_interrupt(QTestState *qts, unsigned int slot)
{
    gint64 deadline = g_get_monotonic_time() + MMIX_VIRTIO_TIMEOUT_US;

    while (!(qtest_readl(qts, mmix_virtio_slot(slot) +
                         QVIRTIO_MMIO_INTERRUPT_STATUS) & 1)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
    }
}

static void mmix_virtio_check_dma(QTestState *qts, uint64_t req_addr)
{
    g_assert_cmphex(qtest_readb(qts, req_addr +
                                offsetof(MMIXVirtioBlkReq, status)), ==,
                    VIRTIO_BLK_S_OK);
}

static void mmix_virtio_cleanup(QVirtioMMIODevice *dev, QVirtQueue *vq,
                                QGuestAllocator *alloc)
{
    qvirtqueue_cleanup(dev->vdev.bus, vq, alloc);
    alloc_destroy(alloc);
}

static void test_mmix_virtio_slots_and_boundaries(void)
{
    static const unsigned int active[] = { 0, 1, 31 };
    QTestState *qts = qtest_init("-machine virt");
    unsigned int slot;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(active); i++) {
        uint64_t base = mmix_virtio_slot(active[i]);

        g_assert_cmphex(qtest_readl(qts, base + QVIRTIO_MMIO_MAGIC_VALUE), ==,
                        0x74726976);
        g_assert_cmphex(qtest_readl(qts, base + QVIRTIO_MMIO_VERSION), ==, 1);
        g_assert_cmphex(qtest_readl(qts, base + QVIRTIO_MMIO_DEVICE_ID), ==,
                        0);
        g_assert_cmphex(qtest_readl(qts, base + QVIRTIO_MMIO_VENDOR_ID), ==,
                        0x554d4551);
    }

    qtest_writel(qts, mmix_virtio_slot(0) + MMIX_VIRTIO_REGISTER_SIZE,
                 0x5a5aa5a5);
    g_assert_cmphex(qtest_readl(qts, mmix_virtio_slot(0) +
                                MMIX_VIRTIO_REGISTER_SIZE), !=, 0x5a5aa5a5);
    qtest_writeb(qts, mmix_virtio_slot(31) + MMIX_VIRTIO_STRIDE - 1, 0x5a);
    g_assert_cmphex(qtest_readb(qts, mmix_virtio_slot(31) +
                                MMIX_VIRTIO_STRIDE - 1), !=, 0x5a);

    for (slot = MMIX_VIRTIO_ACTIVE_SLOTS;
         slot < MMIX_VIRTIO_SLOT_CAPACITY; slot++) {
        g_assert_cmphex(qtest_readl(qts, mmix_virtio_slot(slot) +
                                    QVIRTIO_MMIO_MAGIC_VALUE), !=,
                        0x74726976);
    }
    for (slot = 0; slot < MMIX_DISCOVERABLE_SLOT_CAPACITY; slot++) {
        uint64_t base = MMIX_DISCOVERABLE_BASE + slot * MMIX_VIRTIO_STRIDE;

        g_assert_cmphex(qtest_readl(qts, base +
                                    QVIRTIO_MMIO_MAGIC_VALUE), !=,
                        0x74726976);
    }

    qtest_quit(qts);
}

static void mmix_test_virtio_attachment_order(bool rng_first)
{
    const char *devices = rng_first ?
        "-device virtio-rng-device,id=rngdev,rng=rng0 "
        "-device virtio-blk-device,id=blkdev,drive=blk0" :
        "-device virtio-blk-device,id=blkdev,drive=blk0 "
        "-device virtio-rng-device,id=rngdev,rng=rng0";
    g_autofree char *args = g_strdup_printf(
        "-machine virt "
        "-drive file=null-co://,if=none,format=raw,id=blk0 "
        "-object rng-builtin,id=rng0 %s", devices);
    QTestState *qts = qtest_init(args);

    g_assert_cmphex(qtest_readl(qts, mmix_virtio_slot(0) +
                                QVIRTIO_MMIO_DEVICE_ID), ==,
                    rng_first ? VIRTIO_ID_RNG : VIRTIO_ID_BLOCK);
    g_assert_cmphex(qtest_readl(qts, mmix_virtio_slot(1) +
                                QVIRTIO_MMIO_DEVICE_ID), ==,
                    rng_first ? VIRTIO_ID_BLOCK : VIRTIO_ID_RNG);
    g_assert_cmphex(qtest_readl(qts, mmix_virtio_slot(2) +
                                QVIRTIO_MMIO_DEVICE_ID), ==, 0);
    mmix_assert_transport_owner(qts, 0, rng_first ?
                                "virtio-rng-device" :
                                "virtio-blk-device");
    mmix_assert_transport_owner(qts, 1, rng_first ?
                                "virtio-blk-device" :
                                "virtio-rng-device");
    mmix_assert_transport_owner(qts, 2, NULL);

    qtest_quit(qts);
}

static void test_mmix_virtio_attachment_block_rng(void)
{
    mmix_test_virtio_attachment_order(false);
}

static void test_mmix_virtio_attachment_rng_block(void)
{
    mmix_test_virtio_attachment_order(true);
}

static void test_mmix_virtio_transport_exhaustion(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *stderr_text = NULL;
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
    int wait_status;
    unsigned int i;

    g_ptr_array_add(argv, (void *)qtest_qemu_binary(NULL));
    g_ptr_array_add(argv, (void *)"-machine");
    g_ptr_array_add(argv, (void *)"virt");
    g_ptr_array_add(argv, (void *)"-display");
    g_ptr_array_add(argv, (void *)"none");
    g_ptr_array_add(argv, (void *)"-monitor");
    g_ptr_array_add(argv, (void *)"none");
    g_ptr_array_add(argv, (void *)"-serial");
    g_ptr_array_add(argv, (void *)"none");
    for (i = 0; i <= MMIX_VIRTIO_ACTIVE_SLOTS; i++) {
        g_ptr_array_add(argv, (void *)"-device");
        g_ptr_array_add(argv,
                        g_strdup_printf("virtio-serial-device,id=serial%u",
                                        i));
    }
    g_ptr_array_add(argv, NULL);

    g_assert_true(g_spawn_sync(NULL, (char **)argv->pdata, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &stderr_text,
                               &wait_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(wait_status, !=, 0);
    g_assert_nonnull(strstr(stderr_text,
                           "A 'virtio-bus' bus was found but is full"));
    g_assert_nonnull(strstr(stderr_text, "serial32"));

    for (i = 10; i + 1 < argv->len; i += 2) {
        g_free(g_ptr_array_index(argv, i));
    }
}

static void test_mmix_virtio_reset_state(void)
{
    QTestState *qts = qtest_init(
        "-machine virt -object rng-builtin,id=rng0 "
        "-device virtio-rng-device,rng=rng0");

    g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE +
                                QVIRTIO_MMIO_DEVICE_ID), ==, VIRTIO_ID_RNG);
    qtest_writel(qts, MMIX_VIRTIO_BASE + QVIRTIO_MMIO_DEVICE_STATUS,
                 VIRTIO_CONFIG_S_ACKNOWLEDGE);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE +
                                QVIRTIO_MMIO_DEVICE_STATUS), ==,
                    VIRTIO_CONFIG_S_ACKNOWLEDGE);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE +
                                QVIRTIO_MMIO_DEVICE_ID), ==, VIRTIO_ID_RNG);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE +
                                QVIRTIO_MMIO_DEVICE_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MMIX_VIRTIO_BASE +
                                QVIRTIO_MMIO_INTERRUPT_STATUS), ==, 0);
    mmix_assert_transport_owner(qts, 0, "virtio-rng-device");

    qtest_quit(qts);
}

static void test_mmix_virtio_migration_state(void)
{
    static const char *args =
        "-machine virt -object rng-builtin,id=rng0 "
        "-device virtio-rng-device,rng=rng0";
    g_autoptr(GError) error = NULL;
    g_autofree char *tmpdir =
        g_dir_make_tmp("mmix-virtio-mmio-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    QTestState *from;
    QTestState *to;

    g_assert_no_error(error);
    g_assert_nonnull(tmpdir);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf("%s -incoming %s", args, uri);
    from = qtest_init(args);
    to = qtest_init(incoming);

    qtest_writel(from, MMIX_VIRTIO_BASE + QVIRTIO_MMIO_DEVICE_STATUS,
                 VIRTIO_CONFIG_S_ACKNOWLEDGE);
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(from, "STOP");
    qtest_qmp_eventwait(to, "RESUME");

    g_assert_cmphex(qtest_readl(to, MMIX_VIRTIO_BASE +
                                QVIRTIO_MMIO_DEVICE_ID), ==, VIRTIO_ID_RNG);
    g_assert_cmphex(qtest_readl(to, MMIX_VIRTIO_BASE +
                                QVIRTIO_MMIO_DEVICE_STATUS), ==,
                    VIRTIO_CONFIG_S_ACKNOWLEDGE);
    mmix_assert_transport_owner(to, 0, "virtio-rng-device");

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
}

static void test_mmix_virtio_first_slot_irq_and_dma(void)
{
    const unsigned int source = MMIX_VIRTIO_IRQ_BASE;
    uint64_t bit = mmix_intc_source_bit(source);
    QTestState *qts = mmix_virtio_blocks_start(1, 1);
    QVirtioMMIODevice dev;
    QGuestAllocator alloc;
    QVirtQueue *vq;
    uint64_t req_addr;
    uint32_t free_head;
    uint32_t used_head;

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    mmix_intc_write_enable(qts, 0, source, bit);
    mmix_virtio_blk_init(qts, 0, &dev, &alloc, &vq);
    free_head = mmix_virtio_blk_submit_read(qts, &alloc, &dev, vq,
                                             &req_addr);
    mmix_virtio_wait_interrupt(qts, 0);

    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, source);
    g_assert_true(qvirtqueue_get_buf(qts, vq, &used_head, NULL));
    g_assert_cmpuint(used_head, ==, free_head);
    mmix_virtio_check_dma(qts, req_addr);

    qtest_writel(qts, mmix_virtio_slot(0) +
                 QVIRTIO_MMIO_INTERRUPT_ACK, 1);
    mmix_intc_complete(qts, 0, source);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_virtio_cleanup(&dev, vq, &alloc);
    qtest_quit(qts);
}

static void test_mmix_virtio_shared_irq_retrigger(void)
{
    const unsigned int source = MMIX_VIRTIO_IRQ_BASE;
    uint64_t bit = mmix_intc_source_bit(source);
    QTestState *qts = mmix_virtio_blocks_start(1, 2);
    QVirtioMMIODevice dev;
    QGuestAllocator alloc;
    QVirtQueue *vq;
    uint64_t req_addr;
    uint32_t free_head;
    uint32_t used_head;

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    mmix_intc_write_enable(qts, 0, source, bit);
    mmix_intc_write_enable(qts, 1, source, bit);
    mmix_virtio_blk_init(qts, 0, &dev, &alloc, &vq);
    free_head = mmix_virtio_blk_submit_read(qts, &alloc, &dev, vq,
                                             &req_addr);
    mmix_virtio_wait_interrupt(qts, 0);

    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 1), ==, source);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, 0);

    mmix_intc_complete(qts, 1, source);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, source);
    g_assert_true(qvirtqueue_get_buf(qts, vq, &used_head, NULL));
    g_assert_cmpuint(used_head, ==, free_head);
    mmix_virtio_check_dma(qts, req_addr);

    qtest_writel(qts, mmix_virtio_slot(0) +
                 QVIRTIO_MMIO_INTERRUPT_ACK, 1);
    mmix_intc_complete(qts, 0, source);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    mmix_virtio_cleanup(&dev, vq, &alloc);
    qtest_quit(qts);
}

static void test_mmix_virtio_last_slot_irq_and_dma(void)
{
    const unsigned int slot = MMIX_VIRTIO_ACTIVE_SLOTS - 1;
    const unsigned int source = MMIX_VIRTIO_IRQ_BASE + slot;
    uint64_t bit = mmix_intc_source_bit(source);
    QTestState *qts = mmix_virtio_blocks_start(MMIX_VIRTIO_ACTIVE_SLOTS, 1);
    QVirtioMMIODevice dev;
    QGuestAllocator alloc;
    QVirtQueue *vq;
    uint64_t req_addr;
    uint32_t free_head;
    uint32_t used_head;

    qtest_irq_intercept_out_named(qts, MMIX_INTC_QOM_PATH,
                                  MMIX_INTC_OUTPUT_IRQ);
    mmix_intc_write_enable(qts, 0, source, bit);
    mmix_virtio_blk_init(qts, slot, &dev, &alloc, &vq);
    free_head = mmix_virtio_blk_submit_read(qts, &alloc, &dev, vq,
                                             &req_addr);
    mmix_virtio_wait_interrupt(qts, slot);

    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, bit);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(mmix_intc_claim(qts, 0), ==, source);
    g_assert_true(qvirtqueue_get_buf(qts, vq, &used_head, NULL));
    g_assert_cmpuint(used_head, ==, free_head);
    mmix_virtio_check_dma(qts, req_addr);

    qtest_writel(qts, mmix_virtio_slot(slot) +
                 QVIRTIO_MMIO_INTERRUPT_ACK, 1);
    mmix_intc_complete(qts, 0, source);
    g_assert_cmphex(mmix_intc_pending(qts, source) & bit, ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));

    mmix_virtio_cleanup(&dev, vq, &alloc);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/virtio-mmio/slots-and-boundaries",
                   test_mmix_virtio_slots_and_boundaries);
    qtest_add_func("/mmix/virtio-mmio/attachment-order/block-rng",
                   test_mmix_virtio_attachment_block_rng);
    qtest_add_func("/mmix/virtio-mmio/attachment-order/rng-block",
                   test_mmix_virtio_attachment_rng_block);
    qtest_add_func("/mmix/virtio-mmio/transport-exhaustion",
                   test_mmix_virtio_transport_exhaustion);
    qtest_add_func("/mmix/virtio-mmio/reset-state",
                   test_mmix_virtio_reset_state);
    qtest_add_func("/mmix/virtio-mmio/migration-state",
                   test_mmix_virtio_migration_state);
    qtest_add_func("/mmix/virtio-mmio/first-slot-irq-and-dma",
                   test_mmix_virtio_first_slot_irq_and_dma);
    qtest_add_func("/mmix/virtio-mmio/shared-irq-retrigger",
                   test_mmix_virtio_shared_irq_retrigger);
    qtest_add_func("/mmix/virtio-mmio/last-slot-irq-and-dma",
                   test_mmix_virtio_last_slot_irq_and_dma);

    return g_test_run();
}

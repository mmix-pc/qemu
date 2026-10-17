/*
 * libqos virtio MMIO driver
 *
 * Copyright (c) 2014 Marc Marí
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "../libqtest.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "virtio.h"
#include "virtio-mmio.h"
#include "libqos-malloc.h"
#include "qgraph.h"
#include "standard-headers/linux/virtio_ring.h"

static uint32_t qvirtio_mmio_readl(QVirtioMMIODevice *dev, uint64_t offset)
{
    uint32_t value;

    if (dev->version < 2) {
        return qtest_readl(dev->qts, dev->addr + offset);
    }

    qtest_memread(dev->qts, dev->addr + offset, &value, sizeof(value));
    return le32_to_cpu(value);
}

static void qvirtio_mmio_writel(QVirtioMMIODevice *dev, uint64_t offset,
                                uint32_t value)
{
    if (dev->version < 2) {
        qtest_writel(dev->qts, dev->addr + offset, value);
        return;
    }

    value = cpu_to_le32(value);
    qtest_memwrite(dev->qts, dev->addr + offset, &value, sizeof(value));
}

static uint8_t qvirtio_mmio_config_readb(QVirtioDevice *d, uint64_t off)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);

    return qtest_readb(dev->qts,
                       dev->addr + QVIRTIO_MMIO_DEVICE_SPECIFIC + off);
}

static uint16_t qvirtio_mmio_config_readw(QVirtioDevice *d, uint64_t off)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    uint64_t offset = QVIRTIO_MMIO_DEVICE_SPECIFIC + off;

    if (dev->version >= 2) {
        uint16_t value;

        qtest_memread(dev->qts, dev->addr + offset, &value, sizeof(value));
        return le16_to_cpu(value);
    }
    return qtest_readw(dev->qts, dev->addr + offset);
}

static uint32_t qvirtio_mmio_config_readl(QVirtioDevice *d, uint64_t off)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);

    return qvirtio_mmio_readl(dev, QVIRTIO_MMIO_DEVICE_SPECIFIC + off);
}

static uint64_t qvirtio_mmio_config_readq(QVirtioDevice *d, uint64_t off)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    uint64_t offset = QVIRTIO_MMIO_DEVICE_SPECIFIC + off;

    if (dev->version >= 2) {
        uint64_t value;

        qtest_memread(dev->qts, dev->addr + offset, &value, sizeof(value));
        return le64_to_cpu(value);
    }
    return qtest_readq(dev->qts, dev->addr + offset);
}

static uint64_t qvirtio_mmio_get_features(QVirtioDevice *d)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    uint64_t lo;
    uint64_t hi = 0;

    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_HOST_FEATURES_SEL, 0);
    lo = qvirtio_mmio_readl(dev, QVIRTIO_MMIO_HOST_FEATURES);

    if (dev->version >= 2) {
        qvirtio_mmio_writel(dev, QVIRTIO_MMIO_HOST_FEATURES_SEL, 1);
        hi = qvirtio_mmio_readl(dev, QVIRTIO_MMIO_HOST_FEATURES);
    }

    return (hi << 32) | lo;
}

static void qvirtio_mmio_set_features(QVirtioDevice *d, uint64_t features)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    dev->features = features;
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_GUEST_FEATURES_SEL, 0);
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_GUEST_FEATURES, features);

    if (dev->version >= 2) {
        qvirtio_mmio_writel(dev, QVIRTIO_MMIO_GUEST_FEATURES_SEL, 1);
        qvirtio_mmio_writel(dev, QVIRTIO_MMIO_GUEST_FEATURES, features >> 32);
    }
}

static uint64_t qvirtio_mmio_get_guest_features(QVirtioDevice *d)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    return dev->features;
}

static uint8_t qvirtio_mmio_get_status(QVirtioDevice *d)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    return (uint8_t)qvirtio_mmio_readl(dev, QVIRTIO_MMIO_DEVICE_STATUS);
}

static void qvirtio_mmio_set_status(QVirtioDevice *d, uint8_t status)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_DEVICE_STATUS, status);
}

static bool qvirtio_mmio_get_queue_isr_status(QVirtioDevice *d, QVirtQueue *vq)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    uint32_t isr;

    isr = qvirtio_mmio_readl(dev, QVIRTIO_MMIO_INTERRUPT_STATUS) & 1;
    if (isr != 0) {
        qvirtio_mmio_writel(dev, QVIRTIO_MMIO_INTERRUPT_ACK, 1);
        return true;
    }

    return false;
}

static bool qvirtio_mmio_get_config_isr_status(QVirtioDevice *d)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    uint32_t isr;

    isr = qvirtio_mmio_readl(dev, QVIRTIO_MMIO_INTERRUPT_STATUS) & 2;
    if (isr != 0) {
        qvirtio_mmio_writel(dev, QVIRTIO_MMIO_INTERRUPT_ACK, 2);
        return true;
    }

    return false;
}

static void qvirtio_mmio_wait_config_isr_status(QVirtioDevice *d,
                                                gint64 timeout_us)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    gint64 start_time = g_get_monotonic_time();

    do {
        g_assert(g_get_monotonic_time() - start_time <= timeout_us);
        qtest_clock_step(dev->qts, 100);
    } while (!qvirtio_mmio_get_config_isr_status(d));
}

static void qvirtio_mmio_queue_select(QVirtioDevice *d, uint16_t index)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_SEL, index);
    g_assert_cmphex(qvirtio_mmio_readl(
                        dev, dev->version >= 2 ? QVIRTIO_MMIO_QUEUE_READY :
                                                QVIRTIO_MMIO_QUEUE_PFN),
                    ==, 0);
}

static uint16_t qvirtio_mmio_get_queue_size(QVirtioDevice *d)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    return (uint16_t)qvirtio_mmio_readl(dev,
                                        QVIRTIO_MMIO_QUEUE_NUM_MAX);
}

static void qvirtio_mmio_set_queue_address(QVirtioDevice *d, QVirtQueue *vq)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    if (dev->version < 2) {
        qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_PFN,
                            vq->desc / dev->page_size);
        return;
    }

    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_DESC_LOW, vq->desc);
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_DESC_HIGH, vq->desc >> 32);
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_AVAIL_LOW, vq->avail);
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_AVAIL_HIGH, vq->avail >> 32);
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_USED_LOW, vq->used);
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_USED_HIGH, vq->used >> 32);
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_READY, 1);
}

static QVirtQueue *qvirtio_mmio_virtqueue_setup(QVirtioDevice *d,
                                        QGuestAllocator *alloc, uint16_t index)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    QVirtQueue *vq;
    uint64_t addr;

    vq = g_malloc0(sizeof(*vq));
    vq->vdev = d;
    qvirtio_mmio_queue_select(d, index);
    if (dev->version < 2) {
        qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_ALIGN, dev->page_size);
    }

    vq->index = index;
    vq->size = qvirtio_mmio_get_queue_size(d);
    vq->free_head = 0;
    vq->num_free = vq->size;
    vq->align = dev->page_size;
    vq->indirect = dev->features & (1ull << VIRTIO_RING_F_INDIRECT_DESC);
    vq->event = dev->features & (1ull << VIRTIO_RING_F_EVENT_IDX);

    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_NUM, vq->size);

    /* Check different than 0 */
    g_assert_cmpint(vq->size, !=, 0);

    /* Check power of 2 */
    g_assert_cmpint(vq->size & (vq->size - 1), ==, 0);

    addr = guest_alloc(alloc, qvring_size(vq->size, dev->page_size));
    qvring_init(dev->qts, alloc, vq, addr);
    qvirtio_mmio_set_queue_address(d, vq);

    return vq;
}

static void qvirtio_mmio_virtqueue_cleanup(QVirtQueue *vq,
                                           QGuestAllocator *alloc)
{
    guest_free(alloc, vq->desc);
    g_free(vq);
}

static void qvirtio_mmio_virtqueue_kick(QVirtioDevice *d, QVirtQueue *vq)
{
    QVirtioMMIODevice *dev = container_of(d, QVirtioMMIODevice, vdev);
    qvirtio_mmio_writel(dev, QVIRTIO_MMIO_QUEUE_NOTIFY, vq->index);
}

const QVirtioBus qvirtio_mmio = {
    .config_readb = qvirtio_mmio_config_readb,
    .config_readw = qvirtio_mmio_config_readw,
    .config_readl = qvirtio_mmio_config_readl,
    .config_readq = qvirtio_mmio_config_readq,
    .get_features = qvirtio_mmio_get_features,
    .set_features = qvirtio_mmio_set_features,
    .get_guest_features = qvirtio_mmio_get_guest_features,
    .get_status = qvirtio_mmio_get_status,
    .set_status = qvirtio_mmio_set_status,
    .get_queue_isr_status = qvirtio_mmio_get_queue_isr_status,
    .wait_config_isr_status = qvirtio_mmio_wait_config_isr_status,
    .queue_select = qvirtio_mmio_queue_select,
    .get_queue_size = qvirtio_mmio_get_queue_size,
    .set_queue_address = qvirtio_mmio_set_queue_address,
    .virtqueue_setup = qvirtio_mmio_virtqueue_setup,
    .virtqueue_cleanup = qvirtio_mmio_virtqueue_cleanup,
    .virtqueue_kick = qvirtio_mmio_virtqueue_kick,
};

static void *qvirtio_mmio_get_driver(void *obj, const char *interface)
{
    QVirtioMMIODevice *virtio_mmio = obj;
    if (!g_strcmp0(interface, "virtio-bus")) {
        return &virtio_mmio->vdev;
    }
    fprintf(stderr, "%s not present in virtio-mmio\n", interface);
    g_assert_not_reached();
}

static void qvirtio_mmio_start_hw(QOSGraphObject *obj)
{
    QVirtioMMIODevice *dev = (QVirtioMMIODevice *) obj;
    qvirtio_start_device(&dev->vdev);
}

void qvirtio_mmio_init_device(QVirtioMMIODevice *dev, QTestState *qts,
                              uint64_t addr, uint32_t page_size)
{
    uint32_t magic;

    dev->qts = qts;
    dev->addr = addr;
    qtest_memread(qts, addr + QVIRTIO_MMIO_MAGIC_VALUE,
                  &magic, sizeof(magic));
    magic = le32_to_cpu(magic);
    if (magic == ('v' | 'i' << 8 | 'r' << 16 | 't' << 24)) {
        qtest_memread(qts, addr + QVIRTIO_MMIO_VERSION,
                      &dev->version, sizeof(dev->version));
        dev->version = le32_to_cpu(dev->version);
    } else {
        magic = qtest_readl(qts, addr + QVIRTIO_MMIO_MAGIC_VALUE);
        dev->version = qtest_readl(qts, addr + QVIRTIO_MMIO_VERSION);
    }
    g_assert(magic == ('v' | 'i' << 8 | 'r' << 16 | 't' << 24));
    g_assert(dev->version == 1 || dev->version == 2);

    dev->page_size = page_size;
    dev->vdev.device_type = qvirtio_mmio_readl(dev,
                                               QVIRTIO_MMIO_DEVICE_ID);
    dev->vdev.bus = &qvirtio_mmio;

    if (dev->version < 2) {
        qvirtio_mmio_writel(dev, QVIRTIO_MMIO_GUEST_PAGE_SIZE, page_size);
    }

    dev->obj.get_driver = qvirtio_mmio_get_driver;
    dev->obj.start_hw = qvirtio_mmio_start_hw;
}

static void virtio_mmio_register_nodes(void)
{
    qos_node_create_driver("virtio-mmio", NULL);
    qos_node_produces("virtio-mmio", "virtio-bus");
}

libqos_init(virtio_mmio_register_nodes);

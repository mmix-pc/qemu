/*
 * QTest testcase for the MMIX virt framebuffer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qemu/units.h"

#define MMIX_VIRT_FRAMEBUFFER_CONTROL_BASE 0x0001000018000000ULL
#define MMIX_VIRT_DEFAULT_RAM_SIZE (512 * MiB)
#define MMIX_VIRT_MIN_RAM_SIZE (128 * MiB)
#define MMIX_VIRT_INITIAL_STACK_BASE 0x00010000ULL
#define MMIX_VIRT_INITIAL_STACK_SIZE (64 * 0x8000ULL)

#define MMIX_VIRT_FRAMEBUFFER_WIDTH 1024
#define MMIX_VIRT_FRAMEBUFFER_HEIGHT 768
#define MMIX_VIRT_FRAMEBUFFER_STRIDE 4096
#define MMIX_VIRT_FRAMEBUFFER_FORMAT_XRGB8888 1
#define MMIX_VIRT_FRAMEBUFFER_VISIBLE_SIZE \
    (MMIX_VIRT_FRAMEBUFFER_HEIGHT * MMIX_VIRT_FRAMEBUFFER_STRIDE)
#define MMIX_VIRT_FRAMEBUFFER_SIZE MMIX_VIRT_FRAMEBUFFER_VISIBLE_SIZE

#define MMIX_VIRT_FRAMEBUFFER_REG_WIDTH 0x00
#define MMIX_VIRT_FRAMEBUFFER_REG_HEIGHT 0x08
#define MMIX_VIRT_FRAMEBUFFER_REG_STRIDE 0x10
#define MMIX_VIRT_FRAMEBUFFER_REG_FORMAT 0x18
#define MMIX_VIRT_FRAMEBUFFER_REG_BASE 0x20
#define MMIX_VIRT_FRAMEBUFFER_REG_SIZE 0x28
#define MMIX_VIRT_FRAMEBUFFER_REG_FLUSH 0x30

typedef struct MMIXPPMImage {
    uint8_t *data;
    size_t len;
    size_t pixels_offset;
    unsigned width;
    unsigned height;
} MMIXPPMImage;

static uint64_t mmix_framebuffer_reg(unsigned reg)
{
    return MMIX_VIRT_FRAMEBUFFER_CONTROL_BASE + reg;
}

static uint64_t mmix_framebuffer_base(QTestState *qts)
{
    return qtest_readq(
        qts, mmix_framebuffer_reg(MMIX_VIRT_FRAMEBUFFER_REG_BASE));
}

static uint64_t mmix_framebuffer_pixel_addr(QTestState *qts,
                                            unsigned x, unsigned y)
{
    return mmix_framebuffer_base(qts) +
           (uint64_t)y * MMIX_VIRT_FRAMEBUFFER_STRIDE + x * 4;
}

static void mmix_framebuffer_write_pixel(QTestState *qts, unsigned x,
                                         unsigned y, uint8_t r, uint8_t g,
                                         uint8_t b)
{
    const uint8_t pixel[4] = { 0, r, g, b };

    qtest_memwrite(qts, mmix_framebuffer_pixel_addr(qts, x, y), pixel,
                   sizeof(pixel));
}

static void mmix_framebuffer_flush(QTestState *qts)
{
    qtest_writeq(qts, mmix_framebuffer_reg(MMIX_VIRT_FRAMEBUFFER_REG_FLUSH),
                 1);
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
    cursor++;
    image.pixels_offset = cursor - image.data;
    g_assert_cmpuint(image.len - image.pixels_offset, ==,
                     (size_t)image.width * image.height * 3);
    return image;
}

static void mmix_ppm_assert_pixel(const MMIXPPMImage *image,
                                  unsigned x, unsigned y,
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

static char *mmix_framebuffer_screendump(QTestState *qts, unsigned sequence)
{
    g_autofree char *tmpdir = g_canonicalize_filename(g_get_tmp_dir(), NULL);
    char *path = g_strdup_printf("%s/mmix-framebuffer-%u-%u.ppm", tmpdir,
                                 (unsigned)getpid(), sequence);

    g_unlink(path);
    qtest_qmp_assert_success(
        qts,
        "{'execute':'screendump','arguments':{'filename':%s,'format':'ppm'}}",
        path);
    return path;
}

static bool mmix_qmp_has_command(QTestState *qts, const char *command)
{
    QDict *response = qtest_qmp(qts, "{'execute':'query-commands'}");
    QList *commands = qdict_get_qlist(response, "return");
    const QListEntry *entry;
    bool found = false;

    QLIST_FOREACH_ENTRY(commands, entry) {
        QDict *info = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(info, "name"), command)) {
            found = true;
            break;
        }
    }

    qobject_unref(response);
    return found;
}

static void mmix_framebuffer_assert_placement(const char *memory,
                                              uint64_t ram_size)
{
    g_autofree char *args = memory ?
        g_strdup_printf("-machine virt -m %s", memory) :
        g_strdup("-machine virt");
    QTestState *qts = qtest_init(args);
    uint64_t base = mmix_framebuffer_base(qts);

    g_assert_cmphex(base, ==, ram_size - MMIX_VIRT_FRAMEBUFFER_SIZE);
    g_assert_cmphex(base % (8 * KiB), ==, 0);
    g_assert_cmphex(qtest_readq(
                        qts, mmix_framebuffer_reg(
                                 MMIX_VIRT_FRAMEBUFFER_REG_SIZE)), ==,
                    MMIX_VIRT_FRAMEBUFFER_SIZE);
    g_assert_cmphex(MMIX_VIRT_INITIAL_STACK_BASE +
                    MMIX_VIRT_INITIAL_STACK_SIZE, <=, base);

    qtest_quit(qts);
}

static void test_mmix_framebuffer_placement_minimum(void)
{
    mmix_framebuffer_assert_placement("128M", MMIX_VIRT_MIN_RAM_SIZE);
}

static void test_mmix_framebuffer_placement_default(void)
{
    mmix_framebuffer_assert_placement(NULL, MMIX_VIRT_DEFAULT_RAM_SIZE);
}

static void test_mmix_framebuffer_placement_above_4g(void)
{
    mmix_framebuffer_assert_placement("8G", 8 * GiB);
}

static void test_mmix_framebuffer_registers(void)
{
    QTestState *qts = qtest_init("-machine virt");
    uint64_t expected_base =
        MMIX_VIRT_DEFAULT_RAM_SIZE - MMIX_VIRT_FRAMEBUFFER_SIZE;
    const struct {
        unsigned reg;
        uint64_t value;
    } registers[] = {
        { MMIX_VIRT_FRAMEBUFFER_REG_WIDTH, MMIX_VIRT_FRAMEBUFFER_WIDTH },
        { MMIX_VIRT_FRAMEBUFFER_REG_HEIGHT, MMIX_VIRT_FRAMEBUFFER_HEIGHT },
        { MMIX_VIRT_FRAMEBUFFER_REG_STRIDE, MMIX_VIRT_FRAMEBUFFER_STRIDE },
        { MMIX_VIRT_FRAMEBUFFER_REG_FORMAT,
          MMIX_VIRT_FRAMEBUFFER_FORMAT_XRGB8888 },
        { MMIX_VIRT_FRAMEBUFFER_REG_BASE, expected_base },
        { MMIX_VIRT_FRAMEBUFFER_REG_SIZE, MMIX_VIRT_FRAMEBUFFER_SIZE },
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(registers); i++) {
        uint64_t addr = mmix_framebuffer_reg(registers[i].reg);

        g_assert_cmphex(qtest_readq(qts, addr), ==, registers[i].value);
        qtest_writeq(qts, addr, ~registers[i].value);
        g_assert_cmphex(qtest_readq(qts, addr), ==, registers[i].value);
    }
    g_assert_cmphex(qtest_readq(
                        qts, mmix_framebuffer_reg(
                                 MMIX_VIRT_FRAMEBUFFER_REG_FLUSH)), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_framebuffer_memory_and_flush(void)
{
    QTestState *qts = qtest_init("-machine virt");
    const uint8_t first_pixel[4] = { 0, 0x12, 0x34, 0x56 };
    const uint8_t middle_pixel[4] = { 0, 0x5a, 0xa5, 0xc3 };
    const uint8_t last_pixel[4] = { 0, 0xab, 0xcd, 0xef };
    const uint8_t stack_value[4] = { 0xde, 0xad, 0xbe, 0xef };
    uint64_t base = mmix_framebuffer_base(qts);
    uint8_t actual[4];

    qtest_memwrite(qts, MMIX_VIRT_INITIAL_STACK_BASE, stack_value,
                   sizeof(stack_value));
    qtest_memwrite(qts, base, first_pixel,
                   sizeof(first_pixel));
    qtest_memread(qts, base, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), first_pixel, sizeof(first_pixel));
    qtest_memread(qts, MMIX_VIRT_INITIAL_STACK_BASE, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), stack_value, sizeof(stack_value));

    qtest_memwrite(qts,
                   base + MMIX_VIRT_FRAMEBUFFER_SIZE - sizeof(last_pixel),
                   last_pixel, sizeof(last_pixel));
    qtest_memread(qts,
                  base + MMIX_VIRT_FRAMEBUFFER_SIZE - sizeof(actual),
                  actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), last_pixel, sizeof(last_pixel));

    mmix_framebuffer_write_pixel(qts, 500, 400, 0x5a, 0xa5, 0xc3);
    qtest_memread(qts, mmix_framebuffer_pixel_addr(qts, 500, 400), actual,
                  sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), middle_pixel,
                    sizeof(middle_pixel));

    mmix_framebuffer_flush(qts);

    qtest_quit(qts);
}

static void test_mmix_framebuffer_reset(void)
{
    QTestState *qts = qtest_init("-machine virt");
    const uint8_t pixel[4] = { 0, 0x12, 0x34, 0x56 };
    uint64_t base = mmix_framebuffer_base(qts);
    uint8_t actual[sizeof(pixel)];

    qtest_memwrite(qts, base, pixel, sizeof(pixel));
    mmix_framebuffer_flush(qts);
    qtest_system_reset(qts);

    g_assert_cmphex(mmix_framebuffer_base(qts), ==, base);
    g_assert_cmphex(qtest_readq(
                        qts, mmix_framebuffer_reg(
                                 MMIX_VIRT_FRAMEBUFFER_REG_SIZE)), ==,
                    MMIX_VIRT_FRAMEBUFFER_SIZE);
    qtest_memread(qts, base, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), pixel, sizeof(pixel));

    qtest_quit(qts);
}

static void test_mmix_framebuffer_render(void)
{
    QTestState *qts = qtest_init("-machine virt");
    g_autofree char *initial_path = NULL;
    g_autofree char *first_path = NULL;
    g_autofree char *second_path = NULL;
    MMIXPPMImage initial;
    MMIXPPMImage first;
    MMIXPPMImage second;

    if (!mmix_qmp_has_command(qts, "screendump")) {
        g_test_skip("screendump requires a QEMU build with pixman");
        qtest_quit(qts);
        return;
    }

    initial_path = mmix_framebuffer_screendump(qts, 0);
    initial = mmix_ppm_load(initial_path);
    g_assert_cmpuint(initial.width, ==, MMIX_VIRT_FRAMEBUFFER_WIDTH);
    g_assert_cmpuint(initial.height, ==, MMIX_VIRT_FRAMEBUFFER_HEIGHT);
    mmix_ppm_assert_pixel(&initial, 10, 20, 0, 0, 0);
    g_clear_pointer(&initial.data, g_free);
    g_assert_cmpint(g_unlink(initial_path), ==, 0);

    mmix_framebuffer_write_pixel(qts, 10, 20, 0x12, 0x34, 0x56);
    mmix_framebuffer_write_pixel(qts, 1000, 700, 0xab, 0xcd, 0xef);
    mmix_framebuffer_flush(qts);
    first_path = mmix_framebuffer_screendump(qts, 1);
    first = mmix_ppm_load(first_path);
    mmix_ppm_assert_pixel(&first, 10, 20, 0x12, 0x34, 0x56);
    mmix_ppm_assert_pixel(&first, 1000, 700, 0xab, 0xcd, 0xef);
    mmix_ppm_assert_pixel(&first, 500, 400, 0, 0, 0);
    g_clear_pointer(&first.data, g_free);
    g_assert_cmpint(g_unlink(first_path), ==, 0);

    qtest_system_reset(qts);
    mmix_framebuffer_write_pixel(qts, 500, 400, 0x5a, 0xa5, 0xc3);
    mmix_framebuffer_flush(qts);
    second_path = mmix_framebuffer_screendump(qts, 2);
    second = mmix_ppm_load(second_path);
    mmix_ppm_assert_pixel(&second, 10, 20, 0x12, 0x34, 0x56);
    mmix_ppm_assert_pixel(&second, 1000, 700, 0xab, 0xcd, 0xef);
    mmix_ppm_assert_pixel(&second, 500, 400, 0x5a, 0xa5, 0xc3);
    g_clear_pointer(&second.data, g_free);
    g_assert_cmpint(g_unlink(second_path), ==, 0);

    qtest_quit(qts);
}

static void test_mmix_framebuffer_startup_failure(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *stderr_text = NULL;
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-machine", "virt",
        "-m", "127M",
        "-display", "none",
        "-monitor", "none",
        "-serial", "none",
        NULL,
    };
    int wait_status;

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &stderr_text,
                               &wait_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(wait_status, !=, 0);
    g_assert_nonnull(strstr(stderr_text,
                           "below the minimum 0x8000000"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/mmix/framebuffer/placement/minimum",
                   test_mmix_framebuffer_placement_minimum);
    qtest_add_func("/mmix/framebuffer/placement/default",
                   test_mmix_framebuffer_placement_default);
    qtest_add_func("/mmix/framebuffer/placement/above-4g",
                   test_mmix_framebuffer_placement_above_4g);
    qtest_add_func("/mmix/framebuffer/registers",
                   test_mmix_framebuffer_registers);
    qtest_add_func("/mmix/framebuffer/memory-and-flush",
                   test_mmix_framebuffer_memory_and_flush);
    qtest_add_func("/mmix/framebuffer/reset",
                   test_mmix_framebuffer_reset);
    qtest_add_func("/mmix/framebuffer/render",
                   test_mmix_framebuffer_render);
    qtest_add_func("/mmix/framebuffer/startup-failure",
                   test_mmix_framebuffer_startup_failure);

    return g_test_run();
}

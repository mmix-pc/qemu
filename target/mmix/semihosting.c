/*
 * QEMU MMIX semihosting helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "semihosting.h"
#include "exec/helper-proto.h"
#include "exec/log.h"
#include "qemu/main-loop.h"
#include "semihosting/semihost.h"
#include "semihosting/syscalls.h"
#include "system/memory.h"

#define MMIX_SEMIHOSTING_STRING_MAX 256
#define MMIX_SEMIHOSTING_BUFFER_MAX (1024 * 1024)
#define MMIX_SEMIHOSTING_GUESTFD_NONE 0
#define MMIX_SEMIHOSTING_SUCCESS 0
#define MMIX_SEMIHOSTING_FAILURE UINT64_MAX
/*
 * Keep the current UART-backed output sink behind the MMIX semihosting
 * console boundary. The sink writes the MMIX virt UART0 THR byte register.
 * Later console work can route this through QEMU's semihosting backend
 * without changing the TRAP dispatch shape.
 */
#define MMIX_SEMIHOSTING_CONSOLE_TX 0x10000000ULL

typedef enum MMIXSemihostingService {
    MMIX_SEMIHOSTING_SERVICE_HALT = 0,
    MMIX_SEMIHOSTING_SERVICE_FOPEN = 1,
    MMIX_SEMIHOSTING_SERVICE_FCLOSE = 2,
    MMIX_SEMIHOSTING_SERVICE_FREAD = 3,
    MMIX_SEMIHOSTING_SERVICE_FGETS = 4,
    MMIX_SEMIHOSTING_SERVICE_FWRITE = 6,
    MMIX_SEMIHOSTING_SERVICE_FPUTS = 7,
    MMIX_SEMIHOSTING_SERVICE_FSEEK = 9,
    MMIX_SEMIHOSTING_SERVICE_FTELL = 10,
} MMIXSemihostingService;

typedef enum MMIXSemihostingHandle {
    MMIX_SEMIHOSTING_HANDLE_STDIN = 0,
    MMIX_SEMIHOSTING_HANDLE_STDOUT = 1,
    MMIX_SEMIHOSTING_HANDLE_STDERR = 2,
    MMIX_SEMIHOSTING_HANDLE_FIRST_FILE = 3,
} MMIXSemihostingHandle;

typedef enum MMIXSemihostingFileMode {
    MMIX_SEMIHOSTING_MODE_TEXT_READ = 0,
    MMIX_SEMIHOSTING_MODE_TEXT_WRITE = 1,
    MMIX_SEMIHOSTING_MODE_BINARY_READ = 2,
    MMIX_SEMIHOSTING_MODE_BINARY_WRITE = 3,
    MMIX_SEMIHOSTING_MODE_BINARY_READ_WRITE = 4,
} MMIXSemihostingFileMode;

typedef enum MMIXSemihostingAction {
    MMIX_SEMIHOSTING_ACTION_HALT,
    MMIX_SEMIHOSTING_ACTION_FOPEN,
    MMIX_SEMIHOSTING_ACTION_FCLOSE,
    MMIX_SEMIHOSTING_ACTION_FREAD,
    MMIX_SEMIHOSTING_ACTION_FGETS,
    MMIX_SEMIHOSTING_ACTION_FWRITE,
    MMIX_SEMIHOSTING_ACTION_FPUTS_CONSOLE,
    MMIX_SEMIHOSTING_ACTION_FPUTS_BAD_HANDLE,
    MMIX_SEMIHOSTING_ACTION_FSEEK,
    MMIX_SEMIHOSTING_ACTION_FTELL,
    MMIX_SEMIHOSTING_ACTION_UNSUPPORTED,
} MMIXSemihostingAction;

typedef struct MMIXSemihostingCall {
    MMIXSemihostingAction action;
    uint32_t service;
    uint32_t handle;
} MMIXSemihostingCall;

typedef struct MMIXSemihostingArgs2 {
    uint64_t arg2;
} MMIXSemihostingArgs2;

typedef struct MMIXSemihostingArgs3 {
    uint64_t arg2;
    uint64_t arg3;
} MMIXSemihostingArgs3;

static G_NORETURN void mmix_semihosting_halt(CPUMMIXState *env)
{
    mmix_cpu_shutdown_with_log(env, "MMIX hosted Halt",
                               mmix_cpu_read_reg(env, 255) & 0xff);
}

static MMIXSemihostingCall mmix_semihosting_decode_call(uint32_t service,
                                                        uint32_t handle)
{
    MMIXSemihostingCall call = {
        .action = MMIX_SEMIHOSTING_ACTION_UNSUPPORTED,
        .service = service,
        .handle = handle,
    };

    switch (service) {
    case MMIX_SEMIHOSTING_SERVICE_HALT:
        if (handle == 0) {
            call.action = MMIX_SEMIHOSTING_ACTION_HALT;
        }
        break;
    case MMIX_SEMIHOSTING_SERVICE_FOPEN:
        call.action = MMIX_SEMIHOSTING_ACTION_FOPEN;
        break;
    case MMIX_SEMIHOSTING_SERVICE_FCLOSE:
        call.action = MMIX_SEMIHOSTING_ACTION_FCLOSE;
        break;
    case MMIX_SEMIHOSTING_SERVICE_FREAD:
        call.action = MMIX_SEMIHOSTING_ACTION_FREAD;
        break;
    case MMIX_SEMIHOSTING_SERVICE_FGETS:
        call.action = MMIX_SEMIHOSTING_ACTION_FGETS;
        break;
    case MMIX_SEMIHOSTING_SERVICE_FWRITE:
        call.action = MMIX_SEMIHOSTING_ACTION_FWRITE;
        break;
    case MMIX_SEMIHOSTING_SERVICE_FPUTS:
        if (handle == MMIX_SEMIHOSTING_HANDLE_STDOUT ||
            handle == MMIX_SEMIHOSTING_HANDLE_STDERR) {
            call.action = MMIX_SEMIHOSTING_ACTION_FPUTS_CONSOLE;
        } else {
            call.action = MMIX_SEMIHOSTING_ACTION_FPUTS_BAD_HANDLE;
        }
        break;
    case MMIX_SEMIHOSTING_SERVICE_FSEEK:
        call.action = MMIX_SEMIHOSTING_ACTION_FSEEK;
        break;
    case MMIX_SEMIHOSTING_SERVICE_FTELL:
        call.action = MMIX_SEMIHOSTING_ACTION_FTELL;
        break;
    default:
        break;
    }

    return call;
}

static const char *mmix_semihosting_service_name(uint32_t service)
{
    switch (service) {
    case MMIX_SEMIHOSTING_SERVICE_HALT:
        return "Halt";
    case MMIX_SEMIHOSTING_SERVICE_FOPEN:
        return "Fopen";
    case MMIX_SEMIHOSTING_SERVICE_FCLOSE:
        return "Fclose";
    case MMIX_SEMIHOSTING_SERVICE_FREAD:
        return "Fread";
    case MMIX_SEMIHOSTING_SERVICE_FGETS:
        return "Fgets";
    case MMIX_SEMIHOSTING_SERVICE_FWRITE:
        return "Fwrite";
    case MMIX_SEMIHOSTING_SERVICE_FPUTS:
        return "Fputs";
    case MMIX_SEMIHOSTING_SERVICE_FSEEK:
        return "Fseek";
    case MMIX_SEMIHOSTING_SERVICE_FTELL:
        return "Ftell";
    default:
        return "unknown";
    }
}

static const char *mmix_semihosting_standard_handle_name(uint32_t handle)
{
    switch (handle) {
    case MMIX_SEMIHOSTING_HANDLE_STDIN:
        return "StdIn";
    case MMIX_SEMIHOSTING_HANDLE_STDOUT:
        return "StdOut";
    case MMIX_SEMIHOSTING_HANDLE_STDERR:
        return "StdErr";
    default:
        return NULL;
    }
}

static bool mmix_semihosting_is_standard_handle(uint32_t handle)
{
    return mmix_semihosting_standard_handle_name(handle) != NULL;
}

static bool mmix_semihosting_is_regular_file_handle(uint32_t handle)
{
    return handle >= MMIX_SEMIHOSTING_HANDLE_FIRST_FILE &&
           handle < MMIX_SEMIHOSTING_HANDLES;
}

static int mmix_semihosting_guestfd_from_slot(uint32_t slot)
{
    g_assert(slot != MMIX_SEMIHOSTING_GUESTFD_NONE);
    return slot - 1;
}

static uint32_t mmix_semihosting_guestfd_to_slot(int guestfd)
{
    g_assert(guestfd >= 0);
    return guestfd + 1;
}

static void mmix_semihosting_set_file_handle(CPUMMIXState *env,
                                             uint32_t handle,
                                             int guestfd, uint8_t mode)
{
    g_assert(mmix_semihosting_is_regular_file_handle(handle));

    if (guestfd < 0) {
        env->semihosting_file_guestfds[handle] = MMIX_SEMIHOSTING_GUESTFD_NONE;
        env->semihosting_file_modes[handle] = 0;
        return;
    }

    env->semihosting_file_guestfds[handle] =
        mmix_semihosting_guestfd_to_slot(guestfd);
    env->semihosting_file_modes[handle] = mode;
}

static bool mmix_semihosting_file_mode_flags(uint64_t mode, int *flags)
{
    /*
     * semihost_sys_open() takes GDB File-I/O open flags; the generic
     * semihosting layer converts them for the selected backend.
     */
    switch (mode) {
    case MMIX_SEMIHOSTING_MODE_TEXT_READ:
    case MMIX_SEMIHOSTING_MODE_BINARY_READ:
        *flags = GDB_O_RDONLY;
        return true;
    case MMIX_SEMIHOSTING_MODE_TEXT_WRITE:
    case MMIX_SEMIHOSTING_MODE_BINARY_WRITE:
        *flags = GDB_O_WRONLY | GDB_O_CREAT | GDB_O_TRUNC;
        return true;
    case MMIX_SEMIHOSTING_MODE_BINARY_READ_WRITE:
        *flags = GDB_O_RDWR | GDB_O_CREAT | GDB_O_TRUNC;
        return true;
    default:
        return false;
    }
}

static bool mmix_semihosting_mode_can_read(uint8_t mode)
{
    switch (mode) {
    case MMIX_SEMIHOSTING_MODE_TEXT_READ:
    case MMIX_SEMIHOSTING_MODE_BINARY_READ:
    case MMIX_SEMIHOSTING_MODE_BINARY_READ_WRITE:
        return true;
    default:
        return false;
    }
}

static bool mmix_semihosting_mode_can_write(uint8_t mode)
{
    switch (mode) {
    case MMIX_SEMIHOSTING_MODE_TEXT_WRITE:
    case MMIX_SEMIHOSTING_MODE_BINARY_WRITE:
    case MMIX_SEMIHOSTING_MODE_BINARY_READ_WRITE:
        return true;
    default:
        return false;
    }
}

static uint64_t mmix_semihosting_short_count(uint64_t done, uint64_t requested)
{
    return done - requested;
}

static uint64_t mmix_semihosting_io_failure(uint64_t requested)
{
    return MMIX_SEMIHOSTING_FAILURE - requested;
}

static bool mmix_semihosting_validate_regular_file_handle(
    CPUMMIXState *env, const MMIXSemihostingCall *call)
{
    const char *service_name = mmix_semihosting_service_name(call->service);
    const char *handle_name;

    if (mmix_semihosting_is_standard_handle(call->handle)) {
        handle_name = mmix_semihosting_standard_handle_name(call->handle);
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted %s unsupported standard handle %s at "
                      "0x%016" PRIx64 "\n",
                      service_name, handle_name, env->pc);
        return false;
    }

    if (!mmix_semihosting_is_regular_file_handle(call->handle)) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted %s invalid file handle %u at 0x%016"
                      PRIx64 "\n",
                      service_name, call->handle, env->pc);
        return false;
    }

    return true;
}

static void mmix_semihosting_fail_bad_file_handle(
    CPUMMIXState *env, const MMIXSemihostingCall *call)
{
    mmix_semihosting_validate_regular_file_handle(env, call);
    mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
}

static bool mmix_semihosting_file_handle_guestfd(CPUMMIXState *env,
                                                 const MMIXSemihostingCall *call,
                                                 int *guestfd)
{
    const char *service_name = mmix_semihosting_service_name(call->service);
    uint32_t slot;

    if (!mmix_semihosting_validate_regular_file_handle(env, call)) {
        return false;
    }

    slot = env->semihosting_file_guestfds[call->handle];
    if (slot == MMIX_SEMIHOSTING_GUESTFD_NONE) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted %s unopened file handle %u at 0x%016"
                      PRIx64 "\n",
                      service_name, call->handle, env->pc);
        return false;
    }

    *guestfd = mmix_semihosting_guestfd_from_slot(slot);
    return true;
}

static uint8_t mmix_semihosting_file_handle_mode(CPUMMIXState *env,
                                                 uint32_t handle)
{
    g_assert(mmix_semihosting_is_regular_file_handle(handle));
    return env->semihosting_file_modes[handle];
}

static void mmix_semihosting_close_complete(CPUState *cs, uint64_t ret,
                                            int err)
{
    CPUMMIXState *env = cpu_env(cs);

    if (ret == (uint64_t)-1) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted file close failed during handle release "
                      "at 0x%016" PRIx64 ": errno %d\n",
                      env->pc, err);
    }
}

static void mmix_semihosting_fclose_complete(CPUState *cs, uint64_t ret,
                                             int err)
{
    CPUMMIXState *env = cpu_env(cs);

    if (ret == (uint64_t)-1) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted Fclose failed at 0x%016" PRIx64
                      ": errno %d\n",
                      env->pc, err);
        mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
        return;
    }

    mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_SUCCESS);
}

static void mmix_semihosting_release_file_handle(CPUMMIXState *env,
                                                 uint32_t handle)
{
    int guestfd;

    g_assert(mmix_semihosting_is_regular_file_handle(handle));

    if (env->semihosting_file_guestfds[handle] ==
        MMIX_SEMIHOSTING_GUESTFD_NONE) {
        return;
    }

    guestfd = mmix_semihosting_guestfd_from_slot(
        env->semihosting_file_guestfds[handle]);
    mmix_semihosting_set_file_handle(env, handle, -1, 0);
    semihost_sys_close(env_cpu(env), mmix_semihosting_close_complete, guestfd);
}

void mmix_cpu_release_semihosting_file_handles(CPUMMIXState *env)
{
    uint32_t handle;

    for (handle = MMIX_SEMIHOSTING_HANDLE_FIRST_FILE;
         handle < MMIX_SEMIHOSTING_HANDLES; handle++) {
        mmix_semihosting_release_file_handle(env, handle);
    }
}

static void mmix_semihosting_fopen_complete(CPUState *cs, uint64_t ret,
                                            int err)
{
    CPUMMIXState *env = cpu_env(cs);
    uint8_t handle = env->semihosting_pending_open_handle;
    uint8_t mode = env->semihosting_pending_open_mode;

    env->semihosting_pending_open_handle = 0;
    env->semihosting_pending_open_mode = 0;

    if (ret == (uint64_t)-1) {
        if (err != 0) {
            qemu_log_mask(LOG_UNIMP,
                          "MMIX hosted Fopen failed for handle %u at 0x%016"
                          PRIx64 ": errno %d\n",
                          handle, env->pc, err);
        }
        mmix_semihosting_set_file_handle(env, handle, -1, 0);
        mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
        return;
    }

    mmix_semihosting_set_file_handle(env, handle, ret, mode);
    mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_SUCCESS);
}

static void mmix_semihosting_io_complete(CPUState *cs, uint64_t ret, int err)
{
    CPUMMIXState *env = cpu_env(cs);
    uint64_t requested = env->semihosting_pending_io_length;

    env->semihosting_pending_io_length = 0;

    if (ret == (uint64_t)-1 || err != 0) {
        mmix_cpu_write_reg(env, 255,
                           mmix_semihosting_io_failure(requested));
        return;
    }

    if (ret > requested) {
        ret = requested;
    }
    mmix_cpu_write_reg(env, 255,
                       mmix_semihosting_short_count(ret, requested));
}

static void mmix_semihosting_fseek_complete(CPUState *cs, uint64_t ret,
                                            int err)
{
    CPUMMIXState *env = cpu_env(cs);

    if (ret == (uint64_t)-1 || err != 0) {
        mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
        return;
    }

    mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_SUCCESS);
}

static void mmix_semihosting_ftell_complete(CPUState *cs, uint64_t ret,
                                            int err)
{
    CPUMMIXState *env = cpu_env(cs);

    if (ret == (uint64_t)-1 || err != 0) {
        mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
        return;
    }

    mmix_cpu_write_reg(env, 255, ret);
}

static G_NORETURN void
mmix_semihosting_raise_disabled(CPUMMIXState *env,
                                const MMIXSemihostingCall *call)
{
    qemu_log_mask(LOG_UNIMP,
                  "MMIX semihosting disabled for hosted TRAP service %u "
                  "handle %u at 0x%016" PRIx64 "\n",
                  call->service, call->handle, env->pc);
    helper_raise_illegal_instruction(env);
}

static G_NORETURN void
mmix_semihosting_raise_fputs_bad_handle(CPUMMIXState *env,
                                        const MMIXSemihostingCall *call)
{
    qemu_log_mask(LOG_UNIMP,
                  "MMIX hosted Fputs unsupported handle %u at 0x%016"
                  PRIx64 "\n",
                  call->handle, env->pc);
    helper_raise_illegal_instruction(env);
}

static G_NORETURN void
mmix_semihosting_raise_unsupported(CPUMMIXState *env,
                                   const MMIXSemihostingCall *call)
{
    qemu_log_mask(LOG_UNIMP,
                  "MMIX unsupported hosted TRAP service %u handle %u at "
                  "0x%016" PRIx64 "\n",
                  call->service, call->handle, env->pc);
    helper_raise_illegal_instruction(env);
}

static bool mmix_semihosting_translate_byte(CPUMMIXState *env,
                                            uint64_t address,
                                            MMUAccessType access,
                                            const char *service_name,
                                            const char *operand_name,
                                            hwaddr *physical)
{
    MMIXAddressTranslation translation;

    if (!mmix_translate_address(env, address, access, true, false,
                                &translation)) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted %s invalid %s address 0x%016" PRIx64 "\n",
                      service_name, operand_name, address);
        return false;
    }

    *physical = translation.physical;
    return true;
}

static bool mmix_semihosting_read_byte(CPUMMIXState *env, uint64_t address,
                                       const char *service_name,
                                       const char *operand_name,
                                       uint8_t *byte)
{
    CPUState *cs = env_cpu(env);
    MemTxResult result;
    hwaddr physical;

    if (!mmix_semihosting_translate_byte(env, address, MMU_DATA_LOAD,
                                         service_name, operand_name,
                                         &physical)) {
        return false;
    }

    *byte = address_space_ldub(cs->as, physical,
                               MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted %s could not read %s address 0x%016"
                      PRIx64 "\n",
                      service_name, operand_name, address);
        return false;
    }
    return true;
}

static bool mmix_semihosting_read_octa(CPUMMIXState *env, uint64_t address,
                                       const char *service_name,
                                       const char *operand_name,
                                       uint64_t *value)
{
    uint8_t byte;
    uint64_t current;
    uint64_t result = 0;
    size_t i;

    for (i = 0; i < 8; i++) {
        current = address + i;
        if (current < address) {
            qemu_log_mask(LOG_UNIMP,
                          "MMIX hosted %s invalid %s address 0x%016" PRIx64
                          "\n",
                          service_name, operand_name, current);
            return false;
        }
        if (!mmix_semihosting_read_byte(env, current, service_name,
                                        operand_name, &byte)) {
            return false;
        }
        result = (result << 8) | byte;
    }

    *value = result;
    return true;
}

static bool mmix_semihosting_read_cstring(CPUMMIXState *env, uint64_t address,
                                          const char *service_name,
                                          const char *operand_name,
                                          GByteArray *bytes)
{
    uint8_t byte;
    uint64_t current;
    size_t i;

    for (i = 0; i < MMIX_SEMIHOSTING_STRING_MAX; i++) {
        current = address + i;
        if (current < address) {
            qemu_log_mask(LOG_UNIMP,
                          "MMIX hosted %s invalid %s address 0x%016" PRIx64
                          "\n",
                          service_name, operand_name, current);
            return false;
        }
        if (!mmix_semihosting_read_byte(env, current, service_name,
                                        operand_name, &byte)) {
            return false;
        }
        if (byte == 0) {
            return true;
        }
        g_byte_array_append(bytes, &byte, 1);
    }

    qemu_log_mask(LOG_UNIMP,
                  "MMIX hosted %s %s at 0x%016" PRIx64
                  " exceeds %u bytes without NUL\n",
                  service_name, operand_name, address,
                  MMIX_SEMIHOSTING_STRING_MAX);
    return false;
}

static bool mmix_semihosting_read_args2(CPUMMIXState *env,
                                        const MMIXSemihostingCall *call,
                                        MMIXSemihostingArgs2 *args)
{
    (void)call;

    args->arg2 = mmix_cpu_read_reg(env, 255);
    return true;
}

static bool mmix_semihosting_read_args3(CPUMMIXState *env,
                                        const MMIXSemihostingCall *call,
                                        MMIXSemihostingArgs3 *args)
{
    uint64_t arg_block = mmix_cpu_read_reg(env, 255);
    const char *service_name = mmix_semihosting_service_name(call->service);

    if (arg_block + 8 < arg_block) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted %s invalid argument block address 0x%016"
                      PRIx64 "\n",
                      service_name, arg_block + 8);
        return false;
    }
    if (!mmix_semihosting_read_octa(env, arg_block, service_name,
                                    "argument block", &args->arg2)) {
        return false;
    }
    if (!mmix_semihosting_read_octa(env, arg_block + 8, service_name,
                                    "argument block", &args->arg3)) {
        return false;
    }
    return true;
}

static bool mmix_semihosting_check_counted_buffer(CPUMMIXState *env,
                                                  uint64_t address,
                                                  uint64_t length,
                                                  MMUAccessType access,
                                                  const char *service_name,
                                                  const char *operand_name)
{
    uint64_t current;
    hwaddr physical;
    size_t i;

    if (length > MMIX_SEMIHOSTING_BUFFER_MAX) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted %s %s length %" PRIu64
                      " exceeds %u bytes\n",
                      service_name, operand_name, length,
                      (unsigned)MMIX_SEMIHOSTING_BUFFER_MAX);
        return false;
    }

    for (i = 0; i < length; i++) {
        current = address + i;
        if (current < address) {
            qemu_log_mask(LOG_UNIMP,
                          "MMIX hosted %s invalid %s address 0x%016" PRIx64
                          "\n",
                          service_name, operand_name, current);
            return false;
        }
        if (!mmix_semihosting_translate_byte(env, current, access,
                                             service_name, operand_name,
                                             &physical)) {
            return false;
        }
    }
    return true;
}

static bool mmix_semihosting_read_counted_buffer(CPUMMIXState *env,
                                                 uint64_t address,
                                                 uint64_t length,
                                                 const char *service_name,
                                                 const char *operand_name,
                                                 GByteArray *bytes)
{
    uint8_t byte;
    uint64_t current;
    size_t i;

    if (!mmix_semihosting_check_counted_buffer(env, address, length,
                                               MMU_DATA_LOAD, service_name,
                                               operand_name)) {
        return false;
    }

    g_byte_array_set_size(bytes, 0);
    for (i = 0; i < length; i++) {
        current = address + i;
        if (!mmix_semihosting_read_byte(env, current, service_name,
                                        operand_name, &byte)) {
            return false;
        }
        g_byte_array_append(bytes, &byte, 1);
    }
    return true;
}

static bool mmix_semihosting_write_byte(CPUMMIXState *env, uint64_t address,
                                        const char *service_name,
                                        const char *operand_name,
                                        uint8_t byte)
{
    CPUState *cs = env_cpu(env);
    MemTxResult result;
    hwaddr physical;

    if (!mmix_semihosting_translate_byte(env, address, MMU_DATA_STORE,
                                         service_name, operand_name,
                                         &physical)) {
        return false;
    }

    address_space_stb(cs->as, physical, byte, MEMTXATTRS_UNSPECIFIED,
                      &result);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted %s could not write %s address 0x%016"
                      PRIx64 "\n",
                      service_name, operand_name, address);
        return false;
    }
    return true;
}

static const char *mmix_semihosting_console_name(uint32_t handle)
{
    switch (handle) {
    case MMIX_SEMIHOSTING_HANDLE_STDOUT:
        return "StdOut";
    case MMIX_SEMIHOSTING_HANDLE_STDERR:
        return "StdErr";
    default:
        g_assert_not_reached();
    }
}

static bool mmix_semihosting_write_console(CPUMMIXState *env,
                                           const char *service_name,
                                           uint32_t handle,
                                           const GByteArray *bytes)
{
    CPUState *cs = env_cpu(env);
    MemTxResult result;
    size_t i;

    for (i = 0; i < bytes->len; i++) {
        address_space_stb(cs->as, MMIX_SEMIHOSTING_CONSOLE_TX, bytes->data[i],
                          MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_UNIMP,
                          "MMIX hosted %s could not write %s at "
                          "0x%016" PRIx64 "\n",
                          service_name,
                          mmix_semihosting_console_name(handle), env->pc);
            return false;
        }
    }

    return true;
}

static void mmix_semihosting_fputs_console(CPUMMIXState *env,
                                           const MMIXSemihostingCall *call)
{
    GByteArray *bytes;
    uint64_t address;

    if (!semihosting_enabled(false)) {
        mmix_semihosting_raise_disabled(env, call);
    }

    bytes = g_byte_array_new();
    address = mmix_cpu_read_reg(env, 255);

    if (!mmix_semihosting_read_cstring(env, address, "Fputs", "string",
                                       bytes)) {
        g_byte_array_free(bytes, true);
        helper_raise_illegal_instruction(env);
    }

    if (!mmix_semihosting_write_console(env, "Fputs", call->handle, bytes)) {
        g_byte_array_free(bytes, true);
        helper_raise_illegal_instruction(env);
    }

    mmix_cpu_write_reg(env, 255, bytes->len);
    g_byte_array_free(bytes, true);
}

static void mmix_semihosting_fopen(CPUMMIXState *env,
                                   const MMIXSemihostingCall *call)
{
    MMIXSemihostingArgs3 args;
    GByteArray *bytes = g_byte_array_new();
    const char *service_name = mmix_semihosting_service_name(call->service);
    int flags;

    if (!mmix_semihosting_validate_regular_file_handle(env, call)) {
        g_byte_array_free(bytes, true);
        mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
        return;
    }
    if (!mmix_semihosting_read_args3(env, call, &args) ||
        !mmix_semihosting_read_cstring(env, args.arg2, service_name,
                                       "pathname", bytes)) {
        g_byte_array_free(bytes, true);
        helper_raise_illegal_instruction(env);
    }
    if (!mmix_semihosting_file_mode_flags(args.arg3, &flags)) {
        qemu_log_mask(LOG_UNIMP,
                      "MMIX hosted Fopen unsupported mode %" PRIu64
                      " for handle %u at 0x%016" PRIx64 "\n",
                      args.arg3, call->handle, env->pc);
        g_byte_array_free(bytes, true);
        mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
        return;
    }

    mmix_semihosting_release_file_handle(env, call->handle);
    env->semihosting_pending_open_handle = call->handle;
    env->semihosting_pending_open_mode = args.arg3;
    semihost_sys_open(env_cpu(env), mmix_semihosting_fopen_complete,
                      args.arg2, bytes->len + 1, flags, 0644);
    g_byte_array_free(bytes, true);
}

static void mmix_semihosting_fclose(CPUMMIXState *env,
                                    const MMIXSemihostingCall *call)
{
    int guestfd;

    if (!mmix_semihosting_file_handle_guestfd(env, call, &guestfd)) {
        mmix_semihosting_fail_bad_file_handle(env, call);
        return;
    }

    mmix_semihosting_set_file_handle(env, call->handle, -1, 0);
    semihost_sys_close(env_cpu(env), mmix_semihosting_fclose_complete,
                       guestfd);
}

static void mmix_semihosting_fread(CPUMMIXState *env,
                                   const MMIXSemihostingCall *call)
{
    MMIXSemihostingArgs3 args;
    const char *service_name = mmix_semihosting_service_name(call->service);
    int guestfd;

    if (!mmix_semihosting_read_args3(env, call, &args)) {
        helper_raise_illegal_instruction(env);
    }
    if (call->handle == MMIX_SEMIHOSTING_HANDLE_STDIN) {
        if (!mmix_semihosting_check_counted_buffer(env, args.arg2, args.arg3,
                                                   MMU_DATA_STORE,
                                                   service_name, "buffer")) {
            mmix_cpu_write_reg(env, 255,
                               mmix_semihosting_io_failure(args.arg3));
            return;
        }

        env->semihosting_pending_io_length = args.arg3;
        BQL_LOCK_GUARD();
        semihost_sys_read(env_cpu(env), mmix_semihosting_io_complete,
                          MMIX_SEMIHOSTING_HANDLE_STDIN, args.arg2,
                          args.arg3);
        return;
    }
    if (!mmix_semihosting_file_handle_guestfd(env, call, &guestfd) ||
        !mmix_semihosting_mode_can_read(
            mmix_semihosting_file_handle_mode(env, call->handle)) ||
        !mmix_semihosting_check_counted_buffer(env, args.arg2, args.arg3,
                                               MMU_DATA_STORE, service_name,
                                               "buffer")) {
        mmix_cpu_write_reg(env, 255,
                           mmix_semihosting_io_failure(args.arg3));
        return;
    }

    env->semihosting_pending_io_length = args.arg3;
    semihost_sys_read(env_cpu(env), mmix_semihosting_io_complete,
                      guestfd, args.arg2, args.arg3);
}

static bool mmix_semihosting_read_guestfd_byte(CPUMMIXState *env,
                                               int guestfd, bool need_bql,
                                               uint64_t address,
                                               uint8_t *byte)
{
    env->semihosting_pending_io_length = 1;
    if (need_bql) {
        BQL_LOCK_GUARD();
        semihost_sys_read(env_cpu(env), mmix_semihosting_io_complete,
                          guestfd, address, 1);
    } else {
        semihost_sys_read(env_cpu(env), mmix_semihosting_io_complete,
                          guestfd, address, 1);
    }

    if (mmix_cpu_read_reg(env, 255) != 0) {
        return false;
    }
    return mmix_semihosting_read_byte(env, address, "Fgets", "buffer", byte);
}

static bool mmix_semihosting_read_handle_guestfd(
    CPUMMIXState *env, const MMIXSemihostingCall *call,
    int *guestfd, bool *need_bql)
{
    if (call->handle == MMIX_SEMIHOSTING_HANDLE_STDIN) {
        *guestfd = MMIX_SEMIHOSTING_HANDLE_STDIN;
        *need_bql = true;
        return true;
    }

    *need_bql = false;
    return mmix_semihosting_file_handle_guestfd(env, call, guestfd) &&
           mmix_semihosting_mode_can_read(
               mmix_semihosting_file_handle_mode(env, call->handle));
}

static void mmix_semihosting_fgets(CPUMMIXState *env,
                                   const MMIXSemihostingCall *call)
{
    MMIXSemihostingArgs3 args;
    const char *service_name = mmix_semihosting_service_name(call->service);
    int guestfd;
    bool need_bql;
    uint64_t count;
    uint8_t byte;

    if (!mmix_semihosting_read_args3(env, call, &args)) {
        helper_raise_illegal_instruction(env);
    }
    if (args.arg3 == 0 ||
        !mmix_semihosting_read_handle_guestfd(env, call, &guestfd,
                                              &need_bql) ||
        !mmix_semihosting_check_counted_buffer(env, args.arg2, args.arg3,
                                               MMU_DATA_STORE, service_name,
                                               "buffer")) {
        mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
        return;
    }

    for (count = 0; count + 1 < args.arg3; count++) {
        if (!mmix_semihosting_read_guestfd_byte(env, guestfd, need_bql,
                                                args.arg2 + count, &byte)) {
            mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
            return;
        }
        if (byte == '\n') {
            count++;
            break;
        }
    }

    if (!mmix_semihosting_write_byte(env, args.arg2 + count, service_name,
                                     "buffer", 0)) {
        mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
        return;
    }

    mmix_cpu_write_reg(env, 255, count);
}

static void mmix_semihosting_fwrite(CPUMMIXState *env,
                                    const MMIXSemihostingCall *call)
{
    MMIXSemihostingArgs3 args;
    GByteArray *bytes;
    const char *service_name = mmix_semihosting_service_name(call->service);
    int guestfd;

    bytes = g_byte_array_new();
    if (!mmix_semihosting_read_args3(env, call, &args)) {
        g_byte_array_free(bytes, true);
        helper_raise_illegal_instruction(env);
    }
    if (call->handle == MMIX_SEMIHOSTING_HANDLE_STDIN) {
        mmix_semihosting_validate_regular_file_handle(env, call);
        g_byte_array_free(bytes, true);
        mmix_cpu_write_reg(env, 255, mmix_semihosting_io_failure(args.arg3));
        return;
    }
    if (call->handle == MMIX_SEMIHOSTING_HANDLE_STDOUT ||
        call->handle == MMIX_SEMIHOSTING_HANDLE_STDERR) {
        if (!mmix_semihosting_read_counted_buffer(env, args.arg2, args.arg3,
                                                  service_name, "buffer",
                                                  bytes) ||
            !mmix_semihosting_write_console(env, service_name, call->handle,
                                            bytes)) {
            g_byte_array_free(bytes, true);
            mmix_cpu_write_reg(env, 255,
                               mmix_semihosting_io_failure(args.arg3));
            return;
        }
        g_byte_array_free(bytes, true);
        mmix_cpu_write_reg(env, 255,
                           mmix_semihosting_short_count(args.arg3,
                                                        args.arg3));
        return;
    }
    if (!mmix_semihosting_file_handle_guestfd(env, call, &guestfd) ||
        !mmix_semihosting_mode_can_write(
            mmix_semihosting_file_handle_mode(env, call->handle)) ||
        !mmix_semihosting_check_counted_buffer(env, args.arg2, args.arg3,
                                               MMU_DATA_LOAD, service_name,
                                               "buffer")) {
        g_byte_array_free(bytes, true);
        mmix_cpu_write_reg(env, 255, mmix_semihosting_io_failure(args.arg3));
        return;
    }
    g_byte_array_free(bytes, true);

    env->semihosting_pending_io_length = args.arg3;
    semihost_sys_write(env_cpu(env), mmix_semihosting_io_complete,
                       guestfd, args.arg2, args.arg3);
}

static void mmix_semihosting_fseek(CPUMMIXState *env,
                                   const MMIXSemihostingCall *call)
{
    MMIXSemihostingArgs2 args;
    int64_t offset;
    int whence;
    int guestfd;

    if (!mmix_semihosting_read_args2(env, call, &args) ||
        !mmix_semihosting_file_handle_guestfd(env, call, &guestfd)) {
        mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
        return;
    }

    offset = args.arg2;
    if (offset < 0) {
        /*
         * MMIXware uses -1 for end-of-file, -2 for one byte before EOF, etc.
         */
        offset++;
        whence = GDB_SEEK_END;
    } else {
        whence = GDB_SEEK_SET;
    }

    semihost_sys_lseek(env_cpu(env), mmix_semihosting_fseek_complete,
                       guestfd, offset, whence);
}

static void mmix_semihosting_ftell(CPUMMIXState *env,
                                   const MMIXSemihostingCall *call)
{
    int guestfd;

    if (!mmix_semihosting_file_handle_guestfd(env, call, &guestfd)) {
        mmix_cpu_write_reg(env, 255, MMIX_SEMIHOSTING_FAILURE);
        return;
    }

    semihost_sys_lseek(env_cpu(env), mmix_semihosting_ftell_complete,
                       guestfd, 0, GDB_SEEK_CUR);
}

static void mmix_semihosting_file_service(CPUMMIXState *env,
                                          const MMIXSemihostingCall *call)
{
    if (!semihosting_enabled(false)) {
        mmix_semihosting_raise_disabled(env, call);
    }

    switch (call->action) {
    case MMIX_SEMIHOSTING_ACTION_FOPEN:
        mmix_semihosting_fopen(env, call);
        return;
    case MMIX_SEMIHOSTING_ACTION_FCLOSE:
        mmix_semihosting_fclose(env, call);
        return;
    case MMIX_SEMIHOSTING_ACTION_FTELL:
        mmix_semihosting_ftell(env, call);
        return;
    case MMIX_SEMIHOSTING_ACTION_FREAD:
        mmix_semihosting_fread(env, call);
        return;
    case MMIX_SEMIHOSTING_ACTION_FGETS:
        mmix_semihosting_fgets(env, call);
        return;
    case MMIX_SEMIHOSTING_ACTION_FWRITE:
        mmix_semihosting_fwrite(env, call);
        return;
    case MMIX_SEMIHOSTING_ACTION_FSEEK:
        mmix_semihosting_fseek(env, call);
        return;
    default:
        g_assert_not_reached();
    }
}

void helper_mmix_semihosting_trap(CPUMMIXState *env, uint32_t service,
                                  uint32_t handle)
{
    MMIXSemihostingCall call = mmix_semihosting_decode_call(service, handle);

    switch (call.action) {
    case MMIX_SEMIHOSTING_ACTION_HALT:
        mmix_semihosting_halt(env);
        break;
    case MMIX_SEMIHOSTING_ACTION_FOPEN:
    case MMIX_SEMIHOSTING_ACTION_FCLOSE:
    case MMIX_SEMIHOSTING_ACTION_FREAD:
    case MMIX_SEMIHOSTING_ACTION_FGETS:
    case MMIX_SEMIHOSTING_ACTION_FWRITE:
    case MMIX_SEMIHOSTING_ACTION_FSEEK:
    case MMIX_SEMIHOSTING_ACTION_FTELL:
        mmix_semihosting_file_service(env, &call);
        return;
    case MMIX_SEMIHOSTING_ACTION_FPUTS_CONSOLE:
        mmix_semihosting_fputs_console(env, &call);
        return;
    case MMIX_SEMIHOSTING_ACTION_FPUTS_BAD_HANDLE:
        mmix_semihosting_raise_fputs_bad_handle(env, &call);
    case MMIX_SEMIHOSTING_ACTION_UNSUPPORTED:
        mmix_semihosting_raise_unsupported(env, &call);
    default:
        g_assert_not_reached();
    }
}

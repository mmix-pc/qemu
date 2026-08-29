mmix_ss = ss.source_set()
mmix_ss.add(when: 'CONFIG_MMIX_VIRT', if_true: files(
  'boot-payload.c',
  'boot-plan.c',
  'elf-loader.c',
  'fdt-builder.c',
  'fdt-validator.c',
  'framebuffer.c',
  'intc.c',
  'ipi.c',
  'kernel-loader.c',
  'mmo-hosted-plan.c',
  'mmo-hosted-state.c',
  'mmo-hosted-vmstate.c',
  'mmo-loader.c',
  'physical-layout.c',
  'ram-reservation.c',
  'sparse-memory.c',
  'timer.c',
  'virt.c',
))

hw_arch += {'mmix': mmix_ss}

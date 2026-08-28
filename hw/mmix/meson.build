mmix_ss = ss.source_set()
mmix_ss.add(when: 'CONFIG_MMIX_VIRT', if_true: files(
  'boot-plan.c',
  'elf-loader.c',
  'framebuffer.c',
  'intc.c',
  'ipi.c',
  'kernel-loader.c',
  'mmo-loader.c',
  'physical-layout.c',
  'ram-reservation.c',
  'timer.c',
  'virt.c',
))

hw_arch += {'mmix': mmix_ss}

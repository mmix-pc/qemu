mmix_ss = ss.source_set()
mmix_ss.add(when: 'CONFIG_MMIX_VIRT', if_true: files(
  'intc.c',
  'ipi.c',
  'physical-layout.c',
  'ram-reservation.c',
  'timer.c',
  'virt.c',
))

hw_arch += {'mmix': mmix_ss}

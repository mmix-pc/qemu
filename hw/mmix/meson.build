mmix_ss = ss.source_set()
mmix_ss.add(when: 'CONFIG_MMIX_VIRT', if_true: files(
  'physical-layout.c',
  'virt.c',
))

hw_arch += {'mmix': mmix_ss}

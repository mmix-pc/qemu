mmix_ss = ss.source_set()
mmix_ss.add(when: 'CONFIG_MMIX_VIRT', if_true: files(
  'elf-loader.c',
  'kernel-loader.c',
  'mmo-loader.c',
  'virt.c',
))

hw_arch += {'mmix': mmix_ss}

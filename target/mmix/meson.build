mmix_ss = ss.source_set()
mmix_ss.add(files(
  'cpu.c',
  'helper.c',
  'translate.c',
))

mmix_common_system_ss = ss.source_set()
mmix_common_system_ss.add(files('gdbstub.c'))

target_arch += {'mmix': mmix_ss}
target_common_system_arch += {'mmix': mmix_common_system_ss}

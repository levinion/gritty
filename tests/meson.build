test_sources = files(
  'test_main.cpp',
  'test_util.cpp',
  'test_grid.cpp',
  'test_vt.cpp',
  'test_vt_foot.cpp',
  'test_unicode_width.cpp',
  'test_osc_colors.cpp',
  'test_palette.cpp',
  'test_memory.cpp',
  'test_cache_memory.cpp',
  'test_renderer.cpp',
  'test_alpha.cpp',
  'test_platform.cpp',
  'test_input_foot.cpp',
  'test_session.cpp',
  'test_config.cpp',
  'test_kitty_graphics.cpp',
  'test_sixel.cpp',
  'test_selection.cpp',
)

if host_os == 'darwin'
  test_sources += files('test_mac_input.mm')
endif

test_exe = executable('gritty_tests',
  test_sources,
  dependencies: [gritty_core_dep, doctest_dep],
)

test('unit_tests', test_exe, suite: 'core')

# Explicit memory workloads; run manually, do not gate RSS across machines.
memory_probe = executable('gritty_memory_probe', 'memory_probe.cpp', dependencies: gritty_core_dep)

# esctest2 transport; no fonts, renderer or native window required.
protocol_driver = executable('gritty_protocol_driver',
  files('terminal_driver.cpp'), dependencies: gritty_core_dep,
)

if host_os == 'darwin' and get_option('gui_tests')
  cocoa_args = ['-DGRITTY_TEST_SOURCE_DIR="' + meson.project_source_root() + '"']
  presentation_probe = executable('gritty_presentation_probe', 'presentation_probe.mm',
    dependencies: gritty_core_dep, objcpp_args: cocoa_args,
  )
  perf_probe = executable('gritty_perf_probe', 'perf_probe.mm',
    dependencies: gritty_core_dep, objcpp_args: cocoa_args,
  )
  cocoa_test_exe = executable('gritty_cocoa_tests',
    files('test_main.cpp', 'test_cocoa_surface.mm', 'test_cocoa_resize.mm', 'test_iosurface_pool.mm'),
    dependencies: [gritty_core_dep, doctest_dep],
    cpp_args: cocoa_args,
    objcpp_args: cocoa_args,
  )
  test('cocoa_surface', cocoa_test_exe, suite: 'gui', is_parallel: false)
endif

# Exercise a real tmux server on a private socket, not only synthetic VT streams.
tmux_program = find_program('tmux', required: true)
tmux_test_exe = executable('gritty_tmux_tests',
  files('test_main.cpp', 'test_tmux.cpp'),
  dependencies: [gritty_core_dep, doctest_dep],
)
test('tmux_integration', tmux_test_exe,
  suite: 'integration',
  env: ['GRITTY_TMUX=' + tmux_program.full_path()],
  timeout: 60,
)

test_sources = files(
  'test_main.cpp',
  'test_util.cpp',
  'test_grid.cpp',
  'test_vt.cpp',
  'test_vt_foot.cpp',
  'test_unicode_width.cpp',
  'test_osc_colors.cpp',
  'test_cache_memory.cpp',
  'test_renderer.cpp',
  'test_alpha.cpp',
  'test_platform.cpp',
  'test_input_foot.cpp',
  'test_session.cpp',
  'test_config.cpp',
  'test_cli.cpp',
  'test_kitty_graphics.cpp',
)

if host_os == 'darwin'
  test_sources += files('test_mac_input.mm', 'test_iosurface_pool.mm')
endif

test_exe = executable('gritty_tests',
  test_sources,
  dependencies: [gritty_core_dep, doctest_dep],
)

test('unit_tests', test_exe, suite: 'core')

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

test_sources = files(
  'test_main.cpp',
  'test_util.cpp',
  'test_grid.cpp',
  'test_vt.cpp',
  'test_vt_foot.cpp',
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

test('unit_tests', test_exe)

if host_os == 'darwin' and get_option('gui_tests')
  cocoa_test_exe = executable('gritty_cocoa_tests',
    files('test_main.cpp', 'test_cocoa_surface.mm'),
    dependencies: [gritty_core_dep, doctest_dep],
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
  env: ['GRITTY_TMUX=' + tmux_program.full_path()],
  timeout: 60,
)

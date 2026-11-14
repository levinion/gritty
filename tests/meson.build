test_sources = files(
  'test_main.cpp',
  'test_util.cpp',
  'test_grid.cpp',
  'test_vt.cpp',
  'test_renderer.cpp',
  'test_platform.cpp',
  'test_config.cpp',
  'test_kitty_graphics.cpp',
  'test_sixel.cpp',
  'test_selection.cpp',
)

test_exe = executable('gritty_tests',
  test_sources,
  dependencies: [gritty_core_dep, doctest_dep],
)

test('unit_tests', test_exe)

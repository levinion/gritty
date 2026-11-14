platform_sources = files(
  'platform/posix_pty.cpp',
)

if host_os == 'darwin'
  platform_sources += files(
    'platform/macos/ct_discovery.mm',
    'platform/macos/mac_input.mm',
    'platform/macos/kqueue_loop.mm',
    'platform/macos/cocoa_surface.mm',
    'platform/macos/clipboard.mm',
  )
endif

core_sources = files(
  'config/app_config.cpp',
  'config/toml_loader.cpp',
  'core/screen_grid.cpp',
  'core/terminal_engine.cpp',
  'core/vt_parser.cpp',
  'core/terminal_session.cpp',
  'core/kitty_graphics.cpp',
  'core/sixel_decoder.cpp',
  'core/selection.cpp',
  'core/overlays.cpp',
  'render/font_engine.cpp',
  'render/box_drawing.cpp',
  'render/cpu_renderer.cpp',
  'input/input_mapper.cpp',
) + platform_sources

squirt_core_lib = static_library('squirt_core',
  core_sources,
  include_directories: inc_dir,
  dependencies: [common_deps, platform_deps],
)

squirt_core_dep = declare_dependency(
  link_with: squirt_core_lib,
  include_directories: inc_dir,
  dependencies: [common_deps, platform_deps],
)

squirt_sources = files(
  'main.cpp',
)

squirt_exe = executable('squirt',
  squirt_sources,
  dependencies: [squirt_core_dep],
  install: true,
)

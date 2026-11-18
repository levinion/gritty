platform_sources = files(
  'platform/posix_pty.cpp',
)

if host_os == 'darwin'
  platform_sources += files(
    'platform/macos/ct_discovery.mm',
    'platform/macos/mac_input.mm',
    'platform/macos/kqueue_loop.mm',
    'platform/macos/cocoa_surface.mm',
    'platform/macos/cocoa_view.mm',
    'platform/macos/iosurface_pool.mm',
    'platform/macos/application_icon.mm',
    'platform/macos/clipboard.mm',
  )
endif

core_sources = files(
  'config/app_config.cpp',
  'config/toml_loader.cpp',
  'core/screen_grid.cpp',
  'core/combining_store.cpp',
  'core/cell_extra_store.cpp',
  'core/terminal_engine.cpp',
  'core/terminal_modes.cpp',
  'core/vt_parser.cpp',
  'core/vt_dispatch.cpp',
  'core/vt_sgr.cpp',
  'core/vt_strings.cpp',
  'core/terminal_session.cpp',
  'core/kitty_graphics.cpp',
  'core/sixel_decoder.cpp',
  'core/selection.cpp',
  'core/overlays.cpp',
  'render/font_engine.cpp',
  'render/glyph_cache.cpp',
  'render/box_drawing.cpp',
  'render/cpu_renderer.cpp',
  'input/input_mapper.cpp',
  'input/mouse_tracker.cpp',
) + platform_sources

gritty_core_lib = static_library('gritty_core',
  core_sources,
  include_directories: inc_dir,
  dependencies: [common_deps, platform_deps],
)

gritty_core_dep = declare_dependency(
  link_with: gritty_core_lib,
  include_directories: inc_dir,
  dependencies: [common_deps, platform_deps],
)

gritty_sources = files(
  'main.cpp',
)

gritty_exe = executable('gritty',
  gritty_sources,
  dependencies: [gritty_core_dep],
  install: true,
)

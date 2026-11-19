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
  'util/base64.cpp',
) + platform_sources

# Exceptions are treated as programming errors, not control flow. Tests keep
# exceptions enabled for doctest; TOML++ uses its error-result ABI everywhere.
no_exception_args = [
  '-fno-exceptions',
  '-DTOML_EXCEPTIONS=0',
]

# nixpkgs builds libtomlplusplus with exceptions; compile one noex
# implementation into a static library instead of linking its ex ABI or
# header-only-inlining the parser into every target.
no_toml_dylib_args = [
  '-UTOML_HEADER_ONLY',
  '-DTOML_HEADER_ONLY=0',
  '-UTOML_SHARED_LIB',
  '-DTOML_SHARED_LIB=0',
  '-DTOML_ENABLE_FORMATTERS=0',
]

toml_noex_lib = static_library('gritty_toml_noex',
  files('config/toml_noexcept.cpp'),
  include_directories: inc_dir,
  dependencies: [tomlplusplus_dep],
  cpp_args: no_exception_args + no_toml_dylib_args,
)

toml_noex_dep = declare_dependency(
  link_with: toml_noex_lib,
  include_directories: inc_dir,
  dependencies: [tomlplusplus_dep],
)

gritty_core_lib = static_library('gritty_core',
  core_sources,
  include_directories: inc_dir,
  dependencies: [common_deps, platform_deps, toml_noex_dep],
  cpp_args: no_exception_args + no_toml_dylib_args,
  objcpp_args: no_exception_args,
)

gritty_core_dep = declare_dependency(
  link_with: gritty_core_lib,
  include_directories: inc_dir,
  dependencies: [common_deps, platform_deps, toml_noex_dep],
)

gritty_sources = files(
  'main.cpp',
)

gritty_exe = executable('gritty',
  gritty_sources,
  dependencies: [gritty_core_dep],
  cpp_args: no_exception_args,
  install: true,
)

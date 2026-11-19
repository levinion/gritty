project('gritty', ['cpp', 'c'],
  version: '0.1.0',
  default_options: [
    'cpp_std=c++23',
    'c_std=c17',
    'warning_level=3',
    'b_ndebug=if-release',
  ]
)

# Single source of truth for the CLI, PTY environment and XTVERSION reply.
add_project_arguments(
  '-DGRITTY_VERSION="' + meson.project_version() + '"',
  language: 'cpp',
)

cpp = meson.get_compiler('cpp')
host_os = host_machine.system()

if host_os == 'linux'
  add_project_arguments('-DPLATFORM_LINUX=1', language: 'cpp')
elif host_os == 'darwin'
  # Meson reports aarch64 for native builds and arm64 for some Nix cross builds.
  if host_machine.cpu_family() != 'aarch64' and host_machine.cpu_family() != 'arm64'
    error('macOS requires Apple Silicon (arm64); Intel macOS is not supported')
  endif
  add_languages('objcpp', native: false)
  add_project_arguments('-DPLATFORM_MACOS=1', language: ['cpp', 'objcpp'])
else
  error('Unsupported platform: ' + host_os)
endif

# Common dependencies
pixman_dep = dependency('pixman-1', version: '>= 0.40.0')
freetype_dep = dependency('freetype2', version: '>= 24.0.0')
harfbuzz_dep = dependency('harfbuzz', version: '>= 5.0.0')
utf8proc_dep = dependency('libutf8proc', fallback: ['utf8proc', 'utf8proc_dep'], required: false)
if not utf8proc_dep.found()
  utf8proc_dep = dependency('utf8proc')
endif
png_dep = dependency('libpng')
zlib_dep = dependency('zlib')
threads_dep = dependency('threads')

common_deps = [
  pixman_dep,
  freetype_dep,
  harfbuzz_dep,
  utf8proc_dep,
  png_dep,
  zlib_dep,
  threads_dep,
]

platform_deps = []
platform_sources = []

if host_os == 'darwin'
  add_project_link_arguments('-Wl,-dead_strip', language: ['cpp', 'c', 'objcpp'])
  apple_frameworks = dependency('appleframeworks', modules: [
    'Cocoa',
    'CoreText',
    'CoreGraphics',
    'CoreFoundation',
    'QuartzCore',
    'ImageIO',
    'IOSurface',
    'UserNotifications',
  ])
  platform_deps += [apple_frameworks]
elif host_os == 'linux'
  add_project_link_arguments('-Wl,--gc-sections', language: ['cpp', 'c'])
  wayland_client_dep = dependency('wayland-client')
  wayland_cursor_dep = dependency('wayland-cursor')
  wayland_protocols_dep = dependency('wayland-protocols', version: '>= 1.25')
  xkbcommon_dep = dependency('xkbcommon')
  fontconfig_dep = dependency('fontconfig')
  platform_deps += [
    wayland_client_dep,
    wayland_cursor_dep,
    wayland_protocols_dep,
    xkbcommon_dep,
    fontconfig_dep,
  ]
endif

inc_dir = include_directories('src')

subdir('src')

if get_option('benchmarks')
  subdir('benchmark')
endif

if get_option('tests')
  doctest_dep = dependency('doctest', fallback: ['doctest', 'doctest_dep'])
  subdir('tests')
endif

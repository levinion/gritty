bench_sources = files(
  'micro/bench_main.cpp',
  'micro/bench_vt_parser.cpp',
  'micro/bench_renderer.cpp',
  'micro/bench_latency.cpp',
)

gritty_bench = executable('gritty-bench',
  bench_sources,
  include_directories: [inc_dir],
  dependencies: [gritty_core_dep],
  cpp_args: ['-DGRITTY_BENCH_SOURCE_DIR="' + meson.project_source_root() + '"'],
)

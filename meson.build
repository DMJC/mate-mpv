project('mate-mpv', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++17', 'warning_level=2'])

gtk_dep = dependency('gtk+-3.0')

executable(
  'mate-mpv',
  'src/main.cpp',
  dependencies: [gtk_dep],
  install: true
)

project('mate-mpv', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++17', 'warning_level=2'])

gtk_dep = dependency('gtk+-3.0')
mpv_dep = dependency('mpv')
epoxy_dep = dependency('epoxy')

i18n = import('i18n')

gtk_dep = dependency('gtk+-3.0')

icon_install_dir = join_paths(get_option('prefix'), get_option('datadir'), meson.project_name())

executable(
  'mate-mpv',
  'src/main.cpp',
  'src/langlist.cpp',
  dependencies: [gtk_dep, mpv_dep, epoxy_dep],
  install: true
)

install_subdir('data', install_dir: icon_install_dir)

install_data('mate-mpv.desktop',
  install_dir: join_paths(get_option('prefix'), get_option('datadir'), 'applications')
)

install_subdir('help',
  install_dir: join_paths(get_option('datadir'))
)

subdir('po')

project('mate-mpv', 'cpp',
  version: '1.0.0',
  meson_version: '>=0.56.0',
  default_options: [
    'cpp_std=c++17',
    'warning_level=2',
    'werror=false',
  ]
)

i18n = import('i18n')

# ---- Dependencies ----
gtk_dep  = dependency('gtkmm-3.0', required: true)
mpv_dep  = dependency('mpv', required: true)
epoxy_dep = dependency('epoxy', required: true)

deps = [gtk_dep, mpv_dep, epoxy_dep]

# ---- Paths ----
prefix  = get_option('prefix')
datadir = get_option('datadir')
localedir = get_option('localedir')

# App data install dir (icons, ui files, etc.)
app_datadir = join_paths(prefix, datadir, meson.project_name())

# ---- Build ----
executable('mate-mpv',
  'mate-mpv.cpp',
  dependencies: deps,
  cpp_args: [
    '-DICON_INSTALL_DIR="' + app_datadir + '"',
    '-DGETTEXT_PACKAGE="mate-mpv"',
    '-DLOCALEDIR="' + join_paths(prefix, localedir) + '"',
  ],
  install: true
)

# ---- Install data ----
install_subdir('data', install_dir: app_datadir)

install_data('mate-mpv.desktop',
  install_dir: join_paths(prefix, datadir, 'applications')
)

# Help should be under: /usr/share/help/<locale>/mate-mpv
# Put your Mallard docs in help/<locale>/mate-mpv/...
install_subdir('help',
  install_dir: join_paths(prefix, datadir, 'help')
)

# Enable translations when you have po/ set up
# subdir('po')

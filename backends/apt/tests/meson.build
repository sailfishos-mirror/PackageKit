
apt_test_data_dir = meson.current_source_dir() / 'testdata'

apt_tests_exe = executable(
  'apt-tests',
  'apt-tests.cpp',
  'definitions.cpp',
  include_directories: [
    packagekit_src_include,
  ],
  dependencies: [
    packagekit_glib2_dep,
    packagekit_backend_apt_dep,
    gstreamer_dep,
    apt_pkg_dep,
  ],
  build_by_default: true,
  install: false,
)

test(
  'apt-backend-tests',
  apt_tests_exe,
  args: [apt_test_data_dir],
)

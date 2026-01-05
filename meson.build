project('deepin_remote_desktop', 'c',
        version: '0.1.0',
        default_options: [
          'c_std=c17',
          'warning_level=3',
          'werror=true'
        ])

add_project_arguments([
  '-DG_LOG_USE_STRUCTURED=1',
  '-DG_LOG_DOMAIN="drd"',
  '-D_GNU_SOURCE',
  '-Wno-deprecated-declarations',
  '-Wno-error=pedantic',
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-unused-parameter'
], language: 'c')

cc = meson.get_compiler('c')
pam_dep = dependency('pam', required: false)
if not pam_dep.found()
  pam_dep = cc.find_library('pam', required: true)
endif

glib_dep = dependency('glib-2.0', version: '>=2.64')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
gobject_dep = dependency('gobject-2.0')
freerdp_server_dep = dependency('freerdp-server3', required: true)
freerdp_core_dep = dependency('freerdp3', required: true)
winpr_dep = dependency('winpr3', required: true)
x11_dep = dependency('x11', required: true)
xext_dep = dependency('xext', required: true)
xdamage_dep = dependency('xdamage', required: true)
xfixes_dep = dependency('xfixes', required: true)
xtst_dep = dependency('xtst', required: true)

subdir('src')
subdir('data')

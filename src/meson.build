src_inc = include_directories('.')
gnome = import('gnome')

deps = [
  glib_dep,
  gio_dep,
  gio_unix_dep,
  gobject_dep,
  freerdp_server_dep,
  freerdp_core_dep,
  winpr_dep,
  x11_dep,
  xext_dep,
  xdamage_dep,
  xfixes_dep,
  xtst_dep,
  pam_dep,
  avcodec_dep,
  avutil_dep,
  swscale_dep,
  vaapi_dep
]

media_sources = files(
  'capture/drd_capture_manager.c',
  'capture/drd_x11_capture.c',
  'encoding/drd_encoding_manager.c',
  'input/drd_input_dispatcher.c',
  'input/drd_x11_input.c',
  'utils/drd_frame.c',
  'utils/drd_frame_queue.c',
  'utils/drd_capture_metrics.c'
)

core_sources = files(
  'core/drd_application.c',
  'core/drd_server_runtime.c',
  'core/drd_config.c',
  'session/drd_rdp_session.c',
  'session/drd_rdp_graphics_pipeline.c',
  'transport/drd_rdp_listener.c',
  'transport/drd_rdp_routing_token.c',
  'security/drd_tls_credentials.c',
  'security/drd_local_session.c',
  'security/drd_nla_sam.c',
  'utils/drd_log.c',
  'utils/drd_system_info.c',
  'system/drd_system_daemon.c',
  'system/drd_handover_daemon.c'
)

dbus_gen_remote_desktop = gnome.gdbus_codegen('drd-dbus-remote-desktop',
                                              'org.deepin.RemoteDesktop.xml',
                                              interface_prefix: 'org.deepin.RemoteDesktop.',
                                              namespace: 'DrdDBusRemoteDesktop')
dbus_gen_lightdm = gnome.gdbus_codegen('drd-dbus-lightdm',
                                          'org.deepin.DisplayManager.xml',
                                          interface_prefix: 'org.deepin.DisplayManager.',
                                          namespace: 'DrdDBusLightdm')
core_sources += dbus_gen_remote_desktop
core_sources += dbus_gen_lightdm

exe_sources = []
exe_sources += media_sources
exe_sources += core_sources
exe_sources += files('main.c')

executable('deepin-remote-desktop', exe_sources,
           include_directories: src_inc,
           dependencies: deps,
           install: true,
           install_dir: get_option('bindir'))

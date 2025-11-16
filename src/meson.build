src_inc = include_directories('.')

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
  pam_dep
]

media_sources = files(
  'capture/drd_capture_manager.c',
  'capture/drd_x11_capture.c',
  'encoding/drd_encoding_manager.c',
  'encoding/drd_raw_encoder.c',
  'encoding/drd_rfx_encoder.c',
  'input/drd_input_dispatcher.c',
  'input/drd_x11_input.c',
  'utils/drd_frame.c',
  'utils/drd_frame_queue.c',
  'utils/drd_encoded_frame.c'
)

core_sources = files(
  'core/drd_application.c',
  'core/drd_server_runtime.c',
  'core/drd_config.c',
  'session/drd_rdp_session.c',
  'session/drd_rdp_graphics_pipeline.c',
  'transport/drd_rdp_listener.c',
  'security/drd_tls_credentials.c',
  'security/drd_local_session.c',
  'security/drd_nla_sam.c',
  'utils/drd_log.c'
)

drd_media = static_library(
  'drd-media',
  media_sources,
  include_directories: src_inc,
  dependencies: deps
)

drd_core = static_library(
  'drd-core',
  core_sources,
  include_directories: src_inc,
  dependencies: deps,
  link_with: drd_media
)

executable('deepin-remote-desktop', ['main.c'],
           include_directories: src_inc,
           link_with: drd_core,
           dependencies: deps,
           install: false)

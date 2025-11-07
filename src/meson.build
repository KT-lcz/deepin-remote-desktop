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
  xtst_dep
]

media_sources = files(
  'capture/grdc_capture_manager.c',
  'capture/grdc_x11_capture.c',
  'encoding/grdc_encoding_manager.c',
  'encoding/grdc_raw_encoder.c',
  'encoding/grdc_rfx_encoder.c',
  'input/grdc_input_dispatcher.c',
  'input/grdc_x11_input.c',
  'utils/grdc_frame.c',
  'utils/grdc_frame_queue.c',
  'utils/grdc_encoded_frame.c'
)

core_sources = files(
  'core/grdc_application.c',
  'core/grdc_server_runtime.c',
  'core/grdc_config.c',
  'session/grdc_rdp_session.c',
  'transport/grdc_rdp_listener.c',
  'security/grdc_tls_credentials.c',
  'security/grdc_nla_sam.c'
)

grdc_media = static_library(
  'grdc-media',
  media_sources,
  include_directories: src_inc,
  dependencies: deps
)

grdc_core = static_library(
  'grdc-core',
  core_sources,
  include_directories: src_inc,
  dependencies: deps,
  link_with: grdc_media
)

executable('grdc-server', ['main.c'],
           include_directories: src_inc,
           link_with: grdc_core,
           dependencies: deps,
           install: false)

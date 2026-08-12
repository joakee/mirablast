project('mirablast', 'c',
  version: '0.2.0',
  meson_version: '>= 0.61',
  default_options: ['warning_level=1', 'c_std=gnu11'])

gnome = import('gnome')

gtk_dep       = dependency('gtk+-3.0')
indicator_dep = dependency('ayatana-appindicator3-0.1')
glib_dep      = dependency('glib-2.0', version: '>= 2.66')
gio_unix_dep  = dependency('gio-unix-2.0')
nm_dep        = dependency('libnm')
gst_dep       = dependency('gstreamer-1.0')
gst_base_dep  = dependency('gstreamer-base-1.0')
gst_rtsp_dep  = dependency('gstreamer-rtsp-server-1.0')
gst_rtsplib_dep = dependency('gstreamer-rtsp-1.0')
gst_video_dep = dependency('gstreamer-video-1.0')
gst_pbutils_dep = dependency('gstreamer-pbutils-1.0')
pulse_dep     = dependency('libpulse-mainloop-glib')

vendor_inc = include_directories('vendor')

enum_files = gnome.mkenums_simple('nd-enum-types',
  sources: files('vendor/nd-sink.h'))

vendor_sources = files(
  'vendor/nd-dummy-provider.c',
  'vendor/nd-dummy-wfd-sink.c',
  'vendor/nd-firewalld.c',
  'vendor/nd-meta-provider.c',
  'vendor/nd-meta-sink.c',
  'vendor/nd-nm-device-registry.c',
  'vendor/nd-provider.c',
  'vendor/nd-pulseaudio.c',
  'vendor/nd-sink.c',
  'vendor/nd-uri-helpers.c',
  'vendor/nd-wfd-p2p-provider.c',
  'vendor/nd-wfd-p2p-sink.c',
  'vendor/wfd/wfd-audio-codec.c',
  'vendor/wfd/wfd-client.c',
  'vendor/wfd/wfd-media.c',
  'vendor/wfd/wfd-media-factory.c',
  'vendor/wfd/wfd-params.c',
  'vendor/wfd/wfd-resolution.c',
  'vendor/wfd/wfd-server.c',
  'vendor/wfd/wfd-session-pool.c',
  'vendor/wfd/wfd-video-codec.c',
)

vendor_lib = static_library('gnd-backend',
  vendor_sources + enum_files,
  include_directories: vendor_inc,
  dependencies: [glib_dep, gio_unix_dep, nm_dep, gst_dep, gst_base_dep,
                 gst_rtsp_dep, gst_rtsplib_dep, gst_video_dep, gst_pbutils_dep, pulse_dep])

app_sources = files(
  'src/main.c',
  'src/mb-session.c',
  'src/mb-settings-dialog.c',
  'src/mb-settings.c',
  'src/mb-sink-window.c',
  'src/mb-tray.c',
)

executable('mirablast-app',
  app_sources,
  include_directories: [vendor_inc],
  link_with: vendor_lib,
  dependencies: [gtk_dep, indicator_dep, glib_dep, gio_unix_dep, nm_dep,
                 gst_dep, gst_base_dep, gst_rtsp_dep, gst_rtsplib_dep, gst_video_dep, gst_pbutils_dep, pulse_dep],
  install: true)

install_data('data/mirablast.desktop',
  install_dir: get_option('datadir') / 'applications')

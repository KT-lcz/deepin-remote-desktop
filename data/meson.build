dbus_confdir = join_paths(get_option('sysconfdir'), 'dbus-1/system.d')
greeters_confdir = join_paths(get_option('sysconfdir'), 'deepin', 'greeters.d')
share_confdir = join_paths(get_option('datadir'), 'deepin-remote-desktop')

systemd_dep = dependency('systemd', required: false)
if systemd_dep.found()
  systemd_system_unit_dir = systemd_dep.get_variable(
    pkgconfig: 'systemdsystemunitdir',
    pkgconfig_define: [
      'prefix', get_option('prefix')
    ]
  )
  systemd_user_unit_dir = systemd_dep.get_variable(
    pkgconfig: 'systemduserunitdir',
    pkgconfig_define: [
      'prefix', get_option('prefix')
    ]
  )
else
  systemd_system_unit_dir = join_paths(get_option('prefix'), 'lib', 'systemd', 'system')
  systemd_user_unit_dir = join_paths(get_option('prefix'), 'lib', 'systemd', 'user')
endif

install_data('org.deepin.RemoteDesktop.conf',
             install_dir: dbus_confdir)

install_subdir('config.d',
               install_dir: share_confdir)

install_subdir('certs',
               install_dir: share_confdir)

install_data('11-deepin-remote-desktop-handover',
             install_dir: greeters_confdir)

install_data('deepin-remote-desktop-system.service',
             install_dir: systemd_system_unit_dir)

install_data([
               'deepin-remote-desktop-handover.service',
               'deepin-remote-desktop-user.service'
             ],
             install_dir: systemd_user_unit_dir)

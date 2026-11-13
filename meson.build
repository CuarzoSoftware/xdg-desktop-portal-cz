project(
    'cz-portal',
    'c', 'cpp',
    version : '0.1.0',
    meson_version: '>= 0.60.0',
    default_options: [
        'warning_level=2',
        'cpp_std=c++20',
    ]
)

# Screen casting needs PipeWire: without it (e.g. no pipewire-devel) the portal isn't built
pipewire_dep = dependency('libpipewire-0.3', version : '>= 0.3.62', required : false)

if not pipewire_dep.found()
    warning('libpipewire-0.3 not found: xdg-desktop-portal-cz will not be built')
    subdir_done()
endif

cz_core_dep         = dependency('cz-core', fallback:['cz-core', 'cz_core_dep'])
wayland_client_dep  = dependency('wayland-client')
gbm_dep             = dependency('gbm')
drm_dep             = dependency('libdrm')
systemd_dep         = dependency(['libsystemd', 'libelogind', 'basu'])

# -------------- PROTOCOLS --------------

wayland_scanner = find_program(dependency('wayland-scanner', native : true).get_variable(pkgconfig : 'wayland_scanner'))
protocol_sources = []

foreach xml : [
    'ext-image-capture-source-v1',
    'ext-image-copy-capture-v1',
    'ext-foreign-toplevel-list-v1',
    'linux-dmabuf-v1',
    'xdg-output-unstable-v1']
    protocol_sources += custom_target(xml + '-header',
        input : 'protocols' / xml + '.xml',
        output : xml + '-client-protocol.h',
        command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
    protocol_sources += custom_target(xml + '-code',
        input : 'protocols' / xml + '.xml',
        output : xml + '-protocol.c',
        command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

# -------------- EXECUTABLE --------------

libexecdir = get_option('prefix') / get_option('libexecdir')
datadir = get_option('prefix') / get_option('datadir')

executable(
    'xdg-desktop-portal-cz',
    sources : run_command('find', './src', '-type', 'f', '-name', '*.cpp', check : false).stdout().strip().split('\n') + protocol_sources,
    include_directories : ['./src'],
    dependencies : [
        cz_core_dep,
        wayland_client_dep,
        pipewire_dep,
        gbm_dep,
        drm_dep,
        systemd_dep
    ],
    install : true,
    install_dir : libexecdir)

# -------------- PORTAL REGISTRATION --------------

conf = configuration_data()
conf.set('libexecdir', libexecdir)

configure_file(
    input : 'data/org.freedesktop.impl.portal.desktop.cz.service.in',
    output : 'org.freedesktop.impl.portal.desktop.cz.service',
    configuration : conf,
    install_dir : datadir / 'dbus-1' / 'services')

configure_file(
    input : 'data/xdg-desktop-portal-cz.service.in',
    output : 'xdg-desktop-portal-cz.service',
    configuration : conf,
    install_dir : get_option('prefix') / 'lib' / 'systemd' / 'user')

# xdg-desktop-portal only reads these from its own data dir (not from e.g. /usr/local)
portal_data_dir = get_option('portal_data_dir')
install_data('data/cz.portal', install_dir : portal_data_dir / 'portals')
install_data('data/cuarzo-portals.conf', install_dir : portal_data_dir)

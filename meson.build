project(
	'wlsunset',
	'c',
	meson_version: '>=0.53.0',
	default_options: [
		'c_std=c11',
		'warning_level=3',
		'werror=true',
	],
)

add_project_arguments(
	[
		'-Wundef',
		'-Wunused',
		'-Wlogical-op',
		'-Wmissing-include-dirs',
		'-Wold-style-definition', # nop
		'-Wpointer-arith',
		'-Wstrict-prototypes',
		'-Wimplicit-fallthrough',
		'-Wmissing-prototypes',
		'-Wno-unknown-warning-option',
		'-Wno-unused-command-line-argument',
		'-Wvla',
		'-Wl,--exclude-libs=ALL',
	],
	language: 'c',
)

scanner = find_program('wayland-scanner')
scanner_private_code = generator(scanner, output: '@BASENAME@-protocol.c', arguments: ['private-code', '@INPUT@', '@OUTPUT@'])
scanner_client_header = generator(scanner, output: '@BASENAME@-client-protocol.h', arguments: ['client-header', '@INPUT@', '@OUTPUT@'])

protocols_src = [scanner_private_code.process('wlr-gamma-control-unstable-v1.xml')]
protocols_headers = [scanner_client_header.process('wlr-gamma-control-unstable-v1.xml')]

wl_client = dependency('wayland-client')
wl_protocols = dependency('wayland-protocols')
lib_protocols = static_library('protocols', protocols_src + protocols_headers, dependencies: wl_client)
protocols_dep = declare_dependency(link_with: lib_protocols, sources: protocols_headers)

cc = meson.get_compiler('c')
m = cc.find_library('m')

executable(
	'wlsunset',
	['main.c'],
	dependencies: [protocols_dep, m],
	install: true
)

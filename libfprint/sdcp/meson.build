sdcp_truststore_gresource_xml = custom_target('sdcp-truststore.gresource',
    input : 'generate-gresource.py',
    output : 'sdcp-truststore.gresource.xml',
    command : [find_program('python3'), '@INPUT@', meson.current_source_dir(), '@OUTPUT@'],
)

sdcp_truststore_resource_h = custom_target('fpi-sdcp-truststore-resource.h',
    input : sdcp_truststore_gresource_xml,
    output : 'fpi-sdcp-truststore-resource.h',
    command : ['glib-compile-resources',
               '--target=@OUTPUT@',
               '--sourcedir=' + meson.current_source_dir(),
               '--internal',
               '--generate',
               '--c-name', 'fpi_sdcp_truststore',
               '--manual-register',
               '@INPUT@']
)

sdcp_truststore_resource_c = custom_target('fpi-sdcp-truststore-resource.c',
    depends : [sdcp_truststore_resource_h],
    input : sdcp_truststore_gresource_xml,
    output : 'fpi-sdcp-truststore-resource.c',
    command : ['glib-compile-resources',
               '--target=@OUTPUT@',
               '--sourcedir=' + meson.current_source_dir(),
               '--internal',
               '--generate',
               '--c-name', 'fpi_sdcp_truststore',
               '--manual-register',
               '@INPUT@']
)

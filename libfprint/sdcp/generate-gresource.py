#!/usr/bin/python3

import os
import sys

if len(sys.argv) != 3:
  print("generate-gresource.py: Generates SDCP Truststore GResource XML file from certificates in ./truststore/*.pem")
  print("Usage: generate-gresource.py <libfprint_sdcp_source_dir> <output_xml_file>")
  sys.exit(1)

gresource_prefix = "/org/freedesktop/fprint/sdcp"
relative_folder = "truststore"
full_folder = os.path.join(sys.argv[1], relative_folder)
output = sys.argv[2]

files = [f for f in os.listdir(full_folder) if os.path.isfile(os.path.join(full_folder, f)) and f.endswith('.pem')]

with open(output, 'w') as f:
  f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
  f.write('<gresources>\n')
  f.write(f'  <gresource prefix="{gresource_prefix}">\n')
  for file in files:
    f.write(f'    <file compressed="true">{relative_folder}/{file}</file>\n')
  f.write('  </gresource>\n')
  f.write('</gresources>\n')

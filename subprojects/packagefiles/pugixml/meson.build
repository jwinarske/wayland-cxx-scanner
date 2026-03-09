# SPDX-License-Identifier: MIT
# Minimal Meson build definition for pugixml 1.14 (used as a wrap fallback).
project('pugixml', 'cpp', version: '1.14', license: 'MIT',
    default_options: ['cpp_std=c++14'])

pugixml_inc = include_directories('src')

pugixml_lib = library('pugixml',
    'src/pugixml.cpp',
    include_directories: pugixml_inc,
    install: true,
)

pugixml_dep = declare_dependency(
    include_directories: pugixml_inc,
    link_with: pugixml_lib,
)

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 wayland-cxx-scanner contributors
#
# Sphinx configuration for wayland-cxx-scanner.
# Read the Docs builds this automatically using .readthedocs.yaml.

import os

# ---------------------------------------------------------------------------
# Project identity
# ---------------------------------------------------------------------------
project = 'wayland-cxx-scanner'
copyright = '2026, wayland-cxx-scanner contributors'
author = 'wayland-cxx-scanner contributors'
release = '0.1.0'
version = '0.1'

# ---------------------------------------------------------------------------
# Extensions
# ---------------------------------------------------------------------------
extensions = [
    'myst_parser',
]

# MyST Markdown feature set
myst_enable_extensions = [
    'colon_fence',
    'deflist',
    'tasklist',
]

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
source_suffix = {
    '.rst': 'restructuredtext',
    '.md':  'markdown',
}

master_doc = 'index'

exclude_patterns = [
    '_build',
    'Thumbs.db',
    '.DS_Store',
]

# ---------------------------------------------------------------------------
# HTML output — Read the Docs theme
# ---------------------------------------------------------------------------
html_theme = 'sphinx_rtd_theme'

html_theme_options = {
    'navigation_depth': 4,
    'titles_only':      False,
}

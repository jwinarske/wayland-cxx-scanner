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
release = '1.0.0'
version = '0.1'

# ---------------------------------------------------------------------------
# Extensions
# ---------------------------------------------------------------------------
extensions = [
    'myst_parser',
    'breathe',
]

# MyST Markdown feature set
myst_enable_extensions = [
    'colon_fence',
    'deflist',
    'tasklist',
]

# ---------------------------------------------------------------------------
# Breathe — bridge Doxygen XML into Sphinx
# ---------------------------------------------------------------------------
# RTD runs Doxygen before Sphinx (see .readthedocs.yaml build.commands).
# The XML output lands in _doxygen/xml/ relative to the repository root.
breathe_projects = {
    'wayland-cxx-scanner': os.path.join(
        os.path.dirname(__file__), '..', '_doxygen', 'xml'
    ),
}
breathe_default_project = 'wayland-cxx-scanner'
breathe_default_members = ('members', 'undoc-members')

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

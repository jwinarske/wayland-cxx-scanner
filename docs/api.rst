API Reference
=============

Framework Headers (``include/wl/``)
------------------------------------

.. contents:: On this page
   :local:
   :depth: 2

Proxy & CRTP Base
~~~~~~~~~~~~~~~~~

.. doxygenclass:: wl::CProxy
   :members:

.. doxygenclass:: wl::CProxyImpl
   :members:

.. doxygenfunction:: wl::construct

.. doxygenfunction:: wl::construct_at_end

Resource (Server-Side)
~~~~~~~~~~~~~~~~~~~~~~

.. doxygenclass:: wl::CResourceImpl
   :members:

Ownership & RAII
~~~~~~~~~~~~~~~~

.. doxygenclass:: wl::WlPtr
   :members:

.. doxygenclass:: wl::FdHandle
   :members:

.. doxygenclass:: wl::FileHandle
   :members:

.. doxygenclass:: wl::ScopeExit
   :members:

Display & Event Loop
~~~~~~~~~~~~~~~~~~~~

.. doxygenclass:: wl::DisplayHandle
   :members:

Registry
~~~~~~~~

.. doxygenclass:: wl::CRegistry
   :members:

Event Map (Legacy)
~~~~~~~~~~~~~~~~~~

.. doxygenclass:: wl::CEventMap
   :members:

Scanner Tool
------------

.. doxygennamespace:: wl::scanner
   :members:

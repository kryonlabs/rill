# Wayland protocol source

`wlr-foreign-toplevel-management-unstable-v1.xml` is copied from
[wlr-protocols](https://gitlab.freedesktop.org/wlroots/wlr-protocols), via the
[upstream GitHub mirror](https://github.com/swaywm/wlr-protocols/blob/master/unstable/wlr-foreign-toplevel-management-unstable-v1.xml).
The copyright and permissive license are preserved in the XML.

The Makefile generates client declarations and protocol code using
`wayland-scanner`. Generated files stay in `build/protocols`. The server
header is generated only for the protocol test. No compositor or Wayland
server library is required for the normal application build.

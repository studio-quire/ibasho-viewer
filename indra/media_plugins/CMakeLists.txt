# -*- cmake -*-
# Modified for Ibasho (2026-10-23): no LibVLC media plugin (VLC is GPL; the CEF plugin plays media).
# Changes are marked "Ibasho".

add_subdirectory(base)
add_subdirectory(cef)
# Ibasho: no add_subdirectory(libvlc)
add_subdirectory(example)

if (LINUX)
    add_subdirectory(gstreamer10)
endif (LINUX)

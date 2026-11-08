// The OXOS File Manager's server (src/uploader/, spec/oxos_internal/31_upload_server.md): when X is pressed
// during the boot splash (spec/oxos_internal/32), a web page on the local network uploads files and folders to the
// disk, creates and deletes folders and files, and launches an XBE. Boot-only: it runs before the first title, from
// pages that title's start frees.
#pragma once
#include <xk/types.h>

// Serves until a browser launches an XBE: true with its NT path in LaunchPath. False when the network card cannot
// be used. Called again when the launched XBE did not load, with LastLaunch its status, which the screen shows.
bool UploadServe(NTSTATUS LastLaunch, char *LaunchPath, ULONG Size);

// After the launched XBE has loaded, before its title starts: the network card stopped, the screen and the server's
// memory freed.
void UploadStop();

// The page (uploader/page.cpp) and its length.
const char *UploadPage(ULONG *Length);

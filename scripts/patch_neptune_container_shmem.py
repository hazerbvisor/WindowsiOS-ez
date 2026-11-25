#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Adapt pinned virglrenderer anonymous shared files to an iOS app's own temporary directory."""
import pathlib, sys
p=pathlib.Path(sys.argv[1])/"src/mesa/util/anon_file.c"
s=p.read_text()
anchor="   const char *group = getenv(\"APP_SANDBOX_GROUP_ID\");"
assert s.count(anchor)==1, "Unexpected pinned renderer source"
s=s.replace(anchor, """   /* WindowsiOS-ez supplies its own app-container temporary directory.
    * Use an unlinked regular file with MAP_SHARED rather than a named
    * POSIX shm object, which requires a provisioned app group on iOS.
    * The file descriptor is still transferable by SCM_RIGHTS.
    * Fail closed if the requested directory cannot be used. */
   const char *container = getenv("WINIOS_NEPTUNE_SHM_DIR");
   if (container && container[0]) {
      if (container[0] != '/' || size < 0) {
         errno = EINVAL;
         return -1;
      }
      const char suffix[] = "/neptune-shared-XXXXXX";
      size_t len = strlen(container);
      if (len > SIZE_MAX - sizeof(suffix)) {
         errno = ENAMETOOLONG;
         return -1;
      }
      char *path = malloc(len + sizeof(suffix));
      if (!path)
         return -1;
      memcpy(path, container, len);
      memcpy(path + len, suffix, sizeof(suffix));
      fd = mkstemp(path);
      if (fd < 0) {
         free(path);
         return -1;
      }
      if (unlink(path) < 0) {
         int saved = errno;
         close(fd);
         free(path);
         errno = saved;
         return -1;
      }
      free(path);
      if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || ftruncate(fd, size) < 0) {
         int saved = errno;
         close(fd);
         errno = saved;
         return -1;
      }
      return fd;
   }
""" + anchor)
s=s.replace("#include <stdlib.h>","#include <stdlib.h>\n#include <stdint.h>",1)
p.write_text(s)

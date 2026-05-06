/*
Copyright: Boaz Segev, 2018-2019
License: MIT
*/
#ifndef H_FIO_TMPFILE_H
/** a simple helper to create temporary files and file names */
#define H_FIO_TMPFILE_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

static inline int fio_tmpfile(void) {
  // create a temporary file to contain the data.
  int fd = 0;
#ifdef _WIN32
  /* Use Windows API to get a proper temp directory */
  char tmpdir[MAX_PATH];
  DWORD len = GetTempPathA(MAX_PATH, tmpdir);
  if (len == 0 || len > MAX_PATH - 32) {
    /* Fallback: try environment variables */
    const char *env = getenv("TEMP");
    if (!env) env = getenv("TMP");
    if (!env) env = ".";
    len = (DWORD)strlen(env);
    if (len > MAX_PATH - 32) len = MAX_PATH - 32;
    memcpy(tmpdir, env, len);
  }
  /* Ensure trailing separator */
  if (len > 0 && tmpdir[len - 1] != '\\' && tmpdir[len - 1] != '/') {
    tmpdir[len++] = '\\';
  }
  memcpy(tmpdir + len, "facil_io_XXXXXXXX", 17);
  tmpdir[len + 17] = '\0';
  fd = mkstemp(tmpdir);
#else
#ifdef P_tmpdir
  if (P_tmpdir[sizeof(P_tmpdir) - 1] == '/') {
    char name_template[] = P_tmpdir "facil_io_tmpfile_XXXXXXXX";
    fd = mkstemp(name_template);
  } else {
    char name_template[] = P_tmpdir "/facil_io_tmpfile_XXXXXXXX";
    fd = mkstemp(name_template);
  }
#else
  char name_template[] = "/tmp/facil_io_tmpfile_XXXXXXXX";
  fd = mkstemp(name_template);
#endif
#endif
  return fd;
}
#endif

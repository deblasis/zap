#ifndef FIO_WIN32_H
#define FIO_WIN32_H
/*
 * Windows compatibility layer for facil.io
 *
 * Provides POSIX-like wrappers using Win32/Winsock2 APIs.
 * Included only when compiling on Windows (_WIN32).
 *
 * Copyright: MIT License (same as facil.io)
 */

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* Windows 7+ */
#endif

/* Winsock must be included before windows.h */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <process.h>   /* _beginthreadex */
#include <signal.h>    /* signal() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <io.h>
#include <fcntl.h>

/* ==========================================================================
 * poll() — Winsock2 provides WSAPoll which is poll()-compatible
 * The mingw headers (bundled with Zig) already define pollfd and WSAPoll
 * ========================================================================== */

/* poll() wrapper using WSAPoll */
static inline int fio_win_poll(struct pollfd *fds, unsigned long nfds, int timeout) {
    return WSAPoll(fds, nfds, timeout);
}
/* NOTE: poll override moved to fio_win32_fdmap.h for fd translation */

/* ==========================================================================
 * pthread emulation using Win32 threads
 * ========================================================================== */

typedef HANDLE fio_thread_t;

#define pthread_t fio_thread_t

static inline int pthread_create(pthread_t *thread, void *attr,
                                  void *(*start_routine)(void *), void *arg) {
    (void)attr;
    *thread = (pthread_t)_beginthreadex(NULL, 0,
        (unsigned int (__stdcall *)(void *))start_routine, arg, 0, NULL);
    return *thread == 0 ? -1 : 0;
}

static inline int pthread_join(pthread_t thread, void **retval) {
    (void)retval;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

static inline int pthread_detach(pthread_t thread) {
    CloseHandle(thread);
    return 0;
}

/* atfork — no-op on Windows (no fork) */
static inline int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                                  void (*child)(void)) {
    (void)prepare; (void)parent; (void)child;
    return 0;
}

/* ==========================================================================
 * pipe() emulation using _pipe()
 * ========================================================================== */

static inline int fio_win_pipe(int fds[2]) {
    return _pipe(fds, 4096, _O_BINARY);
}

#define pipe(fds) fio_win_pipe(fds)

/* ==========================================================================
 * fork() — not available on Windows, return error
 * facil.io has a weak fio_fork() that returns -1 on Windows
 * ========================================================================== */

/* ==========================================================================
 * Signals — Windows has limited signal support
 * ========================================================================== */

#ifndef SIGPIPE
#define SIGPIPE 13
#endif

#ifndef SIGCHLD
#define SIGCHLD 17
#endif

#ifndef SIGUSR1
#define SIGUSR1 10
#endif

/* Simplified sigaction for Windows */
struct sigaction {
    void (*sa_handler)(int);
    int sa_flags;
    int sa_mask; /* simplified — not a full sigset_t */
};

#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif
#ifndef SA_NOCLDSTOP
#define SA_NOCLDSTOP 0x00000001
#endif

static inline int sigemptyset(int *set) { (void)set; return 0; }

static inline int sigaction(int sig, const struct sigaction *act,
                             struct sigaction *oact) {
    (void)sig;
    if (oact && act) {
        oact->sa_handler = act->sa_handler;
    }
    if (act && act->sa_handler) {
        signal(sig, act->sa_handler);
    }
    return 0;
}

/* SIGCONT — no direct Windows equivalent, use raise trick */
#ifndef SIGCONT
#define SIGCONT 18
#endif

/* waitpid/WNOHANG — stubs for Windows (no child process mgmt) */
#ifndef WNOHANG
#define WNOHANG 1
#endif
static inline pid_t waitpid(pid_t pid, int *status, int options) {
    (void)pid; (void)status; (void)options;
    errno = ECHILD;
    return -1;
}

/* ==========================================================================
 * clock_gettime — mingw already provides this, but guard just in case
 * ========================================================================== */

/* mingw's pthread_time.h already declares clock_gettime. If it's missing,
 * we provide a fallback. */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

/* ==========================================================================
 * mmap/munmap emulation using VirtualAlloc
 * ========================================================================== */

#ifndef PROT_READ
#define PROT_READ   0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE  0x2
#endif

#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

static inline void *fio_mmap_anon(size_t size) {
    void *ptr = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
    /* Return MAP_FAILED on failure so sys_alloc can detect it */
    return ptr ? ptr : MAP_FAILED;
}

static inline int fio_munmap(void *addr, size_t size) {
    (void)size;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

/* Override mmap for anonymous mappings (facil.io only uses anon mmap) */
/* Note: We ignore addr/alignment hints — VirtualAlloc always returns page-aligned memory */
#define mmap(addr, len, prot, flags, fd, off) \
    fio_mmap_anon(len)
#define munmap(addr, len) \
    fio_munmap(addr, len)

/* ==========================================================================
 * close() — NOTE: overridden by fio_win32_fdmap.h for fd translation
 * ========================================================================== */

/* ==========================================================================
 * Socket API differences
 * ========================================================================== */

/* struct sockaddr_un (Unix domain sockets) — not natively available on Windows */
struct sockaddr_un {
    unsigned short sun_family;
    char sun_path[108];
};
#ifndef AF_UNIX
#define AF_UNIX 1
#endif

/* ==========================================================================
 * Winsock initialization — auto-init via constructor
 * Must happen before any socket operations
 * ========================================================================== */

static inline int fio_winsock_init(void) {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData);
}

static inline void fio_winsock_cleanup(void) {
    WSACleanup();
}

static void __attribute__((constructor)) fio_winsock_auto_init(void) {
    fio_winsock_init();
}

static void __attribute__((destructor)) fio_winsock_auto_cleanup(void) {
    fio_winsock_cleanup();
}

/* ==========================================================================
 * FD_SETSIZE increase — default is 64 on Windows, we need more
 * ========================================================================== */
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

/* ==========================================================================
 * Missing types/functions
 * ========================================================================== */

/* pid_t already defined by mingw */

/* ssize_t already defined by mingw */

/* kill() — Windows uses TerminateProcess, but we can use raise() for self-signaling */
static inline int fio_win_kill(pid_t pid, int sig) {
    if (pid == 0 || pid == getpid()) {
        return raise(sig);
    }
    /* For other processes, use TerminateProcess */
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!hProcess) return -1;
    BOOL result = TerminateProcess(hProcess, (UINT)sig);
    CloseHandle(hProcess);
    return result ? 0 : -1;
}
#define kill(pid, sig) fio_win_kill(pid, sig)

/* ioctl — NOTE: overridden by fio_win32_fdmap.h for fd translation */

/* fchmod — not available on Windows, no-op */
static inline int fchmod(int fd, unsigned int mode) {
    (void)fd; (void)mode;
    return 0; /* no-op on Windows */
}

/* pread — Windows doesn't have pread, use ReadFile with OVERLAPPED */
static inline ssize_t fio_win_pread(int fd, void *buf, size_t count, long long offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)((offset >> 32) & 0xFFFFFFFF);
    DWORD bytesRead = 0;
    if (!ReadFile(h, buf, (DWORD)count, &bytesRead, &ov)) {
        if (GetLastError() != ERROR_HANDLE_EOF) return -1;
    }
    return (ssize_t)bytesRead;
}
#define pread fio_win_pread

/* pwrite — same approach as pread */
static inline ssize_t fio_win_pwrite(int fd, const void *buf, size_t count, long long offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)((offset >> 32) & 0xFFFFFFFF);
    DWORD bytesWritten = 0;
    if (!WriteFile(h, buf, (DWORD)count, &bytesWritten, &ov)) {
        return -1;
    }
    return (ssize_t)bytesWritten;
}
#define pwrite fio_win_pwrite

/* S_ISLNK — not available on Windows, always return 0 */
#define S_ISLNK(mode) 0
#define S_ISSOCK(mode) 0

/* struct rlimit / getrlimit — Windows doesn't have this */
struct rlimit {
    unsigned long rlim_cur;
    unsigned long rlim_max;
};
#define RLIMIT_NOFILE 0
static inline int getrlimit(int resource, struct rlimit *rlim) {
    (void)resource;
    rlim->rlim_cur = 1024;  /* reasonable default */
    rlim->rlim_max = 10240;
    return 0;
}
static inline int setrlimit(int resource, const struct rlimit *rlim) {
    (void)resource; (void)rlim;
    return 0; /* no-op */
}
typedef unsigned long rlim_t;

/* wait/waitpid family — Windows doesn't have POSIX process waiting */
static inline pid_t wait(int *status) {
    (void)status;
    errno = ECHILD;
    return -1;
}

/* WIFEXITED / WEXITSTATUS macros — for Windows these are stubs */
#define WIFEXITED(status) ((status) != -1)
#define WEXITSTATUS(status) (0)
#define WTERMSIG(status) (0)

/* getopt isn't available on Windows — facil.io's CLI uses it */

#endif /* _WIN32 */
#endif /* FIO_WIN32_H */

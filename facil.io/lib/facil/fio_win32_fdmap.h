#ifndef FIO_WIN32_FDMAP_H
#define FIO_WIN32_FDMAP_H
/*
 * Windows FD Mapping Layer for facil.io
 *
 * Problem: Windows sockets are opaque HANDLEs (large integers like 0x1234),
 * but facil.io indexes fd_data[] using the fd directly (assumes small POSIX
 * integers: 0, 1, 2, ... 1023).
 *
 * Solution: Override socket(), accept(), close(), etc. to transparently map
 * Windows handles to compact integer indices that facil.io can use as array
 * indices. The mapping is stored in a simple lookup table.
 *
 * All facil.io code continues to use "fd" as a small integer — the translation
 * happens at the syscall boundary.
 */

#ifdef _WIN32

/* ==========================================================================
 * FD Table: maps compact fd -> Windows socket handle
 * ========================================================================== */

#define FIO_WIN_FD_TABLE_INIT_CAPA 4096

typedef struct {
    intptr_t *map;       /* map[compact_fd] = windows socket handle, 0 = free */
    int *free_stack;     /* stack of free compact fd indices */
    int free_top;        /* number of free entries in stack */
    int capa;            /* table capacity */
    fio_lock_i lock;     /* thread safety */
} fio_win_fd_table_s;

static fio_win_fd_table_s fio_win_fd_table_g = {NULL, NULL, 0, 0, FIO_LOCK_INIT};

static void fio_win_fd_table_init(void) {
    if (fio_win_fd_table_g.map)
        return;
    fio_win_fd_table_g.capa = FIO_WIN_FD_TABLE_INIT_CAPA;
    fio_win_fd_table_g.map = (intptr_t *)calloc(fio_win_fd_table_g.capa, sizeof(intptr_t));
    fio_win_fd_table_g.free_stack = (int *)malloc(fio_win_fd_table_g.capa * sizeof(int));
    fio_win_fd_table_g.free_top = 0;
    /* Populate free stack. Skip 0,1,2 (stdin/stdout/stderr convention). */
    for (int i = fio_win_fd_table_g.capa - 1; i >= 3; i--) {
        fio_win_fd_table_g.free_stack[fio_win_fd_table_g.free_top++] = i;
    }
}

/* Register a Windows handle → get compact fd. Returns -1 on OOM. */
static int fio_win_fd_register(intptr_t handle) {
    fio_lock(&fio_win_fd_table_g.lock);
    if (!fio_win_fd_table_g.map)
        fio_win_fd_table_init();
    if (fio_win_fd_table_g.free_top == 0) {
        int old_capa = fio_win_fd_table_g.capa;
        int new_capa = old_capa * 2;
        intptr_t *new_map = (intptr_t *)realloc(fio_win_fd_table_g.map, new_capa * sizeof(intptr_t));
        if (!new_map) { fio_unlock(&fio_win_fd_table_g.lock); return -1; }
        int *new_stack = (int *)realloc(fio_win_fd_table_g.free_stack, new_capa * sizeof(int));
        if (!new_stack) { fio_unlock(&fio_win_fd_table_g.lock); return -1; }
        fio_win_fd_table_g.map = new_map;
        fio_win_fd_table_g.free_stack = new_stack;
        memset(fio_win_fd_table_g.map + old_capa, 0, (new_capa - old_capa) * sizeof(intptr_t));
        for (int i = new_capa - 1; i >= old_capa; i--)
            fio_win_fd_table_g.free_stack[fio_win_fd_table_g.free_top++] = i;
        fio_win_fd_table_g.capa = new_capa;
    }
    int fd = fio_win_fd_table_g.free_stack[--fio_win_fd_table_g.free_top];
    fio_win_fd_table_g.map[fd] = handle;
    fio_unlock(&fio_win_fd_table_g.lock);
    return fd;
}

/* Release a compact fd. Thread-safe. */
static void fio_win_fd_release(int fd) {
    if (fd < 3 || !fio_win_fd_table_g.map || fd >= fio_win_fd_table_g.capa)
        return;
    fio_lock(&fio_win_fd_table_g.lock);
    fio_win_fd_table_g.map[fd] = 0;
    if (fio_win_fd_table_g.free_top < fio_win_fd_table_g.capa)
        fio_win_fd_table_g.free_stack[fio_win_fd_table_g.free_top++] = fd;
    fio_unlock(&fio_win_fd_table_g.lock);
}

/* Translate compact fd → real Windows handle. Falls back to pass-through. */
static inline intptr_t fio_win_fd_to_handle(int fd) {
    if (!fio_win_fd_table_g.map || fd < 3 || fd >= fio_win_fd_table_g.capa)
        return (intptr_t)fd;
    intptr_t h = fio_win_fd_table_g.map[fd];
    return h ? h : (intptr_t)fd;
}

/* ==========================================================================
 * Override socket functions: socket, accept, close, bind, connect, listen,
 * setsockopt, getsockopt, getsockname, getpeername, recv, send, fcntl, ioctl
 * ========================================================================== */

/* Save originals before we override */
#define fio_win_orig_socket    socket
#define fio_win_orig_accept    accept
#define fio_win_orig_closesocket closesocket
#define fio_win_orig_bind      bind
#define fio_win_orig_connect   connect
#define fio_win_orig_listen    listen
#define fio_win_orig_setsockopt setsockopt
#define fio_win_orig_getsockopt getsockopt
#define fio_win_orig_getsockname getsockname
#define fio_win_orig_getpeername getpeername
#define fio_win_orig_recv      recv
#define fio_win_orig_send      send

/* Override socket(): returns compact fd */
#undef socket
static inline int fio_win_socket(int domain, int type, int protocol) {
    SOCKET h = fio_win_orig_socket(domain, type, protocol);
    if (h == INVALID_SOCKET)
        return -1;
    int fd = fio_win_fd_register((intptr_t)h);
    if (fd == -1) {
        fio_win_orig_closesocket(h);
        errno = ENOMEM;
    }
    return fd;
}
#define socket fio_win_socket

/* Override accept(): returns compact fd */
#undef accept
static inline int fio_win_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    SOCKET h = fio_win_orig_accept((SOCKET)fio_win_fd_to_handle(sockfd), addr, addrlen);
    if (h == INVALID_SOCKET)
        return -1;
    int fd = fio_win_fd_register((intptr_t)h);
    if (fd == -1) {
        fio_win_orig_closesocket(h);
        errno = ENOMEM;
    }
    return fd;
}
#define accept fio_win_accept

/* Override close(): for sockets, use closesocket + release */
#undef close
static inline int fio_win_close(intptr_t fd) {
    intptr_t h = fio_win_fd_to_handle((int)fd);
    fio_win_fd_release((int)fd);
    /* Try closesocket first, fall back to _close for non-socket fds */
    if (fio_win_orig_closesocket((SOCKET)h) == 0)
        return 0;
    if (WSAGetLastError() != WSAENOTSOCK)
        return -1;
    return _close((int)h);
}
#define close(fd) fio_win_close((intptr_t)(fd))

/* Override bind() */
#undef bind
static inline int fio_win_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return fio_win_orig_bind((SOCKET)fio_win_fd_to_handle(sockfd), addr, addrlen);
}
#define bind fio_win_bind

/* Override connect() */
#undef connect
static inline int fio_win_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return fio_win_orig_connect((SOCKET)fio_win_fd_to_handle(sockfd), addr, addrlen);
}
#define connect fio_win_connect

/* Override listen() */
#undef listen
static inline int fio_win_listen(int sockfd, int backlog) {
    return fio_win_orig_listen((SOCKET)fio_win_fd_to_handle(sockfd), backlog);
}
#define listen(f,b) fio_win_listen(f,b)

/* Override setsockopt() */
#undef setsockopt
static inline int fio_win_setsockopt(int sockfd, int level, int optname, const char *optval, int optlen) {
    return fio_win_orig_setsockopt((SOCKET)fio_win_fd_to_handle(sockfd), level, optname, optval, optlen);
}
#define setsockopt fio_win_setsockopt

/* Override getsockopt() */
#undef getsockopt
static inline int fio_win_getsockopt(int sockfd, int level, int optname, char *optval, int *optlen) {
    return fio_win_orig_getsockopt((SOCKET)fio_win_fd_to_handle(sockfd), level, optname, optval, optlen);
}
#define getsockopt fio_win_getsockopt

/* Override getsockname() */
#undef getsockname
static inline int fio_win_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return fio_win_orig_getsockname((SOCKET)fio_win_fd_to_handle(sockfd), addr, addrlen);
}
#define getsockname fio_win_getsockname

/* Override getpeername() */
#undef getpeername
static inline int fio_win_getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return fio_win_orig_getpeername((SOCKET)fio_win_fd_to_handle(sockfd), addr, addrlen);
}
#define getpeername fio_win_getpeername

/* Override recv() */
#undef recv
static inline int fio_win_recv(int sockfd, void *buf, size_t len, int flags) {
    return fio_win_orig_recv((SOCKET)fio_win_fd_to_handle(sockfd), buf, (int)len, flags);
}
#define recv fio_win_recv

/* Override send() */
#undef send
static inline int fio_win_send(int sockfd, const void *buf, size_t len, int flags) {
    return fio_win_orig_send((SOCKET)fio_win_fd_to_handle(sockfd), buf, (int)len, flags);
}
#define send fio_win_send

/* Override WSAPoll (the poll wrapper from fio_win32.h) to translate fds */
#undef poll
static inline int fio_win_poll_translated(struct pollfd *fds, unsigned long nfds, int timeout) {
    /* Translate compact fds in pollfd array to real handles for WSAPoll */
    struct pollfd *translated = (struct pollfd *)alloca(nfds * sizeof(struct pollfd));
    for (unsigned long i = 0; i < nfds; i++) {
        translated[i].fd = (SOCKET)fio_win_fd_to_handle(fds[i].fd);
        translated[i].events = fds[i].events;
        translated[i].revents = 0;
    }
    int ret = WSAPoll(translated, nfds, timeout);
    if (ret > 0) {
        /* Copy revents back */
        for (unsigned long i = 0; i < nfds; i++)
            fds[i].revents = translated[i].revents;
    }
    return ret;
}
#define poll(fds, nfds, timeout) fio_win_poll_translated(fds, nfds, timeout)

/* Override fcntl: non-blocking mode via ioctlsocket */
#undef fcntl
static inline int fio_win_fcntl(int fd, int cmd, ...) {
    if (cmd == 1 /* F_GETFL */) return 0; /* pretend all is fine */
    if (cmd == 2 /* F_SETFL */) {
        /* Set non-blocking mode */
        u_long mode = 1; /* non-blocking */
        return ioctlsocket((SOCKET)fio_win_fd_to_handle(fd), FIONBIO, &mode);
    }
    return 0;
}
#define fcntl fio_win_fcntl

/* Override ioctl as well (some codepaths use ioctl(FIONBIO) directly) */
#undef ioctl
static inline int fio_win_ioctl(int fd, long cmd, u_long *argp) {
    return ioctlsocket((SOCKET)fio_win_fd_to_handle(fd), (long)cmd, argp);
}
#define ioctl fio_win_ioctl

/* Override unlink (Unix sockets use file paths, no-op on Windows) */
#define unlink(path) (-1)

#endif /* _WIN32 */
#endif /* FIO_WIN32_FDMAP_H */

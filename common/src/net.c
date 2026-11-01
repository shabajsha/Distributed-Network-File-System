#include "dnfs_shared.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* MSG_NOSIGNAL: a peer that has gone away returns EPIPE instead of raising
 * SIGPIPE, which would kill the whole process. */
int dnfs_send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* write() loop for regular files; send() only works on sockets. */
int dnfs_write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int dnfs_send_line(int fd, const char *msg) {
    size_t len = strlen(msg);
    char *buf = malloc(len + 1);
    if (!buf) {
        return -1;
    }
    memcpy(buf, msg, len);
    buf[len] = '\n';
    int rc = dnfs_send_all(fd, buf, len + 1);
    free(buf);
    return rc;
}

void dnfs_set_timeout(int fd, int timeout_sec) {
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* On Linux SO_SNDTIMEO also bounds connect(). */
int dnfs_connect(const char *ip, int port, int timeout_sec) {
    if (!ip || !*ip || port <= 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (timeout_sec > 0) {
        dnfs_set_timeout(fd, timeout_sec);
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void line_reader_init(LineReader *r, int fd) {
    memset(r, 0, sizeof(*r));
    r->fd = fd;
}

/* Returns the next line without its '\n' (caller frees), or NULL on EOF,
 * error, timeout, or a line longer than DNFS_MAX_LINE (too_long is set). */
char *line_reader_next(LineReader *r) {
    for (;;) {
        char *nl = r->len > r->scanned ? memchr(r->buf + r->scanned, '\n', r->len - r->scanned) : NULL;
        if (nl) {
            size_t line_len = (size_t)(nl - r->buf);
            char *line = malloc(line_len + 1);
            if (!line) {
                return NULL;
            }
            memcpy(line, r->buf, line_len);
            line[line_len] = '\0';
            if (line_len > 0 && line[line_len - 1] == '\r') {
                line[line_len - 1] = '\0';
            }
            r->len -= line_len + 1;
            memmove(r->buf, nl + 1, r->len);
            r->scanned = 0;
            return line;
        }
        r->scanned = r->len;

        if (r->len >= DNFS_MAX_LINE) {
            r->too_long = 1;
            return NULL;
        }
        if (r->cap - r->len < 4096) {
            size_t new_cap = r->cap ? r->cap * 2 : 8192;
            char *tmp = realloc(r->buf, new_cap);
            if (!tmp) {
                return NULL;
            }
            r->buf = tmp;
            r->cap = new_cap;
        }
        ssize_t n = recv(r->fd, r->buf + r->len, r->cap - r->len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return NULL;
        }
        r->len += (size_t)n;
    }
}

void line_reader_free(LineReader *r) {
    free(r->buf);
    r->buf = NULL;
    r->len = r->cap = r->scanned = 0;
}

char *dnfs_recv_line(int fd) {
    LineReader r;
    line_reader_init(&r, fd);
    char *line = line_reader_next(&r);
    line_reader_free(&r);
    return line;
}

void sb_init(StrBuf *sb) {
    memset(sb, 0, sizeof(*sb));
}

static int sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->failed) {
        return 0;
    }
    if (sb->len + extra + 1 <= sb->cap) {
        return 1;
    }
    size_t new_cap = sb->cap ? sb->cap : 256;
    while (new_cap < sb->len + extra + 1) {
        new_cap *= 2;
    }
    char *tmp = realloc(sb->data, new_cap);
    if (!tmp) {
        sb->failed = 1;
        return 0;
    }
    sb->data = tmp;
    sb->cap = new_cap;
    return 1;
}

void sb_appendn(StrBuf *sb, const char *s, size_t n) {
    if (!s || !sb_reserve(sb, n)) {
        return;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_append(StrBuf *sb, const char *s) {
    if (s) {
        sb_appendn(sb, s, strlen(s));
    }
}

void sb_appendf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0 || !sb_reserve(sb, (size_t)needed)) {
        sb->failed = 1;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)needed;
}

/* Appends s as a quoted, escaped JSON string. */
void sb_append_json(StrBuf *sb, const char *s) {
    char *escaped = json_escape(s ? s : "");
    if (!escaped) {
        sb->failed = 1;
        return;
    }
    sb_append(sb, "\"");
    sb_append(sb, escaped);
    sb_append(sb, "\"");
    free(escaped);
}

char *sb_take(StrBuf *sb) {
    if (sb->failed) {
        sb_free(sb);
        return NULL;
    }
    if (!sb->data) {
        if (!sb_reserve(sb, 0)) {
            return NULL;
        }
        sb->data[0] = '\0';
    }
    char *data = sb->data;
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return data;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

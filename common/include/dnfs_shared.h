#ifndef DNFS_SHARED_H
#define DNFS_SHARED_H

/* Code shared by the client, name server and storage server:
 * name rules, message framing, JSON helpers and authentication. */

#include <stddef.h>

#define DNFS_DEFAULT_NM_PORT 9000
#define DNFS_MAX_LINE (8 * 1024 * 1024)
#define DNFS_HEX_LEN 64
#define DNFS_TICKET_TTL 300
#define DNFS_SECRET_FILE "dnfs.secret"

/* names.c */
int dnfs_valid_filename(const char *name);
int dnfs_valid_username(const char *name);
int dnfs_env_int(const char *name, int fallback);
int dnfs_nm_port(void);

/* net.c: every message is one JSON object followed by '\n' */
int dnfs_send_all(int fd, const void *buf, size_t len);
int dnfs_write_all(int fd, const void *buf, size_t len);
int dnfs_send_line(int fd, const char *msg);
int dnfs_connect(const char *ip, int port, int timeout_sec);
void dnfs_set_timeout(int fd, int timeout_sec);

typedef struct {
    int fd;
    char *buf;
    size_t len;
    size_t cap;
    size_t scanned;
    int too_long;
} LineReader;

void line_reader_init(LineReader *r, int fd);
char *line_reader_next(LineReader *r);
void line_reader_free(LineReader *r);
char *dnfs_recv_line(int fd);

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int failed;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_append(StrBuf *sb, const char *s);
void sb_appendn(StrBuf *sb, const char *s, size_t n);
void sb_appendf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_append_json(StrBuf *sb, const char *s);
char *sb_take(StrBuf *sb);
void sb_free(StrBuf *sb);

/* json.c: lookups only search the top level of the object they are given */
char *json_escape(const char *src);
const char *json_skip_ws(const char *p);
const char *json_skip_value(const char *p);
char *json_read_string(const char *p, const char **end);
const char *json_find_value(const char *json, const char *key);
char *json_get_str_alloc(const char *json, const char *key);
int json_get_str(const char *json, const char *key, char *out, size_t out_size);
int json_get_long(const char *json, const char *key, long *out);
int json_get_bool(const char *json, const char *key);

/* auth.c */
int dnfs_random_hex(char *out, size_t nbytes);
int dnfs_load_secret(char *out, size_t out_size, int create_if_missing);
void dnfs_make_ticket(const char *secret, const char *user, const char *file,
                      const char *op, long expires, char out[DNFS_HEX_LEN + 1]);
int dnfs_check_ticket(const char *secret, const char *user, const char *file,
                      const char *op, long expires, const char *ticket);
int dnfs_hash_password(const char *password, char *out, size_t out_size);
int dnfs_verify_password(const char *password, const char *stored);

#endif /* DNFS_SHARED_H */

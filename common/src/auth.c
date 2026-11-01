#include "dnfs_shared.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#define PBKDF2_ITERATIONS 100000
#define SALT_BYTES 16
#define HASH_BYTES 32

static void to_hex(const unsigned char *in, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0F];
    }
    out[2 * n] = '\0';
}

static int from_hex(const char *in, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned int byte;
        if (sscanf(in + 2 * i, "%2x", &byte) != 1) {
            return 0;
        }
        out[i] = (unsigned char)byte;
    }
    return in[2 * n] == '\0';
}

int dnfs_random_hex(char *out, size_t nbytes) {
    unsigned char buf[64];
    if (nbytes > sizeof(buf) || RAND_bytes(buf, (int)nbytes) != 1) {
        return 0;
    }
    to_hex(buf, nbytes, out);
    return 1;
}

static void strip_newline(char *s) {
    s[strcspn(s, "\r\n")] = '\0';
}

/* The name server and storage servers share one secret, used to sign the
 * tickets that let a client use a storage server. It comes from the
 * DNFS_SECRET environment variable, else from ./dnfs.secret. The name server
 * creates that file on first start; copy it to other machines. */
int dnfs_load_secret(char *out, size_t out_size, int create_if_missing) {
    const char *env = getenv("DNFS_SECRET");
    if (env && *env) {
        if (strlen(env) < 16 || strlen(env) >= out_size) {
            fprintf(stderr, "DNFS_SECRET must be 16 to %zu characters\n", out_size - 1);
            return 0;
        }
        strcpy(out, env);
        return 1;
    }

    FILE *f = fopen(DNFS_SECRET_FILE, "r");
    if (f) {
        int ok = fgets(out, (int)out_size, f) != NULL;
        fclose(f);
        if (ok) {
            strip_newline(out);
            ok = strlen(out) >= 16;
        }
        return ok;
    }
    if (!create_if_missing) {
        return 0;
    }

    char generated[2 * 32 + 1];
    if (!dnfs_random_hex(generated, 32) || strlen(generated) >= out_size) {
        return 0;
    }
    int fd = open(DNFS_SECRET_FILE, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return 0;
    }
    char line[sizeof(generated) + 1];
    snprintf(line, sizeof(line), "%s\n", generated);
    ssize_t written = write(fd, line, strlen(line));
    close(fd);
    if (written != (ssize_t)strlen(line)) {
        return 0;
    }
    strcpy(out, generated);
    return 1;
}

void dnfs_make_ticket(const char *secret, const char *user, const char *file,
                      const char *op, long expires, char out[DNFS_HEX_LEN + 1]) {
    char msg[512];
    snprintf(msg, sizeof(msg), "%s\n%s\n%s\n%ld", op, user, file, expires);

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), secret, (int)strlen(secret),
         (const unsigned char *)msg, strlen(msg), mac, &mac_len);
    to_hex(mac, mac_len, out);
}

/* A ticket proves the name server allowed this user to run this operation
 * on this file until `expires`. */
int dnfs_check_ticket(const char *secret, const char *user, const char *file,
                      const char *op, long expires, const char *ticket) {
    if (!secret || !user || !file || !op || !ticket || strlen(ticket) != DNFS_HEX_LEN) {
        return 0;
    }
    long now = (long)time(NULL);
    if (expires < now || expires > now + 24 * 3600) {
        return 0;
    }
    char expected[DNFS_HEX_LEN + 1];
    dnfs_make_ticket(secret, user, file, op, expires, expected);
    return CRYPTO_memcmp(expected, ticket, DNFS_HEX_LEN) == 0;
}

/* Stored as "pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>". */
int dnfs_hash_password(const char *password, char *out, size_t out_size) {
    unsigned char salt[SALT_BYTES];
    unsigned char hash[HASH_BYTES];
    if (RAND_bytes(salt, sizeof(salt)) != 1) {
        return 0;
    }
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, sizeof(salt),
                          PBKDF2_ITERATIONS, EVP_sha256(), sizeof(hash), hash) != 1) {
        return 0;
    }
    char salt_hex[2 * SALT_BYTES + 1];
    char hash_hex[2 * HASH_BYTES + 1];
    to_hex(salt, sizeof(salt), salt_hex);
    to_hex(hash, sizeof(hash), hash_hex);
    int n = snprintf(out, out_size, "pbkdf2-sha256$%d$%s$%s", PBKDF2_ITERATIONS, salt_hex, hash_hex);
    return n > 0 && (size_t)n < out_size;
}

int dnfs_verify_password(const char *password, const char *stored) {
    int iterations = 0;
    char salt_hex[2 * SALT_BYTES + 1];
    char hash_hex[2 * HASH_BYTES + 1];
    if (sscanf(stored, "pbkdf2-sha256$%d$%32[0-9a-f]$%64[0-9a-f]", &iterations, salt_hex, hash_hex) != 3 ||
        iterations <= 0) {
        return 0;
    }
    unsigned char salt[SALT_BYTES];
    unsigned char expected[HASH_BYTES];
    unsigned char actual[HASH_BYTES];
    if (!from_hex(salt_hex, salt, sizeof(salt)) || !from_hex(hash_hex, expected, sizeof(expected))) {
        return 0;
    }
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, sizeof(salt),
                          iterations, EVP_sha256(), sizeof(actual), actual) != 1) {
        return 0;
    }
    return CRYPTO_memcmp(expected, actual, sizeof(actual)) == 0;
}

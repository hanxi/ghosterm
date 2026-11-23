#define _POSIX_C_SOURCE 200809L
#include "distribution.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef GTERM_UPDATE_KEY
#include <openssl/evp.h>

static size_t read_bounded(const char *path, unsigned char *buffer, size_t capacity) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    size_t size = fread(buffer, 1, capacity, file);
    int failed = ferror(file);
    if (fclose(file) || failed || size == capacity) {
        return 0;
    }
    return size;
}
#endif

int gterm_verify_update_index(const char *index_path, const char *signature_path) {
#ifndef GTERM_UPDATE_KEY
    (void)index_path;
    (void)signature_path;
    fputs("gterm: update signing key is unavailable\n", stderr);
    return 4;
#else
    static const char domain[] = "GTERM-DISTRIBUTION-INDEX-V1";
    static const char hex[] = GTERM_UPDATE_KEY;
    unsigned char public_key[32];
    for (size_t i = 0; i < sizeof(public_key); i++) {
        unsigned int byte;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1) {
            return 4;
        }
        public_key[i] = (unsigned char)byte;
    }
    unsigned char message[sizeof(domain) + 8193], signature[65];
    memcpy(message, domain, sizeof(domain));
    size_t size = read_bounded(index_path, message + sizeof(domain), 8193);
    if (!size || read_bounded(signature_path, signature, sizeof(signature)) != 64) {
        return 2;
    }
    EVP_PKEY *key =
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, sizeof(public_key));
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    int verified = key && context && EVP_DigestVerifyInit(context, NULL, NULL, NULL, key) == 1 &&
                   EVP_DigestVerify(context, signature, 64, message, sizeof(domain) + size) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return verified ? 0 : 6;
#endif
}

int gterm_distribution_platform(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    puts("macos-arm64");
#elif defined(__linux__) && defined(__x86_64__)
    puts("linux-x86_64");
#elif defined(__linux__) && defined(__aarch64__)
    puts("linux-arm64");
#else
    return 4;
#endif
    return fflush(stdout) ? 5 : 0;
}

int gterm_distribution_command(const char *executable, const char *command) {
    char script[4096];
    const char *base = strrchr(executable, '/');
    if (base) {
        int length = snprintf(script,
                              sizeof(script),
                              "%.*s/gterm-devkit/scripts/install-gterm.sh",
                              (int)(base - executable),
                              executable);
        if (length > 0 && (size_t)length < sizeof(script)) {
            execl("/bin/sh", "sh", script, command, executable, (char *)NULL);
        }
    }
    fputs("gterm: installation manager is unavailable\n", stderr);
    return 4;
}

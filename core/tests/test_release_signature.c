#include "ghosterm.h"
#include "extension/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef GT_TEST_RELEASE_SIGNATURE
#include <openssl/evp.h>
#endif
static int failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)
#ifdef GT_TEST_RELEASE_SIGNATURE
static int
signed_bytes(EVP_PKEY *key, const unsigned char *bytes, size_t size, unsigned char out[64]) {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    size_t count = 64;
    int ok = context && EVP_DigestSignInit(context, NULL, NULL, NULL, key) == 1 &&
             EVP_DigestSign(context, out, &count, bytes, size) == 1 && count == 64;
    EVP_MD_CTX_free(context);
    return ok;
}
#endif
int main(void) {
    unsigned char public_key[32] = {0}, signature[64] = {0};
    const unsigned char descriptor[] = "{\"schemaVersion\":1}";
    size_t size = sizeof(descriptor) - 1;
    CHECK(gt_ext_release_verify(NULL, 32, signature, 64, descriptor, size) == GT_ERR_INVALID_ARG);
    CHECK(gt_ext_release_verify(public_key, 31, signature, 64, descriptor, size) ==
          GT_ERR_INVALID_ARG);
    CHECK(gt_ext_release_verify(public_key, 33, signature, 64, descriptor, size) ==
          GT_ERR_INVALID_ARG);
    CHECK(gt_ext_release_verify(public_key, 32, NULL, 64, descriptor, size) == GT_ERR_INVALID_ARG);
    CHECK(gt_ext_release_verify(public_key, 32, signature, 63, descriptor, size) ==
          GT_ERR_INVALID_ARG);
    CHECK(gt_ext_release_verify(public_key, 32, signature, 65, descriptor, size) ==
          GT_ERR_INVALID_ARG);
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, NULL, size) == GT_ERR_INVALID_ARG);
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, 0) ==
          GT_ERR_INVALID_ARG);
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, 65537) ==
          GT_ERR_INVALID_ARG);
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, SIZE_MAX) ==
          GT_ERR_INVALID_ARG);
#ifndef GT_TEST_RELEASE_SIGNATURE
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, size) ==
          GT_ERR_UNSUPPORTED);
#else
    EVP_PKEY_CTX *generator = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    EVP_PKEY *key = NULL;
    CHECK(generator && EVP_PKEY_keygen_init(generator) == 1 &&
          EVP_PKEY_keygen(generator, &key) == 1);
    EVP_PKEY_CTX_free(generator);
    if (!key) {
        return 1;
    }
    size_t public_size = 32;
    CHECK(EVP_PKEY_get_raw_public_key(key, public_key, &public_size) == 1 && public_size == 32);
    static const unsigned char domain[] = "GHOSTERM-PLUGIN-RELEASE-V1";
    unsigned char message[sizeof(domain) + sizeof(descriptor)];
    memcpy(message, domain, sizeof(domain));
    memcpy(message + sizeof(domain), descriptor, size);
    CHECK(signed_bytes(key, message, sizeof(domain) + size, signature));
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, size) == GT_OK);
    CHECK(gt__plugin_memory_info().used == 0);
    CHECK(gt__plugin_memory_limit(sizeof(domain) + size - 1));
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, size) ==
          GT_ERR_INTERNAL);
    CHECK(gt__plugin_memory_info().used == 0);
    CHECK(gt__plugin_memory_limit(sizeof(domain) + size));
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, size) == GT_OK);
    CHECK(gt__plugin_memory_info().used == 0 &&
          gt__plugin_memory_info().peak == sizeof(domain) + size);
    CHECK(gt__plugin_memory_limit(GT_PLUGIN_MEMORY_DEFAULT));
    signature[63] ^= 1;
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, size) == GT_ERR_AUTH);
    signature[63] ^= 1;
    public_key[0] ^= 1;
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, size) != GT_OK);
    public_key[0] ^= 1;
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, size - 1) ==
          GT_ERR_AUTH);
    CHECK(signed_bytes(key, descriptor, size, signature));
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, size) == GT_ERR_AUTH);
    /* Same domain text without the separator NUL must be rejected. */
    memcpy(message, domain, sizeof(domain) - 1);
    memcpy(message + sizeof(domain) - 1, descriptor, size);
    CHECK(signed_bytes(key, message, sizeof(domain) - 1 + size, signature));
    CHECK(gt_ext_release_verify(public_key, 32, signature, 64, descriptor, size) == GT_ERR_AUTH);
    size_t max_size = sizeof(domain) + GT_EXT_RELEASE_DESCRIPTOR_MAX;
    unsigned char *large = malloc(max_size);
    CHECK(large != NULL);
    if (large) {
        memcpy(large, domain, sizeof(domain));
        memset(large + sizeof(domain), 'x', GT_EXT_RELEASE_DESCRIPTOR_MAX);
        /* The C primitive authenticates raw bytes, not JSON validity. */
        CHECK(signed_bytes(key, large, max_size, signature));
        CHECK(gt_ext_release_verify(public_key,
                                    32,
                                    signature,
                                    64,
                                    large + sizeof(domain),
                                    GT_EXT_RELEASE_DESCRIPTOR_MAX) == GT_OK);
        large[sizeof(domain) + 123] ^= 1;
        CHECK(gt_ext_release_verify(public_key,
                                    32,
                                    signature,
                                    64,
                                    large + sizeof(domain),
                                    GT_EXT_RELEASE_DESCRIPTOR_MAX) == GT_ERR_AUTH);
        free(large);
    }
    CHECK(gt__plugin_memory_info().used == 0);
    EVP_PKEY_free(key);
#endif
    printf("release_signature: %d failures\n", failures);
    return failures ? 1 : 0;
}

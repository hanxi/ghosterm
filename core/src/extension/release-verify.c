#include "ghosterm/extension_release.h"
#include "extension/memory.h"
#include <stdlib.h>
#include <string.h>
#ifdef GT_HAVE_RELEASE_SIGNATURE
#include <openssl/evp.h>
#endif

gt_status_t gt_ext_release_verify(const uint8_t *public_key,
                                  size_t public_key_size,
                                  const uint8_t *signature,
                                  size_t signature_size,
                                  const uint8_t *descriptor,
                                  size_t descriptor_size) {
    if (!public_key || public_key_size != 32 || !signature || signature_size != 64 || !descriptor ||
        !descriptor_size || descriptor_size > GT_EXT_RELEASE_DESCRIPTOR_MAX) {
        return GT_ERR_INVALID_ARG;
    }
#ifndef GT_HAVE_RELEASE_SIGNATURE
    return GT_ERR_UNSUPPORTED;
#else
    /* sizeof includes exactly the domain separator NUL. Never hash or
     * re-encode the caller's descriptor before PureEdDSA verification. */
    static const unsigned char domain[] = "GHOSTERM-PLUGIN-RELEASE-V1";
    size_t size = sizeof(domain) + descriptor_size;
    unsigned char *message = gt__plugin_memory_alloc(NULL, size);
    if (!message) {
        return GT_ERR_INTERNAL;
    }
    memcpy(message, domain, sizeof(domain));
    memcpy(message + sizeof(domain), descriptor, descriptor_size);
    EVP_PKEY *key =
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, public_key, public_key_size);
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    gt_status_t result = GT_ERR_INTERNAL;
    if (key && context && EVP_DigestVerifyInit(context, NULL, NULL, NULL, key) == 1) {
        int checked = EVP_DigestVerify(context, signature, signature_size, message, size);
        result = checked == 1 ? GT_OK : checked == 0 ? GT_ERR_AUTH : GT_ERR_INTERNAL;
    }
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    gt__plugin_memory_free(NULL, message, size);
    return result;
#endif
}

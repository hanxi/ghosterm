#include "ghosterm.h"
#include <jni.h>
#include <stdlib.h>

JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_NativeReleaseSignature_verify(
    JNIEnv *e, jobject self, jbyteArray public_key, jbyteArray signature, jbyteArray descriptor) {
    (void)self;
    if (!public_key || !signature || !descriptor) {
        return GT_ERR_INVALID_ARG;
    }
    jsize key_size = (*e)->GetArrayLength(e, public_key);
    jsize signature_size = (*e)->GetArrayLength(e, signature);
    jsize size = (*e)->GetArrayLength(e, descriptor);
    if (key_size != 32 || signature_size != 64 || size < 1 ||
        size > GT_EXT_RELEASE_DESCRIPTOR_MAX) {
        return GT_ERR_INVALID_ARG;
    }
    uint8_t key[32], signed_bytes[64];
    uint8_t *bytes = malloc((size_t)size);
    if (!bytes) {
        return GT_ERR_INTERNAL;
    }
    (*e)->GetByteArrayRegion(e, public_key, 0, 32, (jbyte *)key);
    (*e)->GetByteArrayRegion(e, signature, 0, 64, (jbyte *)signed_bytes);
    (*e)->GetByteArrayRegion(e, descriptor, 0, size, (jbyte *)bytes);
    gt_status_t status = GT_ERR_INTERNAL;
    if (!(*e)->ExceptionCheck(e)) {
        status = gt_ext_release_verify(
            key, sizeof(key), signed_bytes, sizeof(signed_bytes), bytes, (size_t)size);
    }
    free(bytes);
    return (jint)status;
}

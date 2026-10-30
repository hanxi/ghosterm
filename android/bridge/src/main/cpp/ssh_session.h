#ifndef GHOSTERM_ANDROID_SSH_SESSION_H
#define GHOSTERM_ANDROID_SSH_SESSION_H
#include <jni.h>
#include <stdatomic.h>
#include "ghosterm.h"
typedef struct gaj_ssh gaj_ssh;
/* Control-plane copies only. Connect and all Core access run on the terminal owner. */
gaj_ssh *gaj_ssh_new(JNIEnv *, jbyteArray host, jint port, jbyteArray user,
    jbyteArray known_hosts, jobjectArray addresses, jintArray scopes, jint timeout);
void gaj_ssh_free(gaj_ssh *);
gt_ssh_t *gaj_ssh_connect(gaj_ssh *, atomic_bool *stop);
gt_transport_t *gaj_ssh_open_shell(gaj_ssh *, gt_ssh_t *, const gt_shell_opts_t *, atomic_bool *, gt_status_t *);
gt_sftp_t *gaj_ssh_open_sftp(gaj_ssh *, gt_ssh_t *, atomic_bool *, gt_status_t *);
jobjectArray gaj_ssh_info(JNIEnv *, gaj_ssh *);
void gaj_ssh_respond(JNIEnv *, gaj_ssh *, jlong request, jint kind, jbyteArray first, jbyteArray second);
#endif

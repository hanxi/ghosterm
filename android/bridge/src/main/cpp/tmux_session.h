#ifndef GHOSTERM_ANDROID_TMUX_SESSION_H
#define GHOSTERM_ANDROID_TMUX_SESSION_H
#include <jni.h>
#include <stdatomic.h>
#include "ghosterm.h"
typedef struct gaj_tmux_target gaj_tmux_target;
/* A registry identity and snapshot token, never a borrowed Core handle.
 * Creating/freeing this token performs no Core operation. */
gaj_tmux_target *gaj_tmux_target_new(JNIEnv *, jlong owner, jlong snapshot, jint index);
void gaj_tmux_target_free(gaj_tmux_target *);
/* Called only by the terminal's native IO owner. The management owner
 * serializes validation/attach, then moves the ordinary transport to caller.
 * Cancellation waits for the command to release the borrowed attach SSH. */
gt_transport_t *gaj_tmux_attach(gaj_tmux_target *, gt_ssh_t *,
    const gt_shell_opts_t *, atomic_bool *, gt_status_t *, char *error, size_t capacity);
#endif

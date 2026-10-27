/* Linux user deployment; exclusive authenticated SSH owner, off UI thread. */
#ifndef GT_SESSION_DEPLOY_H
#define GT_SESSION_DEPLOY_H
#include "ghosterm/types.h"
typedef struct gt_ssh gt_ssh_t;
/* The source archive is validated in the app bundle, but only the five runtime
 * files are uploaded; remote releases from earlier clients remain reusable. */
enum {GT_BUNDLE_FILES=6,GT_BUNDLE_UPLOAD_FILES=5,GT_DEPLOY_PATH=1024};
typedef struct {char name[40],sha256[65];uint64_t size;} gt_bundle_file_t;
typedef struct {
  char directory[GT_DEPLOY_PATH],release[96],arch[24],min_glibc[24],loader[160],contract[65];
  gt_bundle_file_t files[GT_BUNDLE_FILES];
} gt_session_bundle_t;
typedef struct {char arch[24],glibc[24],home[512];uint32_t uid;} gt_linux_probe_t;
typedef enum {GT_DEPLOY_PROBE,GT_DEPLOY_UPLOAD,GT_DEPLOY_VERIFY,GT_DEPLOY_ACTIVATE,GT_DEPLOY_DONE} gt_deploy_phase_t;
typedef struct {
  uint32_t timeout_ms;void *context;bool (*cancelled)(void *);
  void (*progress)(void *,gt_deploy_phase_t,unsigned file,uint64_t bytes,uint64_t total);
} gt_deploy_options_t;
typedef struct {bool reused,published,publication_unknown;char daemon[GT_DEPLOY_PATH],proxy[GT_DEPLOY_PATH];} gt_deploy_result_t;
/* Catalog is caller-trusted, not a signature/authenticity verifier. Strict fixed
 * inventory, lengths, regular local files, sizes and metadata; remote SHA256 is
 * checked before any payload runs. No private keys or credentials in bundles. */
gt_status_t gt__session_bundle_read(const char *,gt_session_bundle_t *);
/* Single total monotonic deadline (1..120000ms), cancellation between bounded
 * nonblocking IO attempts. No concurrent SFTP/exec handles. Failure preserves
 * existing releases; ambiguous directory publication is reported explicitly.
 * Cancellation/network failure may invalidate SSH connection.
 * Existing running daemon is never stopped, upgraded or migrated here. */
gt_status_t gt__ssh_probe_linux(gt_ssh_t *,const gt_deploy_options_t *,gt_linux_probe_t *);
gt_status_t gt__ssh_deploy_linux(gt_ssh_t *,const gt_session_bundle_t *,const char *absolute_root,
  const gt_deploy_options_t *,gt_deploy_result_t *);
#endif

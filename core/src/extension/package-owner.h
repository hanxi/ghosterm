#ifndef GT_PRIVATE_PACKAGE_OWNER_H
#define GT_PRIVATE_PACKAGE_OWNER_H
#include "ghosterm/extension_package.h"
#include "extension/package-check.h"

struct gt_ext_package {
    atomic_bool stopped;
    gt_package_snapshot_t *snapshot;
    gt_package_checked_t *checked;
    gt_ext_package_info_t info;
};
#endif

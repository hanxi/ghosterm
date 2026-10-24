/* POSIX host admission only. Pure metadata, no kernel/engine/UI dependency. */
#ifndef GT_POSIX_PTY_SIZE_H
#define GT_POSIX_PTY_SIZE_H
#include "ghosterm/types.h"
/* Target POSIX winsize pixels are 16-bit. Open may use zero cell defaults;
 * resize must name positive cells. Zero pixels explicitly means unspecified. */
static inline gt_status_t gt__pty_size_check(uint16_t cols,uint16_t rows,uint32_t width,uint32_t height,bool opening) {
  if(!opening && (!cols || !rows))return GT_ERR_INVALID_ARG;
  if(width>UINT16_MAX || height>UINT16_MAX)return GT_ERR_UNSUPPORTED;
  return GT_OK;
}
#endif

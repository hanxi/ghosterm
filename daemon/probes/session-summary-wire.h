/* Experimental summary snapshot codec, not production IPC/schema. */
#ifndef GT_SESSION_SUMMARY_WIRE_H
#define GT_SESSION_SUMMARY_WIRE_H
#include "session/program-status.h"
#include "session-command-wire.h"
enum {PSW_RECORDS=32,PSW_STRINGS=65536,PSW_MAX=65536+32*20+8};
/* One largest output (16 KiB), two u64 lengths, maximal program and command summaries. This is a
 * single-item allowance, not permission for an unbounded pending stream. */
enum {PSW_RESTORE_BYTES=16+16384+PSW_MAX+CSW_MAX};
/* Caller owns bounded buffers. Encode failure sets length=0. Decode is
 * transactional, rejects clear records, duplicates, NULs and trailing bytes.
 * Success owns an independent store; no wire pointer survives decode. */
bool psw_encode(const gt_program_status_store_t *,void *,size_t,size_t *length);
bool psw_decode(const void *,size_t,gt_program_status_store_t **);
/* Budget-aware decode owns structure/entries/strings in the supplied domain.
 * Temporary parsing stack and input buffer remain caller responsibilities. */
bool psw_decode_with_allocator(const void *,size_t,const gt_status_allocator_t *,gt_program_status_store_t **);
#endif

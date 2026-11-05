/* Private ResourceStore. Single non-UI owner, ordinary local filesystem IO.
 * This is the storage boundary, not source-file scope checks, a wire receiver
 * or an authorization UI. Bind only with a native Session lease after consent.
 * Shared store quotas span its bound instances; use one store per connection.
 * No persistence/recovery promise; staged files are unlinked after checking
 * their two handles. Failed unlink/identity checks retain charged cleanup.
 * Published immutable copies survive scope seal until their final release. */
#ifndef GT_BRIDGE_RESOURCE_H
#define GT_BRIDGE_RESOURCE_H
#include "extension/session.h"
#define GT_BRIDGE_RESOURCE_SLOTS 64u
#define GT_BRIDGE_RESOURCE_SCOPES 8u
#define GT_BRIDGE_RESOURCE_BYTES_MAX 16777216u
#define GT_BRIDGE_RESOURCE_TOTAL_MAX 134217728u
#define GT_BRIDGE_RESOURCE_CHUNK_MAX 16384u
typedef struct gt_bridge_resource_store gt_bridge_resource_store_t;
typedef enum {GT_BRIDGE_RESOURCE_OK,GT_BRIDGE_RESOURCE_WAIT,GT_BRIDGE_RESOURCE_INVALID,
 GT_BRIDGE_RESOURCE_DENIED,GT_BRIDGE_RESOURCE_CLOSED,GT_BRIDGE_RESOURCE_PROTOCOL,
 GT_BRIDGE_RESOURCE_LIMIT,GT_BRIDGE_RESOURCE_IO,GT_BRIDGE_RESOURCE_UNSUPPORTED,
 GT_BRIDGE_RESOURCE_NOT_FOUND} gt_bridge_resource_status_t;
typedef struct {int directory_fd;uint64_t total_bytes,resource_bytes;size_t slots;} gt_bridge_resource_config_t;
typedef struct {uint64_t handle;char token[33];} gt_bridge_resource_ref_t;
typedef struct {
  char resource_id[33],sha256[65],mime[129],display_name[1025];
  uint64_t generation,size;bool size_known;
} gt_bridge_resource_descriptor_t;
typedef struct {
  gt_bridge_resource_descriptor_t descriptor;
  uint64_t received,reserved;uint32_t references;bool published;
} gt_bridge_resource_info_t;
typedef struct {uint64_t reserved_bytes;size_t resources,scopes;} gt_bridge_resource_usage_t;
/* Borrow directory_fd; own a CLOEXEC duplicate. Caller supplies a trusted private
 * directory, including its ACL/mount policy; this layer checks POSIX mode/uid.
 * All filenames are OS-random internal names;
 * metadata is never used as a path. An initial native ref keeps each item alive.
 * Allocator context outlives destroy; fixed workspace, no allocation per chunk.
 * Calls perform bounded-size IO, not a hard latency/disk deadline guarantee. */
gt_bridge_resource_status_t gt__bridge_resource_open(const gt_bridge_resource_config_t *,const gt_allocator_t *,gt_bridge_resource_store_t **);
gt_bridge_resource_status_t gt__bridge_resource_bind(gt_bridge_resource_store_t *,const gt_bridge_lease_t *,uint64_t scope_budget,uint64_t *owner);
/* Per-scope generation is a strict increasing high-water mark (not per name).
 * Failed begins do not advance it; successful begin consumes it even on abort.
 * Known sizes reserve their full length; unknown sizes reserve resource_bytes.
 * Both retained snapshots and active reservations count until final release. */
gt_bridge_resource_status_t gt__bridge_resource_begin(gt_bridge_resource_store_t *,uint64_t,const gt_bridge_lease_t *,const gt_bridge_resource_descriptor_t *,gt_bridge_resource_ref_t *);
gt_bridge_resource_status_t gt__bridge_resource_write(gt_bridge_resource_store_t *,uint64_t,const gt_bridge_lease_t *,const gt_bridge_resource_ref_t *,uint64_t offset,const void *,size_t,size_t *accepted);
/* eof_complete is a caller fact from the completed resource stream, never an
 * SSH process exit code. Length, declared digest and computed digest must all
 * agree. Failure discards the staging item; publication is one owner transition.
 * Duplicate completion is idempotent only for that retained, matching ref. */
gt_bridge_resource_status_t gt__bridge_resource_finish(gt_bridge_resource_store_t *,uint64_t,const gt_bridge_lease_t *,const gt_bridge_resource_ref_t *,bool eof_complete,uint64_t,const char sha256[65]);
gt_bridge_resource_status_t gt__bridge_resource_info(gt_bridge_resource_store_t *,uint64_t,const gt_bridge_lease_t *,const gt_bridge_resource_ref_t *,gt_bridge_resource_info_t *);
gt_bridge_resource_status_t gt__bridge_resource_read(gt_bridge_resource_store_t *,uint64_t,const gt_bridge_lease_t *,const gt_bridge_resource_ref_t *,uint64_t offset,void *,size_t,size_t *read_bytes);
gt_bridge_resource_status_t gt__bridge_resource_retain(gt_bridge_resource_store_t *,uint64_t,const gt_bridge_lease_t *,const gt_bridge_resource_ref_t *);
gt_bridge_resource_status_t gt__bridge_resource_release(gt_bridge_resource_store_t *,uint64_t,const gt_bridge_lease_t *,const gt_bridge_resource_ref_t *);
/* Seal rejects new traffic, discards staging, keeps published snapshots; native
 * scoped snapshot reads are still valid, not renewed remote permissions. */
gt_bridge_resource_status_t gt__bridge_resource_seal(gt_bridge_resource_store_t *,uint64_t,const gt_bridge_lease_t *);
/* Seal and hand off lifetime to retained snapshots. Automatically unbinds after
 * the final ref/cleanup is released; Store itself must still outlive all pins. */
gt_bridge_resource_status_t gt__bridge_resource_retire(gt_bridge_resource_store_t *,uint64_t,const gt_bridge_lease_t *);
gt_bridge_resource_status_t gt__bridge_resource_unbind(gt_bridge_resource_store_t *,uint64_t,const gt_bridge_lease_t *);
bool gt__bridge_resource_usage(const gt_bridge_resource_store_t *,gt_bridge_resource_usage_t *);
/* Refuses while any bound scope remains, including sealed scopes with pins.
 * Once no scopes remain it consumes the store, including on directory close IO
 * error; never retry a consumed fd/handle. No fsync/crash durability claim. */
gt_bridge_resource_status_t gt__bridge_resource_destroy(gt_bridge_resource_store_t *);
#endif

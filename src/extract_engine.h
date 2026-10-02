#ifndef EXTRACT_ENGINE_H
#define EXTRACT_ENGINE_H
/* Nexus-facing entry point for the archive engines (ZIP / RAR / 7z, incl.
 * encrypted + multi-volume — the inherited, host-tested leaves).
 *
 * Why a new header instead of the inherited extract.h: that one is the OLD
 * libmicrohttpd endpoint (`api_extract(MHD_Connection *, …)`), so it drags the
 * whole server in and cannot be called from the worker or tested off-target.
 * This is the replacement contract — transport-free, one call, progress and
 * cancel by callback.
 *
 * The adapter (src/extract_engine.c) keeps the dispatch rules that the old
 * endpoint already proved: format by file name, volume-set detection by member
 * name, and the one honest refusal ('x.rar.001' sets, which unrar cannot open
 * because it chains 'x.partNN.rar' itself).
 *
 * `progress` / `cancel` may be NULL. `progress` is called with the number of
 * bytes written so far; returning non-zero aborts the unpack (the engines are
 * also polled through `cancel`, which is the cheaper channel). */
#include "nexus_common.h"

typedef enum {
  NEXUS_CONFLICT_FAIL = 0,
  NEXUS_CONFLICT_OVERWRITE = 1,
  NEXUS_CONFLICT_MERGE = 2
} nexus_conflict_t;

typedef int (*nexus_extract_progress_fn)(uint64_t written, void *ctx);
typedef int (*nexus_extract_cancel_fn)(void *ctx);

typedef struct {
  nexus_conflict_t           conflict;
  const char                *password; /* NULL or "" when the caller has none */
  int                        large;    /* !=0 -> the LARGE limit profile
                                          (2 TiB total / 1 TiB per file). The
                                          caller must have checked free space;
                                          the safe profile is the default. */
  nexus_extract_progress_fn  progress;
  nexus_extract_cancel_fn    cancel;
  void                      *ctx;
} nexus_extract_req_t;

typedef struct {
  nexus_err_t err;
  char message[192]; /* human-readable, already translated by the engines */
} nexus_extract_result_t;

nexus_err_t nexus_extract(const char *archive_path, const char *dst_dir,
                          const nexus_extract_req_t *req,
                          nexus_extract_result_t *out /* may be NULL */);
#endif

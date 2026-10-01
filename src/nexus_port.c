/* NEXUS-only endpoints, adapted to the owendswang framework.
 *
 * The NEXUS front-end calls routes this back-end did not have:
 *   /api/status /api/app/register /api/fetch
 *   /api/save/list|backup|restore
 *   /api/pkg/scan /api/pkg/enqueue /api/install/poll
 *
 * They are implemented here against the existing task list (file_task_t) rather
 * than NEXUS's own task_engine, so /api/tasks, the progress plumbing and the
 * single-active-task rule all stay in one place.
 *
 * Save domain NOTE: only the file-level snapshot / restore half is implemented
 * here (forced snapshot, atomic landing, rollback — the safety-critical part the
 * NEXUS audit found missing everywhere). The PFS mount / decrypt / re-sign half
 * needs the PS5 kernel and the save AuthID; it is PS5-only and marked below.
 * Paths can be redirected for the host test via WFM_SAVE_ROOT / WFM_SNAPSHOT_DIR.
 */
#include "filemgr_internal.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app_installer.h"
#include "json_util.h"
#include "path_util.h"
#include "pkg_info.h"
#include "pkg_installer.h"
#include "transfer.h"
#include "websrv.h"

/* ====================================================================== */
/* shared helpers                                                          */
/* ====================================================================== */

/* Atomic single-file copy: land into a temp name + rename, so a crash never
 * leaves a half-written destination. Returns 0 on success. */
static int
copy_file_atomic(const char *src, const char *dst) {
  char temp[PATH_MAX];
  FILE *in = NULL, *out = NULL;
  char buf[65536];
  size_t n;
  int ret = -1;
  int fd = -1;

  if(strlen(dst) + 32 >= sizeof(temp)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  snprintf(temp, sizeof(temp), "%s.wfm-port-%ld.tmp", dst, (long)getpid());
  if(!(in = fopen(src, "rb"))) return -1;
  if((fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) goto done;
  if(!(out = fdopen(fd, "wb"))) goto done;
  fd = -1;
  while((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if(fwrite(buf, 1, n, out) != n) goto done;
  }
  if(ferror(in)) goto done;
  if(fflush(out) || fsync(fileno(out)) || fclose(out)) { out = NULL; goto done; }
  out = NULL;
  if(rename(temp, dst)) goto done;
  ret = 0;

done:
  if(out) fclose(out);
  if(fd >= 0) close(fd);
  if(in) fclose(in);
  if(ret) unlink(temp);
  return ret;
}

static void
mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  size_t n = strlen(path);

  if(!n || n >= sizeof(tmp)) return;
  memcpy(tmp, path, n + 1);
  for(size_t i = 1; i < n; i++) {
    if(tmp[i] == '/') {
      tmp[i] = 0;
      mkdir(tmp, 0777);
      chmod(tmp, 0777);
      tmp[i] = '/';
    }
  }
  mkdir(tmp, 0777);
  chmod(tmp, 0777);
}

/* Recursive copy; files land atomically. Returns 0 on success. */
static int
copy_tree(const char *src, const char *dst) {
  struct stat st;
  DIR *d;
  struct dirent *ent;

  if(lstat(src, &st)) return -1;
  if(S_ISDIR(st.st_mode)) {
    mkdir_p(dst);
    if(!(d = opendir(src))) return -1;
    while((ent = readdir(d))) {
      char s[PATH_MAX], t[PATH_MAX];
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(path_join(s, sizeof(s), src, ent->d_name) ||
         path_join(t, sizeof(t), dst, ent->d_name) ||
         copy_tree(s, t)) {
        closedir(d);
        return -1;
      }
    }
    closedir(d);
    return 0;
  }
  if(!S_ISREG(st.st_mode)) return 0;   /* skip specials / symlinks */
  return copy_file_atomic(src, dst);
}

static void
local_ip(char *out, size_t n) {
  int fd;
  struct sockaddr_in a;
  socklen_t len = sizeof(a);

  out[0] = 0;
  if((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) return;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(80);
  inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
  if(!connect(fd, (struct sockaddr *)&a, sizeof(a)) &&
     !getsockname(fd, (struct sockaddr *)&a, &len)) {
    snprintf(out, n, "%s", inet_ntoa(a.sin_addr));
  }
  close(fd);
}

/* Queue a background task driven by `fn`. Returns NULL when another task is
 * already running (the single-active-task rule is shared with the file ops). */
static file_task_t *
queue_task(task_op_t op, const char *src, const char *dst, const char *label,
           void *(*fn)(void *)) {
  file_task_t *task = calloc(1, sizeof(*task));

  if(!task) return NULL;
  task->op = op;
  task->state = TASK_QUEUED;
  snprintf(task->src, sizeof(task->src), "%s", src ? src : "");
  snprintf(task->dst, sizeof(task->dst), "%s", dst ? dst : "");
  snprintf(task->current, sizeof(task->current), "%s",
           label ? label : (src ? src : ""));
  task->created_at = task->updated_at = time(NULL);

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free_task(task);
    return NULL;
  }
  task->id = g_next_task_id++;
  task->next = g_tasks;
  g_tasks = task;
  pthread_mutex_unlock(&g_tasks_lock);

  if(pthread_create(&task->thread, NULL, fn, task)) {
    task_update(task, TASK_FAILED, NULL, 0, "pthread_create failed");
  } else {
    pthread_detach(task->thread);
  }
  return task;
}

static enum MHD_Result
task_id_response(struct MHD_Connection *conn, unsigned long id) {
  strbuf_t b = {0};
  strbuf_printf(&b, "{\"ok\":true,\"task_id\":%lu}", id);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

/* ====================================================================== */
/* /api/status · /api/app/register · /api/fetch                            */
/* ====================================================================== */

enum MHD_Result
api_status(struct MHD_Connection *conn, const char *body, size_t body_size) {
  strbuf_t b = {0};
  char ip[64];

  (void)body; (void)body_size;
  local_ip(ip, sizeof(ip));
  strbuf_printf(&b, "{\"ok\":true,\"http\":true,\"port\":%u,\"addr\":", g_listen_port);
  json_escape(&b, ip);
  /* Report the version that is actually LIVE, so it can be read back from any
   * browser with no tools:  http://<ps5>:2026/api/status
   *
   * Why this is worth a field: the payload manager can only show the version
   * baked into the FILE NAME, i.e. the version you *deployed*. That is exactly
   * the wrong answer when an upgrade silently did nothing (stale build, old
   * instance still holding the port), which is the failure this project has
   * actually hit. The value here comes from the running binary's own -D. */
  strbuf_append(&b, ",\"version\":");
  json_escape(&b, VERSION_TAG);
  strbuf_append(&b, "}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

enum MHD_Result
api_app_register(struct MHD_Connection *conn, const char *body, size_t body_size) {
  (void)body; (void)body_size;
#ifdef __SCE__
  /* Manual "add to home screen" (sceAppInstUtil*). The front-end confirms with
   * the user first — never run this while a game is running. */
  if(app_register_manual()) {
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "home-screen registration failed");
  }
  return send_json_ok(conn);
#else
  return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                "home-screen registration is only available on PS5",
                                "app_register_unsupported", NULL);
#endif
}

/* ====================================================================== */
/* /api/fetch — pull a file off SMB / NFS onto a local path.               */
/* ====================================================================== */

/* The dialog posts {src, dst, scheme}: `src` is a NAS address in the same shape
 * a bookmark stores it ("host/share/sub/path", optionally
 * "user:pass@host:port/…"), `dst` is either a directory or a full path.
 *
 * Shape credited to Loopayeh/pkg-sender (MIT), whose PS5 receiver performs the
 * same job over HTTP (/api/files/pull, with "overwrite"/"resume" modes). Three
 * of its calls are kept because they are right for a console:
 *   - RESUME is the default, not restart. A multi-GB game over Wi-Fi is the
 *     normal case, and an interrupted pull must not discard what already
 *     landed. The resume offset is read back from the destination's length, so
 *     the partial file itself is the checkpoint.
 *   - Landing is therefore NOT rename-atomic — for a 40 GB file on a FAT
 *     volume the rename would be a second full copy. The partial file is the
 *     state; a later pull continues it.
 *   - Progress is byte-based, so the existing task UI and the 「拉取」(kind 0)
 *     label work with no new plumbing.
 * The transport is this project's own transfer layer (libsmb2 / libnfs), which
 * reuses the NAS credentials the bookmark already holds. */

#define FETCH_CHUNK (256U * 1024U)

static int
write_all_fd(int fd, const void *buf, size_t len) {
  const char *p = buf;

  while(len) {
    ssize_t n = write(fd, p, len);
    if(n < 0) {
      if(errno == EINTR) continue;
      return -1;
    }
    if(!n) {
      errno = EIO;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static void *
fetch_worker(void *arg) {
  file_task_t *task = arg;
  /* task->src carries "<scheme>://<nas address>", composed by api_fetch so no
     extra field has to be added to file_task_t (which is shared with the file
     ops). */
  char composed[PATH_MAX];
  transfer_src_t *src = NULL;
  unsigned char *buf = NULL;
  char *separator;
  uint64_t remote_size = 0;
  uint64_t offset = 0;
  struct stat st;
  int fd = -1;
  int failed = 0;
  int canceled = 0;
  const char *why = NULL;

  task_update(task, TASK_RUNNING, task->current, 0, NULL);

  snprintf(composed, sizeof(composed), "%s", task->src);
  separator = strstr(composed, "://");
  if(!separator) {
    task_update(task, TASK_FAILED, task->current, 0, "bad fetch source");
    return NULL;
  }
  *separator = 0;

  if(!(buf = malloc(FETCH_CHUNK))) {
    task_update(task, TASK_FAILED, task->current, 0, "out of memory");
    return NULL;
  }
  if(!(src = transfer_open(composed, separator + 3))) {
    why = "NAS 地址无法解析（写成 主机/共享/路径）";
    failed = 1;
    goto out;
  }
  if(transfer_size(src, &remote_size) != NEXUS_OK || !remote_size) {
    why = transfer_last_error(src);
    failed = 1;
    goto out;
  }
  task->total = remote_size;

  /* Resume. A destination holding part of the file is continued; one that is
     not smaller than the source is foreign (or the source shrank) and is
     started over. */
  if(!stat(task->dst, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
     (uint64_t)st.st_size < remote_size) {
    offset = (uint64_t)st.st_size;
  }
  if((fd = open(task->dst, offset ? O_WRONLY
                                  : (O_WRONLY | O_CREAT | O_TRUNC),
                offset ? 0 : 0666)) < 0) {
    why = strerror(errno);
    failed = 1;
    goto out;
  }
  fchmod_0777(fd);
  if(offset && lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    why = strerror(errno);
    failed = 1;
    goto out;
  }

  while(offset < remote_size) {
    size_t want = (remote_size - offset) < FETCH_CHUNK
                    ? (size_t)(remote_size - offset) : FETCH_CHUNK;
    size_t got = 0;

    if(task_cancel_requested(task)) {
      canceled = 1;
      break;
    }
    if(transfer_read(src, offset, buf, want, &got) != NEXUS_OK || !got) {
      why = transfer_last_error(src);
      failed = 1;
      break;
    }
    if(write_all_fd(fd, buf, got)) {
      why = strerror(errno);
      failed = 1;
      break;
    }
    offset += got;
    task_update(task, TASK_RUNNING, task->current, got, NULL);
  }

out:
  if(fd >= 0) close(fd);
  if(src) transfer_close(src);
  free(buf);
  if(canceled) {
    task_update(task, TASK_CANCELED, task->dst, 0, "canceled");
  } else if(failed || offset < remote_size) {
    task_update(task, TASK_FAILED, task->dst, 0,
                why ? why : "拉取中断");
  } else {
    task_update(task, TASK_DONE, task->dst, 0, NULL);
  }
  return NULL;
}

enum MHD_Result
api_fetch(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *src_path = req_param(conn, body, body_size, "src");
  char *dst = req_param(conn, body, body_size, "dst");
  char *scheme = req_param(conn, body, body_size, "scheme");
  char composed[PATH_MAX];
  char target[PATH_MAX];
  const char *base;
  struct stat st;
  file_task_t *task;
  enum MHD_Result result;
  size_t needed;
  size_t dst_len;

  if(!src_path || !*src_path || !dst || !*dst) {
    free(src_path); free(dst); free(scheme);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "源路径与目标目录都要填");
  }
  if(!scheme || !*scheme) {
    free(scheme);
    scheme = strdup("smb");
  }
  if(!scheme) {
    free(src_path); free(dst);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  if(!transfer_scheme_supported(scheme)) {
    free(src_path); free(dst); free(scheme);
    return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                  "NAS backend not compiled in (smb/nfs)",
                                  "fetch_unsupported", NULL);
  }

  needed = (size_t)snprintf(composed, sizeof(composed), "%s://%s",
                            scheme, src_path);
  free(scheme);
  if(needed >= sizeof(composed)) {
    free(src_path); free(dst);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "NAS 地址过长");
  }

  /* A destination that names a directory keeps the remote basename.
     ⚠️ 两种分隔符都要找：粘进来的 Windows 路径
     （"192.168.1.3\Games\PPSA00000-app.pkg"）里一个 '/' 都没有，只找 '/'
     的话 strrchr 返回 NULL，于是「文件名」变成整个 URL，落盘名会带着主机名。 */
  dst_len = strlen(dst);
  base = strrchr(src_path, '/');
  {
    const char *bs = strrchr(src_path, '\\');
    if(bs && (!base || bs > base)) base = bs;
  }
  base = (base && base[1]) ? base + 1 : src_path;
  if(!base[0]) {
    free(src_path); free(dst);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "源路径没有文件名");
  }
  if((!stat(dst, &st) && S_ISDIR(st.st_mode)) ||
     (dst_len && dst[dst_len - 1] == '/')) {
    needed = (size_t)snprintf(target, sizeof(target), "%s/%s", dst, base);
  } else {
    needed = (size_t)snprintf(target, sizeof(target), "%s", dst);
  }
  free(dst);
  if(needed >= sizeof(target)) {
    free(src_path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "目标路径过长");
  }

  task = queue_task(TASK_DOWNLOAD, composed, target, base, fetch_worker);
  free(src_path);
  if(!task) {
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  result = task_id_response(conn, task->id);
  return result;
}

/* ====================================================================== */
/* Save domain: /api/save/list · /api/save/backup · /api/save/restore       */
/* ====================================================================== */

#define SAVEMGR_DEFAULT_SNAPSHOT_DIR "/data/savesnap"

static const char *
save_snapshot_dir(void) {
  const char *e = getenv("WFM_SNAPSHOT_DIR");
  return (e && *e) ? e : SAVEMGR_DEFAULT_SNAPSHOT_DIR;
}

static const char *g_save_roots[] = {
  "/user/data/", "/data/", "/mnt/ext1/user/data/"
};

static int
file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

/* Locate a title's save directory. WFM_SAVE_ROOT narrows the probe (host test). */
static int
find_save_dir(const char *title_id, char *out, size_t outsz) {
  const char *env = getenv("WFM_SAVE_ROOT");

  if(env && *env) {
    snprintf(out, outsz, "%s/%s/savedata_prospero", env, title_id);
    if(file_exists(out)) return 1;
    snprintf(out, outsz, "%s/%s", env, title_id);
    return file_exists(out);
  }
  for(size_t i = 0; i < sizeof(g_save_roots) / sizeof(g_save_roots[0]); i++) {
    snprintf(out, outsz, "%s%s/savedata_prospero", g_save_roots[i], title_id);
    if(file_exists(out)) return 1;
    snprintf(out, outsz, "%s%s", g_save_roots[i], title_id);
    if(file_exists(out)) return 1;
  }
  return 0;
}

/* Escalate to the save AuthID. PS5-only: it drives kernel ucred, which the host
 * does not have. The file-level snapshot/restore below works without it. */
static int
save_escalate(void) {
#ifdef __linux__
  return 0;
#else
  extern int kernel_set_ucred_authid(unsigned long long);
  return kernel_set_ucred_authid(0x4800000000000010ULL) ? -1 : 0;
#endif
}

enum MHD_Result
api_save_list(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *tid = req_param(conn, body, body_size, "title_id");
  char dir[PATH_MAX];
  DIR *d;
  struct dirent *ent;
  strbuf_t b = {0};
  int first = 1;

  if(!tid || !*tid) {
    free(tid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid title id");
  }
  snprintf(dir, sizeof(dir), "%s/%s", save_snapshot_dir(), tid);
  strbuf_append(&b, "{\"ok\":true,\"snapshots\":[");
  if((d = opendir(dir))) {
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(!first) strbuf_append(&b, ",");
      first = 0;
      json_escape(&b, ent->d_name);
    }
    closedir(d);
  }
  strbuf_append(&b, "]}");
  free(tid);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

/* Backup worker: FORCED snapshot of the title's save dir into the snapshot
 * root. There is no flag to skip it — an irreversible write-back without a
 * snapshot is exactly the failure mode this domain must not have. */
static void *
save_backup_worker(void *arg) {
  file_task_t *task = arg;
  char src[PATH_MAX], snap_root[PATH_MAX], snap[PATH_MAX], stamp[32];

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  if(save_escalate() || !find_save_dir(task->src, src, sizeof(src))) {
    task_update(task, TASK_FAILED, task->src, 0, "save data not found");
    return NULL;
  }
  snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
  snprintf(snap_root, sizeof(snap_root), "%s/%s", save_snapshot_dir(), task->src);
  snprintf(snap, sizeof(snap), "%s/%s", snap_root, stamp);
  mkdir_p(snap_root);
  if(copy_tree(src, snap)) {
    task_update(task, TASK_FAILED, src, 0, "snapshot failed");
    return NULL;
  }
  task_update(task, TASK_DONE, snap, 0, NULL);
  return NULL;
}

/* Restore worker: safety-snapshot the CURRENT state, then write the chosen
 * snapshot back; roll back to the safety snapshot if the write-in fails. */
static void *
save_restore_worker(void *arg) {
  file_task_t *task = arg;
  char dst[PATH_MAX], snap[PATH_MAX], bak[PATH_MAX];

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  if(save_escalate() || !find_save_dir(task->src, dst, sizeof(dst))) {
    task_update(task, TASK_FAILED, task->src, 0, "save data not found");
    return NULL;
  }
  snprintf(snap, sizeof(snap), "%s/%s/%s", save_snapshot_dir(), task->src, task->dst);
  if(!file_exists(snap)) {
    task_update(task, TASK_FAILED, snap, 0, "snapshot not found");
    return NULL;
  }
  snprintf(bak, sizeof(bak), "%s.wfm-pre-restore", dst);
  unlink(bak);
  if(copy_tree(dst, bak)) {
    task_update(task, TASK_FAILED, dst, 0, "could not safety-snapshot current save");
    return NULL;
  }
  if(copy_tree(snap, dst)) {
    copy_tree(bak, dst);   /* ROLLBACK */
    task_update(task, TASK_FAILED, dst, 0, "restore failed, rolled back");
    return NULL;
  }
  task_update(task, TASK_DONE, dst, 0, NULL);
  return NULL;
}

enum MHD_Result
api_save_backup(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *tid = req_param(conn, body, body_size, "title_id");
  char src[PATH_MAX];
  file_task_t *t;

  if(!tid || !*tid) {
    free(tid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid title id");
  }
  if(!find_save_dir(tid, src, sizeof(src))) {
    free(tid);
    return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                  "save data not found", "save_not_found", NULL);
  }
  t = queue_task(TASK_COPY, tid, "", tid, save_backup_worker);
  free(tid);
  if(!t) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  return task_id_response(conn, t->id);
}

enum MHD_Result
api_save_restore(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *tid = req_param(conn, body, body_size, "title_id");
  char *snap = req_param(conn, body, body_size, "snapshot");
  char src[PATH_MAX];
  file_task_t *t;

  if(!tid || !*tid || !snap || !*snap) {
    free(tid); free(snap);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid snapshot");
  }
  if(!find_save_dir(tid, src, sizeof(src))) {
    free(tid); free(snap);
    return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                  "save data not found", "save_not_found", NULL);
  }
  t = queue_task(TASK_COPY, tid, snap, tid, save_restore_worker);
  free(tid); free(snap);
  if(!t) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  return task_id_response(conn, t->id);
}

/* ====================================================================== */
/* PKG library: /api/pkg/scan · /api/pkg/enqueue · /api/install/poll        */
/* ====================================================================== */

#define NEXUS_DEFAULT_HOMEBREW_DIR "/data/homebrew"
#define PKG_SCAN_MAX_DEPTH 12

/* Where fPKG images are deployed for ShadowMount. Overridable for the host
 * test; on the console it is the real homebrew drop. */
static const char *
homebrew_dir(void) {
  const char *e = getenv("WFM_HOMEBREW_DIR");
  return (e && *e) ? e : NEXUS_DEFAULT_HOMEBREW_DIR;
}

static int
ends_with_ci(const char *s, const char *suffix) {
  size_t n = strlen(s), e = strlen(suffix);
  if(n <= e) return 0;
  for(size_t i = 0; i < e; i++) {
    char a = s[n - e + i];
    if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if(a != suffix[i]) return 0;
  }
  return 1;
}

static int
is_sony_pkg_name(const char *name) {
  return ends_with_ci(name, ".pkg");
}

static int
is_fpkg_name(const char *name) {
  return ends_with_ci(name, ".ffpkg") || ends_with_ci(name, ".ffpfsc") ||
         ends_with_ci(name, ".exfat");
}

static const char *
base_name(const char *path) {
  const char *b = strrchr(path, '/');
  return b ? b + 1 : path;
}

/* File name with the last extension removed. */
static void
stem_of(const char *path, char *out, size_t outsz) {
  const char *base = base_name(path);
  const char *dot = strrchr(base, '.');
  size_t n = (dot && dot != base) ? (size_t)(dot - base) : strlen(base);
  if(n >= outsz) n = outsz - 1;
  memcpy(out, base, n);
  out[n] = 0;
}

typedef struct {
  strbuf_t *b;
  int first;
} scan_out_t;

static void
scan_dir(const char *dir, scan_out_t *out, int depth);

static void
scan_emit(const char *full, const char *name, const struct stat *st, scan_out_t *out) {
  int fpkg = is_fpkg_name(name);
  char title_id[16] = {0}, title[256] = {0}, version[32] = {0};
  unsigned long long size = (unsigned long long)st->st_size;
  int have_meta = 0;

  if(fpkg) {
    stem_of(full, title, sizeof(title));
  } else if(is_sony_pkg_name(name)) {
    have_meta = pkg_info_read_meta(full, title_id, sizeof(title_id),
                                   title, sizeof(title), version, sizeof(version),
                                   &size) == 0;
    if(!have_meta) stem_of(full, title, sizeof(title));
  } else {
    return;
  }

  if(!out->first) strbuf_append(out->b, ",");
  out->first = 0;
  strbuf_append(out->b, "{\"title_id\":");
  json_escape(out->b, title_id);
  strbuf_append(out->b, ",\"title\":");
  json_escape(out->b, title);
  strbuf_append(out->b, ",\"version\":");
  json_escape(out->b, version);
  strbuf_printf(out->b, ",\"size\":%llu,\"is_fpkg\":%s,\"path\":",
                size, fpkg ? "true" : "false");
  json_escape(out->b, full);
  strbuf_append(out->b, "}");
}

static void
scan_dir(const char *dir, scan_out_t *out, int depth) {
  DIR *d;
  struct dirent *ent;

  if(depth > PKG_SCAN_MAX_DEPTH) return;
  if(!(d = opendir(dir))) return;
  while((ent = readdir(d))) {
    char full[PATH_MAX];
    struct stat st;

    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    if(path_join(full, sizeof(full), dir, ent->d_name) || lstat(full, &st)) continue;
    if(S_ISDIR(st.st_mode)) {
      scan_dir(full, out, depth + 1);
    } else if(S_ISREG(st.st_mode) &&
              (is_sony_pkg_name(ent->d_name) || is_fpkg_name(ent->d_name))) {
      scan_emit(full, ent->d_name, &st, out);
    }
  }
  closedir(d);
}

enum MHD_Result
api_pkg_scan(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *root = req_param(conn, body, body_size, "root");
  strbuf_t b = {0};
  scan_out_t out;

  if(!root || !*root) {
    free(root);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid root");
  }
  strbuf_append(&b, "{\"ok\":true,\"packages\":[");
  out.b = &b;
  out.first = 1;
  scan_dir(root, &out, 0);
  strbuf_append(&b, "]}");
  free(root);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

/* fPKG deploy worker: copy the image into the homebrew dir (ShadowMount picks
 * it up). Progress is byte-based, which the front-end's install dialog reads
 * from total/progress. */
static void *
fpkg_copy_worker(void *arg) {
  file_task_t *task = arg;
  struct stat st;

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  if(stat(task->src, &st) || !S_ISREG(st.st_mode)) {
    task_update(task, TASK_FAILED, task->src, 0, "image not found");
    return NULL;
  }
  mkdir_p(homebrew_dir());
  task->total = (unsigned long long)st.st_size;
  if(copy_tree(task->src, task->dst)) {
    task_update(task, TASK_FAILED, task->dst, 0, "copy to homebrew failed");
    return NULL;
  }
  task_update(task, TASK_DONE, task->dst, task->total, NULL);
  return NULL;
}

enum MHD_Result
api_pkg_enqueue(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *path = fs_path_value(req_param(conn, body, body_size, "path"));
  char dst[PATH_MAX];
  file_task_t *t;
  struct stat st;

  if(!path || !*path) {
    free(path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if(stat(path, &st) || !S_ISREG(st.st_mode)) {
    free(path);
    return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                  "package not found", "file_not_found", NULL);
  }

  if(is_fpkg_name(path)) {
    snprintf(dst, sizeof(dst), "%s/%s", homebrew_dir(), base_name(path));
    t = queue_task(TASK_COPY, path, dst, base_name(path), fpkg_copy_worker);
    free(path);
    if(!t) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
    return task_id_response(conn, t->id);
  }

  if(is_sony_pkg_name(path)) {
    unsigned long id = 0;
    int rc = filemgr_queue_pkg(path, &id);
    free(path);
    if(rc == PKG_INSTALL_UNSUPPORTED) {
      return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                    "package installation is only available on PS5",
                                    "pkg_install_unsupported", NULL);
    }
    if(rc) return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                  "could not queue package installation");
    return task_id_response(conn, id);
  }

  free(path);
  return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                "not a PKG or fPKG image", "pkg_type_invalid", NULL);
}

/* Map an internal task state to the NEXUS front-end's state_name. */
static const char *
task_state_name_nexus(task_state_t s) {
  switch(s) {
    case TASK_QUEUED:   return "queued";
    case TASK_RUNNING:  return "running";
    case TASK_DONE:     return "done";
    case TASK_FAILED:   return "failed";
    case TASK_CANCELED: return "cancelled";
    default:            return "queued";
  }
}

enum MHD_Result
api_install_poll(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *idstr = req_param(conn, body, body_size, "id");
  unsigned long id = idstr ? strtoul(idstr, NULL, 10) : 0;
  file_task_t *task = NULL;
  strbuf_t b = {0};
  int percent = 0;

  free(idstr);
  pthread_mutex_lock(&g_tasks_lock);
  task = find_task_locked(id);
  if(task) {
    percent = task->total ? (int)(task->done * 100 / task->total)
                          : (task->state == TASK_DONE ? 100 : 0);
    strbuf_printf(&b, "{\"ok\":true,\"id\":%lu,\"state_name\":\"%s\","
                      "\"progress\":%llu,\"total\":%llu,\"percent\":%d,"
                      "\"label\":",
                  task->id, task_state_name_nexus(task->state),
                  task->done, task->total, percent);
    json_escape(&b, task->current[0] ? task->current : task->src);
    strbuf_append(&b, ",\"note\":");
    json_escape(&b, task->error[0] ? task->error : "");
    strbuf_append(&b, "}");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(!task) {
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "task not found");
  }
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

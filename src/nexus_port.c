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

/* Recursive delete, best effort. Used to take the restore safety copy back out
 * of the save leaf once it is no longer needed — leaving it there bloats the
 * user's save directory and makes /api/save/scan list a bogus "title". */
static void
remove_tree(const char *path) {
  struct stat st;
  DIR *d;
  struct dirent *ent;

  if(lstat(path, &st)) return;
  if(S_ISDIR(st.st_mode)) {
    if(!(d = opendir(path))) return;
    while((ent = readdir(d))) {
      char child[PATH_MAX];
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(path_join(child, sizeof(child), path, ent->d_name)) continue;
      remove_tree(child);
    }
    closedir(d);
    rmdir(path);
    return;
  }
  unlink(path);
}

/* Marker suffix for the in-leaf safety copy taken right before a restore
 * overwrites the live save. Reserved: the save scan skips it so a copy left by
 * an interrupted restore never shows up as a game title. */
#define SAVE_PRERESTORE_SUFFIX ".wfm-pre-restore"

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
 * already running (the single-active-task rule is shared with the file ops).
 *
 * `srcs` (may be NULL) is TAKEN OVER on every path — success, "another task is
 * running", and failure alike — because the worker may already be built around
 * it. It has to be installed here and not by the caller after the call: the
 * thread is created below, so assigning task->srcs afterwards would race with
 * a worker that reads it in its first instruction. */
static file_task_t *
queue_task_ex(task_op_t op, const char *src, const char *dst, const char *label,
              char **srcs, size_t src_count, void *(*fn)(void *)) {
  file_task_t *task = calloc(1, sizeof(*task));

  if(!task) {
    free_paths(srcs, src_count);
    return NULL;
  }
  task->op = op;
  task->state = TASK_QUEUED;
  task->srcs = srcs;
  task->src_count = src_count;
  snprintf(task->src, sizeof(task->src), "%s", src ? src : "");
  snprintf(task->dst, sizeof(task->dst), "%s", dst ? dst : "");
  snprintf(task->current, sizeof(task->current), "%s",
           label ? label : (src ? src : ""));
  task->created_at = task->updated_at = time(NULL);

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free_task(task);          /* frees task->srcs too */
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

static file_task_t *
queue_task(task_op_t op, const char *src, const char *dst, const char *label,
           void *(*fn)(void *)) {
  return queue_task_ex(op, src, dst, label, NULL, 0, fn);
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

/* 拉取分块不再写死：transfer_read_chunk() 回后端协商出的读上限（SMB2 ≥1MB）。
   旧值 256KB 把复制速度钉死在「每往返 256KB」——千兆内网实测 ~7.5MB/s，正是
   「25GB 传十分钟才 18%」的来源。缓存一份查询结果（同一连接协商不变）。 */
#define FETCH_CHUNK_FALLBACK (256U * 1024U)

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

/* 取一个指向 `url` 的连接：优先复用 slot 里那个（同一个服务器/共享/凭据才允许，
 * 见 transfer_repoint），否则新建。返回 NULL = 地址解析不了或后端没编进来。 */
static transfer_src_t *
nas_source_for(transfer_src_t **slot, const char *url) {
  const char *sep;
  char scheme[8];
  size_t n;

  if(*slot && transfer_repoint(*slot, url)) return *slot;
  if(*slot) { transfer_close(*slot); *slot = NULL; }
  sep = strstr(url, "://");
  if(!sep || (n = (size_t)(sep - url)) >= sizeof(scheme)) return NULL;
  memcpy(scheme, url, n); scheme[n] = 0;
  *slot = transfer_open(scheme, sep + 3);
  return *slot;
}

/* 一个远程文件 → 本地文件，带续传。
 *
 * ★ 单文件拉取（/api/fetch）与 NAS→本地复制（/api/nas/copy）必须走**同一条**路。
 *   续传口径、0777 落盘、取消语义、错误文本一旦各写一份，下一次修 bug 只会修到
 *   其中一份 —— 这正是本项目反复踩的「两份实现各自漂移」。
 *
 * `*slot` 是连接槽（可为 NULL，函数会填上）。返回 0 成功 / -1 失败 / 1 被取消，
 * 失败原因写进 why。 */
static int
nas_pull_file(file_task_t *task, transfer_src_t **slot, const char *url,
              const char *dst, char *why, size_t why_size) {
  unsigned char *buf;
  transfer_src_t *src;
  uint64_t remote_size = 0;
  uint64_t offset = 0;
  size_t chunk;
  struct stat st;
  int fd = -1;
  int outcome = -1;

  chunk = FETCH_CHUNK_FALLBACK;
  if(!(buf = malloc(chunk))) {
    snprintf(why, why_size, "out of memory");
    return -1;
  }
  if(!(src = nas_source_for(slot, url))) {
    snprintf(why, why_size, "NAS 地址无法解析（写成 主机/共享/路径）");
    free(buf);
    return -1;
  }
  if(transfer_size(src, &remote_size) != NEXUS_OK) {
    snprintf(why, why_size, "%s", transfer_last_error(src)
             ? transfer_last_error(src) : "读不到远端文件大小");
    free(buf);
    return -1;
  }
  /* 连接已建立（transfer_size 连上了）才能问协商结果；问不到就退回旧值。 */
  {
    size_t mx = transfer_read_chunk(src);
    if(mx > chunk) {
      unsigned char *nb = realloc(buf, mx);
      if(nb) { buf = nb; chunk = mx; }
    }
  }
  /* ⚠️ 这里**不能**把 size == 0 当失败：空文件（.nomedia、锁文件、空的存档槽）
     是合法的，上一版 fetch 把它判成「拉取中断」，等于整棵目录树里有一个空文件
     就复制不过去。真·不存在的情况由 transfer_size 自己报错。 */
  task_update(task, TASK_RUNNING, task->current, 0, NULL);

  /* Resume. A destination holding part of the file is continued; one that is
     not smaller than the source is foreign (or the source shrank) and is
     started over. */
  if(!stat(dst, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
     (uint64_t)st.st_size < remote_size) {
    offset = (uint64_t)st.st_size;
  }
  if((fd = open(dst, offset ? O_WRONLY : (O_WRONLY | O_CREAT | O_TRUNC),
                offset ? 0 : 0666)) < 0) {
    snprintf(why, why_size, "%s", strerror(errno));
    goto out;
  }
  fchmod_0777(fd);
  if(offset && lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    snprintf(why, why_size, "%s", strerror(errno));
    goto out;
  }

  while(offset < remote_size) {
    size_t want = (remote_size - offset) < chunk
                    ? (size_t)(remote_size - offset) : chunk;
    size_t got = 0;

    if(task_cancel_requested(task)) { outcome = 1; goto out; }
    if(transfer_read(src, offset, buf, want, &got) != NEXUS_OK || !got) {
      snprintf(why, why_size, "%s", transfer_last_error(src)
               ? transfer_last_error(src) : "拉取中断");
      goto out;
    }
    if(write_all_fd(fd, buf, got)) {
      snprintf(why, why_size, "%s", strerror(errno));
      goto out;
    }
    offset += got;
    task_update(task, TASK_RUNNING, task->current, got, NULL);
  }
  outcome = (offset < remote_size) ? -1 : 0;

out:
  if(fd >= 0) close(fd);
  free(buf);
  return outcome;
}

/* /api/fetch 的 worker：task->src 就是 "<scheme>://<nas address>"。 */
static void *
fetch_worker(void *arg) {
  file_task_t *task = arg;
  /* task->src carries "<scheme>://<nas address>", composed by api_fetch so no
     extra field has to be added to file_task_t (which is shared with the file
     ops). */
  char composed[PATH_MAX];
  transfer_src_t *src = NULL;
  char why[256] = {0};
  int rc;

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  snprintf(composed, sizeof(composed), "%s", task->src);

  rc = nas_pull_file(task, &src, composed, task->dst, why, sizeof(why));
  if(src) transfer_close(src);
  if(rc == 1) task_update(task, TASK_CANCELED, task->dst, 0, "canceled");
  else if(rc) task_update(task, TASK_FAILED, task->dst, 0,
                          why[0] ? why : "拉取中断");
  else task_update(task, TASK_DONE, task->dst, 0, NULL);
  return NULL;
}

/* ====================================================================== */
/* NAS → 本地：把远端文件/目录整棵复制到 PS5 的磁盘上                       */
/* ====================================================================== */

/* 递归深度上限：远端目录里有互相指过来的符号链接/mount point 时，
   transfer_list 会一圈圈列下去，直到把内存和栈吃光。32 层对游戏存档、
   备份目录都远远够用，撞上就是真出了问题，明说比挂死好。 */
#define NEXUS_NAS_MAX_DEPTH 32

typedef struct nas_copy_ctx {
  file_task_t    *task;
  transfer_src_t *src;      /* 复用一个连接，见 transfer_repoint */
  char            why[256];
} nas_copy_ctx_t;

/* 扫一棵远端子树，把**文件字节总数**累加进 *bytes；`out_is_dir` 回告那个 URL
 * 到底是目录还是文件。返回 0 正常 / -1 失败 / 1 取消。
 *
 * 为什么要先扫：没有分母，进度条只能是 0% 或者一个「进行中」的幌子 ——
 * 用户上一轮刚抱怨过「解压没进度」，同一个毛病不能换个功能再犯一次。
 * 列目录本身很便宜（每层一次 readdir）；贵的是逐文件传输，不是逐目录列举。
 * 扫描结果不缓存，拉取时重新列一遍：多一遍 readdir，换的是与目录规模无关的
 * 常驻内存 —— PS5 上不能为了「少列一次目录」把几万个路径全部揣在 RAM 里。 */
static int
nas_scan_tree(nas_copy_ctx_t *c, const char *url, uint64_t *bytes,
              int *out_is_dir, int depth) {
  transfer_entry_t *list = NULL;
  int count = 0;
  int i;

  if(out_is_dir) *out_is_dir = 0;
  if(depth >= NEXUS_NAS_MAX_DEPTH) {
    snprintf(c->why, sizeof(c->why), "远端目录超过 %d 层，已停止（可能有环）",
             NEXUS_NAS_MAX_DEPTH);
    return -1;
  }
  if(task_cancel_requested(c->task)) return 1;
  if(!nas_source_for(&c->src, url)) {
    snprintf(c->why, sizeof(c->why), "NAS 地址无法解析（写成 主机/共享/路径）");
    return -1;
  }
  if(transfer_list(c->src, url, &list, &count) != NEXUS_OK) {
    /* 列不动 = 不是目录（transfer_list 对普通文件就是失败）。
       ★ 这里必须把**这个文件自己的字节数**累加进去：否则形如「源本身就是文件」
       的用法（用户在 NAS 视图里勾几个文件复制过来 —— 最常见的情形）分母恒为 0，
       进度条要么 0% 要么一个「进行中」的幌子，正是本项目反复踩的
       「有分子没分母」。真不存在时 transfer_size 同样读不到，那就什么也不加，
       最终由拉取那一步给出真原因（错误文本不在这里编）。 */
    uint64_t file_size = 0;

    free(list);
    if(transfer_size(c->src, &file_size) == NEXUS_OK) *bytes += file_size;
    return 0;
  }
  if(out_is_dir) *out_is_dir = 1;
  for(i = 0; i < count; i++) {
    char child[PATH_MAX];
    int is_dir = 0;
    int rc;

    if(list[i].is_dir) {
      if(snprintf(child, sizeof(child), "%s/%s", url, list[i].name) >=
         (int)sizeof(child)) continue;
      rc = nas_scan_tree(c, child, bytes, &is_dir, depth + 1);
      if(rc) { free(list); return rc; }
    } else {
      *bytes += list[i].size;
    }
  }
  free(list);
  return 0;
}

/* 逐条拉取（目录递归）。返回 0 成功 / -1 失败 / 1 取消。 */
static int
nas_pull_tree(nas_copy_ctx_t *c, const char *url, const char *dst, int depth) {
  transfer_entry_t *list = NULL;
  int count = 0;
  int i;
  int rc;

  if(depth >= NEXUS_NAS_MAX_DEPTH) {
    snprintf(c->why, sizeof(c->why), "远端目录超过 %d 层，已停止（可能有环）",
             NEXUS_NAS_MAX_DEPTH);
    return -1;
  }
  if(task_cancel_requested(c->task)) return 1;
  if(!nas_source_for(&c->src, url)) {
    snprintf(c->why, sizeof(c->why), "NAS 地址无法解析（写成 主机/共享/路径）");
    return -1;
  }
  if(transfer_list(c->src, url, &list, &count) != NEXUS_OK) {
    free(list);
    return nas_pull_file(c->task, &c->src, url, dst, c->why, sizeof(c->why));
  }
  /* 目录：先建（0777 —— umask 会削 mode，mkdir_p 里创建后再 chmod 补上），
     再逐个孩子。**不许**把目录做成原子 rename：目录树是逐个文件落地的，
     中断后保留已拉下来的部分正是续传的前提。 */
  mkdir_p(dst);
  chmod(dst, 0777);
  task_update(c->task, TASK_RUNNING, dst, 0, NULL);
  for(i = 0; i < count; i++) {
    char child_url[PATH_MAX];
    char child_dst[PATH_MAX];

    if(task_cancel_requested(c->task)) { free(list); return 1; }
    if(snprintf(child_url, sizeof(child_url), "%s/%s", url, list[i].name) >=
       (int)sizeof(child_url) || path_join(child_dst, sizeof(child_dst), dst,
                                          list[i].name)) continue;
    if(list[i].is_dir) {
      rc = nas_pull_tree(c, child_url, child_dst, depth + 1);
    } else {
      rc = nas_pull_file(c->task, &c->src, child_url, child_dst,
                         c->why, sizeof(c->why));
    }
    if(rc) { free(list); return rc; }
  }
  free(list);
  return 0;
}

/* 取 URL 里最后一段当落地名。⚠️ 必须走 transfer_url_parse 而不是自己 strrchr：
 * URL 头部可能带 "user:pass@"（含 '/' 不合法但 ':' 合法），而且**共享根**这种
 * 源（path 为空）要退回共享名，否则落地名会是一个空串。 */
static void
nas_basename_of(const char *url, char *out, size_t out_size) {
  transfer_nas_t nas;
  const char *p;

  out[0] = 0;
  if(!transfer_url_parse(url, &nas)) return;
  p = nas.path[0] ? nas.path : nas.share;
  {
    const char *slash = strrchr(p, '/');
    if(slash && slash[1]) p = slash + 1;
  }
  snprintf(out, out_size, "%s", p);
}

static void *
nas_copy_worker(void *arg) {
  file_task_t *task = arg;
  nas_copy_ctx_t ctx;
  uint64_t bytes = 0;
  size_t i;
  int rc = 0;

  memset(&ctx, 0, sizeof(ctx));
  ctx.task = task;
  task_update(task, TASK_RUNNING, "正在统计远端目录…", 0, NULL);

  for(i = 0; i < task->src_count && !rc; i++) {
    int is_dir = 0;
    rc = nas_scan_tree(&ctx, task->srcs[i], &bytes, &is_dir, 0);
  }
  if(rc) goto done;

  /* 分母。0（全是空目录/空文件）时保持 0 —— 前端对 total == 0 显示「进行中」
     而不是 0%，那是诚实的；写个假的 100 出来才是骗人。 */
  task->total = bytes;

  for(i = 0; i < task->src_count && !rc; i++) {
    char base[256];
    char dst[PATH_MAX];

    nas_basename_of(task->srcs[i], base, sizeof(base));
    if(!base[0]) {
      snprintf(ctx.why, sizeof(ctx.why), "源路径没有文件名");
      rc = -1;
      break;
    }
    if(path_join(dst, sizeof(dst), task->dst, base)) {
      snprintf(ctx.why, sizeof(ctx.why), "目标路径过长");
      rc = -1;
      break;
    }
    rc = nas_pull_tree(&ctx, task->srcs[i], dst, 0);
  }

done:
  if(ctx.src) transfer_close(ctx.src);
  if(rc == 1) task_update(task, TASK_CANCELED, task->dst, 0, "canceled");
  else if(rc) task_update(task, TASK_FAILED, task->dst, 0,
                          ctx.why[0] ? ctx.why : "复制中断");
  else task_update(task, TASK_DONE, task->dst, 0, NULL);
  return NULL;
}

/* 把 (scheme, 凭据, 端口, 用户填的地址) 拼成 transfer_url_parse 认的形状：
 *     smb://user:pass@host:port/share/path
 * 端口必须插在**主机之后、路径之前**，而用户给的是「主机/共享/路径」（没有端口位），
 * 所以按第一个分隔符切开再插 —— 直接前后缀拼不上去。
 * 返回 0 成功 / -1（地址为空或拼出来过长）。 */
static int
nas_compose_url(const char *scheme, const char *user, const char *pass,
                const char *port, const char *addr, char *out, size_t out_size) {
  size_t hostlen = strcspn(addr, "/\\");
  int n;

  if(!hostlen) { out[0] = 0; return -1; }
  n = snprintf(out, out_size, "%s://%s%s%s%s%.*s%s%s%s",
               scheme,
               (user && *user) ? user : "",
               (user && *user) ? ":" : "",
               (user && *user) ? (pass ? pass : "") : "",
               (user && *user) ? "@" : "",
               (int)hostlen, addr,
               (port && *port) ? ":" : "",
               (port && *port) ? port : "",
               addr + hostlen);
  if(n < 0 || (size_t)n >= out_size) { out[0] = 0; return -1; }
  return 0;
}

/* POST /api/nas/copy —— 把 NAS 上的文件/目录复制到 PS5 的本地目录。
 *
 * 为什么不是 /api/copy：那条路的第一个动作就是 lstat(src)，而 NAS 路径在本地
 * 根本不存在 ⇒ 用户看到的正是 "source not found"。远程源必须走 transfer 层。
 *
 * 参数：scheme=smb|nfs，paths=换行分隔的 NAS 路径（和地址栏里那串同形：
 * 主机/共享/路径），dst=本地目标目录，user/pass/port 可选。
 * 凭据由这里拼进每个源的 URL；**task->src / current 只用不带凭据的路径**，
 * 因为 /api/tasks 会把它们当 label 回显到页面上。 */
enum MHD_Result
api_nas_copy(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *scheme = req_param(conn, body, body_size, "scheme");
  char *paths = req_param(conn, body, body_size, "paths");
  char *dst = req_param(conn, body, body_size, "dst");
  char *user = req_param(conn, body, body_size, "user");
  char *pass = req_param(conn, body, body_size, "pass");
  char *port = req_param(conn, body, body_size, "port");
  char **srcs = NULL;
  size_t src_count = 0;
  char label[PATH_MAX];
  char label_plain[PATH_MAX];
  struct stat st;
  file_task_t *task;

  if(!paths || !*paths || !dst || !*dst) {
    free(scheme); free(paths); free(dst); free(user); free(pass); free(port);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "源路径与目标目录都要填");
  }
  if(!scheme || !*scheme) {
    free(scheme);
    scheme = strdup("smb");
  }
  if(!scheme) {
    free(paths); free(dst); free(user); free(pass); free(port);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  if(!transfer_scheme_supported(scheme)) {
    free(scheme); free(paths); free(dst); free(user); free(pass); free(port);
    return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                  "NAS backend not compiled in (smb/nfs)",
                                  "nas_unsupported", NULL);
  }
  /* 目标必须是已存在的**本地目录**。让它的父目录落在一个远程 URL 上会写出
     一份相对路径垃圾（"192.168.1.3/PS5_Games/x" 会被当成当前目录下的目录）。 */
  if(dst[0] != '/' || stat(dst, &st) || !S_ISDIR(st.st_mode)) {
    free(scheme); free(paths); free(dst); free(user); free(pass); free(port);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                  "目标必须是已存在的本地目录", "bad_target", NULL);
  }

  /* 一次分配够：先数行数，避免 realloc 的两处失败清理分支。
     故意不用 strtok_r —— 那是 POSIX 扩展，PS5 的 libc 上有没有**没验过**，
     而这里本来就只是按 '\n'/'\r' 切一刀（`lstat` / `getmntinfo` 已经给过一次教训：
     宿主有、SDK 没有的东西，编译能过、上机才炸）。strpbrk 是 C89，安全。 */
  label[0] = 0;
  label_plain[0] = 0;
  {
    const char *q;
    size_t lines = 1;

    for(q = paths; *q; q++) if(*q == '\n') lines++;
    if(!(srcs = calloc(lines, sizeof(*srcs)))) {
      free(scheme); free(paths); free(dst); free(user); free(pass); free(port);
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
    }
  }
  {
    const char *p = paths;

    while(*p) {
      char addr[PATH_MAX];
      char url[PATH_MAX];
      const char *eol = strpbrk(p, "\r\n");
      size_t len = eol ? (size_t)(eol - p) : strlen(p);
      size_t k;
      int dup = 0;

      if(len >= sizeof(addr)) len = sizeof(addr) - 1;
      memcpy(addr, p, len);
      addr[len] = 0;
      p = eol ? eol + 1 : p + len;

      /* 前后空白（从别处粘进来时常见）不参与比对，也不该进 URL。 */
      {
        char *s = addr;
        while(*s == ' ' || *s == '\t') s++;
        if(s != addr) memmove(addr, s, strlen(s) + 1);
      }
      len = strlen(addr);
      while(len && (addr[len - 1] == ' ' || addr[len - 1] == '\t')) addr[--len] = 0;
      if(!len) continue;

      if(nas_compose_url(scheme, user, pass, port, addr, url, sizeof(url)))
        continue;
      /* 去重按**拼好的 URL** 比：同一个路径被重复勾选/重复粘一行时不该拉两遍。 */
      for(k = 0; k < src_count; k++)
        if(!strcmp(srcs[k], url)) { dup = 1; break; }
      if(dup) continue;
      if(!(srcs[src_count] = strdup(url))) {
        free(scheme); free(paths); free(dst); free(user); free(pass); free(port);
        free_paths(srcs, src_count);
        return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
      }
      src_count++;
      if(!label_plain[0]) snprintf(label_plain, sizeof(label_plain), "%s", addr);
    }
  }
  free(scheme); free(paths); free(user); free(pass); free(port);
  if(!src_count) {
    free(dst);
    free(srcs);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "没有可复制的源路径");
  }

  /* 标签里**不带凭据**（/api/tasks 会回显它）。 */
  if(src_count > 1)
    snprintf(label, sizeof(label), "%s 等 %zu 项", label_plain, src_count);
  else
    snprintf(label, sizeof(label), "%s", label_plain);

  task = queue_task_ex(TASK_DOWNLOAD, label_plain, dst, label, srcs, src_count,
                       nas_copy_worker);
  free(dst);
  if(!task) {
    free_paths(srcs, src_count);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  return task_id_response(conn, task->id);
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

/* 存档域的提权。定义在下面（backup/restore 旁边），但**扫描/查询也要用** ——
 * 见 api_save_scan 的说明：不先提权，/user/home 下的 opendir 会被拒，
 * 于是「有没有存档」这个问题永远回答「没有」。 */
static int save_escalate(void);

/* Where save data actually lives.
 *
 * ⚠️ The previous list (/user/data/<tid>/savedata_prospero, /data/<tid>/…) was a
 * guess and matches nothing on a real console. PS5 keeps user saves under the
 * ACCOUNT's home directory — one level deeper than a title id can express:
 *     /user/home/<account-hex>/savedata_prospero/<TITLE_ID>       ← the data
 *     /user/home/<account-hex>/savedata_prospero_meta/<TITLE_ID>  ← index/meta
 * (extended storage mirrors it under /mnt/ext1/home/). The account id cannot be
 * derived from a title id, so that level has to be ENUMERATED rather than
 * templated — which is why the old probe could never succeed, and why a manually
 * typed title id looked like "this console has no save for that game".
 *
 * The flat roots are kept as a cheap fallback (one stat each) for firmware that
 * stores saves directly under the data partition. */
static const char *const g_save_home_roots[] = {
  "/user/home/", "/mnt/ext1/home/"
};
static const char *const g_save_flat_roots[] = {
  "/user/data/", "/data/", "/mnt/ext1/user/data/"
};
static const char *const g_save_leaves[] = {
  "savedata_prospero", "savedata_prospero_meta",
  /* PS4 世代标题在 PS5 上落在同级的 savedata/ · savedata_meta/（psdevwiki
     Save Data：「PS4 titles keep using savedata」）。本机跑的是 PS5 原生标题，
     但用户也可能通过兼容层装 PS4 游戏 —— 这两个多一次 opendir 的成本，
     换来的是「PS4 游戏的存档也能备份」，且顺序放在 prospero 之后，
     同名标题不会被认错世代。 */
  "savedata", "savedata_meta"
};

static int
file_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

/* "<base><leaf>/<title_id>" for every leaf; first hit wins. `base` ends with a
 * slash (or is the WFM_SAVE_ROOT host-test root, see find_save_dir).
 *
 * ★ 层级顺序是 leaf 在前、title id 在后 —— 真机就是
 *     /user/home/<account-hex>/savedata_prospero/<TITLE_ID>
 * （psdevwiki Save Data + gbatemp 实机 FTP 双源确认）。曾经写成
 * <base><title_id>/<leaf>，在真机上永远 stat 不到 ⇒ 备份/恢复与「有没有存档」
 * 一律落到「找不到存档」，手动敲 TITLE_ID 也一样 —— 正是用户报的现象。
 * 这里的顺序必须与 scan_save_base() 保持一致：同一个存档不允许一处说有、
 * 一处说没有。 */
static int
probe_save_leaves(const char *base, const char *title_id, char *out,
                  size_t outsz) {
  size_t i;

  for(i = 0; i < sizeof(g_save_leaves) / sizeof(g_save_leaves[0]); i++) {
    int n = snprintf(out, outsz, "%s%s/%s", base, g_save_leaves[i], title_id);
    if(n < 0 || (size_t)n >= outsz) continue;
    if(file_exists(out)) return 1;
  }
  return 0;
}

/* Flat roots = firmware variants that keep saves straight under the data
 * partition. Both orders get one stat each instead of betting on one: this is
 * only a cheap fallback, and a miss here is indistinguishable from "no save". */
static int
probe_save_flat(const char *root, const char *title_id, char *out, size_t outsz) {
  size_t i;
  int n;

  if(probe_save_leaves(root, title_id, out, outsz)) return 1;
  for(i = 0; i < sizeof(g_save_leaves) / sizeof(g_save_leaves[0]); i++) {
    n = snprintf(out, outsz, "%s%s/%s", root, title_id, g_save_leaves[i]);
    if(n > 0 && (size_t)n < outsz && file_exists(out)) return 1;
  }
  n = snprintf(out, outsz, "%s%s", root, title_id);
  if(n > 0 && (size_t)n < outsz && file_exists(out)) return 1;
  return 0;
}

/* Walk one /user/home level — the account directory is the part we do not know. */
static int
probe_save_home(const char *home_root, const char *title_id, char *out,
                size_t outsz) {
  DIR *d = opendir(home_root);
  struct dirent *e;
  char base[PATH_MAX];
  int found = 0;

  if(!d) return 0;
  while(!found && (e = readdir(d))) {
    if(e->d_name[0] == '.') continue;
    if(snprintf(base, sizeof(base), "%s%s/", home_root, e->d_name) >=
       (int)sizeof(base)) continue;
    found = probe_save_leaves(base, title_id, out, outsz);
  }
  closedir(d);
  return found;
}

/* Locate a title's live save directory. WFM_SAVE_ROOT narrows the probe to a
 * single account home (host test). */
static int
find_save_dir(const char *title_id, char *out, size_t outsz) {
  const char *env = getenv("WFM_SAVE_ROOT");
  size_t i;

  if(!title_id || !*title_id) return 0;
  if(env && *env) {
    char base[PATH_MAX];
    size_t n = strlen(env);
    if(snprintf(base, sizeof(base), "%s%s", env,
                (n && env[n - 1] == '/') ? "" : "/") < (int)sizeof(base) &&
       probe_save_leaves(base, title_id, out, outsz)) return 1;
    snprintf(out, outsz, "%s/%s", env, title_id);
    return file_exists(out);
  }
  for(i = 0; i < sizeof(g_save_home_roots) / sizeof(g_save_home_roots[0]); i++) {
    if(probe_save_home(g_save_home_roots[i], title_id, out, outsz)) return 1;
  }
  for(i = 0; i < sizeof(g_save_flat_roots) / sizeof(g_save_flat_roots[0]); i++) {
    if(probe_save_flat(g_save_flat_roots[i], title_id, out, outsz)) return 1;
  }
  return 0;
}

/* Recursive size of a save directory. A save is a handful of small files; this
 * only labels a row in the UI, so an unreadable entry is skipped instead of
 * failing the whole scan. */
static unsigned long long
save_dir_size(const char *path) {
  DIR *d = opendir(path);
  struct dirent *e;
  unsigned long long total = 0;

  if(!d) return 0;
  while((e = readdir(d))) {
    char child[PATH_MAX];
    struct stat st;

    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if(snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >=
       (int)sizeof(child)) continue;
    if(lstat(child, &st)) continue;
    total += S_ISDIR(st.st_mode) ? save_dir_size(child)
                                 : (unsigned long long)st.st_size;
  }
  closedir(d);
  return total;
}

/* 扫描诊断：把「读不到」和「这台机器上没有」在页面上分开。
 *
 * 🪤 这两件事以前在 UI 上是同一句话（一个空列表），而用户该做的事完全不同 ——
 * 一个是权限/挂载（要提权、要换 AuthID），另一个是「确实没玩过这个游戏」。
 * 用户的机器上明明有游戏，页面却只会说「没找到存档」，我们连该问什么都不知道：
 * 是 /user/home 打不开？还是打开了但里面没有账号目录？
 * 有了这个数组，回来的就是「哪个根 opendir 失败 / 看到几个账号 / 命中几个标题」，
 * 一句话就能定位。 */
typedef struct save_scan_stat {
  char root[PATH_MAX];
  int  opened;     /* opendir 成功 */
  int  accounts;   /* 只有 home 根才有意义 */
  int  titles;     /* 只有叶目录才有意义 */
} save_scan_stat_t;

#define SAVE_SCAN_MAX_STATS 48

static void
save_scan_note(save_scan_stat_t *stats, int *n, const char *root, int opened) {
  if(*n >= SAVE_SCAN_MAX_STATS) return;
  snprintf(stats[*n].root, sizeof(stats[*n].root), "%s", root);
  stats[*n].opened = opened;
  stats[*n].accounts = 0;
  stats[*n].titles = 0;
  (*n)++;
}

static void
save_scan_add_title(save_scan_stat_t *stats, int n, int titles) {
  if(n <= 0) return;
  stats[n - 1].titles += titles;
}

/* A save leaf holds more than games: sce_backupN (system backup slots),
 * per-account "user"/meta trees, restore safety copies. Users read the scan
 * list as "these are my games' saves", so anything that is not shaped like a
 * title id (4 uppercase letters + 5 digits: PPSA…, CUSA…, SCUS…) is noise and
 * is filtered out here — the exact rows the user circled in the save page
 * screenshot ("sce_backup4 · 0 B · savedata", "user · 7.9 MB · meta"). */
static int
save_name_is_title(const char *name) {
  size_t i, n = strlen(name);

  if(n != 9) return 0;
  for(i = 0; i < 4; i++)
    if(name[i] < 'A' || name[i] > 'Z') return 0;
  for(; i < 9; i++)
    if(name[i] < '0' || name[i] > '9') return 0;
  return 1;
}

/* Emit every title directory under "<base><leaf>". */
static void
scan_save_base(const char *base, strbuf_t *b, int *first,
               save_scan_stat_t *stats, int *nstats) {
  size_t l;

  for(l = 0; l < sizeof(g_save_leaves) / sizeof(g_save_leaves[0]); l++) {
    char leaf_dir[PATH_MAX];
    DIR *titles;
    struct dirent *td;
    int hits = 0;

    if(snprintf(leaf_dir, sizeof(leaf_dir), "%s%s", base, g_save_leaves[l]) >=
       (int)sizeof(leaf_dir)) continue;
    save_scan_note(stats, nstats, leaf_dir, 0);
    if(!(titles = opendir(leaf_dir))) continue;
    stats[*nstats - 1].opened = 1;
    while((td = readdir(titles))) {
      char title_dir[PATH_MAX];
      struct stat st;

      if(td->d_name[0] == '.') continue;
      /* 恢复前的安全副本住在这个叶里，它不是「一个游戏」（见上面那个 #define） */
      {
        size_t nl = strlen(td->d_name);
        size_t sl = strlen(SAVE_PRERESTORE_SUFFIX);
        if(nl > sl && !strcmp(td->d_name + nl - sl, SAVE_PRERESTORE_SUFFIX))
          continue;
      }
      if(snprintf(title_dir, sizeof(title_dir), "%s/%s", leaf_dir, td->d_name) >=
         (int)sizeof(title_dir)) continue;
      if(lstat(title_dir, &st) || !S_ISDIR(st.st_mode)) continue;
      if(!save_name_is_title(td->d_name)) continue;
      if(!*first) strbuf_append(b, ",");
      *first = 0;
      strbuf_append(b, "{\"title_id\":");
      json_escape(b, td->d_name);
      strbuf_append(b, ",\"path\":");
      json_escape(b, title_dir);
      strbuf_printf(b, ",\"size\":%llu,\"mtime\":%lld,\"kind\":",
                    save_dir_size(title_dir), (long long)st.st_mtime);
      json_escape(b, g_save_leaves[l]);
      strbuf_append(b, "}");
      hits++;
    }
    closedir(titles);
    save_scan_add_title(stats, *nstats, hits);
  }
}

/* A home root holds one directory per account; every account is a base.
 * `stats`/`nstats` 可为 NULL（其它调用方不关心诊断）。 */
static void
scan_save_accounts(const char *home_root, strbuf_t *b, int *first,
                   save_scan_stat_t *stats, int *nstats) {
  DIR *d = opendir(home_root);
  struct dirent *e;
  char base[PATH_MAX];
  int accounts = 0;
  int local_n = 0;
  save_scan_stat_t local_stats[SAVE_SCAN_MAX_STATS];

  if(!stats) { stats = local_stats; nstats = &local_n; }
  save_scan_note(stats, nstats, home_root, d ? 1 : 0);
  if(!d) return;
  while((e = readdir(d))) {
    if(e->d_name[0] == '.') continue;
    if(snprintf(base, sizeof(base), "%s%s/", home_root, e->d_name) >=
       (int)sizeof(base)) continue;
    accounts++;
    scan_save_base(base, b, first, stats, nstats);
  }
  closedir(d);
  stats[*nstats - 1].accounts = accounts;
}

/* Every title with save data on this console.
 *
 * The save page had no way to answer "what is on here": its only endpoint was
 * /api/save/list, which lists SNAPSHOTS — the things you already backed up — so
 * a console that had never been backed up showed an empty page no matter what.
 * This is the scan that page was missing, and it is also what makes a manually
 * typed title id answerable (see api_save_list's save_found).
 *
 * ★ 提权必须在扫描**之前**：/user/home/<账号hex> 属存档域，没有存档 AuthID
 *   （0x4800000000000010）时 opendir 会被拒 —— 于是扫描恒返回空列表，
 *   页面永远是「没有存档」，而备份/恢复那边因为先调了 save_escalate() 却是好的。
 *   这就是用户「手动输 ID 也查不到」的直接原因：查询和备份走的不是同一条权限路径。 */
enum MHD_Result
api_save_scan(struct MHD_Connection *conn, const char *body, size_t body_size) {
  const char *env = getenv("WFM_SAVE_ROOT");
  strbuf_t b = {0};
  save_scan_stat_t stats[SAVE_SCAN_MAX_STATS];
  int nstats = 0;
  int esc;
  int first = 1;
  size_t i;
  int k;

  (void)body;
  (void)body_size;

  esc = save_escalate();
  strbuf_append(&b, "{\"ok\":true,\"escalated\":");
  strbuf_append(&b, esc ? "false" : "true");
  strbuf_append(&b, ",\"saves\":[");
  if(env && *env) {
    /* Host test: one account home. */
    char base[PATH_MAX];
    size_t n = strlen(env);
    if(snprintf(base, sizeof(base), "%s%s", env,
                (n && env[n - 1] == '/') ? "" : "/") < (int)sizeof(base))
      scan_save_base(base, &b, &first, stats, &nstats);
  } else {
    for(i = 0; i < sizeof(g_save_home_roots) / sizeof(g_save_home_roots[0]); i++)
      scan_save_accounts(g_save_home_roots[i], &b, &first, stats, &nstats);
  }
  strbuf_append(&b, "],\"roots\":[");
  for(k = 0; k < nstats; k++) {
    if(k) strbuf_append(&b, ",");
    strbuf_append(&b, "{\"root\":");
    json_escape(&b, stats[k].root);
    strbuf_printf(&b, ",\"opened\":%s,\"accounts\":%d,\"titles\":%d}",
                  stats[k].opened ? "true" : "false",
                  stats[k].accounts, stats[k].titles);
  }
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
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
  strbuf_append(&b, "{\"ok\":true,\"title_id\":");
  json_escape(&b, tid);
  /* 「有没有存档」和「有没有快照」是两件不同的事。只报 snapshots 的时候，一台
     从没备份过的机器看起来永远是「这台机器上没有这个游戏的存档」——手动输入
     TITLE_ID 也一样是空的，用户根本无法区分「没存档」和「只是还没备份」。 */
  {
    char live[PATH_MAX];
    struct stat st;
    /* ★ 提权必须在 find_save_dir 之前，理由同 api_save_scan：没有存档 AuthID
       时 /user/home/<账号hex> 根本 opendir 不了。备份/恢复那边一直是对的，
       只有「查询」漏了这一步 —— 于是同一个存档「备份能备份、查询说没有」。 */
    save_escalate();
    int found = find_save_dir(tid, live, sizeof(live)) == 1 &&
                !lstat(live, &st) && S_ISDIR(st.st_mode);
    strbuf_append(&b, ",\"save_found\":");
    strbuf_append(&b, found ? "true" : "false");
    strbuf_append(&b, ",\"save_path\":");
    json_escape(&b, found ? live : "");
    strbuf_printf(&b, ",\"save_size\":%llu", found ? save_dir_size(live) : 0ULL);
  }
  strbuf_append(&b, ",\"snapshots\":[");
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
  snprintf(bak, sizeof(bak), "%s" SAVE_PRERESTORE_SUFFIX, dst);
  /* 上一轮的残留（进程被杀）/ 上一次失败的旧副本，先清干净再拍新的。
     ⚠️ 这里原来是 unlink()：对**目录**恒失败，所以安全副本一旦落地就再也不走，
     越攒越多、还会被 /api/save/scan 当成一个「游戏」。 */
  remove_tree(bak);
  if(copy_tree(dst, bak)) {
    task_update(task, TASK_FAILED, dst, 0, "could not safety-snapshot current save");
    return NULL;
  }
  if(copy_tree(snap, dst)) {
    copy_tree(bak, dst);   /* ROLLBACK */
    task_update(task, TASK_FAILED, dst, 0, "restore failed, rolled back");
    return NULL;
  }
  /* 回写成功 = 这次恢复已经结束，安全副本的使命完成：快照才是长期的备份，
     副本留在存档叶里只会占地方（而且用户会把它当成第二个存档）。 */
  remove_tree(bak);
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
/* 存档删除：/api/save/delete（删本机存档）· /api/save/snapdelete（删快照）  */
/* ====================================================================== */

/* 删除是条目计数型任务（前端 isCountKind(7) 按「项」显示），所以和
 * filemgr 的 TASK_DELETE worker 一样：先数一遍拿分母，再逐条删、逐条加进度。
 * 独立实现而不是复用 remove_path()：那条路径挂在 filemgr 的通用 worker 上，
 * 参数模型（多源、目标目录）对存档域是多余的表面积。 */

static unsigned long long
save_count_entries(const char *path) {
  DIR *d = opendir(path);
  struct dirent *e;
  unsigned long long n = 0;

  if(!d) return 0;
  while((e = readdir(d))) {
    char child[PATH_MAX];
    struct stat st;

    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if(snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >=
       (int)sizeof(child)) continue;
    if(lstat(child, &st)) continue;
    n += S_ISDIR(st.st_mode) ? save_count_entries(child) + 1 : 1;
  }
  closedir(d);
  return n;
}

/* 递归删除，每删掉一条（文件或目录）就 task_update 加 1 —— 与 save_count_entries
 * 的口径严格对齐（它也算「目录本身 +1」）。返回 0 成功 / -1 失败 / 1 取消。 */
static int
save_rm_task_r(const char *path, file_task_t *task) {
  DIR *d = opendir(path);
  struct dirent *e;
  int rc = 0;

  if(d) {
    while((e = readdir(d))) {
      char child[PATH_MAX];
      struct stat st;

      if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      if(task_cancel_requested(task)) { closedir(d); return 1; }
      if(snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >=
         (int)sizeof(child)) continue;
      if(lstat(child, &st)) continue;
      if(S_ISDIR(st.st_mode))
        rc = save_rm_task_r(child, task);
      else
        rc = (unlink(child) && errno != ENOENT) ? -1 : 0;
      if(rc) { closedir(d); return rc; }
      task_update(task, TASK_RUNNING, child, 1, NULL);
    }
    closedir(d);
  }
  if(rmdir(path) && errno != ENOENT) return -1;
  task_update(task, TASK_RUNNING, path, 1, NULL);
  return 0;
}

static void *
save_delete_worker(void *arg) {
  file_task_t *task = arg;
  char target[PATH_MAX];
  int rc;

  task_update(task, TASK_RUNNING, task->current, 0, NULL);
  snprintf(target, sizeof(target), "%s", task->src);
  if(task->dst[0]) {
    /* Deleting a LIVE save: the forced-snapshot rule applies here too — an
     * irreversible delete without a snapshot is exactly the failure mode this
     * domain must not have (same discipline as the restore worker). Snapshot
     * deletion (dst empty) is already the user discarding a chosen backup. */
    char snap_root[PATH_MAX], snap[PATH_MAX], stamp[32];
    snprintf(stamp, sizeof(stamp), "%llu", (unsigned long long)time(NULL));
    snprintf(snap_root, sizeof(snap_root), "%s/%s", save_snapshot_dir(), task->dst);
    snprintf(snap, sizeof(snap), "%s/%s", snap_root, stamp);
    mkdir_p(snap_root);
    if(copy_tree(target, snap)) {
      task_update(task, TASK_FAILED, target, 0,
                  "could not snapshot before delete");
      return NULL;
    }
  }
  task_set_total(task, save_count_entries(target) + 1);
  rc = save_rm_task_r(target, task);
  if(rc == 1)      task_update(task, TASK_CANCELED, target, 0, "canceled");
  else if(rc)      task_update(task, TASK_FAILED, target, 0, "delete failed");
  else             task_update(task, TASK_DONE, target, 0, NULL);
  return NULL;
}

/* Snapshot names are timestamps produced by save_backup_worker ("%llu").
 * Restricting to digits keeps the resolved path inside the snapshot root no
 * matter what the request body said (no "..", no separators, no absolute). */
static int
save_snapshot_name_ok(const char *name) {
  size_t i;

  if(!name || !*name) return 0;
  for(i = 0; name[i]; i++)
    if(name[i] < '0' || name[i] > '9') return 0;
  return 1;
}

/* POST /api/save/delete {title_id} —— 删掉这个游戏的本机存档。
 * ★ 强制快照铁律在这里同样生效：worker 先拍一份快照再删 —— 用户手滑删掉的
 * 存档必须能从快照里救回来。「不可逆 ⇒ 宁可功能少不可丢档」是本域第一规则。 */
enum MHD_Result
api_save_delete(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *tid = req_param(conn, body, body_size, "title_id");
  char live[PATH_MAX];
  file_task_t *t;

  if(!tid || !*tid) {
    free(tid);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid title id");
  }
  save_escalate();
  if(!find_save_dir(tid, live, sizeof(live))) {
    free(tid);
    return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                  "save data not found", "save_not_found", NULL);
  }
  /* task->src = 存档绝对路径（worker 删它），task->dst = TITLE_ID（worker
   * 用它落快照目录）。标签只放 TITLE_ID —— /api/tasks 会回显。 */
  t = queue_task_ex(TASK_DELETE, live, tid, tid, NULL, 0, save_delete_worker);
  free(tid);
  if(!t) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  return task_id_response(conn, t->id);
}

/* POST /api/save/snapdelete {title_id, snapshot} —— 删一份快照（不带存档）。
 * 快照是用户显式选择丢弃的备份，不做二次快照，但名字必须是纯数字时间戳。 */
enum MHD_Result
api_save_snapdelete(struct MHD_Connection *conn, const char *body,
                    size_t body_size) {
  char *tid = req_param(conn, body, body_size, "title_id");
  char *snap = req_param(conn, body, body_size, "snapshot");
  char dir[PATH_MAX], target[PATH_MAX];
  struct stat st;
  file_task_t *t;

  if(!tid || !*tid || !save_snapshot_name_ok(snap)) {
    free(tid); free(snap);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid snapshot");
  }
  snprintf(dir, sizeof(dir), "%s/%s", save_snapshot_dir(), tid);
  snprintf(target, sizeof(target), "%s/%s", dir, snap);
  if(stat(target, &st) || !S_ISDIR(st.st_mode)) {
    free(tid); free(snap);
    return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND,
                                  "snapshot not found", "snapshot_not_found", NULL);
  }
  /* task->src = 快照绝对路径；dst 留空 = 不用再拍快照（删的本来就是快照）。 */
  t = queue_task_ex(TASK_DELETE, target, "", tid, NULL, 0, save_delete_worker);
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

/* Install straight from a URL (typically http://<pc>:<port>/<file.pkg> served
 * by tools/serve-pkg.py). sceAppInstUtilInstallByPackage accepts http:// URIs
 * and the system installer downloads the file itself (Range requests), so the
 * PKG never touches the console disk. The task queue treats the URL like a
 * local path: pkg_installer_install() passes any non-/data/ string through to
 * metadata.uri verbatim. */
enum MHD_Result
api_pkg_install_url(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *url = req_param(conn, body, body_size, "url");
  unsigned long id = 0;
  int rc;

  if(!url || strlen(url) < sizeof("http://x/") - 1 ||
     strlen(url) > 512 || strncmp(url, "http://", 7) ||
     !memchr(url + 7, '/', strlen(url) - 7)) {
    free(url);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                  "url must start with http://",
                                  "pkg_url_invalid", NULL);
  }
  rc = filemgr_queue_pkg(url, &id);
  free(url);
  if(rc == PKG_INSTALL_UNSUPPORTED) {
    return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                  "package installation is only available on PS5",
                                  "pkg_install_unsupported", NULL);
  }
  if(rc) {
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "could not queue package installation");
  }
  return task_id_response(conn, id);
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
    if(task->error_code[0]) {
      strbuf_append(&b, ",\"error_code\":");
      json_escape(&b, task->error_code);
      if(task->error_arg[0]) {
        strbuf_append(&b, ",\"error_arg\":");
        json_escape(&b, task->error_arg);
      }
    }
    strbuf_append(&b, "}");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(!task) {
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "task not found");
  }
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

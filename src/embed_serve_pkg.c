/* The Windows helper the payload hands out: assets/serve-pkg.exe.
 *
 * Served straight off the payload's web root so the PS5 page can offer it as a
 * download — the whole point is that a user who can open http://<ps5>:2026/ has
 * no other place to get it from.
 *
 * WHY .incbin AND NOT gen-asset-module.py
 * The generator emits a C array with one initialiser per byte: a 9 MB binary
 * becomes ~55 MB of C source, and that single translation unit then dominates
 * every build. `.incbin` splices the same bytes into .rodata with none of it,
 * which is the only reason embedding a real executable stays practical.
 *
 * ⚠️ The path is resolved by the ASSEMBLER, relative to the directory make runs
 * in (the project root), not to this file. Do not "fix" it to ../assets/...
 */
#include <stddef.h>

void asset_register(const char *path, const void *data, size_t size,
                    const char *mime, const char *encoding);

__asm__(
  ".section .rodata\n"
  ".balign 16\n"
  ".globl wfm_serve_pkg_exe_start\n"
  "wfm_serve_pkg_exe_start:\n"
  ".incbin \"assets/serve-pkg.exe\"\n"
  ".globl wfm_serve_pkg_exe_end\n"
  "wfm_serve_pkg_exe_end:\n"
  ".previous\n");

extern const unsigned char wfm_serve_pkg_exe_start[];
extern const unsigned char wfm_serve_pkg_exe_end[];

__attribute__((constructor)) static void
embed_serve_pkg_constructor(void) {
  asset_register("/serve-pkg.exe", wfm_serve_pkg_exe_start,
                 (size_t)(wfm_serve_pkg_exe_end - wfm_serve_pkg_exe_start),
                 "application/octet-stream", 0);
}

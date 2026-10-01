ifneq ($(filter-out linux linux-deps clean,$(MAKECMDGOALS)),)
  ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
  else
    $(error PS5_PAYLOAD_SDK is undefined)
  endif
endif
ifeq ($(MAKECMDGOALS),)
  ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
  else
    $(error PS5_PAYLOAD_SDK is undefined)
  endif
endif

# ------------------------------------------------------------ version / identity
# ⚠️ VERSION_TAG MUST stay inside $(BIN) (see the program-name block below).
# The PS5 payload manager (and etaHEN's toolbox -> plugins/payload ELFs menu)
# lists payloads by FILE NAME, so a version that is not part of the file name is
# simply not visible on the console. That is exactly what the previous
# web-file-mgr-vX.Y.Z.elf naming bought, and what this build lost while it was
# still producing a bare NEXUS.elf.
# Side benefit: because the version is in the target name, bumping it also
# defeats make's "CFLAGS changed but nothing depends on it" staleness — a plain
# VERSION_TAG edit alone would silently reuse every stale object and link a
# binary that still reports the old version.
VERSION_TAG := v1.0.0
# ⚠️ TITLE_ID MUST match "titleId" in assets/param.json. app_installer.c uses
# TITLE_ID to create /user/app/<id>, but the PS5 launcher reads the title id out
# of the param.json we drop beside it; if they disagree the app is installed
# under a directory the launcher does not recognise. param.json says NEXS88888.
TITLE_ID    := NEXS88888
PYTHON      ?= python3
STRIP       ?= $(PS5_PAYLOAD_SDK)/bin/prospero-strip
PKG_CONFIG  ?= $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
HOST_CC     ?= cc
HOST_STRIP  ?= strip
HOST_PKG_CONFIG ?= pkg-config
# Set NAS=0 to drop the vendored SMB/NFS backends (see below).
NAS         ?= 1

# ---------------------------------------------------------------- program name
# ⚠️ The version goes in $(BIN) and deliberately NOT in PROCESS_NAME (src/main.c).
# That split is the whole point, so do not "tidy" it up:
#   * the payload manager lists payloads by FILE NAME -> the version must be in
#     the file name or the console shows nothing;
#   * main.c replaces older instances by matching the THREAD name against
#     PROCESS_NAME. If PROCESS_NAME carried the version, loading v1.0.1 while
#     v1.0.0 was still running would match nothing -> two live instances, and the
#     new one would quietly fall through to port 2027 instead of taking over.
# PROCESS_NAME is therefore the version-free stem; BIN carries the version.
BIN        := PS5-Nexus-$(VERSION_TAG).elf
LINUX_BIN  := PS5-Nexus-linux-$(VERSION_TAG)
COMMON_SRCS := src/main.c src/websrv.c src/filemgr.c src/file_response.c src/task.c \
  src/upload.c src/download.c src/text.c src/list.c src/space.c src/fs_util.c \
  src/json_util.c src/path_util.c src/asset.c src/mime.c src/notify.c \
  src/pkg_installer.c src/pkg_info.c src/nexus_port.c src/transfer.c \
  src/archive_extract.c src/archive_helper.c
PS5_SRCS    := $(COMMON_SRCS) src/app_installer.c
LINUX_SRCS  := $(COMMON_SRCS)
BASE_ASSETS := $(filter-out %.dds,$(wildcard assets/*))
ifneq ($(filter linux,$(MAKECMDGOALS)),)
ASSETS      := $(BASE_ASSETS)
else
ASSETS      := $(filter-out assets/icon0.png,$(BASE_ASSETS))
endif
GEN_SRCS    := $(patsubst assets/%,gen/%, $(ASSETS:=.c))

# ------------------------------------------------------- NAS backends (vendor)
# Vendored libsmb2 (SMB2/3) and libnfs (NFSv3/v4) behind src/transfer.c. Without
# them transfer_open() returns NULL for smb:/nfs: and the UI's NAS bookmarks can
# never connect — that is the whole "NAS 连不上" symptom.
#
#⚠️ They MUST be compiled in their own invocations, never folded into the single
# $(CC) command that builds our own sources: upstream code does not survive
# -Wall -Werror, and its per-library -I chain (two different files are called
# config.h) cannot coexist with ours in one command line. Hence the two object
# trees + pattern rules below, and the "-w" in their flag sets.
#
# Each library gets its OWN flag set. Both do `#ifdef HAVE_CONFIG_H #include
# "config.h"`, and defining HAVE_CONFIG_H globally would make unrelated TUs pick
# up one of these config.h files from a shared -I chain; scoping the macro to
# exactly the TUs that own the config.h avoids that.
#
# libsmb2's lib/*.c do `#include "smb2.h"` / "libsmb2-private.h", which live in
# include/smb2 -- hence the second -I. _U_ is upstream's unused-parameter
# annotation, defined so the vendor code compiles without us editing it.
SMB_TP_FLAGS := -O2 -w -Ithird_party/libsmb2 -Ithird_party/libsmb2/include \
  -Ithird_party/libsmb2/include/smb2 -DHAVE_CONFIG_H \
  "-D_U_=__attribute__((unused))"
# libnfs's generated rpcgen headers (libnfs-raw-*.h) sit next to their TUs in the
# mount/nfs/nfs4/nlm/nsm/portmap/rquota dirs, not in include/ -- upstream's
# lib/Makefile.am adds one -I per dir, and so do we.
NFS_TP_FLAGS := -O2 -w -Ithird_party/libnfs -Ithird_party/libnfs/include \
  -Ithird_party/libnfs/include/nfsc -Ithird_party/libnfs/mount \
  -Ithird_party/libnfs/nfs -Ithird_party/libnfs/nfs4 -Ithird_party/libnfs/nlm \
  -Ithird_party/libnfs/nsm -Ithird_party/libnfs/portmap \
  -Ithird_party/libnfs/rquota -DHAVE_CONFIG_H \
  "-D_U_=__attribute__((unused))"

ifeq ($(NAS),1)
TP_SRCS := $(wildcard third_party/libsmb2/lib/*.c) \
  $(wildcard third_party/libnfs/lib/*.c) $(wildcard third_party/libnfs/mount/*.c) \
  $(wildcard third_party/libnfs/nfs/*.c) $(wildcard third_party/libnfs/nfs4/*.c) \
  $(wildcard third_party/libnfs/nlm/*.c) $(wildcard third_party/libnfs/nsm/*.c) \
  $(wildcard third_party/libnfs/portmap/*.c) $(wildcard third_party/libnfs/rquota/*.c)
else
TP_SRCS :=
endif

PS5_OBJDIR     := ps5-obj
LINUX_OBJDIR   := linux-obj
PS5_TP_OBJS    := $(patsubst %.c,$(PS5_OBJDIR)/%.o,$(TP_SRCS))
LINUX_TP_OBJS  := $(patsubst %.c,$(LINUX_OBJDIR)/%.o,$(TP_SRCS))

CFLAGS := -Oz -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Werror -ffunction-sections -fdata-sections -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
CFLAGS += `$(PKG_CONFIG) libmicrohttpd --cflags`
LDFLAGS := -Wl,--gc-sections
LDADD  := `$(PKG_CONFIG) libmicrohttpd --libs`
LDADD  += -lSceIpmi -lSceAppInstUtil -lSceUserService
LINUX_CFLAGS := -O2 -flto -Wall -Werror -Isrc -DVERSION_TAG=\"$(VERSION_TAG)\" -DTITLE_ID=\"$(TITLE_ID)\"
LINUX_CFLAGS += `$(HOST_PKG_CONFIG) libmicrohttpd --cflags`
LINUX_LDADD := `$(HOST_PKG_CONFIG) libmicrohttpd --libs` -pthread

# ⚠️ NEXUS_HAVE_* for OUR OWN TUs (src/transfer.c) MUST sit AFTER the base
# CFLAGS / LINUX_CFLAGS ':=' assignments above. A later ':=' clobbers an earlier
# '+=', so appending them next to the vendor flags would leave the SMB/NFS glue
# #if'd out while the libraries still get linked (and then dropped by
# --gc-sections) -- i.e. exactly the silent "NAS never connects" failure.
ifeq ($(NAS),1)
CFLAGS       += -DNEXUS_HAVE_LIBSMB2 -Ithird_party/libsmb2/include
CFLAGS       += -DNEXUS_HAVE_LIBNFS -Ithird_party/libnfs/include -Ithird_party/libnfs/include/nfsc
LINUX_CFLAGS += -DNEXUS_HAVE_LIBSMB2 -Ithird_party/libsmb2/include
LINUX_CFLAGS += -DNEXUS_HAVE_LIBNFS -Ithird_party/libnfs/include -Ithird_party/libnfs/include/nfsc
endif

.PHONY: all linux deps linux-deps clean

all: deps $(BIN)

linux: linux-deps $(LINUX_BIN)

deps:
	@$(PKG_CONFIG) --exists libmicrohttpd || ./install-libmicrohttpd.sh

linux-deps:
	@$(HOST_PKG_CONFIG) --exists libmicrohttpd || \
	  (echo "libmicrohttpd development package is required for make linux" >&2; exit 1)

gen:
	mkdir gen

clean:
	rm -rf $(BIN) $(LINUX_BIN) gen $(PS5_OBJDIR) $(LINUX_OBJDIR)

gen/%.c: assets/% gen-asset-module.py | gen
	$(PYTHON) gen-asset-module.py --path $* $< > $@

# Vendor objects: PS5 tree (prospero-clang) and host tree (cc). Different
# architectures, so they must never share an object directory.
# ⚠️ config.h is an explicit prerequisite: it is hand-maintained (not
# autotools-generated) and we DO edit it, but make sees no dependency through
# `#include` -- without this line a config.h fix silently reuses the stale
# objects and the same link error comes back after an apparently clean edit.
$(PS5_OBJDIR)/third_party/libsmb2/%.o: third_party/libsmb2/%.c third_party/libsmb2/config.h
	@mkdir -p $(dir $@)
	$(CC) $(SMB_TP_FLAGS) -c -o $@ $<
$(PS5_OBJDIR)/third_party/libnfs/%.o: third_party/libnfs/%.c third_party/libnfs/config.h
	@mkdir -p $(dir $@)
	$(CC) $(NFS_TP_FLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/third_party/libsmb2/%.o: third_party/libsmb2/%.c third_party/libsmb2/config.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(SMB_TP_FLAGS) -c -o $@ $<
$(LINUX_OBJDIR)/third_party/libnfs/%.o: third_party/libnfs/%.c third_party/libnfs/config.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(NFS_TP_FLAGS) -c -o $@ $<

$(BIN): $(PS5_SRCS) $(GEN_SRCS) $(PS5_TP_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.c,$^) $(PS5_TP_OBJS) $(LDADD)
	$(STRIP) $@

$(LINUX_BIN): $(LINUX_SRCS) $(GEN_SRCS) $(LINUX_TP_OBJS)
	$(HOST_CC) $(LINUX_CFLAGS) -o $@ $(filter %.c,$^) $(LINUX_TP_OBJS) $(LINUX_LDADD)
	$(HOST_STRIP) $@

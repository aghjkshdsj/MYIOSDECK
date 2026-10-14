#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Metal objects of an ended 32-bit game (docs/SESSION_REUSE.md).

DXMT's winemetal (src/winemetal/unix/winemetal_unix.c, GPL-3.0-or-later, Madeira's DXMT fork)
creates a Metal texture or buffer for a game and hands it the +1 reference; the game gives it
back with NSObject_release. A game that ends without releasing them (ExitProcess skips most
teardown, a Force quit skips all of it) leaves them alive for the app run: with session reuse
every Steins;Gate start added about 330 MB (log of 2026-10-13: footprint 970 MB, 1.3 GB, 1.65 GB,
1.97 GB after four starts; DXMT's census counts 409 MB LIVE per game).

Each texture and buffer a 32-bit process creates is recorded with its guest window, and the
references that process holds are counted through NSObject_retain/release; the record takes one
reference of its own, so nothing is freed behind it. When Wine tears the window down
(virtual_ios.c's reclaim, engine/wine/patches/session_reuse.py), winemetal_release_window()
gives back the references the dead process still held, then its own. It stops giving back the
process's references as soon as only the record's own is left, so a reference given back by a
path the count did not see is never released twice. 64-bit processes are not recorded.

Usage: metal_owner.py <dxmt checkout>. Idempotent (marker line).
"""
import sys

MARK = "/* MYIOSDECK metal-owner */"

TRACKER = MARK + r'''
/* MYIOSDECK (engine/dxmt/patches/metal_owner.py): textures and buffers by the 32-bit process that
 * made them, so the window teardown of an ended game gives them back. */
#include <os/lock.h>
extern unsigned long ios_wow_base(void);
struct mo_entry { uintptr_t obj; unsigned long base; uint32_t refs; };
#define MO_SLOTS 65536   /* open addressing; obj 1 = a removed entry */
static struct mo_entry g_mo[MO_SLOTS];
static os_unfair_lock g_mo_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic unsigned g_mo_live;
static unsigned mo_hash(uintptr_t p) { return (unsigned)((p >> 4) * 2654435761u) & (MO_SLOTS - 1); }

/* the slot holding p, or -1 (lock held) */
static int mo_find(uintptr_t p)
{
  unsigned i = mo_hash(p), n;
  for (n = 0; n < MO_SLOTS; n++, i = (i + 1) & (MO_SLOTS - 1)) {
    if (g_mo[i].obj == p) return (int)i;
    if (!g_mo[i].obj) return -1;
  }
  return -1;
}

/* a texture or buffer a 32-bit process just made: its +1, and the record's own */
static void mo_add(obj_handle_t handle)
{
  uintptr_t p = (uintptr_t)handle;
  unsigned long base = ios_wow_base();
  unsigned i, n;
  int at;
  if (!p || !base || (wmtr_enabled() && RM_IS_REMOTE((uint64_t)p))) return;
  os_unfair_lock_lock(&g_mo_lock);
  if ((at = mo_find(p)) >= 0) {   /* a freed object's address, freed by a path not counted */
    g_mo[at].base = base;
    g_mo[at].refs = 1;
    os_unfair_lock_unlock(&g_mo_lock);
    [(NSObject *)p retain];
    return;
  }
  for (i = mo_hash(p), n = 0; n < MO_SLOTS; n++, i = (i + 1) & (MO_SLOTS - 1)) {
    if (g_mo[i].obj <= 1) {
      g_mo[i].obj = p;
      g_mo[i].base = base;
      g_mo[i].refs = 1;
      atomic_fetch_add_explicit(&g_mo_live, 1, memory_order_relaxed);
      os_unfair_lock_unlock(&g_mo_lock);
      [(NSObject *)p retain];   /* the record's own */
      return;
    }
  }
  os_unfair_lock_unlock(&g_mo_lock);   /* full: not recorded */
}

/* NSObject_retain/release from the guest: +1 or -1 on a recorded object. Returns 1 when the
 * process gave back its last reference: the record's own is then released by the caller. */
static int mo_ref(uintptr_t p, int delta)
{
  int at, last = 0;
  if (!p || !atomic_load_explicit(&g_mo_live, memory_order_relaxed)) return 0;
  os_unfair_lock_lock(&g_mo_lock);
  if ((at = mo_find(p)) >= 0) {
    if (delta > 0) g_mo[at].refs++;
    else if (g_mo[at].refs && !--g_mo[at].refs) {
      g_mo[at].obj = 1;
      atomic_fetch_sub_explicit(&g_mo_live, 1, memory_order_relaxed);
      last = 1;
    }
  }
  os_unfair_lock_unlock(&g_mo_lock);
  return last;
}

/* Wine's window teardown (virtual_ios.c): the dead process's objects go. */
void winemetal_release_window(unsigned long base)
{
  unsigned i, objs = 0, given = 0;
  if (!base || !atomic_load_explicit(&g_mo_live, memory_order_relaxed)) return;
  @autoreleasepool {
    for (i = 0; i < MO_SLOTS; i++) {
      NSObject *o;
      uint32_t refs;
      os_unfair_lock_lock(&g_mo_lock);
      if (g_mo[i].obj <= 1 || g_mo[i].base != base) { os_unfair_lock_unlock(&g_mo_lock); continue; }
      o = (NSObject *)g_mo[i].obj;
      refs = g_mo[i].refs;
      g_mo[i].obj = 1;
      atomic_fetch_sub_explicit(&g_mo_live, 1, memory_order_relaxed);
      os_unfair_lock_unlock(&g_mo_lock);
      for (; refs && [o retainCount] > 1; refs--, given++) [o release];
      [o release];   /* the record's own */
      objs++;
    }
  }
  fprintf(stderr, "[metal-owner] window B=%#lx: %u texture(s) and buffer(s) of the ended process given back "
          "(%u of its references)\n", base, objs, given);
}

#define MO_WRAP(name, type) \
  static NTSTATUS myiosdeck_orig##name(void *obj); \
  static NTSTATUS name(void *obj) { \
    NTSTATUS st = myiosdeck_orig##name(obj); \
    if (!st) mo_add(((type *)obj)->ret); \
    return st; \
  }
MO_WRAP(_MTLDevice_newBuffer, struct unixcall_mtldevice_newbuffer)
MO_WRAP(_MTLDevice_newTexture, struct unixcall_mtldevice_newtexture)
MO_WRAP(_MTLDevice_newSharedTexture, struct unixcall_mtldevice_newtexture)
MO_WRAP(_MTLBuffer_newTexture, struct unixcall_mtlbuffer_newtexture)
MO_WRAP(_MTLTexture_newTextureView, struct unixcall_mtltexture_newtextureview)

'''

FIXES = [
    ("static NTSTATUS\n_NSObject_retain(NSObject **obj) {\n",
     TRACKER + "static NTSTATUS\n_NSObject_retain(NSObject **obj) {\n"),
    ("  [*obj retain];\n  return STATUS_SUCCESS;\n",
     "  [*obj retain];\n"
     "  mo_ref((uintptr_t)*obj, 1);   " + MARK + "\n"
     "  return STATUS_SUCCESS;\n"),
    ("  [*obj release];\n  return STATUS_SUCCESS;\n",
     "  if (mo_ref((uintptr_t)*obj, -1)) [*obj release];   " + MARK + " the record's own, after the last\n"
     "  [*obj release];\n"
     "  return STATUS_SUCCESS;\n"),
    ("static NTSTATUS\n_MTLDevice_newBuffer(void *obj) {\n",
     "static NTSTATUS\nmyiosdeck_orig_MTLDevice_newBuffer(void *obj) {\n"),
    ("static NTSTATUS\n_MTLDevice_newTexture(void *obj) {\n",
     "static NTSTATUS\nmyiosdeck_orig_MTLDevice_newTexture(void *obj) {\n"),
    ("static NTSTATUS\n_MTLDevice_newSharedTexture(void *obj) {\n",
     "static NTSTATUS\nmyiosdeck_orig_MTLDevice_newSharedTexture(void *obj) {\n"),
    ("static NTSTATUS\n_MTLBuffer_newTexture(void *obj) {\n",
     "static NTSTATUS\nmyiosdeck_orig_MTLBuffer_newTexture(void *obj) {\n"),
    ("static NTSTATUS\n_MTLTexture_newTextureView(void *obj) {\n",
     "static NTSTATUS\nmyiosdeck_orig_MTLTexture_newTextureView(void *obj) {\n"),
]


def main():
    path = sys.argv[1] + "/src/winemetal/unix/winemetal_unix.c"
    s = open(path).read()
    if MARK in s:
        print("metal_owner: already applied")
        return
    for old, new in FIXES:
        if s.count(old) != 1:
            sys.exit("metal_owner.py: anchor found %d times: %r" % (s.count(old), old[:70]))
        s = s.replace(old, new)
    open(path, "w").write(s)
    print("metal_owner: applied to %s" % path)


if __name__ == "__main__":
    main()

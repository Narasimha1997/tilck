/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/string_util.h>

#include <tilck/kernel/fs/vfs.h>
#include <tilck/kernel/kmalloc.h>
#include <tilck/kernel/errno.h>
#include <tilck/kernel/process.h>
#include <tilck/kernel/user.h>

#include "fs_int.h"

#include <dirent.h> // system header

static u32 next_device_id;

/*
 * ----------------------------------------------------
 * VFS locking wrappers
 * ----------------------------------------------------
 */

void vfs_exlock(fs_handle h)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;

   if (hb->fops->exlock)
      hb->fops->exlock(h);
}

void vfs_exunlock(fs_handle h)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;

   if (hb->fops->exunlock)
      hb->fops->exunlock(h);
}

void vfs_shlock(fs_handle h)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;

   if (hb->fops->shlock)
      hb->fops->shlock(h);
}

void vfs_shunlock(fs_handle h)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;

   if (hb->fops->shunlock)
      hb->fops->shunlock(h);
}

void vfs_fs_exlock(filesystem *fs)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(fs != NULL);
   ASSERT(fs->fsops->fs_exlock);

   fs->fsops->fs_exlock(fs);
}

void vfs_fs_exunlock(filesystem *fs)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(fs != NULL);
   ASSERT(fs->fsops->fs_exunlock);

   fs->fsops->fs_exunlock(fs);
}

void vfs_fs_shlock(filesystem *fs)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(fs != NULL);
   ASSERT(fs->fsops->fs_shlock);

   fs->fsops->fs_shlock(fs);
}

void vfs_fs_shunlock(filesystem *fs)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(fs != NULL);
   ASSERT(fs->fsops->fs_shunlock);

   fs->fsops->fs_shunlock(fs);
}

static filesystem *
get_retained_fs_at(const char *path, const char **fs_path_ref)
{
   mountpoint *mp, *best_match = NULL;
   u32 len, pl, best_match_len = 0;
   filesystem *fs = NULL;
   mp_cursor cur;

   *fs_path_ref = NULL;
   pl = (u32)strlen(path);

   mountpoint_iter_begin(&cur);

   while ((mp = mountpoint_get_next(&cur))) {

      len = mp_check_match(mp->path, mp->path_len, path, pl);

      if (len > best_match_len) {
         best_match = mp;
         best_match_len = len;
      }
   }

   if (best_match) {
      *fs_path_ref = (best_match_len < pl) ? path + best_match_len - 1 : "/";
      fs = best_match->fs;
      retain_obj(fs);
   }

   mountpoint_iter_end(&cur);
   return fs;
}


/*
 * ----------------------------------------------------
 * Main VFS functions
 * ----------------------------------------------------
 */


static int vfs_resolve(filesystem *fs, const char *path, vfs_path *rp)
{
   func_get_entry get_entry = fs->fsops->get_entry;
   fs_path_struct e;
   const char *pc;
   void *idir;

   get_entry(fs, NULL, NULL, 0, &e);

   idir = e.inode; /* idir = root's inode */
   bzero(rp, sizeof(*rp));

   ASSERT(*path == '/');
   pc = ++path;

   /* Always set the `fs` field, no matter what */
   rp->fs = fs;

   if (!*path) {
      /* path was just "/" */
      rp->fs_path = e;
      rp->last_comp = path;
      return 0;
   }

   while (*path) {

      if (*path != '/') {
         path++;
         continue;
      }

      /*
       * We hit a slash '/' in the path: we now must lookup this path component.
       *
       * NOTE: the code in upper layers normalizes the user paths, but it makes
       * sense to ASSERT that.
       */

      ASSERT(path[1] != '/');

      get_entry(fs, idir, pc, path - pc, &e);

      if (!e.inode) {

         if (path[1])
            return -ENOENT; /* the path does NOT end here: no such entity */

         /* no such entity, but the path ends here, with a trailing slash */
         break;
      }

      /* We've found an entity for this path component (pc) */

      if (!path[1]) {

         /* the path ends here, with a trailing slash */

         if (e.type != VFS_DIR)
            return -ENOTDIR; /* that's a problem only if `e` is NOT a dir */

         break;
      }

      idir = e.inode;
      pc = ++path;
   }

   ASSERT(path - pc > 0);

   get_entry(fs, idir, pc, path - pc, &rp->fs_path);
   rp->last_comp = pc;
   return 0;
}

int vfs_open(const char *path, fs_handle *out, int flags, mode_t mode)
{
   const char *fs_path;
   filesystem *fs;
   vfs_path p;
   int rc;

   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(path != NULL);
   ASSERT(*path == '/'); /* VFS works only with absolute paths */

   if (flags & O_ASYNC)
      return -EINVAL; /* TODO: Tilck does not support ASYNC I/O yet */

   if ((flags & O_TMPFILE) == O_TMPFILE)
      return -EOPNOTSUPP; /* TODO: Tilck does not support O_TMPFILE yet */

   if (!(fs = get_retained_fs_at(path, &fs_path)))
      return -ENOENT;

   /* See the comment in vfs.h about the "fs-lock" funcs */
   vfs_fs_exlock(fs);
   {
      rc = vfs_resolve(fs, fs_path, &p);

      if (!rc)
         rc = fs->fsops->open(&p, out, flags, mode);
   }
   vfs_fs_exunlock(fs);

   if (rc == 0) {

      /* open() succeeded, the FS is already retained */
      ((fs_handle_base *) *out)->fl_flags = flags;

      if (flags & O_CLOEXEC)
         ((fs_handle_base *) *out)->fd_flags |= FD_CLOEXEC;

   } else {
      /* open() failed, we need to release the FS */
      release_obj(fs);
   }

   return rc;
}

void vfs_close(fs_handle h)
{
   /*
    * TODO: consider forcing also vfs_close() to be run always with preemption
    * enabled. Reason: when one day when actual I/O devices will be supported,
    * close() might need in some cases to do some I/O.
    *
    * What prevents vfs_close() to run with preemption enabled is the function
    * terminate_process() which requires disabled preemption, because of its
    * (primitive) sync with signal handling.
    */
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   filesystem *fs = hb->fs;

#ifndef UNIT_TEST_ENVIRONMENT
   process_info *pi = get_curr_task()->pi;
   remove_all_mappings_of_handle(pi, h);
#endif

   fs->fsops->close(h);
   release_obj(fs);

   /* while a filesystem is mounted, the minimum ref-count it can have is 1 */
   ASSERT(get_ref_count(fs) > 0);
}

int vfs_dup(fs_handle h, fs_handle *dup_h)
{
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   int rc;

   if (!hb)
      return -EBADF;

   if ((rc = hb->fs->fsops->dup(h, dup_h)))
      return rc;

   /* The new file descriptor does NOT share old file descriptor's fd_flags */
   ((fs_handle_base*) *dup_h)->fd_flags = 0;

   retain_obj(hb->fs);
   ASSERT(*dup_h != NULL);
   return 0;
}

ssize_t vfs_read(fs_handle h, void *buf, size_t buf_size)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   ssize_t ret;

   if (!hb->fops->read)
      return -EBADF;

   if ((hb->fl_flags & O_WRONLY) && !(hb->fl_flags & O_RDWR))
      return -EBADF; /* file not opened for reading */

   vfs_shlock(h);
   {
      ret = hb->fops->read(h, buf, buf_size);
   }
   vfs_shunlock(h);
   return ret;
}

ssize_t vfs_write(fs_handle h, void *buf, size_t buf_size)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   ssize_t ret;

   if (!hb->fops->write)
      return -EBADF;

   if (!(hb->fl_flags & (O_WRONLY | O_RDWR)))
      return -EBADF; /* file not opened for writing */

   vfs_exlock(h);
   {
      ret = hb->fops->write(h, buf, buf_size);
   }
   vfs_exunlock(h);
   return ret;
}

off_t vfs_seek(fs_handle h, s64 off, int whence)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);
   off_t ret;

   if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
      return -EINVAL; /* Tilck does NOT support SEEK_DATA and SEEK_HOLE */

   fs_handle_base *hb = (fs_handle_base *) h;

   if (!hb->fops->seek)
      return -ESPIPE;

   vfs_shlock(h);
   {
      // NOTE: this won't really work for big offsets in case off_t is 32-bit.
      ret = hb->fops->seek(h, (off_t) off, whence);
   }
   vfs_shunlock(h);
   return ret;
}

int vfs_ioctl(fs_handle h, uptr request, void *argp)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   int ret;

   if (!hb->fops->ioctl)
      return -ENOTTY; // Yes, ENOTTY *IS* the right error. See the man page.

   vfs_exlock(h);
   {
      ret = hb->fops->ioctl(h, request, argp);
   }
   vfs_exunlock(h);
   return ret;
}

int vfs_fstat64(fs_handle h, struct stat64 *statbuf)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(h != NULL);

   fs_handle_base *hb = (fs_handle_base *) h;
   int ret;

   vfs_shlock(h);
   {
      ret = hb->fs->fsops->fstat(h, statbuf);
   }
   vfs_shunlock(h);
   return ret;
}

int vfs_stat64(const char *path, struct stat64 *statbuf)
{
   fs_handle h = NULL;
   int rc;

   if ((rc = vfs_open(path, &h, O_RDONLY, 0)) < 0)
      return rc;

   /* If vfs_open() succeeded, `h` must be != NULL */
   ASSERT(h != NULL);

   rc = vfs_fstat64(h, statbuf);
   vfs_close(h);
   return 0;
}

typedef struct {

   fs_handle_base *h;
   struct linux_dirent64 *dirp;
   u32 buf_size;
   u32 offset;
   int curr_index;
   struct linux_dirent64 ent;

} vfs_getdents_ctx;

static inline unsigned char
vfs_type_to_linux_dirent_type(enum vfs_entry_type t)
{
   static const unsigned char table[] =
   {
      [VFS_NONE]        = DT_UNKNOWN,
      [VFS_FILE]        = DT_REG,
      [VFS_DIR]         = DT_DIR,
      [VFS_SYMLINK]     = DT_LNK,
      [VFS_CHAR_DEV]    = DT_CHR,
      [VFS_BLOCK_DEV]   = DT_BLK,
      [VFS_PIPE]        = DT_FIFO,
   };

   ASSERT(t != VFS_NONE);
   return table[t];
}

static int vfs_getdents_cb(vfs_dent64 *vde, void *arg)
{
   vfs_getdents_ctx *ctx = arg;

   if (ctx->curr_index < ctx->h->pos) {
      ctx->curr_index++;
      return 0; // continue
   }

   const u16 fl = (u16)strlen(vde->name);
   const u16 entry_size = sizeof(struct linux_dirent64) + fl + 1;

   if (ctx->offset + entry_size > ctx->buf_size) {

      if (!ctx->offset) {

         /*
         * We haven't "returned" any entries yet and the buffer is too small
         * for our first entry.
         */

         return -EINVAL;
      }

      /* We "returned" at least one entry */
      return (int) ctx->offset;
   }

   ctx->ent.d_ino = vde->ino;
   ctx->ent.d_off = ctx->offset + entry_size;
   ctx->ent.d_reclen = entry_size;
   ctx->ent.d_type = vfs_type_to_linux_dirent_type(vde->type);

   struct linux_dirent64 *user_ent = (void *)((char *)ctx->dirp + ctx->offset);

   if (copy_to_user(user_ent, &ctx->ent, sizeof(ctx->ent)) < 0)
      return -EFAULT;

   if (copy_to_user(user_ent->d_name, vde->name, fl + 1) < 0)
      return -EFAULT;

   ctx->offset += entry_size;
   ctx->curr_index++;
   ctx->h->pos++;
   return 0;
}

int vfs_getdents64(fs_handle h, struct linux_dirent64 *user_dirp, u32 buf_size)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   fs_handle_base *hb = (fs_handle_base *) h;
   int rc;

   ASSERT(hb != NULL);
   ASSERT(hb->fs->fsops->getdents_new);

   vfs_getdents_ctx ctx = {
      .h = hb,
      .dirp = user_dirp,
      .buf_size = buf_size,
      .offset = 0,
      .curr_index = 0,
      .ent = { 0 },
   };

   /* See the comment in vfs.h about the "fs-locks" */
   vfs_fs_shlock(hb->fs);
   {
      rc = hb->fs->fsops->getdents_new(hb, &vfs_getdents_cb, &ctx);

      if (!rc)
         rc = (int) ctx.offset;
   }
   vfs_fs_shunlock(hb->fs);
   return rc;
}

int vfs_fcntl(fs_handle h, int cmd, int arg)
{
   NO_TEST_ASSERT(is_preemption_enabled());
   fs_handle_base *hb = (fs_handle_base *) h;
   int ret;

   if (!hb->fops->fcntl)
      return -EINVAL;

   vfs_exlock(h);
   {
      ret = hb->fops->fcntl(h, cmd, arg);
   }
   vfs_exunlock(h);
   return ret;
}

int vfs_mkdir(const char *path, mode_t mode)
{
   const char *fs_path;
   filesystem *fs;
   vfs_path p;
   int rc;

   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(path != NULL);
   ASSERT(*path == '/'); /* VFS works only with absolute paths */

   if (!(fs = get_retained_fs_at(path, &fs_path)))
      return -ENOENT;

   if (!(fs->flags & VFS_FS_RW))
      return -EROFS;

   if (!fs->fsops->mkdir)
      return -EPERM;

   /* See the comment in vfs.h about the "fs-lock" funcs */
   vfs_fs_exlock(fs);
   {
      rc = vfs_resolve(fs, fs_path, &p);

      if (!rc)
         rc = fs->fsops->mkdir(&p, mode);
   }
   vfs_fs_exunlock(fs);
   release_obj(fs);     /* it was retained by get_retained_fs_at() */
   return rc;
}

int vfs_rmdir(const char *path)
{
   const char *fs_path;
   filesystem *fs;
   vfs_path p;
   int rc;

   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(path != NULL);
   ASSERT(*path == '/'); /* VFS works only with absolute paths */

   if (!(fs = get_retained_fs_at(path, &fs_path)))
      return -ENOENT;

   if (!(fs->flags & VFS_FS_RW))
      return -EROFS;

   if (!fs->fsops->rmdir)
      return -EPERM;

   /* See the comment in vfs.h about the "fs-lock" funcs */
   vfs_fs_exlock(fs);
   {
      rc = vfs_resolve(fs, fs_path, &p);

      if (!rc)
         rc = fs->fsops->rmdir(&p);
   }
   vfs_fs_exunlock(fs);
   release_obj(fs);     /* it was retained by get_retained_fs_at() */
   return rc;
}

int vfs_unlink(const char *path)
{
   const char *fs_path;
   filesystem *fs;
   vfs_path p;
   int rc;

   NO_TEST_ASSERT(is_preemption_enabled());
   ASSERT(path != NULL);
   ASSERT(*path == '/'); /* VFS works only with absolute paths */

   if (!(fs = get_retained_fs_at(path, &fs_path)))
      return -ENOENT;

   if (!(fs->flags & VFS_FS_RW))
      return -EROFS;

   if (!fs->fsops->unlink)
      return -EROFS;

   /* See the comment in vfs.h about the "fs-lock" funcs */
   vfs_fs_exlock(fs);
   {
      rc = vfs_resolve(fs, fs_path, &p);

      if (!rc)
         rc = fs->fsops->unlink(&p);
   }
   vfs_fs_exunlock(fs);
   release_obj(fs);     /* it was retained by get_retained_fs_at() */
   return rc;
}

u32 vfs_get_new_device_id(void)
{
   return next_device_id++;
}

/*
 * ----------------------------------------------------
 * Ready-related VFS functions
 * ----------------------------------------------------
 */

bool vfs_read_ready(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   bool r;

   if (!hb->fops->read_ready)
      return true;

   vfs_shlock(h);
   {
      r = hb->fops->read_ready(h);
   }
   vfs_shunlock(h);
   return r;
}

bool vfs_write_ready(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   bool r;

   if (!hb->fops->write_ready)
      return true;

   vfs_shlock(h);
   {
      r = hb->fops->write_ready(h);
   }
   vfs_shunlock(h);
   return r;
}

bool vfs_except_ready(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;
   bool r;

   if (!hb->fops->except_ready)
      return false;

   vfs_shlock(h);
   {
      r = hb->fops->except_ready(h);
   }
   vfs_shunlock(h);
   return r;
}

kcond *vfs_get_rready_cond(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;

   if (!hb->fops->get_rready_cond)
      return NULL;

   return hb->fops->get_rready_cond(h);
}

kcond *vfs_get_wready_cond(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;

   if (!hb->fops->get_wready_cond)
      return NULL;

   return hb->fops->get_wready_cond(h);
}

kcond *vfs_get_except_cond(fs_handle h)
{
   fs_handle_base *hb = (fs_handle_base *) h;

   if (!hb->fops->get_except_cond)
      return NULL;

   return hb->fops->get_except_cond(h);
}

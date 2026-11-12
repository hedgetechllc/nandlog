// The record logger over one littlefs file. See lfs_bd.h. Baseline harness code.
#include <string.h>
#include "lfs_bd.h"

// The record logger over one littlefs file ----------------------------------------------------------------------------

int lfa_mount(lfa_t *a, const struct lfs_config *cfg, void *file_buf, bool allow_format)
{
   memset(a, 0, sizeof(*a));
   int err = lfs_mount(&a->lfs, cfg);
   if (err && !allow_format)
      return err;
   if (err)
   {
      if ((err = lfs_format(&a->lfs, cfg)) != 0)
         return err;
      if ((err = lfs_mount(&a->lfs, cfg)) != 0)
         return err;
   }
   a->file_cfg.buffer = file_buf;
   return lfs_file_opencfg(&a->lfs, &a->file, "log", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND, &a->file_cfg);
}

int lfa_append(lfa_t *a, uint8_t type, uint32_t timestamp, const void *data, uint16_t length)
{
   uint8_t frame[7];
   memcpy(frame, &length, 2);
   frame[2] = type;
   memcpy(frame + 3, &timestamp, 4);
   if (lfs_file_write(&a->lfs, &a->file, frame, sizeof(frame)) != (lfs_ssize_t)sizeof(frame))
      return -1;
   if (lfs_file_write(&a->lfs, &a->file, data, length) != (lfs_ssize_t)length)
      return -1;
   return 0;
}

int lfa_flush(lfa_t *a)
{
   return lfs_file_sync(&a->lfs, &a->file);
}

int lfa_unmount(lfa_t *a)
{
   const int err = lfs_file_close(&a->lfs, &a->file);
   const int err2 = lfs_unmount(&a->lfs);
   return err ? err : err2;
}

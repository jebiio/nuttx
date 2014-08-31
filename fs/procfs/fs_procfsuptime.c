/****************************************************************************
 * fs/procfs/fs_procfsuptime.c
 *
 *   Copyright (C) 2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/statfs.h>
#include <sys/stat.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/procfs.h>

#if !defined(CONFIG_DISABLE_MOUNTPOINT) && defined(CONFIG_FS_PROCFS)
#ifndef CONFIG_FS_PROCFS_EXCLUDE_PROCESS

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
/* Determines the size of an intermediate buffer that must be large enough
 * to handle the longest line generated by this logic.
 */

#define UPTIME_LINELEN 16

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes one open "file" */

struct uptime_file_s
{
  struct procfs_file_s  base;        /* Base open file structure */
  unsigned int linesize;             /* Number of valid characters in line[] */
  char line[UPTIME_LINELEN];         /* Pre-allocated buffer for formatted lines */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* File system methods */

static int     uptime_open(FAR struct file *filep, FAR const char *relpath,
                 int oflags, mode_t mode);
static int     uptime_close(FAR struct file *filep);
static ssize_t uptime_read(FAR struct file *filep, FAR char *buffer,
                 size_t buflen);

static int     uptime_dup(FAR const struct file *oldp,
                 FAR struct file *newp);

static int     uptime_stat(FAR const char *relpath, FAR struct stat *buf);

/****************************************************************************
 * Private Variables
 ****************************************************************************/

/****************************************************************************
 * Public Variables
 ****************************************************************************/

/* See fs_mount.c -- this structure is explicitly externed there.
 * We use the old-fashioned kind of initializers so that this will compile
 * with any compiler.
 */

const struct procfs_operations uptime_operations =
{
  uptime_open,       /* open */
  uptime_close,      /* close */
  uptime_read,       /* read */
  NULL,               /* write */

  uptime_dup,        /* dup */

  NULL,              /* opendir */
  NULL,              /* closedir */
  NULL,              /* readdir */
  NULL,              /* rewinddir */

  uptime_stat        /* stat */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: uptime_open
 ****************************************************************************/

static int uptime_open(FAR struct file *filep, FAR const char *relpath,
                      int oflags, mode_t mode)
{
  FAR struct uptime_file_s *attr;

  fvdbg("Open '%s'\n", relpath);

  /* PROCFS is read-only.  Any attempt to open with any kind of write
   * access is not permitted.
   *
   * REVISIT:  Write-able proc files could be quite useful.
   */

  if ((oflags & O_WRONLY) != 0 || (oflags & O_RDONLY) == 0)
    {
      fdbg("ERROR: Only O_RDONLY supported\n");
      return -EACCES;
    }

  /* "uptime" is the only acceptable value for the relpath */

  if (strcmp(relpath, "uptime") != 0)
    {
      fdbg("ERROR: relpath is '%s'\n", relpath);
      return -ENOENT;
    }

  /* Allocate a container to hold the file attributes */

  attr = (FAR struct uptime_file_s *)kzalloc(sizeof(struct uptime_file_s));
  if (!attr)
    {
      fdbg("ERROR: Failed to allocate file attributes\n");
      return -ENOMEM;
    }

  /* Save the attributes as the open-specific state in filep->f_priv */

  filep->f_priv = (FAR void *)attr;
  return OK;
}

/****************************************************************************
 * Name: uptime_close
 ****************************************************************************/

static int uptime_close(FAR struct file *filep)
{
  FAR struct uptime_file_s *attr;

  /* Recover our private data from the struct file instance */

  attr = (FAR struct uptime_file_s *)filep->f_priv;
  DEBUGASSERT(attr);

  /* Release the file attributes structure */

  kmm_free(attr);
  filep->f_priv = NULL;
  return OK;
}

/****************************************************************************
 * Name: uptime_read
 ****************************************************************************/

static ssize_t uptime_read(FAR struct file *filep, FAR char *buffer,
                           size_t buflen)
{
  FAR struct uptime_file_s *attr;
  size_t linesize;
  off_t offset;
  ssize_t ret;

#ifdef CONFIG_SYSTEM_TIME64
  uint64_t ticktime;
#if !defined(CONFIG_HAVE_DOUBLE) || !defined(CONFIG_LIBC_FLOATINGPOINT)
  uint64_t sec;
#endif

#else
  uint32_t ticktime;
#if !defined(CONFIG_HAVE_DOUBLE) || !defined(CONFIG_LIBC_FLOATINGPOINT)
  uint32_t sec;
#endif
#endif

#if defined(CONFIG_HAVE_DOUBLE) && defined(CONFIG_LIBC_FLOATINGPOINT)
  double now;
#else
  unsigned int remainder;
  unsigned int csec;
#endif

  fvdbg("buffer=%p buflen=%d\n", buffer, (int)buflen);

  /* Recover our private data from the struct file instance */

  attr = (FAR struct uptime_file_s *)filep->f_priv;
  DEBUGASSERT(attr);

  /* If f_pos is zero, then sample the system time.  Otherwise, use
   * the cached system time from the previous read().  It is necessary
   * save the cached value in case, for example, the user is reading
   * the time one byte at a time.  In that case, the time must remain
   * stable throughout the reads.
   */

  if (filep->f_pos == 0)
    {
#ifdef CONFIG_SYSTEM_TIME64
      /* 64-bit timer */

      ticktime = clock_systimer64();
#else
      /* 32-bit timer */

      ticktime = clock_systimer();
#endif

#if defined(CONFIG_HAVE_DOUBLE) && defined(CONFIG_LIBC_FLOATINGPOINT)
      /* Convert the system up time to a seconds + hundredths of seconds string */

      now       = (double)ticktime / (double)CLOCKS_PER_SEC;
      linesize  = snprintf(attr->line, UPTIME_LINELEN, "%10.2f\n", now);

#else
      /* Convert the system up time to seconds + hundredths of seconds */

      sec       = ticktime / CLOCKS_PER_SEC;
      remainder = (unsigned int)(ticktime % CLOCKS_PER_SEC);
      csec      = (100 * remainder + (CLOCKS_PER_SEC / 2)) / CLOCKS_PER_SEC;

      /* Make sure that rounding did not force the hundredths of a second above 99 */

      if (csec > 99)
        {
          sec++;
          csec -= 100;
        }

      /* Convert the seconds + hundredths of seconds to a string */

      linesize = snprintf(attr->line, UPTIME_LINELEN, "%7lu.%02u\n", sec, csec);

#endif
      /* Save the linesize in case we are re-entered with f_pos > 0 */

      attr->linesize = linesize;
    }

  /* Transfer the system up time to user receive buffer */

  offset = filep->f_pos;
  ret    = procfs_memcpy(attr->line, attr->linesize, buffer, buflen, &offset);

  /* Update the file offset */

  if (ret > 0)
    {
      filep->f_pos += ret;
    }

  return ret;
}

/****************************************************************************
 * Name: uptime_dup
 *
 * Description:
 *   Duplicate open file data in the new file structure.
 *
 ****************************************************************************/

static int uptime_dup(FAR const struct file *oldp, FAR struct file *newp)
{
  FAR struct uptime_file_s *oldattr;
  FAR struct uptime_file_s *newattr;

  fvdbg("Dup %p->%p\n", oldp, newp);

  /* Recover our private data from the old struct file instance */

  oldattr = (FAR struct uptime_file_s *)oldp->f_priv;
  DEBUGASSERT(oldattr);

  /* Allocate a new container to hold the task and attribute selection */

  newattr = (FAR struct uptime_file_s *)kmalloc(sizeof(struct uptime_file_s));
  if (!newattr)
    {
      fdbg("ERROR: Failed to allocate file attributes\n");
      return -ENOMEM;
    }

  /* The copy the file attributes from the old attributes to the new */

  memcpy(newattr, oldattr, sizeof(struct uptime_file_s));

  /* Save the new attributes in the new file structure */

  newp->f_priv = (FAR void *)newattr;
  return OK;
}

/****************************************************************************
 * Name: uptime_stat
 *
 * Description: Return information about a file or directory
 *
 ****************************************************************************/

static int uptime_stat(const char *relpath, struct stat *buf)
{
  /* "uptime" is the only acceptable value for the relpath */

  if (strcmp(relpath, "uptime") != 0)
    {
      fdbg("ERROR: relpath is '%s'\n", relpath);
      return -ENOENT;
    }

  /* "uptime" is the name for a read-only file */

  buf->st_mode    = S_IFREG|S_IROTH|S_IRGRP|S_IRUSR;
  buf->st_size    = 0;
  buf->st_blksize = 0;
  buf->st_blocks  = 0;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#endif /* CONFIG_FS_PROCFS_EXCLUDE_PROCESS */
#endif /* !CONFIG_DISABLE_MOUNTPOINT && CONFIG_FS_PROCFS */

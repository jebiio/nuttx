/****************************************************************************
 * drivers/video/fb.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* Framebuffer character driver */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <poll.h>

#include <nuttx/kmalloc.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/video/fb.h>
#include <nuttx/clock.h>
#include <nuttx/wdog.h>
#include <nuttx/mm/circbuf.h>

/****************************************************************************
 * Pre-processor definitions
 ****************************************************************************/

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct fb_priv_s
{
  int overlay;                    /* Overlay number */
};

struct fb_paninfo_s
{
  FAR struct circbuf_s buf;       /* Pan buffer queued list */

  /* Polling fds of waiting threads */

  FAR struct pollfd *fds[CONFIG_VIDEO_FB_NPOLLWAITERS];
};

/* This structure defines one framebuffer device.  Note that which is
 * everything in this structure is constant data set up and initialization
 * time.  Therefore, no there is requirement for serialized access to this
 * structure.
 */

struct fb_chardev_s
{
  FAR struct fb_vtable_s *vtable;   /* Framebuffer interface */
  uint8_t plane;                    /* Video plan number */
  clock_t vsyncoffset;              /* VSync offset ticks */
  struct wdog_s wdog;               /* VSync offset timer */
  mutex_t lock;                     /* Mutual exclusion */
  int16_t crefs;                    /* Number of open references */
  FAR struct fb_paninfo_s *paninfo; /* Pan info array */
  size_t paninfo_count;             /* Pan info count */
};

struct fb_panelinfo_s
{
  FAR void *fbmem;                /* Start of frame buffer memory */
  size_t fblen;                   /* Size of the framebuffer */
  uint8_t fbcount;                /* Count of frame buffer */
  uint8_t bpp;                    /* Bits per pixel */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static FAR struct pollfd **fb_get_free_pollfds(FAR struct fb_chardev_s *fb,
                                               int overlay);
static FAR struct circbuf_s *fb_get_panbuf(FAR struct fb_chardev_s *fb,
                                           int overlay);
static int     fb_add_paninfo(FAR struct fb_vtable_s *vtable,
                              FAR const union fb_paninfo_u *info,
                              int overlay);
static int     fb_open(FAR struct file *filep);
static int     fb_close(FAR struct file *filep);
static ssize_t fb_read(FAR struct file *filep, FAR char *buffer,
                       size_t buflen);
static ssize_t fb_write(FAR struct file *filep, FAR const char *buffer,
                        size_t buflen);
static off_t   fb_seek(FAR struct file *filep, off_t offset, int whence);
static int     fb_ioctl(FAR struct file *filep, int cmd, unsigned long arg);
static int     fb_mmap(FAR struct file *filep,
                       FAR struct mm_map_entry_s *map);
static int     fb_poll(FAR struct file *filep, FAR struct pollfd *fds,
                       bool setup);
static int     fb_get_panelinfo(FAR struct fb_chardev_s *fb,
                                FAR struct fb_panelinfo_s *panelinfo,
                                int overlay);
static int     fb_get_planeinfo(FAR struct fb_chardev_s *fb,
                                FAR struct fb_planeinfo_s *pinfo,
                                uint8_t display);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_fb_fops =
{
  fb_open,       /* open */
  fb_close,      /* close */
  fb_read,       /* read */
  fb_write,      /* write */
  fb_seek,       /* seek */
  fb_ioctl,      /* ioctl */
  fb_mmap,       /* mmap */
  NULL,          /* truncate */
  fb_poll        /* poll */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: fb_get_free_pollfds
 ****************************************************************************/

static FAR struct pollfd **fb_get_free_pollfds(FAR struct fb_chardev_s *fb,
                                               int overlay)
{
  FAR struct fb_paninfo_s *paninfo;
  int id = overlay + 1;
  int i;

  DEBUGASSERT(id >= 0 && id < fb->paninfo_count);

  paninfo = &fb->paninfo[id];

  for (i = 0; i < CONFIG_VIDEO_FB_NPOLLWAITERS; ++i)
    {
      if (!paninfo->fds[i])
        {
          return &paninfo->fds[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: fb_get_panbuf
 ****************************************************************************/

static FAR struct circbuf_s *fb_get_panbuf(FAR struct fb_chardev_s *fb,
                                           int overlay)
{
  int id = overlay + 1;

  DEBUGASSERT(id >= 0 && id < fb->paninfo_count);

  return &(fb->paninfo[id].buf);
}

/****************************************************************************
 * Name: fb_add_paninfo
 ****************************************************************************/

static int fb_add_paninfo(FAR struct fb_vtable_s *vtable,
                          FAR const union fb_paninfo_u *info,
                          int overlay)
{
  FAR struct circbuf_s *panbuf;
  FAR struct fb_chardev_s *fb;
  irqstate_t flags;
  ssize_t ret;

  DEBUGASSERT(vtable != NULL);

  /* Prevent calling before getting the vtable. */

  fb = vtable->priv;
  if (fb == NULL)
    {
      return -EINVAL;
    }

  panbuf = fb_get_panbuf(fb, overlay);
  if (panbuf == NULL)
    {
      return -EINVAL;
    }

  /* Disable the interrupt when writing to the queue to
   * prevent it from being modified by the interrupted
   * thread during the writing process.
   */

  flags = enter_critical_section();

  /* Write planeinfo information to the queue. */

  ret = circbuf_write(panbuf, info, sizeof(union fb_paninfo_u));
  DEBUGASSERT(ret == sizeof(union fb_paninfo_u));

  /* Re-enable interrupts */

  leave_critical_section(flags);
  return ret <= 0 ? -ENOSPC : OK;
}

/****************************************************************************
 * Name: fb_open
 ****************************************************************************/

static int fb_open(FAR struct file *filep)
{
  FAR struct inode *inode;
  FAR struct fb_chardev_s *fb;
  FAR struct fb_priv_s *priv;
  int ret;

  inode = filep->f_inode;
  fb    = inode->i_private;

  DEBUGASSERT(fb->vtable != NULL);

  ret = nxmutex_lock(&fb->lock);
  if (ret < 0)
    {
      return ret;
    }

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      ret = -ENOMEM;
      goto err_out;
    }

  if (fb->crefs == 0)
    {
      if (fb->vtable->open != NULL &&
          (ret = fb->vtable->open(fb->vtable)) < 0)
        {
          goto err_fb;
        }
    }

  fb->crefs++;
  DEBUGASSERT(fb->crefs > 0);

  priv->overlay = FB_NO_OVERLAY;
  filep->f_priv = priv;

  nxmutex_unlock(&fb->lock);
  return 0;

err_fb:
  kmm_free(priv);
err_out:
  nxmutex_unlock(&fb->lock);
  return ret;
}

/****************************************************************************
 * Name: fb_close
 ****************************************************************************/

static int fb_close(FAR struct file *filep)
{
  FAR struct inode *inode;
  FAR struct fb_chardev_s *fb;
  FAR struct fb_priv_s *priv;
  int ret;

  inode = filep->f_inode;
  fb    = inode->i_private;
  priv  = filep->f_priv;

  DEBUGASSERT(fb->vtable != NULL && priv != NULL);

  ret = nxmutex_lock(&fb->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (fb->crefs == 1)
    {
      if (fb->vtable->close != NULL)
        {
          ret = fb->vtable->close(fb->vtable);
        }
    }

  if (ret >= 0)
    {
      DEBUGASSERT(fb->crefs > 0);
      fb->crefs--;
      kmm_free(priv);
    }

  nxmutex_unlock(&fb->lock);
  return ret;
}

/****************************************************************************
 * Name: fb_read
 ****************************************************************************/

static ssize_t fb_read(FAR struct file *filep, FAR char *buffer, size_t len)
{
  FAR struct inode *inode;
  FAR struct fb_chardev_s *fb;
  FAR struct fb_priv_s *priv;
  struct fb_panelinfo_s panelinfo;
  size_t start;
  size_t end;
  size_t size;
  int ret;

  ginfo("len: %u\n", (unsigned int)len);

  /* Get the framebuffer instance */

  inode = filep->f_inode;
  fb    = inode->i_private;
  priv  = filep->f_priv;

  DEBUGASSERT(fb->vtable != NULL && priv != NULL);

  /* Get panel info */

  ret = fb_get_panelinfo(fb, &panelinfo, priv->overlay);

  if (ret < 0)
    {
      return ret;
    }

  /* Get the start and size of the transfer */

  start = filep->f_pos;
  if (start >= panelinfo.fblen)
    {
      return 0;  /* Return end-of-file */
    }

  end = start + len;
  if (end >= panelinfo.fblen)
    {
      end = panelinfo.fblen;
    }

  size = end - start;

  /* And transfer the data from the frame buffer */

  memcpy(buffer, panelinfo.fbmem + start, size);
  filep->f_pos += size;
  return size;
}

/****************************************************************************
 * Name: fb_write
 ****************************************************************************/

static ssize_t fb_write(FAR struct file *filep, FAR const char *buffer,
                        size_t len)
{
  FAR struct inode *inode;
  FAR struct fb_chardev_s *fb;
  FAR struct fb_priv_s *priv;
  struct fb_panelinfo_s panelinfo;
  size_t start;
  size_t end;
  size_t size;
  int ret;

  ginfo("len: %u\n", (unsigned int)len);

  /* Get the framebuffer instance */

  inode = filep->f_inode;
  fb    = inode->i_private;
  priv  = filep->f_priv;

  DEBUGASSERT(fb->vtable != NULL && priv != NULL);

  /* Get panel info */

  ret = fb_get_panelinfo(fb, &panelinfo, priv->overlay);

  if (ret < 0)
    {
      return ret;
    }

  /* Get the start and size of the transfer */

  start = filep->f_pos;
  if (start >= panelinfo.fblen)
    {
      return -EFBIG;  /* Cannot extend the framebuffer */
    }

  end = start + len;
  if (end >= panelinfo.fblen)
    {
      end = panelinfo.fblen;
    }

  size = end - start;

  /* And transfer the data into the frame buffer */

  memcpy(panelinfo.fbmem + start, buffer, size);
  filep->f_pos += size;
  return size;
}

/****************************************************************************
 * Name: fb_seek
 *
 * Description:
 *   Seek the logical file pointer to the specified position.  The offset
 *   is in units of pixels, with offset zero being the beginning of the
 *   framebuffer.
 *
 ****************************************************************************/

static off_t fb_seek(FAR struct file *filep, off_t offset, int whence)
{
  FAR struct inode *inode;
  FAR struct fb_chardev_s *fb;
  FAR struct fb_priv_s *priv;
  struct fb_panelinfo_s panelinfo;
  off_t newpos;
  int ret;

  ginfo("offset: %u whence: %d\n", (unsigned int)offset, whence);

  /* Get the framebuffer instance */

  inode = filep->f_inode;
  fb    = inode->i_private;
  priv  = filep->f_priv;

  DEBUGASSERT(fb->vtable != NULL && priv != NULL);

  /* Determine the new, requested file position */

  switch (whence)
    {
    case SEEK_CUR:
      newpos = filep->f_pos + offset;
      break;

    case SEEK_SET:
      newpos = offset;
      break;

    case SEEK_END:

      /* Get panel info */

      ret = fb_get_panelinfo(fb, &panelinfo, priv->overlay);

      if (ret < 0)
        {
          return ret;
        }

      newpos = panelinfo.fblen + offset;
      break;

    default:

      /* Return EINVAL if the whence argument is invalid */

      return -EINVAL;
    }

  /* Opengroup.org:
   *
   *  "The lseek() function shall allow the file offset to be set beyond the
   *   end of the existing data in the file. If data is later written at this
   *   point, subsequent reads of data in the gap shall return bytes with the
   *   value 0 until data is actually written into the gap."
   *
   * We can conform to the first part, but not the second.  Return EINVAL if
   *  "...the resulting file offset would be negative for a regular file,
   *   block special file, or directory."
   */

  if (newpos >= 0)
    {
      filep->f_pos = newpos;
      ret = newpos;
    }
  else
    {
      ret = -EINVAL;
    }

  return ret;
}

/****************************************************************************
 * Name: fb_ioctl
 *
 * Description:
 *   The standard ioctl method.
 *
 ****************************************************************************/

static int fb_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct inode *inode;
  FAR struct fb_chardev_s *fb;
  int ret;

  ginfo("cmd: %d arg: %ld\n", cmd, arg);

  /* Get the framebuffer instance */

  inode = filep->f_inode;
  fb    = inode->i_private;

  /* Process the IOCTL command */

  switch (cmd)
    {
      case FBIOGET_VIDEOINFO:  /* Get color plane info */
        {
          FAR struct fb_videoinfo_s *vinfo =
            (FAR struct fb_videoinfo_s *)((uintptr_t)arg);

          DEBUGASSERT(vinfo != 0 && fb->vtable != NULL &&
                      fb->vtable->getvideoinfo != NULL);
          ret = fb->vtable->getvideoinfo(fb->vtable, vinfo);
        }
        break;

      case FBIOGET_PLANEINFO:  /* Get video plane info */
        {
          FAR struct fb_planeinfo_s *pinfo =
            (FAR struct fb_planeinfo_s *)((uintptr_t)arg);

          DEBUGASSERT(pinfo != 0);
          ret = fb_get_planeinfo(fb, pinfo, pinfo->display);
        }
        break;

#ifdef CONFIG_FB_CMAP
      case FBIOGET_CMAP:       /* Get RGB color mapping */
        {
          FAR struct fb_cmap_s *cmap =
            (FAR struct fb_cmap_s *)((uintptr_t)arg);

          DEBUGASSERT(cmap != 0 && fb->vtable != NULL &&
                      fb->vtable->getcmap != NULL);
          ret = fb->vtable->getcmap(fb->vtable, cmap);
        }
        break;

      case FBIOPUT_CMAP:       /* Put RGB color mapping */
        {
          FAR const struct fb_cmap_s *cmap =
            (FAR struct fb_cmap_s *)((uintptr_t)arg);

          DEBUGASSERT(cmap != 0 && fb->vtable != NULL &&
                      fb->vtable->putcmap != NULL);
          ret = fb->vtable->putcmap(fb->vtable, cmap);
        }
        break;
#endif
#ifdef CONFIG_FB_HWCURSOR
      case FBIOGET_CURSOR:     /* Get cursor attributes */
        {
          FAR struct fb_cursorattrib_s *attrib =
            (FAR struct fb_cursorattrib_s *)((uintptr_t)arg);

          DEBUGASSERT(attrib != 0 && fb->vtable != NULL &&
                      fb->vtable->getcursor != NULL);
          ret = fb->vtable->getcursor(fb->vtable, attrib);
        }
        break;

      case FBIOPUT_CURSOR:     /* Set cursor attributes */
        {
          FAR struct fb_setcursor_s *cursor =
            (FAR struct fb_setcursor_s *)((uintptr_t)arg);

          DEBUGASSERT(cursor != 0 && fb->vtable != NULL &&
                      fb->vtable->setcursor != NULL);
          ret = fb->vtable->setcursor(fb->vtable, cursor);
        }
        break;
#endif

#ifdef CONFIG_FB_UPDATE
      case FBIO_UPDATE:  /* Update the modified framebuffer data  */
        {
          struct fb_area_s *area = (FAR struct fb_area_s *)((uintptr_t)arg);

          DEBUGASSERT(fb->vtable != NULL && fb->vtable->updatearea != NULL);
          ret = fb->vtable->updatearea(fb->vtable, area);
        }
        break;
#endif

#ifdef CONFIG_FB_SYNC
      case FBIO_WAITFORVSYNC:  /* Wait upon vertical sync */
        {
          ret = fb->vtable->waitforvsync(fb->vtable);
        }
        break;
#endif

#ifdef CONFIG_FB_OVERLAY
      case FBIO_SELECT_OVERLAY:  /* Select video overlay */
        {
          struct fb_overlayinfo_s oinfo;
          FAR struct fb_priv_s *priv = filep->f_priv;

          DEBUGASSERT(priv != NULL && fb->vtable != NULL &&
                      fb->vtable->getoverlayinfo != NULL);
          memset(&oinfo, 0, sizeof(oinfo));
          ret = fb->vtable->getoverlayinfo(fb->vtable, arg, &oinfo);
          if (ret >= 0)
            {
              priv->overlay = arg;
            }
        }
        break;

      case FBIOGET_OVERLAYINFO:  /* Get video overlay info */
        {
          FAR struct fb_overlayinfo_s *oinfo =
            (FAR struct fb_overlayinfo_s *)((uintptr_t)arg);

          DEBUGASSERT(oinfo != 0 && fb->vtable != NULL &&
                      fb->vtable->getoverlayinfo != NULL);
          ret = fb->vtable->getoverlayinfo(fb->vtable,
                                           oinfo->overlay, oinfo);
        }
        break;

      case FBIOSET_TRANSP:  /* Set video overlay transparency */
        {
          FAR struct fb_overlayinfo_s *oinfo =
            (FAR struct fb_overlayinfo_s *)((uintptr_t)arg);

          DEBUGASSERT(oinfo != 0 && fb->vtable != NULL &&
                      fb->vtable->settransp != NULL);
          ret = fb->vtable->settransp(fb->vtable, oinfo);
        }
        break;

      case FBIOSET_CHROMAKEY:  /* Set video overlay chroma key */
        {
          FAR struct fb_overlayinfo_s *oinfo =
            (FAR struct fb_overlayinfo_s *)((uintptr_t)arg);

          DEBUGASSERT(oinfo != 0 && fb->vtable != NULL &&
                      fb->vtable->setchromakey != NULL);
          ret = fb->vtable->setchromakey(fb->vtable, oinfo);
        }
        break;

      case FBIOSET_COLOR:  /* Set video overlay color */
        {
          FAR struct fb_overlayinfo_s *oinfo =
            (FAR struct fb_overlayinfo_s *)((uintptr_t)arg);

          DEBUGASSERT(oinfo != 0 && fb->vtable != NULL &&
                      fb->vtable->setcolor != NULL);
          ret = fb->vtable->setcolor(fb->vtable, oinfo);
        }
        break;

      case FBIOSET_BLANK:  /* Blank or unblank video overlay */
        {
          FAR struct fb_overlayinfo_s *oinfo =
            (FAR struct fb_overlayinfo_s *)((uintptr_t)arg);

          DEBUGASSERT(oinfo != 0 && fb->vtable != NULL &&
                      fb->vtable->setblank != NULL);
          ret = fb->vtable->setblank(fb->vtable, oinfo);
        }
        break;

      case FBIOSET_AREA:  /* Set active video overlay area */
        {
          FAR struct fb_overlayinfo_s *oinfo =
            (FAR struct fb_overlayinfo_s *)((uintptr_t)arg);

          DEBUGASSERT(oinfo != 0 && fb->vtable != NULL &&
                      fb->vtable->setarea != NULL);
          ret = fb->vtable->setarea(fb->vtable, oinfo);
        }
        break;

      case FBIOSET_DESTAREA:  /* Set destination area on the primary FB */
        {
          FAR struct fb_overlayinfo_s *oinfo =
            (FAR struct fb_overlayinfo_s *)((uintptr_t)arg);

          DEBUGASSERT(oinfo != 0 && fb->vtable != NULL &&
                      fb->vtable->setdestarea != NULL);
          ret = fb->vtable->setdestarea(fb->vtable, oinfo);
        }
        break;

#ifdef CONFIG_FB_OVERLAY_BLIT
      case FBIOSET_BLIT:  /* Blit operation between video overlays */
        {
          FAR struct fb_overlayblit_s *blit =
            (FAR struct fb_overlayblit_s *)((uintptr_t)arg);

          DEBUGASSERT(blit != 0 && fb->vtable != NULL &&
                      fb->vtable->blit != NULL);
          ret = fb->vtable->blit(fb->vtable, blit);
        }
        break;

      case FBIOSET_BLEND:  /* Blend operation between video overlays */
        {
          FAR struct fb_overlayblend_s *blend =
            (FAR struct fb_overlayblend_s *)((uintptr_t)arg);

          DEBUGASSERT(blend != 0 && fb->vtable != NULL &&
                      fb->vtable->blend != NULL);
          ret = fb->vtable->blend(fb->vtable, blend);
        }
        break;
#endif

      case FBIOPAN_OVERLAY:
        {
          FAR struct fb_overlayinfo_s *oinfo =
            (FAR struct fb_overlayinfo_s *)((uintptr_t)arg);
          union fb_paninfo_u paninfo;

          DEBUGASSERT(oinfo != 0 && fb->vtable != NULL);

          memcpy(&paninfo, oinfo, sizeof(*oinfo));

          if (fb->vtable->panoverlay != NULL)
            {
              fb->vtable->panoverlay(fb->vtable, oinfo);
            }

          ret = fb_add_paninfo(fb->vtable, &paninfo, oinfo->overlay);
        }
        break;

#endif /* CONFIG_FB_OVERLAY */

      case FBIOSET_POWER:
        {
          DEBUGASSERT(fb->vtable != NULL &&
                      fb->vtable->setpower != NULL);
          ret = fb->vtable->setpower(fb->vtable, (int)arg);
        }
        break;

      case FBIOGET_POWER:
        {
          FAR int *power = (FAR int *)((uintptr_t)arg);

          DEBUGASSERT(power != NULL && fb->vtable != NULL &&
                      fb->vtable->getpower != NULL);
          *(power) = fb->vtable->getpower(fb->vtable);
          ret = OK;
        }
        break;

      case FBIOGET_FRAMERATE:
        {
          FAR int *rate = (FAR int *)((uintptr_t)arg);

          DEBUGASSERT(rate != NULL && fb->vtable != NULL &&
                      fb->vtable->getframerate != NULL);
          *(rate) = fb->vtable->getframerate(fb->vtable);
          ret = OK;
        }
        break;

      case FBIOSET_FRAMERATE:
        {
          DEBUGASSERT(fb->vtable != NULL &&
                      fb->vtable->setframerate != NULL);
          ret = fb->vtable->setframerate(fb->vtable, (int)arg);
        }
        break;

      case FBIOPAN_DISPLAY:
        {
          FAR struct fb_planeinfo_s *pinfo =
            (FAR struct fb_planeinfo_s *)((uintptr_t)arg);
          union fb_paninfo_u paninfo;

          DEBUGASSERT(pinfo != NULL && fb->vtable != NULL);

          memcpy(&paninfo, pinfo, sizeof(*pinfo));

          if (fb->vtable->pandisplay != NULL)
            {
              fb->vtable->pandisplay(fb->vtable, pinfo);
            }

          ret = fb_add_paninfo(fb->vtable, &paninfo, FB_NO_OVERLAY);
        }
        break;

      case FBIOSET_VSYNCOFFSET:
        {
          fb->vsyncoffset = USEC2TICK(arg);
          ret = OK;
        }
        break;

      case FBIOGET_VSCREENINFO:
        {
          struct fb_videoinfo_s vinfo;
          struct fb_planeinfo_s pinfo;
          FAR struct fb_var_screeninfo *varinfo =
            (FAR struct fb_var_screeninfo *)((uintptr_t)arg);

          DEBUGASSERT(varinfo != 0 && fb->vtable != NULL &&
                      fb->vtable->getvideoinfo != NULL);
          ret = fb->vtable->getvideoinfo(fb->vtable, &vinfo);
          if (ret < 0)
            {
              break;
            }

          ret = fb_get_planeinfo(fb, &pinfo, 0);
          if (ret < 0)
            {
              break;
            }

          memset(varinfo, 0, sizeof(struct fb_var_screeninfo));
          varinfo->xres           = vinfo.xres;
          varinfo->yres           = vinfo.yres;
          varinfo->xres_virtual   = pinfo.xres_virtual;
          varinfo->yres_virtual   = pinfo.yres_virtual;
          varinfo->xoffset        = pinfo.xoffset;
          varinfo->yoffset        = pinfo.yoffset;
          varinfo->bits_per_pixel = pinfo.bpp;
          varinfo->grayscale      = FB_ISMONO(vinfo.fmt);
          switch (vinfo.fmt)
            {
              case FB_FMT_Y1:
                varinfo->red.offset    = 0;
                varinfo->green.offset  = 0;
                varinfo->blue.offset   = 0;
                varinfo->red.length    = 1;
                varinfo->green.length  = 1;
                varinfo->blue.length   = 1;
                break;

              case FB_FMT_Y8:
                varinfo->red.offset    = 0;
                varinfo->green.offset  = 0;
                varinfo->blue.offset   = 0;
                varinfo->red.length    = 8;
                varinfo->green.length  = 8;
                varinfo->blue.length   = 8;
                break;

              case FB_FMT_RGB16_555:
                varinfo->red.offset    = 10;
                varinfo->green.offset  = 5;
                varinfo->blue.offset   = 0;
                varinfo->red.length    = 5;
                varinfo->green.length  = 5;
                varinfo->blue.length   = 5;
                break;

              case FB_FMT_RGB16_565:
                varinfo->red.offset    = 11;
                varinfo->green.offset  = 5;
                varinfo->blue.offset   = 0;
                varinfo->red.length    = 5;
                varinfo->green.length  = 6;
                varinfo->blue.length   = 5;
                break;

              case FB_FMT_RGB24:
              case FB_FMT_RGB32:
                varinfo->red.offset    = 16;
                varinfo->green.offset  = 8;
                varinfo->blue.offset   = 0;
                varinfo->red.length    = 8;
                varinfo->green.length  = 8;
                varinfo->blue.length   = 8;
                break;

              case FB_FMT_RGBA32:
                varinfo->red.offset    = 16;
                varinfo->green.offset  = 8;
                varinfo->blue.offset   = 0;
                varinfo->transp.offset = 24;
                varinfo->red.length    = 8;
                varinfo->green.length  = 8;
                varinfo->blue.length   = 8;
                varinfo->transp.length = 8;
                break;
          }
        }
        break;

      case FBIOGET_FSCREENINFO:
        {
          struct fb_videoinfo_s vinfo;
          struct fb_planeinfo_s pinfo;
          FAR struct fb_fix_screeninfo *fixinfo =
            (FAR struct fb_fix_screeninfo *)((uintptr_t)arg);

          DEBUGASSERT(fixinfo != 0 && fb->vtable != NULL &&
                      fb->vtable->getvideoinfo != NULL);
          ret = fb->vtable->getvideoinfo(fb->vtable, &vinfo);
          if (ret < 0)
            {
              break;
            }

          ret = fb_get_planeinfo(fb, &pinfo, 0);
          if (ret < 0)
            {
              break;
            }

          memset(fixinfo, 0, sizeof(struct fb_fix_screeninfo));
#ifdef CONFIG_FB_MODULEINFO
          strlcpy(fixinfo->id, (FAR const char *)vinfo.moduleinfo,
                  sizeof(fixinfo->id));
#endif
          fixinfo->smem_start  = (unsigned long)pinfo.fbmem;
          fixinfo->smem_len    = pinfo.fblen;
          fixinfo->type        = FB_ISYUVPLANAR(vinfo.fmt) ?
                                 FB_TYPE_INTERLEAVED_PLANES :
                                 FB_TYPE_PACKED_PIXELS;
          fixinfo->visual      = FB_ISMONO(vinfo.fmt) ?
                                 FB_VISUAL_MONO10 : FB_VISUAL_TRUECOLOR;
          fixinfo->line_length = pinfo.stride;
        }
        break;

      default:
        if (fb->vtable->ioctl != NULL)
          {
            ret = fb->vtable->ioctl(fb->vtable, cmd, arg);
          }
        else
          {
            gerr("ERROR: Unsupported IOCTL command: %d\n", cmd);
            ret = -ENOTTY;
          }
        break;
    }

  return ret;
}

static int fb_mmap(FAR struct file *filep, FAR struct mm_map_entry_s *map)
{
  FAR struct inode *inode;
  FAR struct fb_chardev_s *fb;
  FAR struct fb_priv_s *priv;
  struct fb_panelinfo_s panelinfo;
  int ret;

  /* Get the framebuffer instance */

  inode = filep->f_inode;
  fb    = inode->i_private;
  priv  = filep->f_priv;

  DEBUGASSERT(fb->vtable != NULL && priv != NULL);

  /* Get panel info */

  ret = fb_get_panelinfo(fb, &panelinfo, priv->overlay);

  if (ret < 0)
    {
      return ret;
    }

  /* Return the address corresponding to the start of frame buffer. */

  if (map->offset >= 0 && map->offset < panelinfo.fblen &&
      map->length && map->offset + map->length <= panelinfo.fblen)
    {
      map->vaddr = (FAR char *)panelinfo.fbmem + map->offset;
      return OK;
    }

  return -EINVAL;
}

/****************************************************************************
 * Name: fb_poll
 *
 * Description:
 *   Wait for framebuffer to be writable.
 *
 ****************************************************************************/

static int fb_poll(FAR struct file *filep, struct pollfd *fds, bool setup)
{
  FAR struct inode *inode;
  FAR struct fb_chardev_s *fb;
  FAR struct fb_priv_s *priv;
  FAR struct circbuf_s *panbuf;
  FAR struct pollfd **pollfds;
  irqstate_t flags;
  int ret = OK;

  /* Get the framebuffer instance */

  inode = filep->f_inode;
  fb    = inode->i_private;
  priv  = filep->f_priv;

  DEBUGASSERT(fb->vtable != NULL && priv != NULL);

  flags = enter_critical_section();

  if (setup)
    {
      pollfds = fb_get_free_pollfds(fb, priv->overlay);
      if (pollfds == NULL)
        {
          ret = -EBUSY;
          goto errout;
        }

      *pollfds = fds;
      fds->priv = pollfds;

      panbuf = fb_get_panbuf(fb, priv->overlay);
      if (!circbuf_is_full(panbuf))
        {
          poll_notify(pollfds, 1, POLLOUT);
        }
    }
  else if (fds->priv != NULL)
    {
      /* This is a request to tear down the poll. */

      FAR struct pollfd **slot = (FAR struct pollfd **)fds->priv;
      *slot = NULL;
      fds->priv = NULL;
    }

errout:
  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: fb_get_panelinfo
 ****************************************************************************/

static int fb_get_panelinfo(FAR struct fb_chardev_s *fb,
                            FAR struct fb_panelinfo_s *panelinfo,
                            int overlay)
{
  struct fb_planeinfo_s pinfo;
  struct fb_videoinfo_s vinfo;
  int ret;

#ifdef CONFIG_FB_OVERLAY
  if (overlay != FB_NO_OVERLAY)
    {
      struct fb_overlayinfo_s oinfo;
      DEBUGASSERT(fb->vtable->getoverlayinfo != NULL);
      memset(&oinfo, 0, sizeof(oinfo));
      ret = fb->vtable->getoverlayinfo(fb->vtable, overlay, &oinfo);

      if (ret < 0)
        {
          gerr("ERROR: getoverlayinfo() failed: %d\n", ret);
          return ret;
        }

      panelinfo->fbmem   = oinfo.fbmem;
      panelinfo->fblen   = oinfo.fblen;
      panelinfo->fbcount = oinfo.yres_virtual == 0 ?
                           1 : (oinfo.yres_virtual / oinfo.yres);
      panelinfo->bpp     = oinfo.bpp;
      return OK;
    }
#endif

  ret = fb_get_planeinfo(fb, &pinfo, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = fb->vtable->getvideoinfo(fb->vtable, &vinfo);
  if (ret < 0)
    {
      gerr("ERROR: getvideoinfo() failed: %d\n", ret);
      return ret;
    }

  panelinfo->fbmem   = pinfo.fbmem;
  panelinfo->fblen   = pinfo.fblen;
  panelinfo->fbcount = pinfo.yres_virtual == 0 ?
                       1 : (pinfo.yres_virtual / vinfo.yres);
  panelinfo->bpp     = pinfo.bpp;

  return OK;
}

/****************************************************************************
 * Name: fb_get_planeinfo
 ****************************************************************************/

static int fb_get_planeinfo(FAR struct fb_chardev_s *fb,
                            FAR struct fb_planeinfo_s *pinfo,
                            uint8_t display)
{
  int ret;

  DEBUGASSERT(fb->vtable != NULL);
  DEBUGASSERT(fb->vtable->getplaneinfo != NULL);

  memset(pinfo, 0, sizeof(struct fb_planeinfo_s));
  pinfo->display = display;

  ret = fb->vtable->getplaneinfo(fb->vtable, fb->plane, pinfo);

  if (ret < 0)
    {
      gerr("ERROR: getplaneinfo() failed: %d\n", ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: fb_do_pollnotify
 ****************************************************************************/

static void fb_do_pollnotify(wdparm_t arg)
{
  FAR struct fb_paninfo_s *paninfo = (FAR struct fb_paninfo_s *)arg;
  irqstate_t flags;
  int i;

  flags = enter_critical_section();

  for (i = 0; i < CONFIG_VIDEO_FB_NPOLLWAITERS; i++)
    {
      if (paninfo->fds[i] != NULL)
        {
          /* Notify framebuffer is writable. */

          poll_notify(&paninfo->fds[i], 1, POLLOUT);
        }
    }

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: fb_pollnotify
 *
 * Description:
 *   Notify the waiting thread that the framebuffer can be written.
 *
 * Input Parameters:
 *   vtable - Pointer to framebuffer's virtual table.
 *
 ****************************************************************************/

static void fb_pollnotify(FAR struct fb_vtable_s *vtable, int overlay)
{
  FAR struct fb_chardev_s *fb;
  int id = overlay + 1;

  DEBUGASSERT(vtable != NULL);

  fb = vtable->priv;

  /* Prevent calling before getting the vtable. */

  if (fb == NULL)
    {
      return;
    }

  DEBUGASSERT(id >= 0 && id < fb->paninfo_count);

  if (fb->vsyncoffset > 0)
    {
      wd_start(&fb->wdog, fb->vsyncoffset, fb_do_pollnotify,
               (wdparm_t)(&fb->paninfo[id]));
    }
  else
    {
      fb_do_pollnotify((wdparm_t)(&fb->paninfo[id]));
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: fb_peek_paninfo
 * Description:
 *   Peek a frame from pan info queue of the specified overlay.
 *
 * Input Parameters:
 *   vtable  - Pointer to framebuffer's virtual table.
 *   info    - Pointer to pan info.
 *   overlay - Overlay index.
 *
 * Returned Value:
 *   Zero is returned on success; a negated errno value is returned on any
 *   failure.
 ****************************************************************************/

int fb_peek_paninfo(FAR struct fb_vtable_s *vtable,
                    FAR union fb_paninfo_u *info,
                    int overlay)
{
  FAR struct circbuf_s *panbuf;
  FAR struct fb_chardev_s *fb;
  irqstate_t flags;
  ssize_t ret;

  /* Prevent calling before getting the vtable. */

  fb = vtable->priv;
  if (fb == NULL)
    {
      return -EINVAL;
    }

  panbuf = fb_get_panbuf(fb, overlay);
  if (panbuf == NULL)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();

  /* Attempt to peek a frame from the vsync queue. */

  ret = circbuf_peek(panbuf, info, sizeof(union fb_paninfo_u));
  DEBUGASSERT(ret <= 0 || ret == sizeof(union fb_paninfo_u));

  /* Re-enable interrupts */

  leave_critical_section(flags);
  return ret <= 0 ? -ENOSPC : OK;
}

/****************************************************************************
 * Name: fb_remove_paninfo
 * Description:
 *   Remove a frame from pan info queue of the specified overlay.
 *
 * Input Parameters:
 *   vtable  - Pointer to framebuffer's virtual table.
 *   overlay - Overlay index.
 *
 * Returned Value:
 *   Zero is returned on success; a negated errno value is returned on any
 *   failure.
 ****************************************************************************/

int fb_remove_paninfo(FAR struct fb_vtable_s *vtable, int overlay)
{
  FAR struct circbuf_s *panbuf;
  FAR struct fb_chardev_s *fb;
  irqstate_t flags;
  ssize_t ret;

  fb = vtable->priv;
  if (fb == NULL)
    {
      return -EINVAL;
    }

  panbuf = fb_get_panbuf(fb, overlay);
  if (panbuf == NULL)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();

  /* Attempt to take a frame from the pan info. */

  ret = circbuf_skip(panbuf, sizeof(union fb_paninfo_u));
  DEBUGASSERT(ret <= 0 || ret == sizeof(union fb_paninfo_u));

  /* Re-enable interrupts */

  leave_critical_section(flags);

  if (ret == sizeof(union fb_paninfo_u))
    {
      fb_pollnotify(vtable, overlay);
    }

  return ret <= 0 ? -ENOSPC : OK;
}

/****************************************************************************
 * Name: fb_paninfo_count
 * Description:
 *   Get pan info count of specified overlay pan info queue.
 *
 * Input Parameters:
 *   vtable  - Pointer to framebuffer's virtual table.
 *   overlay - Overlay index.
 *
 * Returned Value:
 *   a non-negative value is returned on success; a negated errno value is
 *   returned on any failure.
 ****************************************************************************/

int fb_paninfo_count(FAR struct fb_vtable_s *vtable, int overlay)
{
  FAR struct circbuf_s *panbuf;
  FAR struct fb_chardev_s *fb;
  irqstate_t flags;
  ssize_t ret;

  /* Prevent calling before getting the vtable. */

  fb = vtable->priv;
  if (fb == NULL)
    {
      return -EINVAL;
    }

  panbuf = fb_get_panbuf(fb, overlay);
  if (panbuf == NULL)
    {
      return -EINVAL;
    }

  flags = enter_critical_section();

  ret = circbuf_used(panbuf) / sizeof(union fb_paninfo_u);

  leave_critical_section(flags);

  return ret;
}

/****************************************************************************
 * Name: fb_register_device
 *
 * Description:
 *   Register the framebuffer character device at /dev/fbN where N is the
 *   display number if the devices supports only a single plane.  If the
 *   hardware supports multiple color planes, then the device will be
 *   registered at /dev/fbN.M where N is the again display number but M
 *   is the display plane.
 *
 * Input Parameters:
 *   display - The display number for the case of boards supporting multiple
 *             displays or for hardware that supports multiple
 *             layers (each layer is consider a display).  Typically zero.
 *   plane   - Identifies the color plane on hardware that supports separate
 *             framebuffer "planes" for each color component.
 *   vtable  - Pointer to framebuffer's virtual table.
 *
 * Returned Value:
 *   Zero (OK) is returned success; a negated errno value is returned on any
 *   failure.
 *
 ****************************************************************************/

int fb_register_device(int display, int plane,
                       FAR struct fb_vtable_s *vtable)
{
  FAR struct fb_chardev_s *fb;
  struct fb_panelinfo_s panelinfo;
  struct fb_videoinfo_s vinfo;
  char devname[16];
  int nplanes;
  int ret;
  size_t i;

  /* Allocate a framebuffer state instance */

  fb = kmm_zalloc(sizeof(struct fb_chardev_s));
  if (fb == NULL)
    {
      return -ENOMEM;
    }

  DEBUGASSERT((unsigned)plane <= UINT8_MAX);
  DEBUGASSERT(vtable != NULL);
  fb->plane  = plane;
  fb->vtable = vtable;

  /* Initialize the frame buffer instance. */

  DEBUGASSERT(vtable->getvideoinfo != NULL);
  ret = vtable->getvideoinfo(vtable, &vinfo);
  if (ret < 0)
    {
      gerr("ERROR: getvideoinfo() failed: %d\n", ret);
      goto errout_with_fb;
    }

  nplanes = vinfo.nplanes;
  DEBUGASSERT(vinfo.nplanes > 0 && (unsigned)plane < vinfo.nplanes);

#ifdef CONFIG_FB_OVERLAY
  fb->paninfo_count = vinfo.noverlays;
#endif

  /* Add the primary framebuffer */

  fb->paninfo_count += 1;
  fb->paninfo = kmm_zalloc(sizeof(struct fb_paninfo_s) * fb->paninfo_count);

  if (fb->paninfo == NULL)
    {
      gerr("ERROR: alloc panbuf failed\n");
      goto errout_with_fb;
    }

  for (i = 0; i < fb->paninfo_count; i++)
    {
      ret = fb_get_panelinfo(fb, &panelinfo, i - 1);
      if (ret < 0)
        {
          goto errout_with_paninfo;
        }

      ret = circbuf_init(&(fb->paninfo[i].buf), NULL, panelinfo.fbcount
                         * sizeof(union fb_paninfo_u));

      DEBUGASSERT(ret == 0);

      /* Clear the framebuffer memory */

      memset(panelinfo.fbmem, 0, panelinfo.fblen);
    }

  /* Register the framebuffer device */

  if (nplanes < 2)
    {
      snprintf(devname, 16, "/dev/fb%d", display);
    }
  else
    {
      snprintf(devname, 16, "/dev/fb%d.%d", display, plane);
    }

  nxmutex_init(&fb->lock);

  ret = register_driver(devname, &g_fb_fops, 0666, fb);
  if (ret < 0)
    {
      gerr("ERROR: register_driver() failed: %d\n", ret);
      goto errout_with_nxmutex;
    }

  vtable->priv = fb;
  return OK;

errout_with_nxmutex:
  nxmutex_destroy(&fb->lock);
errout_with_paninfo:
  while (i-- > 0)
    {
      circbuf_uninit(&(fb->paninfo[i].buf));
    }

  kmm_free(fb->paninfo);
errout_with_fb:
  kmm_free(fb);
  return ret;
}

/****************************************************************************
 * Name: fb_register
 *
 * Description:
 *   Register the framebuffer character device at /dev/fbN where N is the
 *   display number if the devices supports only a single plane.  If the
 *   hardware supports multiple color planes, then the device will be
 *   registered at /dev/fbN.M where N is the again display number but M
 *   is the display plane.
 *
 * Input Parameters:
 *   display - The display number for the case of boards supporting multiple
 *             displays or for hardware that supports multiple
 *             layers (each layer is consider a display).  Typically zero.
 *   plane   - Identifies the color plane on hardware that supports separate
 *             framebuffer "planes" for each color component.
 *
 * Returned Value:
 *   Zero (OK) is returned success; a negated errno value is returned on any
 *   failure.
 *
 ****************************************************************************/

int fb_register(int display, int plane)
{
  FAR struct fb_vtable_s *vtable;
  int ret;

  /* Initialize the frame buffer device. */

  ret = up_fbinitialize(display);
  if (ret < 0)
    {
      gerr("ERROR: up_fbinitialize() failed for display %d: %d\n",
           display, ret);
      return ret;
    }

  vtable = up_fbgetvplane(display, plane);
  if (vtable == NULL)
    {
      gerr("ERROR: up_fbgetvplane() failed, vplane=%d\n", plane);
      return -EINVAL;
    }

  return fb_register_device(display, plane, vtable);
}

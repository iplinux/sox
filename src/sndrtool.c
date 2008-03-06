/*
 * Sounder/Sndtool format handler: W V Neisius, February 1992
 *
 * June 28, 93: force output to mono.
 * 
 * March 3, 1999 - cbagwell@sprynet.com
 *   Forced extra comment fields to zero.
 */

#include "sox_i.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* Private data used by writer */
typedef struct sndpriv {
  sox_size_t nsamples;
  sox_size_t dataStart;
} *snd_t;

static void sndtwriteheader(sox_format_t * ft, sox_size_t nsamples)
{
  char name_buf[97];

  /* sndtool header */
  sox_writes(ft, "SOUND");      /* magic */
  sox_writeb(ft, 0x1a);
  sox_writew(ft, 0);    /* hGSound */
  sox_writedw(ft, nsamples);
  sox_writedw(ft, 0);
  sox_writedw(ft, nsamples);
  sox_writew(ft, (unsigned)(ft->signal.rate + .5));
  sox_writew(ft, 0);
  sox_writew(ft, 10);
  sox_writew(ft, 4);
  memset(name_buf, 0, 96);
  sprintf(name_buf, "%.62s - File created by SoX", ft->filename);
  sox_writebuf(ft, name_buf, 96);
}

static int seek(sox_format_t * ft, sox_size_t offset)
{
  sox_size_t new_offset, channel_block, alignment;
  snd_t snd = (snd_t) ft->priv;

  new_offset = offset * (ft->encoding.bits_per_sample >> 3);
  /* Make sure request aligns to a channel block (ie left+right) */
  channel_block = ft->signal.channels * (ft->encoding.bits_per_sample >> 3);
  alignment = new_offset % channel_block;
  /* Most common mistaken is to compute something like
   * "skip everthing upto and including this sample" so
   * advance to next sample block in this case.
   */
  if (alignment != 0)
    new_offset += (channel_block - alignment);
  new_offset += snd->dataStart;

  return sox_seeki(ft, (sox_ssize_t) new_offset, SEEK_SET);
}

static int startread(sox_format_t * ft)
{
  snd_t snd = (snd_t) ft->priv;

  char buf[96];

  unsigned short rate;
  int rc;

  /* Needed for rawread() */
  rc = sox_rawstartread(ft);
  if (rc)
    return rc;

  rate = 0;

  /* determine file type */
  /* if first 5 bytes == SOUND then this is probably a sndtool sound */
  /* if first word (16 bits) == 0 
   * and second word is between 4000 & 25000 then this is sounder sound */
  /* otherwise, its probably raw, not handled here */

  if (sox_readbuf(ft, buf, 2) != 2) {
    sox_fail_errno(ft, errno, "SND: unexpected EOF");
    return (SOX_EOF);
  }
  if (strncmp(buf, "\0\0", 2) == 0) {
    /* sounder */
    sox_readw(ft, &rate);
    if (rate < 4000 || rate > 25000) {
      sox_fail_errno(ft, SOX_EFMT, "SND: sample rate out of range");
      return (SOX_EOF);
    }
    sox_seeki(ft, 4, SEEK_CUR);
  } else {
    /* sndtool ? */
    sox_readbuf(ft, &buf[2], 6);
    if (strncmp(buf, "SOUND", 5)) {
      sox_fail_errno(ft, SOX_EFMT, "SND: unrecognized SND format");
      return (SOX_EOF);
    }
    sox_seeki(ft, 12, SEEK_CUR);
    sox_readw(ft, &rate);
    sox_seeki(ft, 6, SEEK_CUR);
    if (sox_readbuf(ft, buf, 96) != 96) {
      sox_fail_errno(ft, SOX_EHDR, "SND: unexpected EOF in SND header");
      return (SOX_EOF);
    }
    sox_debug("comments: %.96s", buf);
  }

  ft->signal.channels = 1;
  ft->signal.rate = rate;
  ft->encoding.encoding = SOX_ENCODING_UNSIGNED;
  ft->encoding.bits_per_sample = 8;

  snd->dataStart = sox_tell(ft);
  ft->length = sox_filelength(ft) - snd->dataStart;

  return (SOX_SUCCESS);
}

static int startwrite(sox_format_t * ft)
{
  snd_t p = (snd_t) ft->priv;
  int rc;

  /* Needed for rawwrite() */
  rc = sox_rawstartwrite(ft);
  if (rc)
    return rc;

  /* write header */
  p->nsamples = 0;
  sndtwriteheader(ft, 0);

  return (SOX_SUCCESS);
}

static sox_size_t write_samples(sox_format_t * ft, const sox_sample_t * buf,
                                sox_size_t len)
{
  snd_t p = (snd_t) ft->priv;

  p->nsamples += len;
  return sox_rawwrite(ft, buf, len);
}

static int stopwrite(sox_format_t * ft)
{
  snd_t p = (snd_t) ft->priv;

  /* fixup file sizes in header */
  if (sox_seeki(ft, 0, 0) != 0) {
    sox_fail_errno(ft, errno,
                   "can't rewind output file to rewrite SND header");
    return SOX_EOF;
  }
  sndtwriteheader(ft, p->nsamples);
  return (SOX_SUCCESS);
}

SOX_FORMAT_HANDLER(sndrtool)
{
  static char const * const names[] = {"sndt", NULL};
  static unsigned const write_encodings[] = {SOX_ENCODING_UNSIGNED, 8, 0, 0};
  static sox_format_handler_t const handler = {
    "Handles Sndtool and Sounder files",
    names, SOX_FILE_LIT_END | SOX_FILE_MONO,
    startread, sox_rawread, sox_rawstopread,
    startwrite, write_samples, stopwrite,
    seek, write_encodings, NULL
  };
  return &handler;
}

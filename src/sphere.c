/*
 * NIST Sphere file format handler.
 *
 * August 7, 2000
 *
 * Copyright (C) 2000 Chris Bagwell (cbagwell@sprynet.com)
 *
 */

#include "sox_i.h"
#include <string.h>

static int start_read(sox_format_t * ft)
{
  sox_encoding_t encoding = SOX_ENCODING_SIGN2;
  sox_size_t     header_size, bytes_read;
  sox_size_t     num_samples = 0;
  unsigned       bytes_per_sample = 0;
  unsigned       channels = 1;
  unsigned       rate = 16000;
  char           fldname[64], fldtype[16], fldsval[128];
  char           * buf;

  /* Magic header */
  if (sox_reads(ft, fldname, 8) || strncmp(fldname, "NIST_1A", 7) != 0) {
    sox_fail_errno(ft, SOX_EHDR, "Sphere header does not begin with magic word 'NIST_1A'");
    return (SOX_EOF);
  }

  if (sox_reads(ft, fldsval, 8)) {
    sox_fail_errno(ft, SOX_EHDR, "Error reading Sphere header");
    return (SOX_EOF);
  }

  /* Determine header size, and allocate a buffer large enough to hold it. */
  sscanf(fldsval, "%u", &header_size);
  buf = xmalloc(header_size);

  /* Skip what we have read so far */
  header_size -= 16;

  if (sox_reads(ft, buf, header_size) == SOX_EOF) {
    sox_fail_errno(ft, SOX_EHDR, "Error reading Sphere header");
    free(buf);
    return (SOX_EOF);
  }

  header_size -= (strlen(buf) + 1);

  while (strncmp(buf, "end_head", 8) != 0) {
    if (strncmp(buf, "sample_n_bytes", 14) == 0)
      sscanf(buf, "%63s %15s %u", fldname, fldtype, &bytes_per_sample);
    else if (strncmp(buf, "channel_count", 13) == 0)
      sscanf(buf, "%63s %15s %u", fldname, fldtype, &channels);
    else if (strncmp(buf, "sample_count ", 13) == 0)
      sscanf(buf, "%53s %15s %u", fldname, fldtype, &num_samples);
    else if (strncmp(buf, "sample_rate ", 12) == 0)
      sscanf(buf, "%53s %15s %u", fldname, fldtype, &rate);
    else if (strncmp(buf, "sample_coding", 13) == 0) {
      sscanf(buf, "%63s %15s %127s", fldname, fldtype, fldsval);
      if (!strcasecmp(fldsval, "ulaw") || !strcasecmp(fldsval, "mu-law"))
        encoding = SOX_ENCODING_ULAW;
      else if (!strcasecmp(fldsval, "pcm"))
        encoding = SOX_ENCODING_SIGN2;
      else {
        sox_fail_errno(ft, SOX_EFMT, "sph: unsupported coding `%s'", fldsval);
        free(buf);
        return SOX_EOF;
      }
    }
    else if (strncmp(buf, "sample_byte_format", 18) == 0) {
      sscanf(buf, "%53s %15s %127s", fldname, fldtype, fldsval);
      if (strcmp(fldsval, "01") == 0)         /* Data is little endian. */
        ft->encoding.reverse_bytes = SOX_IS_BIGENDIAN;
      else if (strcmp(fldsval, "10") == 0)    /* Data is big endian. */
        ft->encoding.reverse_bytes = SOX_IS_LITTLEENDIAN;
      else if (strcmp(fldsval, "1")) {
        sox_fail_errno(ft, SOX_EFMT, "sph: unsupported coding `%s'", fldsval);
        free(buf);
        return SOX_EOF;
      }
    }

    if (sox_reads(ft, buf, header_size) == SOX_EOF) {
      sox_fail_errno(ft, SOX_EHDR, "Error reading Sphere header");
      free(buf);
      return (SOX_EOF);
    }

    header_size -= (strlen(buf) + 1);
  }

  if (!bytes_per_sample)
    bytes_per_sample = encoding == SOX_ENCODING_ULAW? 1 : 2;

  if (encoding == SOX_ENCODING_SIGN2 && bytes_per_sample == 1)
    encoding = SOX_ENCODING_UNSIGNED;

  while (header_size) {
    bytes_read = sox_readbuf(ft, buf, header_size);
    if (bytes_read == 0) {
      free(buf);
      return (SOX_EOF);
    }
    header_size -= bytes_read;
  }
  free(buf);

  if (ft->seekable) {
    /* Check first four bytes of data to see if it's shorten compressed. */
    char           shorten_check[4];

    if (sox_readchars(ft, shorten_check, sizeof(shorten_check)))
      return SOX_EOF;
    sox_seeki(ft, -(sox_ssize_t)sizeof(shorten_check), SEEK_CUR);

    if (!memcmp(shorten_check, "ajkg", sizeof(shorten_check))) {
      sox_fail_errno(ft, SOX_EFMT,
                     "File uses shorten compression, cannot handle this.");
      return (SOX_EOF);
    }
  }

  return sox_check_read_params(ft, channels, (sox_rate_t)rate, encoding,
      bytes_per_sample << 3, (off_t)(num_samples * channels));
}

static int write_header(sox_format_t * ft)
{
  char buf[128];
  long samples = (ft->olength ? ft->olength : ft->length) / ft->signal.channels;

  sox_writes(ft, "NIST_1A\n");
  sox_writes(ft, "   1024\n");

  if (samples) {
    sprintf(buf, "sample_count -i %ld\n", samples);
    sox_writes(ft, buf);
  }

  sprintf(buf, "sample_n_bytes -i %d\n", ft->encoding.bits_per_sample >> 3);
  sox_writes(ft, buf);

  sprintf(buf, "channel_count -i %d\n", ft->signal.channels);
  sox_writes(ft, buf);

  if (ft->encoding.bits_per_sample == 8)
    sprintf(buf, "sample_byte_format -s1 1\n");
  else
    sprintf(buf, "sample_byte_format -s2 %s\n",
            ft->encoding.reverse_bytes != SOX_IS_BIGENDIAN ? "10" : "01");
  sox_writes(ft, buf);

  sprintf(buf, "sample_rate -i %u\n", (unsigned) (ft->signal.rate + .5));
  sox_writes(ft, buf);

  if (ft->encoding.encoding == SOX_ENCODING_ULAW)
    sox_writes(ft, "sample_coding -s4 ulaw\n");
  else
    sox_writes(ft, "sample_coding -s3 pcm\n");

  sox_writes(ft, "end_head\n");

  sox_padbytes(ft, 1024 - (sox_size_t)sox_tell(ft));
  return SOX_SUCCESS;
}

SOX_FORMAT_HANDLER(sphere)
{
  static char const *const names[] = { "sph", "nist", NULL };
  static unsigned const write_encodings[] = {
    SOX_ENCODING_SIGN2, 16, 24, 32, 0,
    SOX_ENCODING_UNSIGNED, 8, 0,
    SOX_ENCODING_ULAW, 8, 0,
    0
  };
  static sox_format_handler_t const handler = {
    SOX_LIB_VERSION_CODE,
    "SPeech HEader Resources; defined by NIST",
    names, SOX_FILE_REWIND,
    start_read, sox_rawread, NULL,
    write_header, sox_rawwrite, NULL,
    sox_rawseek, write_encodings, NULL
  };
  return &handler;
}

/* libSoX repeat effect  Copyright (c) 2004 Jan Paul Schmidt <jps@fundament.org>
 * Re-write (c) 2008 robs@users.sourceforge.net
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "sox_i.h"

typedef struct {
  unsigned      num_repeats, remaining_repeats;
  off_t         num_samples, remaining_samples;
  FILE          * tmp_file;
} priv_t;

static int create(sox_effect_t * effp, int argc, char * * argv)
{
  priv_t * p = (priv_t *)effp->priv;
  p->num_repeats = 1;
  --argc, ++argv;
  do {NUMERIC_PARAMETER(num_repeats, 0, UINT_MAX - 1)} while (0);
  return argc? lsx_usage(effp) : SOX_SUCCESS;
}

static int start(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  if (!p->num_repeats)
    return SOX_EFF_NULL;

  if (!(p->tmp_file = lsx_tmpfile())) {
    lsx_fail("can't create temporary file: %s", strerror(errno));
    return SOX_EOF;
  }
  p->num_samples = p->remaining_samples = 0;
  p->remaining_repeats = p->num_repeats + 1;
  return SOX_SUCCESS;
}

static int flow(sox_effect_t * effp, const sox_sample_t * ibuf,
    sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  if (fwrite(ibuf, sizeof(*ibuf), *isamp, p->tmp_file) != *isamp) {
    lsx_fail("error writing temporary file: %s", strerror(errno));
    return SOX_EOF;
  }
  p->num_samples += *isamp;
  (void)obuf, *osamp = 0; /* samples not output until drain */
  return SOX_SUCCESS;
}

static int drain(sox_effect_t * effp, sox_sample_t * obuf, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t odone = 0, n;

  while ((p->remaining_samples || p->remaining_repeats) && odone < *osamp) {
    if (!p->remaining_samples) {
      p->remaining_samples = p->num_samples;
      --p->remaining_repeats;
      rewind(p->tmp_file);
    }
    n = min(p->remaining_samples, (off_t)(*osamp - odone));
    if ((fread(obuf + odone, sizeof(*obuf), n, p->tmp_file)) != n) {
      lsx_fail("error reading temporary file: %s", strerror(errno));
      return SOX_EOF;
    }
    p->remaining_samples -= n;
    odone += n;
  }
  *osamp = odone;
  return p->remaining_samples || p->remaining_repeats? SOX_SUCCESS : SOX_EOF;
}

static int stop(sox_effect_t * effp)
{
  priv_t * p = (priv_t *)effp->priv;
  fclose(p->tmp_file); /* auto-deleted by lsx_tmpfile */
  return SOX_SUCCESS;
}

sox_effect_handler_t const * lsx_repeat_effect_fn(void)
{
  static sox_effect_handler_t effect = {"repeat", "[count]",
    SOX_EFF_MCHAN | SOX_EFF_LENGTH | SOX_EFF_MODIFY,
    create, start, flow, drain, stop, NULL, sizeof(priv_t)};
  return &effect;
}

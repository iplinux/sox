/* Effect: dft filter     Copyright (c) 2008 robs@users.sourceforge.net
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
#include "fft4g.h"
#include "dft_filter.h"
#include <string.h>

typedef dft_filter_filter_t filter_t;
typedef dft_filter_priv_t priv_t;

static int start(sox_effect_t * effp)
{
  priv_t * p = (priv_t *) effp->priv;

  fifo_create(&p->input_fifo, (int)sizeof(double));
  memset(fifo_reserve(&p->input_fifo,
        p->filter_ptr->post_peak), 0, sizeof(double) * p->filter_ptr->post_peak);
  fifo_create(&p->output_fifo, (int)sizeof(double));
  return SOX_SUCCESS;
}

static void filter(priv_t * p)
{
  int i, num_in = max(0, fifo_occupancy(&p->input_fifo));
  filter_t const * f = p->filter_ptr;
  int const overlap = f->num_taps - 1;
  double * output;

  while (num_in >= f->dft_length) {
    double const * input = fifo_read_ptr(&p->input_fifo);
    fifo_read(&p->input_fifo, f->dft_length - overlap, NULL);
    num_in -= f->dft_length - overlap;

    output = fifo_reserve(&p->output_fifo, f->dft_length);
    fifo_trim_by(&p->output_fifo, overlap);
    memcpy(output, input, f->dft_length * sizeof(*output));

    lsx_rdft(f->dft_length, 1, output, lsx_fft_br, lsx_fft_sc);
    output[0] *= f->coefs[0];
    output[1] *= f->coefs[1];
    for (i = 2; i < f->dft_length; i += 2) {
      double tmp = output[i];
      output[i  ] = f->coefs[i  ] * tmp - f->coefs[i+1] * output[i+1];
      output[i+1] = f->coefs[i+1] * tmp + f->coefs[i  ] * output[i+1];
    }
    lsx_rdft(f->dft_length, -1, output, lsx_fft_br, lsx_fft_sc);
  }
}

static int flow(sox_effect_t * effp, const sox_sample_t * ibuf,
                sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  size_t i, odone = min(*osamp, (size_t)fifo_occupancy(&p->output_fifo));
  double const * s = fifo_read(&p->output_fifo, (int)odone, NULL);
  SOX_SAMPLE_LOCALS;

  for (i = 0; i < odone; ++i)
    *obuf++ = SOX_FLOAT_64BIT_TO_SAMPLE(*s++, effp->clips);
  p->samples_out += odone;

  if (*isamp && odone < *osamp) {
    double * t = fifo_write(&p->input_fifo, (int)*isamp, NULL);
    p->samples_in += (int)*isamp;

    for (i = *isamp; i; --i)
      *t++ = SOX_SAMPLE_TO_FLOAT_64BIT(*ibuf++, effp->clips);
    filter(p);
  }
  else *isamp = 0;
  *osamp = odone;
  return SOX_SUCCESS;
}

static int drain(sox_effect_t * effp, sox_sample_t * obuf, size_t * osamp)
{
  priv_t * p = (priv_t *)effp->priv;
  static size_t isamp = 0;
  size_t samples_out = p->samples_in;
  size_t remaining = samples_out - p->samples_out;
  double * buff = lsx_calloc(1024, sizeof(*buff));

  if ((int)remaining > 0) {
    while ((size_t)fifo_occupancy(&p->output_fifo) < remaining) {
      fifo_write(&p->input_fifo, 1024, buff);
      p->samples_in += 1024;
      filter(p);
    }
    fifo_trim_to(&p->output_fifo, (int)remaining);
    p->samples_in = 0;
  }
  free(buff);
  return flow(effp, 0, obuf, &isamp, osamp);
}

static int stop(sox_effect_t * effp)
{
  priv_t * p = (priv_t *) effp->priv;

  fifo_delete(&p->input_fifo);
  fifo_delete(&p->output_fifo);
  free(p->filter_ptr->coefs);
  memset(p->filter_ptr, 0, sizeof(*p->filter_ptr));
  return SOX_SUCCESS;
}

sox_effect_handler_t const * sox_dft_filter_effect_fn(void)
{
  static sox_effect_handler_t handler = {
    NULL, NULL, SOX_EFF_GAIN, NULL, start, flow, drain, stop, NULL, 0
  };
  return &handler;
}

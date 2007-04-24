/*
 * libSoX SGI/Amiga AIFF format.
 * Used by SGI on 4D/35 and Indigo.
 * This is a subformat of the EA-IFF-85 format.
 * This is related to the IFF format used by the Amiga.
 * But, apparently, not the same.
 * Also AIFF-C format output that is defined in DAVIC 1.4 Part 9 Annex B
 * (usable for japanese-data-broadcasting, specified by ARIB STD-B24.)
 *
 * Copyright 1991-2007 Guido van Rossum And Sundry Contributors
 * 
 * This source code is freely redistributable and may be used for
 * any purpose.  This copyright notice must be maintained. 
 * Guido van Rossum And Sundry Contributors are not responsible for 
 * the consequences of using this software.
 */

#include "sox_i.h"
#include "aiff.h"

#include <math.h>
#include <time.h>      /* for time stamping comments */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>     /* For SEEK_* defines if not found in stdio */
#endif

/* forward declarations */
static double read_ieee_extended(ft_t);
static int aiffwriteheader(ft_t, sox_size_t);
static int aifcwriteheader(ft_t, sox_size_t);
static void write_ieee_extended(ft_t, double);
static double ConvertFromIeeeExtended(unsigned char*);
static void ConvertToIeeeExtended(double, char *);
static int textChunk(char **text, char *chunkDescription, ft_t ft);
static int commentChunk(char **text, char *chunkDescription, ft_t ft);
static void reportInstrument(ft_t ft);

/* Private data used by writer */
typedef struct aiffpriv {
    sox_size_t nsamples;  /* number of 1-channel samples read or written */
                         /* Decrements for read increments for write */
    sox_size_t dataStart; /* need to for seeking */
} *aiff_t;

int sox_aiffseek(ft_t ft, sox_size_t offset) 
{
    aiff_t aiff = (aiff_t ) ft->priv;
    sox_size_t new_offset, channel_block, alignment;

    new_offset = offset * ft->signal.size;
    /* Make sure request aligns to a channel block (ie left+right) */
    channel_block = ft->signal.channels * ft->signal.size;
    alignment = new_offset % channel_block;
    /* Most common mistaken is to compute something like
     * "skip everthing upto and including this sample" so
     * advance to next sample block in this case.
     */
    if (alignment != 0)
        new_offset += (channel_block - alignment);
    new_offset += aiff->dataStart;

    ft->sox_errno = sox_seeki(ft, (sox_ssize_t)new_offset, SEEK_SET);

    if (ft->sox_errno == SOX_SUCCESS)
        aiff->nsamples = ft->length - (new_offset / ft->signal.size);

    return(ft->sox_errno);
}

int sox_aiffstartread(ft_t ft) 
{
        aiff_t aiff = (aiff_t ) ft->priv;
        char buf[5];
        uint32_t totalsize;
        uint32_t chunksize;
        unsigned short channels = 0;
        uint32_t frames;
        unsigned short bits = 0;
        double rate = 0.0;
        uint32_t offset = 0;
        uint32_t blocksize = 0;
        int foundcomm = 0, foundmark = 0, foundinstr = 0, is_sowt = 0;
        struct mark {
                unsigned short id;
                uint32_t position;
                char name[40]; 
        } marks[32];
        unsigned short looptype;
        int i, j;
        unsigned short nmarks = 0;
        unsigned short sustainLoopBegin = 0, sustainLoopEnd = 0,
                       releaseLoopBegin = 0, releaseLoopEnd = 0;
        sox_ssize_t seekto = 0;
        sox_size_t ssndsize = 0;
        char *author;
        char *copyright;
        char *nametext;

        uint8_t trash8;
        uint16_t trash16;
        uint32_t trash32;

        int rc;

        /* FORM chunk */
        if (sox_reads(ft, buf, 4) == SOX_EOF || strncmp(buf, "FORM", 4) != 0)
        {
                sox_fail_errno(ft,SOX_EHDR,"AIFF header does not begin with magic word 'FORM'");
                return(SOX_EOF);
        }
        sox_readdw(ft, &totalsize);
        if (sox_reads(ft, buf, 4) == SOX_EOF || (strncmp(buf, "AIFF", 4) != 0 && 
            strncmp(buf, "AIFC", 4) != 0))
        {
                sox_fail_errno(ft,SOX_EHDR,"AIFF 'FORM' chunk does not specify 'AIFF' or 'AIFC' as type");
                return(SOX_EOF);
        }

        
        /* Skip everything but the COMM chunk and the SSND chunk */
        /* The SSND chunk must be the last in the file */
        while (1) {
                if (sox_reads(ft, buf, 4) == SOX_EOF)
                {
                        if (ssndsize > 0)
                        {
                                break;
                        }
                        else
                        {
                                sox_fail_errno(ft,SOX_EHDR,"Missing SSND chunk in AIFF file");
                                return(SOX_EOF);
                        }
                }
                if (strncmp(buf, "COMM", 4) == 0) {
                        /* COMM chunk */
                        sox_readdw(ft, &chunksize);
                        sox_readw(ft, &channels);
                        sox_readdw(ft, &frames);
                        sox_readw(ft, &bits);
                        rate = read_ieee_extended(ft);
                        chunksize -= 18;
                        if (chunksize > 0)
                        {
                            sox_reads(ft, buf, 4);
                            chunksize -= 4;
                            if (strncmp(buf, "sowt", 4) == 0)
                            {
                                /* CD audio as read on Mac OS machines */
                                /* Need to endian swap all the data */
                                is_sowt = 1;
                            }
                            else if (strncmp(buf, "NONE", 4) != 0)
                            {
                                buf[4] = 0;
                                sox_fail_errno(ft,SOX_EHDR,"AIFC files that contain compressed data are not supported: %s",buf);
                                return(SOX_EOF);
                            }
                        }
                        while(chunksize-- > 0)
                            sox_readb(ft, (unsigned char *)&trash8);
                        foundcomm = 1;
                }
                else if (strncmp(buf, "SSND", 4) == 0) {
                        /* SSND chunk */
                        sox_readdw(ft, &chunksize);
                        sox_readdw(ft, &offset);
                        sox_readdw(ft, &blocksize);
                        chunksize -= 8;
                        ssndsize = chunksize;
                        /* word-align chunksize in case it wasn't
                         * done by writing application already.
                         */
                        chunksize += (chunksize % 2);
                        /* if can't seek, just do sound now */
                        if (!ft->seekable)
                                break;
                        /* else, seek to end of sound and hunt for more */
                        seekto = sox_tell(ft);
                        sox_seeki(ft, (sox_ssize_t)chunksize, SEEK_CUR); 
                }
                else if (strncmp(buf, "MARK", 4) == 0) {
                        /* MARK chunk */
                        sox_readdw(ft, &chunksize);
                        sox_readw(ft, &nmarks);

                        /* Some programs like to always have a MARK chunk
                         * but will set number of marks to 0 and force
                         * software to detect and ignore it.
                         */
                        if (nmarks == 0)
                            foundmark = 0;
                        else
                            foundmark = 1;

                        /* Make sure its not larger then we support */
                        if (nmarks > 32)
                            nmarks = 32;

                        if (chunksize > 2)
                            chunksize -= 2;
                        for(i = 0; i < nmarks && chunksize; i++) {
                                unsigned char len, read_len, tmp_c;

                                if (chunksize < 6)
                                    break;
                                sox_readw(ft, &(marks[i].id));
                                sox_readdw(ft, &(marks[i].position));
                                chunksize -= 6;
                                /* If error reading length then
                                 * don't try to read more bytes
                                 * based on that value.
                                 */
                                if (sox_readb(ft, &len) != SOX_SUCCESS)
                                    break;
                                --chunksize;
                                if (len > chunksize)
                                    len = chunksize;
                                read_len = len;
                                if (read_len > 39)
                                    read_len = 39;
                                for(j = 0; j < len && chunksize; j++) 
                                {
                                    sox_readb(ft, &tmp_c);
                                    if (j < read_len)
                                        marks[i].name[j] = tmp_c;
                                    chunksize--;
                                }
                                marks[i].name[read_len] = 0;
                                if ((len & 1) == 0 && chunksize) {
                                        chunksize--;
                                        sox_readb(ft, (unsigned char *)&trash8);
                                }
                        }
                        /* HA HA!  Sound Designer (and others) makes */
                        /* bogus files. It spits out bogus chunksize */
                        /* for MARK field */
                        while(chunksize-- > 0)
                            sox_readb(ft, (unsigned char *)&trash8);
                }
                else if (strncmp(buf, "INST", 4) == 0) {
                        /* INST chunk */
                        sox_readdw(ft, &chunksize);
                        sox_readb(ft, (unsigned char *)&(ft->instr.MIDInote));
                        sox_readb(ft, (unsigned char *)&trash8);
                        sox_readb(ft, (unsigned char *)&(ft->instr.MIDIlow));
                        sox_readb(ft, (unsigned char *)&(ft->instr.MIDIhi));
                        /* Low  velocity */
                        sox_readb(ft, (unsigned char *)&trash8);
                        /* Hi  velocity */
                        sox_readb(ft, (unsigned char *)&trash8);
                        sox_readw(ft, (unsigned short *)&trash16);/* gain */
                        sox_readw(ft, &looptype); /* sustain loop */
                        ft->loops[0].type = looptype;
                        sox_readw(ft, &sustainLoopBegin); /* begin marker */
                        sox_readw(ft, &sustainLoopEnd);    /* end marker */
                        sox_readw(ft, &looptype); /* release loop */
                        ft->loops[1].type = looptype;
                        sox_readw(ft, &releaseLoopBegin);  /* begin marker */
                        sox_readw(ft, &releaseLoopEnd);    /* end marker */

                        foundinstr = 1;
                }
                else if (strncmp(buf, "APPL", 4) == 0) {
                        sox_readdw(ft, &chunksize);
                        /* word-align chunksize in case it wasn't
                         * done by writing application already.
                         */
                        chunksize += (chunksize % 2);
                        while(chunksize-- > 0)
                            sox_readb(ft, (unsigned char *)&trash8);
                }
                else if (strncmp(buf, "ALCH", 4) == 0) {
                        /* I think this is bogus and gets grabbed by APPL */
                        /* INST chunk */
                        sox_readdw(ft, &trash32);                /* ENVS - jeez! */
                        sox_readdw(ft, &chunksize);
                        while(chunksize-- > 0)
                            sox_readb(ft, (unsigned char *)&trash8);
                }
                else if (strncmp(buf, "ANNO", 4) == 0) {
                        rc = textChunk(&(ft->comment), "Annotation:", ft);
                        if (rc)
                        {
                          /* Fail already called in function */
                          return(SOX_EOF);
                        }
                }
                else if (strncmp(buf, "COMT", 4) == 0) {
                  rc = commentChunk(&(ft->comment), "Comment:", ft);
                  if (rc) {
                    /* Fail already called in function */
                    return(SOX_EOF);
                  }
                }
                else if (strncmp(buf, "AUTH", 4) == 0) {
                  /* Author chunk */
                  rc = textChunk(&author, "Author:", ft);
                  if (rc)
                  {
                      /* Fail already called in function */
                      return(SOX_EOF);
                  }
                  free(author);
                }
                else if (strncmp(buf, "NAME", 4) == 0) {
                  /* Name chunk */
                  rc = textChunk(&nametext, "Name:", ft);
                  if (rc)
                  {
                      /* Fail already called in function */
                      return(SOX_EOF);
                  }
                  free(nametext);
                }
                else if (strncmp(buf, "(c) ", 4) == 0) {
                  /* Copyright chunk */
                  rc = textChunk(&copyright, "Copyright:", ft);
                  if (rc)
                  {
                      /* Fail already called in function */
                      return(SOX_EOF);
                  }
                  free(copyright);
                }
                else {
                        if (sox_eof(ft))
                                break;
                        buf[4] = 0;
                        sox_debug("AIFFstartread: ignoring '%s' chunk", buf);
                        sox_readdw(ft, &chunksize);
                        if (sox_eof(ft))
                                break;
                        /* Skip the chunk using sox_readb() so we may read
                           from a pipe */
                        while (chunksize-- > 0) {
                            if (sox_readb(ft, (unsigned char *)&trash8) == SOX_EOF)
                                        break;
                        }
                }
                if (sox_eof(ft))
                        break;
        }

        /* 
         * if a pipe, we lose all chunks after sound.  
         * Like, say, instrument loops. 
         */
        if (ft->seekable)
        {
                if (seekto > 0)
                        sox_seeki(ft, seekto, SEEK_SET);
                else
                {
                        sox_fail_errno(ft,SOX_EOF,"AIFF: no sound data on input file");
                        return(SOX_EOF);
                }
        }
        /* SSND chunk just read */
        if (blocksize != 0)
            sox_warn("AIFF header has invalid blocksize.  Ignoring but expect a premature EOF");

        while (offset-- > 0) {
                if (sox_readb(ft, (unsigned char *)&trash8) == SOX_EOF)
                {
                        sox_fail_errno(ft,errno,"unexpected EOF while skipping AIFF offset");
                        return(SOX_EOF);
                }
        }

        if (foundcomm) {
                ft->signal.channels = channels;
                ft->signal.rate = rate;
                if (ft->signal.encoding != SOX_ENCODING_UNKNOWN && ft->signal.encoding != SOX_ENCODING_SIGN2)
                    sox_report("AIFF only supports signed data.  Forcing to signed.");
                ft->signal.encoding = SOX_ENCODING_SIGN2;
                if (bits <= 8)
                {
                    ft->signal.size = SOX_SIZE_BYTE;
                    if (bits < 8)
                        sox_report("Forcing data size from %d bits to 8 bits",bits);
                }
                else if (bits <= 16)
                {
                    ft->signal.size = SOX_SIZE_16BIT;
                    if (bits < 16)
                        sox_report("Forcing data size from %d bits to 16 bits",bits);
                }
                else if (bits <= 24)
                {
                    ft->signal.size = SOX_SIZE_24BIT;
                    if (bits < 24)
                        sox_report("Forcing data size from %d bits to 24 bits",bits);
                }
                else if (bits <= 32)
                {
                    ft->signal.size = SOX_SIZE_32BIT;
                    if (bits < 32)
                        sox_report("Forcing data size from %d bits to 32 bits",bits);
                }
                else
                {
                    sox_fail_errno(ft,SOX_EFMT,"unsupported sample size in AIFF header: %d", bits);
                    return(SOX_EOF);
                }
        } else  {
                if ((ft->signal.channels == 0)
                        || (ft->signal.rate == 0)
                        || (ft->signal.encoding == SOX_ENCODING_UNKNOWN)
                        || (ft->signal.size == -1)) {
                  sox_report("You must specify # channels, sample rate, signed/unsigned,");
                  sox_report("and 8/16 on the command line.");
                  sox_fail_errno(ft,SOX_EFMT,"Bogus AIFF file: no COMM section.");
                  return(SOX_EOF);
                }

        }

        aiff->nsamples = ssndsize / ft->signal.size;

        /* Cope with 'sowt' CD tracks as read on Macs */
        if (is_sowt)
        {
                aiff->nsamples -= 4;
                ft->signal.reverse_bytes = !ft->signal.reverse_bytes;
        }
        
        if (foundmark && !foundinstr)
        {
            sox_debug("Ignoring MARK chunk since no INSTR found.");
            foundmark = 0;
        }
        if (!foundmark && foundinstr)
        {
            sox_debug("Ignoring INSTR chunk since no MARK found.");
            foundinstr = 0;
        }
        if (foundmark && foundinstr) {
                int i;
                int slbIndex = 0, sleIndex = 0;
                int rlbIndex = 0, rleIndex = 0;

                /* find our loop markers and save their marker indexes */
                for(i = 0; i < nmarks; i++) { 
                  if(marks[i].id == sustainLoopBegin)
                    slbIndex = i;
                  if(marks[i].id == sustainLoopEnd)
                    sleIndex = i;
                  if(marks[i].id == releaseLoopBegin)
                    rlbIndex = i;
                  if(marks[i].id == releaseLoopEnd)
                    rleIndex = i;
                }

                ft->instr.nloops = 0;
                if (ft->loops[0].type != 0) {
                        ft->loops[0].start = marks[slbIndex].position;
                        ft->loops[0].length = 
                            marks[sleIndex].position - marks[slbIndex].position;
                        /* really the loop count should be infinite */
                        ft->loops[0].count = 1; 
                        ft->instr.loopmode = SOX_LOOP_SUSTAIN_DECAY | ft->loops[0].type;
                        ft->instr.nloops++;
                }
                if (ft->loops[1].type != 0) {
                        ft->loops[1].start = marks[rlbIndex].position;
                        ft->loops[1].length = 
                            marks[rleIndex].position - marks[rlbIndex].position;
                        /* really the loop count should be infinite */
                        ft->loops[1].count = 1;
                        ft->instr.loopmode = SOX_LOOP_SUSTAIN_DECAY | ft->loops[1].type;
                        ft->instr.nloops++;
                } 
        }
        reportInstrument(ft);

        /* Needed because of sox_rawread() */
        rc = sox_rawstartread(ft);
        if (rc)
            return rc;

        ft->length = aiff->nsamples;    /* for seeking */
        aiff->dataStart = sox_tell(ft);

        return(SOX_SUCCESS);
}

/* print out the MIDI key allocations, loop points, directions etc */
static void reportInstrument(ft_t ft)
{
  unsigned loopNum;

  if(ft->instr.nloops > 0)
    sox_report("AIFF Loop markers:");
  for(loopNum  = 0; loopNum < ft->instr.nloops; loopNum++) {
    if (ft->loops[loopNum].count) {
      sox_report("Loop %d: start: %6d", loopNum, ft->loops[loopNum].start);
      sox_report(" end:   %6d", 
              ft->loops[loopNum].start + ft->loops[loopNum].length);
      sox_report(" count: %6d", ft->loops[loopNum].count);
      sox_report(" type:  ");
      switch(ft->loops[loopNum].type & ~SOX_LOOP_SUSTAIN_DECAY) {
      case 0: sox_report("off"); break;
      case 1: sox_report("forward"); break;
      case 2: sox_report("forward/backward"); break;
      }
    }
  }
  sox_report("Unity MIDI Note: %d", ft->instr.MIDInote);
  sox_report("Low   MIDI Note: %d", ft->instr.MIDIlow);
  sox_report("High  MIDI Note: %d", ft->instr.MIDIhi);
}

/* Process a text chunk, allocate memory, display it if verbose and return */
static int textChunk(char **text, char *chunkDescription, ft_t ft) 
{
  uint32_t chunksize;
  sox_readdw(ft, &chunksize);
  /* allocate enough memory to hold the text including a terminating \0 */
  *text = (char *) xmalloc((size_t) chunksize + 1);
  if (sox_readbuf(ft, *text, chunksize) != chunksize)
  {
    sox_fail_errno(ft,SOX_EOF,"AIFF: Unexpected EOF in %s header", chunkDescription);
    return(SOX_EOF);
  }
  *(*text + chunksize) = '\0';
        if (chunksize % 2)
        {
                /* Read past pad byte */
                char c;
                if (sox_readbuf(ft, &c, 1) != 1)
                {
                sox_fail_errno(ft,SOX_EOF,"AIFF: Unexpected EOF in %s header", chunkDescription);
                        return(SOX_EOF);
                }
        }
  sox_debug("%-10s   \"%s\"", chunkDescription, *text);
  return(SOX_SUCCESS);
}

/* Comment lengths are words, not double words, and we can have several, so
   we use a special function, not textChunk().;
 */
static int commentChunk(char **text, char *chunkDescription, ft_t ft)
{
  uint32_t chunksize;
  unsigned short numComments;
  uint32_t timeStamp;
  unsigned short markerId;
  unsigned short totalCommentLength = 0;
  unsigned int totalReadLength = 0;
  unsigned int commentIndex;

  sox_readdw(ft, &chunksize);
  sox_readw(ft, &numComments);
  totalReadLength += 2; /* chunksize doesn't count */
  for(commentIndex = 0; commentIndex < numComments; commentIndex++) {
    unsigned short commentLength;

    sox_readdw(ft, &timeStamp);
    sox_readw(ft, &markerId);
    sox_readw(ft, &commentLength);
    if (((size_t)totalCommentLength) + commentLength > USHRT_MAX) {
        sox_fail_errno(ft,SOX_EOF,"AIFF: Comment too long in %s header", chunkDescription);
        return(SOX_EOF);
    }
    totalCommentLength += commentLength;
    /* allocate enough memory to hold the text including a terminating \0 */
    if(commentIndex == 0) {
      *text = (char *) xmalloc((size_t) totalCommentLength + 1);
    }
    else {
      *text = xrealloc(*text, (size_t) totalCommentLength + 1);
    }

    if (sox_readbuf(ft, *text + totalCommentLength - commentLength, commentLength) != commentLength) {
        sox_fail_errno(ft,SOX_EOF,"AIFF: Unexpected EOF in %s header", chunkDescription);
        return(SOX_EOF);
    }
    *(*text + totalCommentLength) = '\0';
    totalReadLength += totalCommentLength + 4 + 2 + 2; /* include header */
    if (commentLength % 2) {
        /* Read past pad byte */
        char c;
        if (sox_readbuf(ft, &c, 1) != 1) {
            sox_fail_errno(ft,SOX_EOF,"AIFF: Unexpected EOF in %s header", chunkDescription);
            return(SOX_EOF);
        }
    }
  }
  sox_debug("%-10s   \"%s\"", chunkDescription, *text);
  /* make sure we read the whole chunk */
  if (totalReadLength < chunksize) {
       size_t i;
       char c;
       for (i=0; i < chunksize - totalReadLength; i++ )
           sox_readbuf(ft, &c, 1);
  }
  return(SOX_SUCCESS);
}

sox_size_t sox_aiffread(ft_t ft, sox_ssample_t *buf, sox_size_t len)
{
        aiff_t aiff = (aiff_t ) ft->priv;
        sox_ssize_t done;

        if ((sox_size_t)len > aiff->nsamples)
                len = aiff->nsamples;
        done = sox_rawread(ft, buf, len);
        if (done == 0 && aiff->nsamples != 0)
                sox_warn("Premature EOF on AIFF input file");
        aiff->nsamples -= done;
        return done;
}

int sox_aiffstopread(ft_t ft) 
{
        char buf[5];
        uint32_t chunksize;
        uint32_t trash;

        if (!ft->seekable)
        {
            while (! sox_eof(ft)) 
            {
                if (sox_readbuf(ft, buf, 4) != 4)
                        break;

                sox_readdw(ft, &chunksize);
                if (sox_eof(ft))
                        break;
                buf[4] = '\0';
                sox_warn("Ignoring AIFF tail chunk: '%s', %d bytes long", 
                        buf, chunksize);
                if (! strcmp(buf, "MARK") || ! strcmp(buf, "INST"))
                        sox_warn("       You're stripping MIDI/loop info!");
                while (chunksize-- > 0) 
                {
                        if (sox_readb(ft, (unsigned char *)&trash) == SOX_EOF)
                                break;
                }
            }
        }

        /* Needed because of sox_rawwrite() */
        return sox_rawstopread(ft);
}

/* When writing, the header is supposed to contain the number of
   samples and data bytes written.
   Since we don't know how many samples there are until we're done,
   we first write the header with an very large number,
   and at the end we rewind the file and write the header again
   with the right number.  This only works if the file is seekable;
   if it is not, the very large size remains in the header.
   Strictly spoken this is not legal, but the playaiff utility
   will still be able to play the resulting file. */

int sox_aiffstartwrite(ft_t ft)
{
        aiff_t aiff = (aiff_t ) ft->priv;
        int rc;

        /* Needed because sox_rawwrite() */
        rc = sox_rawstartwrite(ft);
        if (rc)
            return rc;

        aiff->nsamples = 0;
        if (ft->signal.encoding < SOX_ENCODING_SIZE_IS_WORD && 
            ft->signal.size == SOX_SIZE_BYTE) {
                sox_report("expanding compressed bytes to signed 16 bits");
                ft->signal.encoding = SOX_ENCODING_SIGN2;
                ft->signal.size = SOX_SIZE_16BIT;
        }
        if (ft->signal.encoding != SOX_ENCODING_UNKNOWN && ft->signal.encoding != SOX_ENCODING_SIGN2)
            sox_report("AIFF only supports signed data.  Forcing to signed.");
        ft->signal.encoding = SOX_ENCODING_SIGN2; /* We have a fixed encoding */

        /* Compute the "very large number" so that a maximum number
           of samples can be transmitted through a pipe without the
           risk of causing overflow when calculating the number of bytes.
           At 48 kHz, 16 bits stereo, this gives ~3 hours of music.
           Sorry, the AIFF format does not provide for an "infinite"
           number of samples. */
        return(aiffwriteheader(ft, 0x7f000000 / (ft->signal.size*ft->signal.channels)));
}

sox_size_t sox_aiffwrite(ft_t ft, const sox_ssample_t *buf, sox_size_t len)
{
        aiff_t aiff = (aiff_t ) ft->priv;
        aiff->nsamples += len;
        sox_rawwrite(ft, buf, len);
        return(len);
}

int sox_aiffstopwrite(ft_t ft)
{
        aiff_t aiff = (aiff_t ) ft->priv;
        int rc;

        /* If we've written an odd number of bytes, write a padding
           NUL */
        if (aiff->nsamples % 2 == 1 && ft->signal.size == 1 && ft->signal.channels == 1)
        {
            sox_ssample_t buf = 0;
            sox_rawwrite(ft, &buf, 1);
        }

        /* Needed because of sox_rawwrite().  Call now to flush
         * buffer now before seeking around below.
         */
        rc = sox_rawstopwrite(ft);
        if (rc)
            return rc;

        if (!ft->seekable)
        {
            sox_fail_errno(ft,SOX_EOF,"Non-seekable file.");
            return(SOX_EOF);
        }
        if (sox_seeki(ft, 0, SEEK_SET) != 0)
        {
                sox_fail_errno(ft,errno,"can't rewind output file to rewrite AIFF header");
                return(SOX_EOF);
        }
        return(aiffwriteheader(ft, aiff->nsamples / ft->signal.channels));
}

static int aiffwriteheader(ft_t ft, sox_size_t nframes)
{
        int hsize =
                8 /*COMM hdr*/ + 18 /*COMM chunk*/ +
                8 /*SSND hdr*/ + 12 /*SSND chunk*/;
        unsigned bits = 0;
        unsigned i;
        sox_size_t padded_comment_size = 0, comment_size = 0;
        sox_size_t comment_chunk_size = 0;

        /* MARK and INST chunks */
        if (ft->instr.nloops) {
          hsize += 8 /* MARK hdr */ + 2 + 16*ft->instr.nloops;
          hsize += 8 /* INST hdr */ + 20; /* INST chunk */
        }

        if (ft->signal.encoding == SOX_ENCODING_SIGN2 && 
            ft->signal.size == SOX_SIZE_BYTE)
                bits = 8;
        else if (ft->signal.encoding == SOX_ENCODING_SIGN2 && 
                 ft->signal.size == SOX_SIZE_16BIT)
                bits = 16;
        else if (ft->signal.encoding == SOX_ENCODING_SIGN2 && 
                 ft->signal.size == SOX_SIZE_24BIT)
                bits = 24;
        else if (ft->signal.encoding == SOX_ENCODING_SIGN2 && 
                 ft->signal.size == SOX_SIZE_32BIT)
                bits = 32;
        else
        {
                sox_fail_errno(ft,SOX_EFMT,"unsupported output encoding/size for AIFF header");
                return(SOX_EOF);
        }

        /* COMT comment chunk -- holds comments text with a timestamp and marker id */
        /* We calculate the comment_chunk_size if we will be writing a comment */
        if (ft->comment)
        {
          comment_size = strlen(ft->comment);
          /* Must put an even number of characters out.
           * True 68k processors OS's seem to require this.
           */
          padded_comment_size = ((comment_size % 2) == 0) ?
                                comment_size : comment_size + 1;
          /* one comment, timestamp, marker ID and text count */
          comment_chunk_size = (2 + 4 + 2 + 2 + padded_comment_size);
          hsize += 8 /* COMT hdr */ + comment_chunk_size; 
        }

        sox_writes(ft, "FORM"); /* IFF header */
        /* file size */
        sox_writedw(ft, hsize + nframes * ft->signal.size * ft->signal.channels); 
        sox_writes(ft, "AIFF"); /* File type */

        /* Now we write the COMT comment chunk using the precomputed sizes */
        if (ft->comment)
        {
          sox_writes(ft, "COMT");
          sox_writedw(ft, comment_chunk_size);

          /* one comment */
          sox_writew(ft, 1);

          /* time stamp of comment, Unix knows of time from 1/1/1970,
             Apple knows time from 1/1/1904 */
          sox_writedw(ft, (unsigned)(time(NULL) + 2082844800));

          /* A marker ID of 0 indicates the comment is not associated
             with a marker */
          sox_writew(ft, 0);

          /* now write the count and the bytes of text */
          sox_writew(ft, padded_comment_size);
          sox_writes(ft, ft->comment);
          if (comment_size != padded_comment_size)
                sox_writes(ft, " ");
        }

        /* COMM chunk -- describes encoding (and #frames) */
        sox_writes(ft, "COMM");
        sox_writedw(ft, 18); /* COMM chunk size */
        sox_writew(ft, ft->signal.channels); /* nchannels */
        sox_writedw(ft, nframes); /* number of frames */
        sox_writew(ft, bits); /* sample width, in bits */
        write_ieee_extended(ft, (double)ft->signal.rate);

        /* MARK chunk -- set markers */
        if (ft->instr.nloops) {
                sox_writes(ft, "MARK");
                if (ft->instr.nloops > 2)
                        ft->instr.nloops = 2;
                sox_writedw(ft, 2 + 16u*ft->instr.nloops);
                sox_writew(ft, ft->instr.nloops);

                for(i = 0; i < ft->instr.nloops; i++) {
                        sox_writew(ft, i + 1);
                        sox_writedw(ft, ft->loops[i].start);
                        sox_writeb(ft, 0);
                        sox_writeb(ft, 0);
                        sox_writew(ft, i*2 + 1);
                        sox_writedw(ft, ft->loops[i].start + ft->loops[i].length);
                        sox_writeb(ft, 0);
                        sox_writeb(ft, 0);
                }

                sox_writes(ft, "INST");
                sox_writedw(ft, 20);
                /* random MIDI shit that we default on */
                sox_writeb(ft, (uint8_t)ft->instr.MIDInote);
                sox_writeb(ft, 0);                       /* detune */
                sox_writeb(ft, (uint8_t)ft->instr.MIDIlow);
                sox_writeb(ft, (uint8_t)ft->instr.MIDIhi);
                sox_writeb(ft, 1);                       /* low velocity */
                sox_writeb(ft, 127);                     /* hi  velocity */
                sox_writew(ft, 0);                               /* gain */

                /* sustain loop */
                sox_writew(ft, ft->loops[0].type);
                sox_writew(ft, 1);                               /* marker 1 */
                sox_writew(ft, 3);                               /* marker 3 */
                /* release loop, if there */
                if (ft->instr.nloops == 2) {
                        sox_writew(ft, ft->loops[1].type);
                        sox_writew(ft, 2);                       /* marker 2 */
                        sox_writew(ft, 4);                       /* marker 4 */
                } else {
                        sox_writew(ft, 0);                       /* no release loop */
                        sox_writew(ft, 0);
                        sox_writew(ft, 0);
                }
        }

        /* SSND chunk -- describes data */
        sox_writes(ft, "SSND");
        /* chunk size */
        sox_writedw(ft, 8 + nframes * ft->signal.channels * ft->signal.size); 
        sox_writedw(ft, 0); /* offset */
        sox_writedw(ft, 0); /* block size */
        return(SOX_SUCCESS);
}

int sox_aifcstartwrite(ft_t ft)
{
        aiff_t aiff = (aiff_t ) ft->priv;
        int rc;

        /* Needed because sox_rawwrite() */
        rc = sox_rawstartwrite(ft);
        if (rc)
            return rc;

        aiff->nsamples = 0;
        if (ft->signal.encoding < SOX_ENCODING_SIZE_IS_WORD && 
            ft->signal.size == SOX_SIZE_BYTE) {
                sox_report("expanding compressed bytes to signed 16 bits");
                ft->signal.encoding = SOX_ENCODING_SIGN2;
                ft->signal.size = SOX_SIZE_16BIT;
        }
        if (ft->signal.encoding != SOX_ENCODING_UNKNOWN && ft->signal.encoding != SOX_ENCODING_SIGN2)
            sox_report("AIFC only supports signed data.  Forcing to signed.");
        ft->signal.encoding = SOX_ENCODING_SIGN2; /* We have a fixed encoding */

        /* Compute the "very large number" so that a maximum number
           of samples can be transmitted through a pipe without the
           risk of causing overflow when calculating the number of bytes.
           At 48 kHz, 16 bits stereo, this gives ~3 hours of music.
           Sorry, the AIFC format does not provide for an "infinite"
           number of samples. */
        return(aifcwriteheader(ft, 0x7f000000 / (ft->signal.size*ft->signal.channels)));
}

int sox_aifcstopwrite(ft_t ft)
{
        aiff_t aiff = (aiff_t ) ft->priv;
        int rc;

        /* If we've written an odd number of bytes, write a padding
           NUL */
        if (aiff->nsamples % 2 == 1 && ft->signal.size == 1 && ft->signal.channels == 1)
        {
            sox_ssample_t buf = 0;
            sox_rawwrite(ft, &buf, 1);
        }

        /* Needed because of sox_rawwrite().  Call now to flush
         * buffer now before seeking around below.
         */
        rc = sox_rawstopwrite(ft);
        if (rc)
            return rc;

        if (!ft->seekable)
        {
            sox_fail_errno(ft,SOX_EOF,"Non-seekable file.");
            return(SOX_EOF);
        }
        if (sox_seeki(ft, 0, SEEK_SET) != 0)
        {
                sox_fail_errno(ft,errno,"can't rewind output file to rewrite AIFC header");
                return(SOX_EOF);
        }
        return(aifcwriteheader(ft, aiff->nsamples / ft->signal.channels));
}

static int aifcwriteheader(ft_t ft, sox_size_t nframes)
{
        unsigned hsize =
                12 /*FVER*/ + 8 /*COMM hdr*/ + 18+4+1+15 /*COMM chunk*/ +
                8 /*SSND hdr*/ + 12 /*SSND chunk*/;
        unsigned bits = 0;

        if (ft->signal.encoding == SOX_ENCODING_SIGN2 && 
            ft->signal.size == SOX_SIZE_BYTE)
                bits = 8;
        else if (ft->signal.encoding == SOX_ENCODING_SIGN2 && 
                 ft->signal.size == SOX_SIZE_16BIT)
                bits = 16;
        else if (ft->signal.encoding == SOX_ENCODING_SIGN2 && 
                 ft->signal.size == SOX_SIZE_24BIT)
                bits = 24;
        else if (ft->signal.encoding == SOX_ENCODING_SIGN2 && 
                 ft->signal.size == SOX_SIZE_32BIT)
                bits = 32;
        else
        {
                sox_fail_errno(ft,SOX_EFMT,"unsupported output encoding/size for AIFC header");
                return(SOX_EOF);
        }

        sox_writes(ft, "FORM"); /* IFF header */
        /* file size */
        sox_writedw(ft, hsize + nframes * ft->signal.size * ft->signal.channels); 
        sox_writes(ft, "AIFC"); /* File type */

        /* FVER chunk */
        sox_writes(ft, "FVER");
        sox_writedw(ft, 4); /* FVER chunk size */
        sox_writedw(ft, 0xa2805140); /* version_date(May23,1990,2:40pm) */

        /* COMM chunk -- describes encoding (and #frames) */
        sox_writes(ft, "COMM");
        sox_writedw(ft, 18+4+1+15); /* COMM chunk size */
        sox_writew(ft, ft->signal.channels); /* nchannels */
        sox_writedw(ft, nframes); /* number of frames */
        sox_writew(ft, bits); /* sample width, in bits */
        write_ieee_extended(ft, (double)ft->signal.rate);

        sox_writes(ft, "NONE"); /*compression_type*/
        sox_writeb(ft, 14);
        sox_writes(ft, "not compressed");
        sox_writeb(ft, 0);

        /* SSND chunk -- describes data */
        sox_writes(ft, "SSND");
        /* chunk size */
        sox_writedw(ft, 8 + nframes * ft->signal.channels * ft->signal.size); 
        sox_writedw(ft, 0); /* offset */
        sox_writedw(ft, 0); /* block size */

        /* Any Private chunks shall appear after the required chunks (FORM,FVER,COMM,SSND) */
        return(SOX_SUCCESS);
}

static double read_ieee_extended(ft_t ft)
{
        char buf[10];
        if (sox_readbuf(ft, buf, 10) != 10)
        {
                sox_fail_errno(ft,SOX_EOF,"EOF while reading IEEE extended number");
                return(SOX_EOF);
        }
        return ConvertFromIeeeExtended((unsigned char *)buf);
}

static void write_ieee_extended(ft_t ft, double x)
{
        char buf[10];
        ConvertToIeeeExtended(x, buf);
        sox_debug_more("converted %g to %o %o %o %o %o %o %o %o %o %o",
                x,
                buf[0], buf[1], buf[2], buf[3], buf[4],
                buf[5], buf[6], buf[7], buf[8], buf[9]);
        (void)sox_writebuf(ft, buf, 10);
}


/*
 * C O N V E R T   T O   I E E E   E X T E N D E D
 */

/* Copyright (C) 1988-1991 Apple Computer, Inc.
 * All rights reserved.
 *
 * Machine-independent I/O routines for IEEE floating-point numbers.
 *
 * NaN's and infinities are converted to HUGE_VAL, which
 * happens to be infinity on IEEE machines.  Unfortunately, it is
 * impossible to preserve NaN's in a machine-independent way.
 * Infinities are, however, preserved on IEEE machines.
 *
 * These routines have been tested on the following machines:
 *    Apple Macintosh, MPW 3.1 C compiler
 *    Apple Macintosh, THINK C compiler
 *    Silicon Graphics IRIS, MIPS compiler
 *    Cray X/MP and Y/MP
 *    Digital Equipment VAX
 *
 *
 * Implemented by Malcolm Slaney and Ken Turkowski.
 *
 * Malcolm Slaney contributions during 1988-1990 include big- and little-
 * endian file I/O, conversion to and from Motorola's extended 80-bit
 * floating-point format, and conversions to and from IEEE single-
 * precision floating-point format.
 *
 * In 1991, Ken Turkowski implemented the conversions to and from
 * IEEE double-precision format, added more precision to the extended
 * conversions, and accommodated conversions involving +/- infinity,
 * NaN's, and denormalized numbers.
 */

#define FloatToUnsigned(f) ((uint32_t)(((int32_t)(f - 2147483648.0)) + 2147483647) + 1)

static void ConvertToIeeeExtended(double num, char *bytes)
{
    int    sign;
    int expon;
    double fMant, fsMant;
    uint32_t hiMant, loMant;

    if (num < 0) {
        sign = 0x8000;
        num *= -1;
    } else {
        sign = 0;
    }

    if (num == 0) {
        expon = 0; hiMant = 0; loMant = 0;
    }
    else {
        fMant = frexp(num, &expon);
        if ((expon > 16384) || !(fMant < 1)) {    /* Infinity or NaN */
            expon = sign|0x7FFF; hiMant = 0; loMant = 0; /* infinity */
        }
        else {    /* Finite */
            expon += 16382;
            if (expon < 0) {    /* denormalized */
                fMant = ldexp(fMant, expon);
                expon = 0;
            }
            expon |= sign;
            fMant = ldexp(fMant, 32);          
            fsMant = floor(fMant); 
            hiMant = FloatToUnsigned(fsMant);
            fMant = ldexp(fMant - fsMant, 32); 
            fsMant = floor(fMant); 
            loMant = FloatToUnsigned(fsMant);
        }
    }
    
    bytes[0] = expon >> 8;
    bytes[1] = expon;
    bytes[2] = hiMant >> 24;
    bytes[3] = hiMant >> 16;
    bytes[4] = hiMant >> 8;
    bytes[5] = hiMant;
    bytes[6] = loMant >> 24;
    bytes[7] = loMant >> 16;
    bytes[8] = loMant >> 8;
    bytes[9] = loMant;
}


/*
 * C O N V E R T   F R O M   I E E E   E X T E N D E D  
 */

/* 
 * Copyright (C) 1988-1991 Apple Computer, Inc.
 * All rights reserved.
 *
 * Machine-independent I/O routines for IEEE floating-point numbers.
 *
 * NaN's and infinities are converted to HUGE_VAL, which
 * happens to be infinity on IEEE machines.  Unfortunately, it is
 * impossible to preserve NaN's in a machine-independent way.
 * Infinities are, however, preserved on IEEE machines.
 *
 * These routines have been tested on the following machines:
 *    Apple Macintosh, MPW 3.1 C compiler
 *    Apple Macintosh, THINK C compiler
 *    Silicon Graphics IRIS, MIPS compiler
 *    Cray X/MP and Y/MP
 *    Digital Equipment VAX
 *
 *
 * Implemented by Malcolm Slaney and Ken Turkowski.
 *
 * Malcolm Slaney contributions during 1988-1990 include big- and little-
 * endian file I/O, conversion to and from Motorola's extended 80-bit
 * floating-point format, and conversions to and from IEEE single-
 * precision floating-point format.
 *
 * In 1991, Ken Turkowski implemented the conversions to and from
 * IEEE double-precision format, added more precision to the extended
 * conversions, and accommodated conversions involving +/- infinity,
 * NaN's, and denormalized numbers.
 */

#define UnsignedToFloat(u)         (((double)((int32_t)(u - 2147483647 - 1))) + 2147483648.0)

/****************************************************************
 * Extended precision IEEE floating-point conversion routine.
 ****************************************************************/

static double ConvertFromIeeeExtended(unsigned char *bytes)
{
    double    f;
    int    expon;
    uint32_t hiMant, loMant;
    
    expon = ((bytes[0] & 0x7F) << 8) | (bytes[1] & 0xFF);
    hiMant    =    ((uint32_t)(bytes[2] & 0xFF) << 24)
            |    ((uint32_t)(bytes[3] & 0xFF) << 16)
            |    ((uint32_t)(bytes[4] & 0xFF) << 8)
            |    ((uint32_t)(bytes[5] & 0xFF));
    loMant    =    ((uint32_t)(bytes[6] & 0xFF) << 24)
            |    ((uint32_t)(bytes[7] & 0xFF) << 16)
            |    ((uint32_t)(bytes[8] & 0xFF) << 8)
            |    ((uint32_t)(bytes[9] & 0xFF));

    if (expon == 0 && hiMant == 0 && loMant == 0) {
        f = 0;
    }
    else {
        if (expon == 0x7FFF) {    /* Infinity or NaN */
            f = HUGE_VAL;
        }
        else {
            expon -= 16383;
            f  = ldexp(UnsignedToFloat(hiMant), expon-=31);
            f += ldexp(UnsignedToFloat(loMant), expon-=32);
        }
    }

    if (bytes[0] & 0x80)
        return -f;
    else
        return f;
}

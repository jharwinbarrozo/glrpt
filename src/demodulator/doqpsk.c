/*
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation; either version 3 of
 *  the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details:
 *
 *  http://www.gnu.org/copyleft/gpl.txt
 */

/*****************************************************************************/

#include "doqpsk.h"

#include "../glrpt/utils.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*****************************************************************************/

#define INTLV_BRANCHES      36      /* Interleaver number of branches */
#define INTLV_DELAY         2048
#define INTLV_BASE_LEN      73728   /* INTLV_BRANCHES * INTLV_DELAY */
#define INTLV_MESG_LEN      2654208 /* INTLV_BRANCHES * INTLV_BASE_LEN */
#define INTLV_DATA_LEN      72      /* Number of actual interleaved symbols */
#define INTLV_SYNCDATA      80      /* Number of interleaved symbols + sync */

#define SYNCD_DEPTH         4       /* How many consecutive sync words to search for */
#define SYNCD_BUF_MARGIN    320     /* SYNCD_DEPTH * INTLV_SYNCDATA */
#define SYNCD_BLOCK_SIZ     400     /* (SYNCD_DEPTH + 1) * INTLV_SYNCDATA */
#define SYNCD_BUF_STEP      240     /* (SYNCD_DEPTH - 1) * INTLV_SYNCDATA */

/*****************************************************************************/

static uint8_t Byte_at_Offset(uint8_t *data);
static bool Find_Sync(
        uint8_t *data,
        int block_siz,
        int step,
        int depth,
        int *offset,
        uint8_t *sync);
static void Resync_Stream(uint8_t *raw_buf, int raw_siz, int *resync_siz);
static inline int8_t Isqrt(int a);

/*****************************************************************************/

static uint8_t *isqrt_table = NULL;

/*****************************************************************************/

/* Uses hard decision (thresholding) to produce an 8-bit
 * byte at a given offset in the soft symbol stream, used
 * to find a sync word for the resynchronizing function */
static uint8_t Byte_at_Offset(uint8_t *data) {
    uint8_t result, test;

    result = 0;

    /* Do a thresholding of 8 consecutive symbols */
    for (uint8_t i = 0; i < 8; i++) {
        if (data[i] < 128)
            test = 1;
        else
            test = 0;

        /* Assemble a sync byte candidate */
        result |= test << i;
    }

    return result;
}

/*****************************************************************************/

/* The sync word could be in any of 8 different orientations, so we
 * will just look for a repeating bit pattern the right distance apart
 * to find the position of a sync word (8-bit byte, 00100111,
 * repeating every 80 symbols in stream) */
static bool Find_Sync(
        uint8_t *data,
        int block_siz,
        int step,
        int depth,
        int *offset,
        uint8_t *sync) {
  int limit;
  uint8_t test;
  bool result;

  *offset = 0;
  result  = false;

  /* Leave room in buffer for look-forward */
  limit = block_siz - step * depth;

  /* Search for a sync byte at the beginning of block */
  for (int i = 0; i < limit; i++) {
    result = true;

    /* Assemble a sync byte candidate */
    *sync = Byte_at_Offset(&data[i]);

    /* Search ahead depth times in buffer to see if
     * there are exactly equal sync byte candidates
     * at intervals of (sync + data = 80 syms) blocks */
    for (int j = 1; j <= depth; j++) {
      /* Break if there is a mismatch at any position */
      test = Byte_at_Offset(&data[i + j * step]);
      if (*sync != test) {
        result = false;
        break;
      }
    }

    /* If an unbroken series of matching sync
     * byte candidates located, record the buffer
     * offset and return success */
    if (result) {
      *offset = i;
      break;
    }
  }

  return result;
}

/*****************************************************************************/

/* 80k symbol rate stream: 00100111 36 bits 36 bits 00100111 36 bits 36 bits...
 * The sync words must be removed and the stream stitched back together */
static void Resync_Stream(uint8_t *raw_buf, int raw_siz, int *resync_siz) {
  uint8_t *src_buf = NULL;

  int tmp,
    posn   = 0,
    offset = 0,
    limit1 = raw_siz - SYNCD_BUF_MARGIN,
    limit2 = raw_siz - INTLV_SYNCDATA;

  uint8_t test = 0, sync = 0;
  bool ok;

  mem_alloc((void **)&src_buf, (size_t)raw_siz);
  memcpy(src_buf, raw_buf, (size_t)raw_siz);

  *resync_siz = 0;

  /* While there is room in the raw buffer for the
   * Find_Sync() function to search for sync candidates */
  while (posn < limit1) {
    /* Only search for sync if look-forward
     * below fails to find a sync train */
    if (!Find_Sync(&src_buf[posn], SYNCD_BLOCK_SIZ,
                INTLV_SYNCDATA, SYNCD_DEPTH, &offset, &sync)) {
      posn += SYNCD_BUF_STEP;
      continue;
    }
    posn += offset;

    /* While there is room in the raw buffer
     * to look forward for sync trains */
    while (posn < limit2) {
      /* Look ahead to prevent it losing sync on weak signal */
      ok = false;

      for (int i = 0; i < 128; i++) {
        tmp = posn + i * INTLV_SYNCDATA;

        if (tmp < limit2) {
          test = Byte_at_Offset(&src_buf[tmp]);

          if (sync == test) {
            ok = true;
            break;
          }
        }
      }

      if (!ok)
          break;

      /* Copy the actual data after the sync
       * train and update the total copied */
      memcpy( &raw_buf[*resync_siz], &src_buf[posn + 8], INTLV_DATA_LEN );
      *resync_siz += INTLV_DATA_LEN;

      /* Advance to next sync train position */
      posn += INTLV_SYNCDATA;
    }
  }

  free_ptr((void **)&src_buf);
}

/*****************************************************************************/

/* Re-synchronizes a stream of soft symbols and de-interleaves */
void De_Interleave(
        uint8_t *raw,
        int raw_siz,
        uint8_t **resync,
        int *resync_siz) {
  int resync_buf_idx, raw_buf_idx;


  /* Re-synchronize the new raw data at the bottom of the raw
   * buffer after the INTLV_MESG_LEN point and to the end */
  Resync_Stream( raw, raw_siz, resync_siz );

  /* Allocate the resynced and deinterleaved buffer */
  if( *resync_siz && (*resync_siz < raw_siz) )
    mem_alloc( (void **)resync, (size_t)*resync_siz );
  else
    Show_Message( "Resync_Stream() failed", "red" );

  /* We de-interleave INTLV_BASE_LEN number of symbols, so that
   * all symbols in raw buffer up to this length are used up. */
  for( resync_buf_idx = 0; resync_buf_idx < *resync_siz; resync_buf_idx++ )
  {
    /* This is the convolutional interleaving
     * algorithm, used in reverse to de-interleave */
    raw_buf_idx =
      resync_buf_idx + (resync_buf_idx % INTLV_BRANCHES) * INTLV_BASE_LEN;
    if( raw_buf_idx < *resync_siz )
      (*resync)[resync_buf_idx] = raw[raw_buf_idx];
  }
}

/*****************************************************************************/

/* Make_Isqrt_Table()
 *
 * Makes the Integer square root table
 */
void Make_Isqrt_Table(void) {
  uint16_t idx;

  mem_alloc( (void **)&isqrt_table, sizeof(uint8_t) * 16385 );
  for( idx = 0; idx < 16385; idx++ )
    isqrt_table[idx] = (uint8_t)( sqrt( (double)idx ) );
}

/*****************************************************************************/

/* Isqrt()
 *
 * Integer square root function
 */
static inline int8_t Isqrt(int a) {
  if( a >= 0 )
    return( (int8_t)isqrt_table[a] );
  else
    return( -((int8_t)isqrt_table[-a]) );
}

/*****************************************************************************/

/* De_Diffcode()
 *
 * "Fixes" a Differential Offset QPSK soft symbols
 * buffer so that it can be decoded by the LRPT decoder
 */
void De_Diffcode(int8_t *buff, uint32_t length) {
  uint32_t idx;
  int x, y;
  int tmp1, tmp2;
  static int prev_i = 0;
  static int prev_q = 0;

  tmp1 = buff[0];
  tmp2 = buff[1];

  buff[0] = Isqrt(  buff[0] * prev_i );
  buff[1] = Isqrt( -buff[1] * prev_q );

  length -= 2;
  for( idx = 2; idx <= length; idx += 2 )
  {
    x = buff[idx];
    y = buff[idx+1];

    buff[idx]   = Isqrt(  buff[idx]   * tmp1 );
    buff[idx+1] = Isqrt( -buff[idx+1] * tmp2 );

    tmp1 = x;
    tmp2 = y;
  }


  prev_i = tmp1;
  prev_q = tmp2;

  return;
}

/*****************************************************************************/

void Free_Isqrt_Table(void) {
  free_ptr( (void **)&isqrt_table );
}

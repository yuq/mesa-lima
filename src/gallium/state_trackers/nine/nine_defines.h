/*
 * Copyright 2011 Joakim Sindholt <opensource@zhasha.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#ifndef _NINE_DEFINES_H_
#define _NINE_DEFINES_H_

#include "pipe/p_defines.h"


#define NINE_RESOURCE_FLAG_LOCKABLE (PIPE_RESOURCE_FLAG_ST_PRIV << 1)
#define NINE_RESOURCE_FLAG_DUMMY    (PIPE_RESOURCE_FLAG_ST_PRIV << 2)

/* vertexdeclaration9.c */
unsigned nine_d3d9_to_nine_declusage(unsigned usage, unsigned index);

#define NINE_DECLUSAGE_POSITION(i)     ( 0 + (i))
#define NINE_DECLUSAGE_BLENDWEIGHT(i)  ( 5 + (i))
#define NINE_DECLUSAGE_BLENDINDICES(i) ( 9 + (i))
#define NINE_DECLUSAGE_NORMAL(i)       (13 + (i))
#define NINE_DECLUSAGE_PSIZE            15
#define NINE_DECLUSAGE_TEXCOORD(i)     (16 + (i))
#define NINE_DECLUSAGE_TANGENT(i)      (32 + (i))
#define NINE_DECLUSAGE_BINORMAL(i)     (34 + (i))
#define NINE_DECLUSAGE_TESSFACTOR       36
#define NINE_DECLUSAGE_POSITIONT        37
#define NINE_DECLUSAGE_COLOR(i)        (38 + (i))
#define NINE_DECLUSAGE_DEPTH            43
#define NINE_DECLUSAGE_FOG              44
#define NINE_DECLUSAGE_SAMPLE           45
#define NINE_DECLUSAGE_NONE             46
#define NINE_DECLUSAGE_LAST             NINE_DECLUSAGE_NONE
#define NINE_DECLUSAGE_COUNT           (NINE_DECLUSAGE_LAST + 1)

#define NINED3DCLEAR_DEPTHSTENCIL   (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)

#endif /* _NINE_DEFINES_H_ */

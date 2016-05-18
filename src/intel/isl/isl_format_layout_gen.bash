#!/usr/bin/env bash
#
# Copyright 2015 Intel Corporation
#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  the rights to use, copy, modify, merge, publish, distribute, sublicense,
#  and/or sell copies of the Software, and to permit persons to whom the
#  Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
#  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
#  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
#  IN THE SOFTWARE.

set -eu
set -o pipefail

cat <<'EOF'
/*
 * Copyright 2015 Intel Corporation
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice (including the next
 *  paragraph) shall be included in all copies or substantial portions of the
 *  Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 *  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

#include "isl.h"

const struct isl_format_layout
isl_format_layouts[] = {
EOF

sed -r '
# Delete comment lines and empty lines
/^[[:space:]]*#/d
/^[[:space:]]*$/d

# Delete spaces
s/[[:space:]]//g

# Translate formats
s/^([A-Za-z0-9_]+),*/ISL_FORMAT_\1,/

# Translate data type of channels
s/\<x([0-9]+),/ISL_VOID@\1,/g
s/\<r([0-9]+),/ISL_RAW@\1,/g
s/\<un([0-9]+),/ISL_UNORM@\1,/g
s/\<sn([0-9]+),/ISL_SNORM@\1,/g
s/\<uf([0-9]+),/ISL_UFLOAT@\1,/g
s/\<sf([0-9]+),/ISL_SFLOAT@\1,/g
s/\<ux([0-9]+),/ISL_UFIXED@\1,/g
s/\<sx([0-9]+),/ISL_SFIXED@\1,/g
s/\<ui([0-9]+),/ISL_UINT@\1,/g
s/\<si([0-9]+),/ISL_SINT@\1,/g
s/\<us([0-9]+),/ISL_USCALED@\1,/g
s/\<ss([0-9]+),/ISL_SSCALED@\1,/g

# Translate colorspaces
# Interpret alpha-only formats as having no colorspace.
s/\<(linear|srgb|yuv)\>/ISL_COLORSPACE_\1/
s/\<alpha\>//

# Translate texture compression
s/\<(dxt|fxt|rgtc|bptc|etc|astc)([0-9]*)\>/ISL_TXC_\1\2/
' |
tr 'a-z' 'A-Z' | # Convert to uppersace
while IFS=, read -r format bpb bw bh bd \
                    red green blue alpha \
                    luminance intensity palette \
                    colorspace txc
do
    : ${colorspace:=ISL_COLORSPACE_NONE}
    : ${txc:=ISL_TXC_NONE}

    cat <<EOF
   [$format] = {
      $format,
      .bs = $((bpb/8)),
      .bw = $bw, .bh = $bh, .bd = $bd,
      .channels = {
          .r = { $red },
          .g = { $green },
          .b = { $blue },
          .a = { $alpha },
          .l = { $luminance },
          .i = { $intensity },
          .p = { $palette },
      },
      .colorspace = $colorspace,
      .txc = $txc,
   },

EOF
done |
sed -r '
# Collapse empty channels
s/\{  \}/{}/

# Split non-empty channels into two members: base type and bit size
s/@/, /
'

# Terminate the table
printf '};\n'

#encoding=utf-8
# Copyright © 2017 Intel Corporation

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

from __future__ import (
    absolute_import, division, print_function, unicode_literals
)

import argparse
import os
import sys
import xml.parsers.expat

from mako.template import Template

TEMPLATE = Template("""\
<%!
from operator import itemgetter
%>\
/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* THIS FILE HAS BEEN GENERATED, DO NOT HAND EDIT.
 *
 * Sizes of bitfields in genxml instructions, structures, and registers.
 */

#ifndef ${guard}
#define ${guard}

#include <stdint.h>

#include "common/gen_device_info.h"
#include "util/macros.h"

#ifdef __cplusplus
extern "C" {
#endif
% for _, field in sorted(fields.iteritems(), key=itemgetter(0)):

/* ${field.container_name}::${field.name} */
% for gen, bits in sorted(field.bits_by_gen.iteritems(), reverse=True):
#define ${gen.prefix(field.token_name, padded=True)}    ${bits}
% endfor

static inline uint32_t ATTRIBUTE_PURE
${field.token_name}(const struct gen_device_info *devinfo)
{
   switch (devinfo->gen) {
   case 9: return ${field.bits(9)};
   case 8: return ${field.bits(8)};
   case 7:
      if (devinfo->is_haswell) {
         return ${field.bits(7.5)};
      } else {
         return ${field.bits(7)};
      }
   case 6: return ${field.bits(6)};
   case 5: return ${field.bits(5)};
   case 4:
      if (devinfo->is_g4x) {
         return ${field.bits(4.5)};
      } else {
         return ${field.bits(4)};
      }
   default:
      unreachable("Invalid hardware generation");
   }
}
% endfor

#ifdef __cplusplus
}
#endif

#endif /* ${guard} */""", output_encoding='utf-8')

def to_alphanum(name):
    substitutions = {
        ' ': '',
        '/': '',
        '[': '',
        ']': '',
        '(': '',
        ')': '',
        '-': '',
        ':': '',
        '.': '',
        ',': '',
        '=': '',
        '>': '',
        '#': '',
        'α': 'alpha',
        '&': '',
        '*': '',
        '"': '',
        '+': '',
        '\'': '',
    }

    for i, j in substitutions.items():
        name = name.replace(i, j)

    return name

def safe_name(name):
    name = to_alphanum(name)
    if not name[0].isalpha():
        name = '_' + name
    return name

class Gen(object):

    def __init__(self, z):
        # Convert potential "major.minor" string
        z = float(z)
        if z < 10:
            z *= 10
        self.tenx = int(z)

    def __lt__(self, other):
        return self.tenx < other.tenx

    def __hash__(self):
        return hash(self.tenx)

    def __eq__(self, other):
        return self.tenx == other.tenx

    def prefix(self, token, padded=False):
        gen = self.tenx
        pad = ''

        if gen % 10 == 0:
            gen //= 10
            if padded:
                pad = ' '

        if token[0] == '_':
            token = token[1:]

        return 'GEN{}_{}{}'.format(gen, token, pad)

class Field(object):

    def __init__(self, container_name, name):
        self.container_name = container_name
        self.name = name
        self.token_name = safe_name('_'.join([self.container_name, self.name, 'bits']))
        self.bits_by_gen = {}

    def add_gen(self, gen, xml_attrs):
        assert isinstance(gen, Gen)
        start = int(xml_attrs['start'])
        end = int(xml_attrs['end'])
        self.bits_by_gen[gen] = 1 + end - start

    def bits(self, gen):
        if not isinstance(gen, Gen):
            gen = Gen(gen)
        return self.bits_by_gen.get(gen, 0)

class XmlParser(object):

    def __init__(self, fields):
        self.parser = xml.parsers.expat.ParserCreate()
        self.parser.StartElementHandler = self.start_element
        self.parser.EndElementHandler = self.end_element

        self.gen = None
        self.container_name = None
        self.fields = fields

    def parse(self, filename):
        with open(filename) as f:
            self.parser.ParseFile(f)

    def start_element(self, name, attrs):
        if name == 'genxml':
            self.gen = Gen(attrs['gen'])
        elif name in ('instruction', 'struct', 'register'):
            self.start_container(attrs)
        elif name == 'field':
            self.start_field(attrs)
        else:
            pass

    def end_element(self, name):
        if name == 'genxml':
            self.gen = None
        elif name in ('instruction', 'struct', 'register'):
            self.container_name = None
        else:
            pass

    def start_container(self, attrs):
        assert self.container_name is None
        self.container_name = attrs['name']

    def start_field(self, attrs):
        if self.container_name is None:
            return

        field_name = attrs.get('name', None)
        if not field_name:
            return

        key = (self.container_name, field_name)
        if key not in self.fields:
            self.fields[key] = Field(self.container_name, field_name)
        self.fields[key].add_gen(self.gen, attrs)

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument('-o', '--output', type=str,
                   help="If OUTPUT is unset or '-', then it defaults to '/dev/stdout'")
    p.add_argument('--cpp-guard', type=str,
                   help='If unset, then CPP_GUARD is derived from OUTPUT.')
    p.add_argument('xml_sources', metavar='XML_SOURCE', nargs='+')

    pargs = p.parse_args()

    if pargs.output in (None, '-'):
        pargs.output = '/dev/stdout'

    if pargs.cpp_guard is None:
        pargs.cpp_guard = os.path.basename(pargs.output).upper().replace('.', '_')

    return pargs

def main():
    pargs = parse_args()

    # Maps (container_name, field_name) => Field
    fields = {}

    for source in pargs.xml_sources:
        XmlParser(fields).parse(source)

    with open(pargs.output, 'wb') as f:
        f.write(TEMPLATE.render(fields=fields, guard=pargs.cpp_guard))

if __name__ == '__main__':
    main()

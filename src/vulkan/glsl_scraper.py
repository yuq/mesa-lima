#! /usr/bin/env python

import cStringIO
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

def print_usage(err):
   print """\
glsl_scraper.py [options] file

This program scrapes a C file for any instance of the GLSL_VK_SHADER macro,
grabs the GLSL source code, compiles it to SPIR-V.  The resulting SPIR-V
code is written to another C file as an array of 32-bit words.

If '-' is passed as the input file or output file, stdin or stdout will be
used instead of a file on disc.

Options:
   -o outfile           Output to the given file (default: stdout)
   --with-glslang=PATH  Full path to the glslangValidator program"""
   exit(err)

class Shader:
   def __init__(self, stage):
      self.stream = cStringIO.StringIO()
      self.stage = stage

      if self.stage == 'VERTEX':
         self.ext = 'vert'
      elif self.stage == 'TESS_CONTROL':
         self.ext = 'tesc'
      elif self.stage == 'TESS_EVALUATION':
         self.ext = 'tese'
      elif self.stage == 'GEOMETRY':
         self.ext = 'geom'
      elif self.stage == 'FRAGMENT':
         self.ext = 'frag'
      elif self.stage == 'COMPUTE':
         self.ext = 'comp'
      else:
         assert False

   def add_text(self, s):
      self.stream.write(s)

   def finish_text(self, line):
      self.line = line

   def glsl_source(self):
      return self.stream.getvalue()

   def compile(self):
      # We can assume if we got here that we have a temp directory and that
      # we're currently living in it.
      glsl_fname = 'shader{0}.{1}'.format(self.line, self.ext)
      spirv_fname = self.ext + '.spv'

      glsl_file = open(glsl_fname, 'w')
      glsl_file.write('#version 420 core\n')
      glsl_file.write(self.glsl_source())
      glsl_file.close()

      out = open('glslang.out', 'wb')
      err = subprocess.call([glslang, '-V', glsl_fname], stdout=out)
      if err != 0:
         out = open('glslang.out', 'r')
         sys.stderr.write(out.read())
         out.close()
         exit(1)

      def dwords(f):
         while True:
            dword_str = f.read(4)
            if not dword_str:
               return
            assert len(dword_str) == 4
            yield struct.unpack('I', dword_str)[0]

      spirv_file = open(spirv_fname, 'rb')
      self.dwords = list(dwords(spirv_file))
      spirv_file.close()

      os.remove(glsl_fname)
      os.remove(spirv_fname)

   def dump_c_code(self, f, glsl_only = False):
      f.write('\n\n')
      var_prefix = '_glsl_helpers_shader{0}'.format(self.line)

      # First dump the GLSL source as strings
      f.write('static const char {0}_glsl_src[] ='.format(var_prefix))
      f.write('\n_ANV_SPIRV_' + self.stage)
      f.write('\n"#version 330\\n"')
      for line in self.glsl_source().splitlines():
         if not line.strip():
            continue
         f.write('\n"{0}\\n"'.format(line))
      f.write(';\n\n')

      if glsl_only:
         return

      # Now dump the SPIR-V source
      f.write('static const uint32_t {0}_spir_v_src[] = {{'.format(var_prefix))
      line_start = 0
      while line_start < len(self.dwords):
         f.write('\n   ')
         for i in range(line_start, min(line_start + 6, len(self.dwords))):
            f.write(' 0x{:08x},'.format(self.dwords[i]))
         line_start += 6
      f.write('\n};\n')

token_exp = re.compile(r'(GLSL_VK_SHADER|\(|\)|,)')

class Parser:
   def __init__(self, f):
      self.infile = f
      self.paren_depth = 0
      self.shader = None
      self.line_number = 1
      self.shaders = []

      def tokenize(f):
         leftover = ''
         for line in f:
            pos = 0
            while True:
               m = token_exp.search(line, pos)
               if m:
                  if m.start() > pos:
                     leftover += line[pos:m.start()]
                  pos = m.end()

                  if leftover:
                     yield leftover
                     leftover = ''

                  yield m.group(0)

               else:
                  leftover += line[pos:]
                  break

            self.line_number += 1

         if leftover:
            yield leftover

      self.token_iter = tokenize(self.infile)

   def handle_shader_src(self):
      paren_depth = 1
      for t in self.token_iter:
         if t == '(':
            paren_depth += 1
         elif t == ')':
            paren_depth -= 1
            if paren_depth == 0:
               return

         self.current_shader.add_text(t)

   def handle_macro(self):
      t = self.token_iter.next()
      assert t == '('
      t = self.token_iter.next()
      t = self.token_iter.next()
      assert t == ','

      stage = self.token_iter.next().strip()

      t = self.token_iter.next()
      assert t == ','

      self.current_shader = Shader(stage)
      self.handle_shader_src()
      self.current_shader.finish_text(self.line_number)

      self.shaders.append(self.current_shader)
      self.current_shader = None

   def run(self):
      for t in self.token_iter:
         if t == 'GLSL_VK_SHADER':
            self.handle_macro()

def open_file(name, mode):
   if name == '-':
      if mode == 'w':
         return sys.stdout
      elif mode == 'r':
         return sys.stdin
      else:
         assert False
   else:
      return open(name, mode)

infname = None
outfname = '-'
glslang = 'glslangValidator'
glsl_only = False

arg_idx = 1
while arg_idx < len(sys.argv):
   if sys.argv[arg_idx] == '-h':
      print_usage(0)
   elif sys.argv[arg_idx] == '-o':
      arg_idx += 1
      outfname = sys.argv[arg_idx]
   elif sys.argv[arg_idx].startswith('--with-glslang='):
      glslang = sys.argv[arg_idx][len('--with-glslang='):]
   elif sys.argv[arg_idx] == '--glsl-only':
      glsl_only = True;
   else:
      infname = sys.argv[arg_idx]
      break
   arg_idx += 1

if arg_idx < len(sys.argv) - 1 or not infname or not outfname:
   print_usage(1)

with open_file(infname, 'r') as infile:
   parser = Parser(infile)
   parser.run()

if not glsl_only:
   # glslangValidator has an absolutely *insane* interface.  We pretty much
   # have to run in a temporary directory.  Sad day.
   current_dir = os.getcwd()
   tmpdir = tempfile.mkdtemp('glsl_scraper')

   try:
      os.chdir(tmpdir)

      for shader in parser.shaders:
         shader.compile()

      os.chdir(current_dir)
   finally:
      shutil.rmtree(tmpdir)

with open_file(outfname, 'w') as outfile:
   outfile.write("""\
/* ===========================  DO NOT EDIT!  ===========================
 *            This file is autogenerated by glsl_scraper.py.
 */

#include <stdint.h>

#define _ANV_SPIRV_MAGIC "\\x03\\x02\\x23\\x07\\0\\0\\0\\0"

#define _ANV_SPIRV_VERTEX           _ANV_SPIRV_MAGIC "\\0\\0\\0\\0"
#define _ANV_SPIRV_TESS_CONTROL     _ANV_SPIRV_MAGIC "\\1\\0\\0\\0"
#define _ANV_SPIRV_TESS_EVALUATION  _ANV_SPIRV_MAGIC "\\2\\0\\0\\0"
#define _ANV_SPIRV_GEOMETRY         _ANV_SPIRV_MAGIC "\\3\\0\\0\\0"
#define _ANV_SPIRV_FRAGMENT         _ANV_SPIRV_MAGIC "\\4\\0\\0\\0"
#define _ANV_SPIRV_COMPUTE          _ANV_SPIRV_MAGIC "\\5\\0\\0\\0"

#define _ANV_GLSL_SRC_VAR2(_line) _glsl_helpers_shader ## _line ## _glsl_src
#define _ANV_GLSL_SRC_VAR(_line) _ANV_GLSL_SRC_VAR2(_line)

#define GLSL_VK_SHADER(device, stage, ...) ({                           \\
   VkShader __shader;                                                   \\
   VkShaderCreateInfo __shader_create_info = {                          \\
      .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,                    \\
      .codeSize = sizeof(_ANV_GLSL_SRC_VAR(__LINE__)),                  \\
      .pCode = _ANV_GLSL_SRC_VAR(__LINE__),                             \\
   };                                                                   \\
   vkCreateShader((VkDevice) device, &__shader_create_info, &__shader); \\
   __shader;                                                            \\
})
""")

   for shader in parser.shaders:
      shader.dump_c_code(outfile, glsl_only)

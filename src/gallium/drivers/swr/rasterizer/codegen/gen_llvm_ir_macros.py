# Copyright (C) 2014-2017 Intel Corporation.   All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

from __future__ import print_function
import os, sys, re
from gen_common import MakoTemplateWriter, ArgumentParser
from argparse import FileType

inst_aliases = {
    'SHUFFLE_VECTOR': 'VSHUFFLE',
    'INSERT_ELEMENT': 'VINSERT',
    'EXTRACT_ELEMENT': 'VEXTRACT',
    'MEM_SET': 'MEMSET',
    'MEM_CPY': 'MEMCOPY',
    'MEM_MOVE': 'MEMMOVE',
    'L_SHR': 'LSHR',
    'A_SHR': 'ASHR',
    'BIT_CAST': 'BITCAST',
    'U_DIV': 'UDIV',
    'S_DIV': 'SDIV',
    'U_REM': 'UREM',
    'S_REM': 'SREM',
    'BIN_OP': 'BINOP',
}

intrinsics = [
    ['VGATHERPD', 'x86_avx2_gather_d_pd_256', ['src', 'pBase', 'indices', 'mask', 'scale'], 'mSimd4FP64Ty'],
    ['VGATHERPS', 'x86_avx2_gather_d_ps_256', ['src', 'pBase', 'indices', 'mask', 'scale'], 'mSimdFP32Ty'],
    ['VGATHERPS_16', 'x86_avx512_gather_dps_512', ['src', 'pBase', 'indices', 'mask', 'scale'], 'mSimd16FP32Ty'],
    ['VGATHERDD', 'x86_avx2_gather_d_d_256', ['src', 'pBase', 'indices', 'mask', 'scale'], 'mSimdInt32Ty'],
    ['VGATHERDD_16', 'x86_avx512_gather_dpi_512', ['src', 'pBase', 'indices', 'mask', 'scale'], 'mSimd16Int32Ty'],
    ['VRCPPS', 'x86_avx_rcp_ps_256', ['a'], 'mSimdFP32Ty'],
    ['VROUND', 'x86_avx_round_ps_256', ['a', 'rounding'], 'mSimdFP32Ty'],
    ['BEXTR_32', 'x86_bmi_bextr_32', ['src', 'control'], 'mInt32Ty'],
    ['VPSHUFB', 'x86_avx2_pshuf_b', ['a', 'b'], 'mSimd32Int8Ty'],
    ['VPERMD', 'x86_avx2_permd', ['a', 'idx'], 'mSimdInt32Ty'],
    ['VPERMPS', 'x86_avx2_permps', ['idx', 'a'], 'mSimdFP32Ty'],
    ['VCVTPD2PS', 'x86_avx_cvt_pd2_ps_256', ['a'], 'mSimdFP32Ty'],
    ['VCVTPH2PS', 'x86_vcvtph2ps_256', ['a'], 'mSimdFP32Ty'],
    ['VCVTPS2PH', 'x86_vcvtps2ph_256', ['a', 'round'], 'mSimdFP16Ty'],
    ['VHSUBPS', 'x86_avx_hsub_ps_256', ['a', 'b'], 'mSimdFP32Ty'],
    ['VPTESTC', 'x86_avx_ptestc_256', ['a', 'b'], 'mInt32Ty'],
    ['VPTESTZ', 'x86_avx_ptestz_256', ['a', 'b'], 'mInt32Ty'],
    ['VFMADDPS', 'x86_fma_vfmadd_ps_256', ['a', 'b', 'c'], 'mSimdFP32Ty'],
    ['VMOVMSKPS', 'x86_avx_movmsk_ps_256', ['a'], 'mInt32Ty'],
    ['VPHADDD', 'x86_avx2_phadd_d', ['a', 'b'], 'mSimdInt32Ty'],
    ['PDEP32', 'x86_bmi_pdep_32', ['a', 'b'], 'mInt32Ty'],
    ['RDTSC', 'x86_rdtsc', [], 'mInt64Ty'],
]

llvm_intrinsics = [
    ['CTTZ', 'cttz', ['a', 'flag'], ['a']],
    ['CTLZ', 'ctlz', ['a', 'flag'], ['a']],
    ['VSQRTPS', 'sqrt', ['a'], ['a']],
    ['STACKSAVE', 'stacksave', [], []],
    ['STACKRESTORE', 'stackrestore', ['a'], []],
    ['VMINPS', 'minnum', ['a', 'b'], ['a']],
    ['VMAXPS', 'maxnum', ['a', 'b'], ['a']],
    ['DEBUGTRAP', 'debugtrap', [], []],
    ['POPCNT', 'ctpop', ['a'], ['a']],
    ['LOG2', 'log2', ['a'], ['a']],
    ['FABS', 'fabs', ['a'], ['a']],
    ['EXP2', 'exp2', ['a'], ['a']],
    ['POW', 'pow', ['a', 'b'], ['a']]
]

this_dir = os.path.dirname(os.path.abspath(__file__))
template = os.path.join(this_dir, 'templates', 'gen_builder.hpp')

def convert_uppercamel(name):
    s1 = re.sub('(.)([A-Z][a-z]+)', r'\1_\2', name)
    return re.sub('([a-z0-9])([A-Z])', r'\1_\2', s1).upper()

'''
    Given an input file (e.g. IRBuilder.h) generates function dictionary.
'''
def parse_ir_builder(input_file):

    functions = []

    lines = input_file.readlines()

    idx = 0
    while idx < len(lines) - 1:
        line = lines[idx].rstrip()
        idx += 1

        #match = re.search(r'\*Create', line)
        match = re.search(r'[\*\s]Create(\w*)\(', line)
        if match is not None:
            #print('Line: %s' % match.group(1))

            if re.search(r'^\s*Create', line) is not None:
                func_sig = lines[idx-2].rstrip() + line
            else:
                func_sig = line

            end_of_args = False
            while not end_of_args:
                end_paren = re.search(r'\)', line)
                if end_paren is not None:
                    end_of_args = True
                else:
                    line = lines[idx].rstrip()
                    func_sig += line
                    idx += 1

            delfunc = re.search(r'LLVM_DELETED_FUNCTION|= delete;', func_sig)

            if not delfunc:
                func = re.search(r'(.*?)\*[\n\s]*(Create\w*)\((.*?)\)', func_sig)
                if func is not None:

                    return_type = func.group(1).strip() + '*'
                    func_name = func.group(2)
                    arguments = func.group(3)

                    func_args = []
                    arg_names = []
                    args = arguments.split(',')
                    for arg in args:
                        arg = arg.strip()
                        if arg:
                            func_args.append(arg)

                            split_args = arg.split('=')
                            arg_name = split_args[0].rsplit(None, 1)[-1]

                            reg_arg = re.search(r'[\&\*]*(\w*)', arg_name)
                            if reg_arg:
                                arg_names += [reg_arg.group(1)]

                    ignore = False

                    # The following functions need to be ignored in openswr.
                    # API change in llvm-5.0 breaks baked autogen files
                    if (
                        (func_name == 'CreateFence' or
                         func_name == 'CreateAtomicCmpXchg' or
                         func_name == 'CreateAtomicRMW')):
                        ignore = True

                    # The following functions need to be ignored.
                    if (func_name == 'CreateInsertNUWNSWBinOp' or
                        func_name == 'CreateMaskedIntrinsic' or
                        func_name == 'CreateAlignmentAssumptionHelper' or
                        func_name == 'CreateLoad'):
                        ignore = True

                    # Convert CamelCase to CAMEL_CASE
                    func_mod = re.search(r'Create(\w*)', func_name)
                    if func_mod:
                        func_mod = func_mod.group(1)
                        func_mod = convert_uppercamel(func_mod)
                        if func_mod[0:2] == 'F_' or func_mod[0:2] == 'I_':
                            func_mod = func_mod[0] + func_mod[2:]

                    # Substitute alias based on CAMEL_CASE name.
                    func_alias = inst_aliases.get(func_mod)
                    if not func_alias:
                        func_alias = func_mod

                        if func_name == 'CreateCall' or func_name == 'CreateGEP':
                            arglist = re.search(r'ArrayRef', ', '.join(func_args))
                            if arglist:
                                func_alias = func_alias + 'A'

                    if not ignore:
                        functions.append({
                                'name'      : func_name,
                                'alias'     : func_alias,
                                'return'    : return_type,
                                'args'      : ', '.join(func_args),
                                'arg_names' : arg_names,
                            })

    return functions

'''
    Auto-generates macros for LLVM IR
'''
def generate_gen_h(functions, output_dir):
    filename = 'gen_builder.hpp'
    output_filename = os.path.join(output_dir, filename)

    templfuncs = []
    for func in functions:
        decl = '%s %s(%s)' % (func['return'], func['alias'], func['args'])

        templfuncs.append({
            'decl'      : decl,
            'intrin'    : func['name'],
            'args'      : func['arg_names'],
        })

    MakoTemplateWriter.to_file(
        template,
        output_filename,
        cmdline=sys.argv,
        comment='Builder IR Wrappers',
        filename=filename,
        functions=templfuncs,
        isX86=False, isIntrin=False)

'''
    Auto-generates macros for LLVM IR
'''
def generate_meta_h(output_dir):
    filename = 'gen_builder_meta.hpp'
    output_filename = os.path.join(output_dir, filename)

    functions = []
    for inst in intrinsics:
        #print('Inst: %s, x86: %s numArgs: %d' % (inst[0], inst[1], len(inst[2])))
        if len(inst[2]) != 0:
            declargs = 'Value* ' + ', Value* '.join(inst[2])
            decl = 'Value* %s(%s, const llvm::Twine& name = "")' % (inst[0], declargs)
        else:
            decl = 'Value* %s(const llvm::Twine& name = "")' % (inst[0])

        functions.append({
            'decl'      : decl,
            'name'      : inst[0],
            'intrin'    : inst[1],
            'args'      : inst[2],
            'returnType': inst[3]
        })

    MakoTemplateWriter.to_file(
        template,
        output_filename,
        cmdline=sys.argv,
        comment='meta intrinsics',
        filename=filename,
        functions=functions,
        isX86=True, isIntrin=False)

def generate_intrin_h(output_dir):
    filename = 'gen_builder_intrin.hpp'
    output_filename = os.path.join(output_dir, filename)

    functions = []
    for inst in llvm_intrinsics:
        #print('Inst: %s, x86: %s numArgs: %d' % (inst[0], inst[1], len(inst[2])))
        if len(inst[2]) != 0:
            declargs = 'Value* ' + ', Value* '.join(inst[2])
            decl = 'Value* %s(%s, const llvm::Twine& name = "")' % (inst[0], declargs)
        else:
            decl = 'Value* %s(const llvm::Twine& name = "")' % (inst[0])

        functions.append({
            'decl'      : decl,
            'intrin'    : inst[1],
            'args'      : inst[2],
            'types'     : inst[3],
        })

    MakoTemplateWriter.to_file(
        template,
        output_filename,
        cmdline=sys.argv,
        comment='llvm intrinsics',
        filename=filename,
        functions=functions,
        isX86=False, isIntrin=True)
'''
    Function which is invoked when this script is started from a command line.
    Will present and consume a set of arguments which will tell this script how
    to behave
'''
def main():

    # Parse args...
    parser = ArgumentParser()
    parser.add_argument('--input', '-i', type=FileType('r'), help='Path to IRBuilder.h', required=False)
    parser.add_argument('--output-dir', '-o', action='store', dest='output', help='Path to output directory', required=True)
    parser.add_argument('--gen_h', help='Generate builder_gen.h', action='store_true', default=False)
    parser.add_argument('--gen_meta_h', help='Generate meta intrinsics. No input is needed.', action='store_true', default=False)
    parser.add_argument('--gen_intrin_h', help='Generate llvm intrinsics. No input is needed.', action='store_true', default=False)
    args = parser.parse_args()

    if not os.path.exists(args.output):
        os.makedirs(args.output)

    if args.input:
        functions = parse_ir_builder(args.input)

        if args.gen_h:
            generate_gen_h(functions, args.output)

    elif args.gen_h:
        print('Need to specify --input for --gen_h!')

    if args.gen_meta_h:
        generate_meta_h(args.output)

    if args.gen_intrin_h:
        generate_intrin_h(args.output)

if __name__ == '__main__':
    main()
# END OF FILE

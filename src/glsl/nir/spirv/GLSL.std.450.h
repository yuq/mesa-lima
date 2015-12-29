/*
** Copyright (c) 2014-2015 The Khronos Group Inc.
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and/or associated documentation files (the "Materials"),
** to deal in the Materials without restriction, including without limitation
** the rights to use, copy, modify, merge, publish, distribute, sublicense,
** and/or sell copies of the Materials, and to permit persons to whom the
** Materials are furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Materials.
**
** MODIFICATIONS TO THIS FILE MAY MEAN IT NO LONGER ACCURATELY REFLECTS KHRONOS
** STANDARDS. THE UNMODIFIED, NORMATIVE VERSIONS OF KHRONOS SPECIFICATIONS AND
** HEADER INFORMATION ARE LOCATED AT https://www.khronos.org/registry/ 
**
** THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
** OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
** THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
** FROM,OUT OF OR IN CONNECTION WITH THE MATERIALS OR THE USE OR OTHER DEALINGS
** IN THE MATERIALS.
*/

#ifndef GLSLstd450_H
#define GLSLstd450_H

const int GLSLstd450Version = 100;
const int GLSLstd450Revision = 1;

enum GLSLstd450 {
    GLSLstd450Bad = 0,              // Don't use

    GLSLstd450Round = 1,
    GLSLstd450RoundEven = 2,
    GLSLstd450Trunc = 3,
    GLSLstd450FAbs = 4,
    GLSLstd450SAbs = 5,
    GLSLstd450FSign = 6,
    GLSLstd450SSign = 7,
    GLSLstd450Floor = 8,
    GLSLstd450Ceil = 9,
    GLSLstd450Fract = 10,

    GLSLstd450Radians = 11,
    GLSLstd450Degrees = 12,
    GLSLstd450Sin = 13,
    GLSLstd450Cos = 14,
    GLSLstd450Tan = 15,
    GLSLstd450Asin = 16,
    GLSLstd450Acos = 17,
    GLSLstd450Atan = 18,
    GLSLstd450Sinh = 19,
    GLSLstd450Cosh = 20,
    GLSLstd450Tanh = 21,
    GLSLstd450Asinh = 22,
    GLSLstd450Acosh = 23,
    GLSLstd450Atanh = 24,
    GLSLstd450Atan2 = 25,

    GLSLstd450Pow = 26,
    GLSLstd450Exp = 27,
    GLSLstd450Log = 28,
    GLSLstd450Exp2 = 29,
    GLSLstd450Log2 = 30,
    GLSLstd450Sqrt = 31,
    GLSLstd450InverseSqrt = 32,

    GLSLstd450Determinant = 33,
    GLSLstd450MatrixInverse = 34,

    GLSLstd450Modf = 35,            // second operand needs an OpVariable to write to
    GLSLstd450ModfStruct = 36,      // no OpVariable operand
    GLSLstd450FMin = 37,
    GLSLstd450NMin = 38,
    GLSLstd450UMin = 39,
    GLSLstd450SMin = 40,
    GLSLstd450FMax = 41,
    GLSLstd450NMax = 42,
    GLSLstd450UMax = 43,
    GLSLstd450SMax = 44,
    GLSLstd450FClamp = 45,
    GLSLstd450NClamp = 46,
    GLSLstd450UClamp = 47,
    GLSLstd450SClamp = 48,
    GLSLstd450FMix = 49,
    GLSLstd450IMix = 50,
    GLSLstd450Step = 51,
    GLSLstd450SmoothStep = 52,

    GLSLstd450Fma = 53,
    GLSLstd450Frexp = 54,            // second operand needs an OpVariable to write to
    GLSLstd450FrexpStruct = 55,      // no OpVariable operand
    GLSLstd450Ldexp = 56,

    GLSLstd450PackSnorm4x8 = 57,
    GLSLstd450PackUnorm4x8 = 58,
    GLSLstd450PackSnorm2x16 = 59,
    GLSLstd450PackUnorm2x16 = 60,
    GLSLstd450PackHalf2x16 = 61,
    GLSLstd450PackDouble2x32 = 62,
    GLSLstd450UnpackSnorm2x16 = 63,
    GLSLstd450UnpackUnorm2x16 = 64,
    GLSLstd450UnpackHalf2x16 = 65,
    GLSLstd450UnpackSnorm4x8 = 66,
    GLSLstd450UnpackUnorm4x8 = 67,
    GLSLstd450UnpackDouble2x32 = 68,

    GLSLstd450Length = 69,
    GLSLstd450Distance = 70,
    GLSLstd450Cross = 71,
    GLSLstd450Normalize = 72,
    GLSLstd450FaceForward = 73,
    GLSLstd450Reflect = 74,
    GLSLstd450Refract = 75,

    GLSLstd450FindILsb = 76,
    GLSLstd450FindSMsb = 77,
    GLSLstd450FindUMsb = 78,

    GLSLstd450InterpolateAtCentroid = 79,
    GLSLstd450InterpolateAtSample = 80,
    GLSLstd450InterpolateAtOffset = 81,

    GLSLstd450Count
};

#endif  // #ifndef GLSLstd450_H

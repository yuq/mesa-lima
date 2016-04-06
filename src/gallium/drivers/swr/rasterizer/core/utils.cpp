/****************************************************************************
* Copyright (C) 2014-2015 Intel Corporation.   All Rights Reserved.
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
*
* @file utils.cpp
*
* @brief Utilities used by SWR core.
*
******************************************************************************/
#if defined(_WIN32)

#if defined(NOMINMAX)
// GDI Plus requires non-std min / max macros be defined :(
#undef NOMINMAX
#endif

#include<Windows.h>
#include <Gdiplus.h>
#include <Gdiplusheaders.h>
#include <cstdint>

using namespace Gdiplus;

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
   uint32_t  num = 0;          // number of image encoders
   uint32_t  size = 0;         // size of the image encoder array in bytes

   ImageCodecInfo* pImageCodecInfo = nullptr;

   GetImageEncodersSize(&num, &size);
   if(size == 0)
      return -1;  // Failure

   pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
   if(pImageCodecInfo == nullptr)
      return -1;  // Failure

   GetImageEncoders(num, size, pImageCodecInfo);

   for(uint32_t j = 0; j < num; ++j)
   {
      if( wcscmp(pImageCodecInfo[j].MimeType, format) == 0 )
      {
         *pClsid = pImageCodecInfo[j].Clsid;
         free(pImageCodecInfo);
         return j;  // Success
      }    
   }

   free(pImageCodecInfo);
   return -1;  // Failure
}

void SaveImageToPNGFile(
    const WCHAR *pFilename,
    void *pBuffer,
    uint32_t width,
    uint32_t height,
    bool broadcastRed = false)
{
    // dump pixels to a png
    // Initialize GDI+.
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    Bitmap *bitmap = new Bitmap(width, height);
    BYTE *pBytes = (BYTE*)pBuffer;
    const uint32_t bytesPerPixel = (broadcastRed ? 1 : 4);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            uint32_t pixel = 0;
            if (broadcastRed)
            {
                pixel = *(uint8_t*)pBytes;
                pixel = pixel | (pixel << 8) | (pixel << 16) | 0xFF000000;
            }
            else
            {
                pixel = *(uint32_t*)pBytes;
                if (pixel == 0xcdcdcdcd)
                {
                    pixel = 0xFFFF00FF;
                }
                else if (pixel == 0xdddddddd)
                {
                    pixel = 0x80FF0000;
                }
                else
                {
                    pixel |= 0xFF000000;
                }
            }

            Color color(pixel);
            bitmap->SetPixel(x, y, color);
            pBytes += bytesPerPixel;
        }

    // Save image.
    CLSID pngClsid;
    GetEncoderClsid(L"image/png", &pngClsid);
    bitmap->Save(pFilename, &pngClsid, nullptr);

    delete bitmap;

    GdiplusShutdown(gdiplusToken);
}

void OpenBitmapFromFile(
    const WCHAR *pFilename,
    void **pBuffer,
    uint32_t *width,
    uint32_t *height)
{
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    Bitmap *bitmap  = new Bitmap(pFilename);

    *width          = bitmap->GetWidth();
    *height         = bitmap->GetHeight();
    *pBuffer        = new BYTE[*width * *height * 4]; // width * height * |RGBA|

    // The folder 'stb_image' contains a PNG open/close module which
    // is far less painful than this is, yo.
    Gdiplus::Color clr;
    for (uint32_t y = 0, idx = 0; y < *height; ++y)
    {
        for (uint32_t x = 0; x < *width; ++x, idx += 4)
        {
            bitmap->GetPixel(x, *height - y - 1, &clr);
            ((BYTE*)*pBuffer)[idx + 0] = clr.GetBlue();
            ((BYTE*)*pBuffer)[idx + 1] = clr.GetGreen();
            ((BYTE*)*pBuffer)[idx + 2] = clr.GetRed();
            ((BYTE*)*pBuffer)[idx + 3] = clr.GetAlpha();
        }
    }

    delete bitmap;
    bitmap = 0;
}
#endif

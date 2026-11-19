#include "image_loaders.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <share.h>
#include <vector>

#include "scripting/errors.h"

#define PNG_NO_CONSOLE_IO
#include "png/png.h"

static void CopyWithAlphaFirst (unsigned char * pixels, const unsigned char * source, size_t bytes)
  {
  for (size_t i = 0; i < bytes; i += 4)
    {
    const unsigned char b = source [i], g = source [i + 1], r = source [i + 2], a = source [i + 3];
    pixels [i] = a;
    pixels [i + 1] = b;
    pixels [i + 2] = g;
    pixels [i + 3] = r;
    }
  }

using ImageStream = std::unique_ptr<FILE, decltype (&fclose)>;

static __int64 RewindAndGetLength (FILE * file)
  {
  if (_fseeki64 (file, 0, SEEK_END) != 0) return -1;
  const __int64 length = _ftelli64 (file);
  return _fseeki64 (file, 0, SEEK_SET) == 0 ? length : -1;
  }

static const size_t BmpReadSize = 64 * 1024;

struct BmpInput
  {
  FILE * file;
  const unsigned char * buffer;
  unsigned __int64 length, position;

  unsigned __int64 Remaining () const
    {
    return position < length ? length - position : 0;
    }

  bool Read (void * dest, size_t bytes, bool pad = false)
    {
    const size_t available = static_cast<size_t> ((std::min<unsigned __int64>) (bytes, Remaining ()));
    if (available < bytes && !pad) return false;
    if (available)
      {
      if (file)
        {
        if (fread (dest, 1, available, file) != available) return false;
        }
      else
        memcpy (dest, buffer + static_cast<size_t> (position), available);
      }
    if (available < bytes)
      memset (static_cast<unsigned char *> (dest) + available, 0, bytes - available);
    position += bytes;
    return true;
    }

  bool Seek (unsigned __int64 offset)
    {
    if (file)
      {
      if (position != offset && _fseeki64 (file, offset, SEEK_SET) != 0) return false;
      }
    position = offset;
    return true;
    }
  };

// RLE commands start at the bottom of the image. Skipped pixels keep palette index zero.
static bool ReadBitmapRle (BmpInput & input, unsigned char * pixels, size_t stride,
                           unsigned int width, unsigned int height, bool four_bit)
  {
  unsigned int x = 0, y = 0;
  unsigned char command [2], literal [256];
  while (input.Remaining () >= sizeof command)
    {
    if (!input.Read (command, sizeof command)) return false;
    const unsigned int count = command [0], value = command [1];
    if (!count && value == 1) return true;
    if (y >= height) return false;
    if (!count && value == 0)
      {
      x = 0;
      ++y;
      continue;
      }
    if (!count && value == 2)
      {
      if (input.Remaining () < sizeof command) return true;
      if (!input.Read (command, sizeof command)
          || command [0] > width - x || command [1] >= height - y) return false;
      x += command [0];
      y += command [1];
      continue;
      }

    const unsigned int run = count ? count : value;
    if (run > width - x) return false;
    if (!count)
      {
      const unsigned int bytes = four_bit ? (run + 1) / 2 : run;
      if (!input.Read (literal, (bytes + 1) & ~1u, true)) return false;
      }
    if (pixels)
      {
      unsigned char * row = pixels + static_cast<size_t> (height - 1 - y) * stride;
      if (four_bit && count)
        {
        unsigned char * out = row + x / 2;
        unsigned int n = run;
        unsigned char pair = static_cast<unsigned char> (value);
        if (x & 1)
          {
          *out = (*out & 0xF0) | (pair >> 4);
          ++out;
          --n;
          pair = static_cast<unsigned char> ((pair << 4) | (pair >> 4));
          }
        memset (out, pair, n / 2);
        if (n & 1) out [n / 2] = (out [n / 2] & 0x0F) | (pair & 0xF0);
        }
      else if (four_bit)
        {
        for (unsigned int i = 0; i < run; ++i)
          {
          const unsigned int pair = literal [i / 2];
          const unsigned int index = i & 1 ? pair & 15 : pair >> 4;
          unsigned char & out = row [(x + i) / 2];
          if ((x + i) & 1) out = (out & 0xF0) | index;
          else             out = (out & 0x0F) | (index << 4);
          }
        }
      else if (count) memset (row + x, value, run);
      else            memcpy (row + x, literal, run);
      }
    x += run;
    }
  // A short command stream leaves the remaining pixels at palette index zero.
  return true;
  }

static void CopyBottomUpRows (unsigned char * pixels, const unsigned char * source, size_t stride,
                              unsigned int height, bool swap_alpha_endianness)
  {
  if (stride == sizeof (DWORD))
    {
    for (unsigned int y = height; y; --y, source += sizeof (DWORD))
      {
      DWORD pixel;
      memcpy (&pixel, source, sizeof pixel);
      if (swap_alpha_endianness) pixel = (pixel << 8) | (pixel >> 24);
      memcpy (pixels + (y - 1) * sizeof pixel, &pixel, sizeof pixel);
      }
    return;
    }
  for (unsigned int y = height; y; --y, source += stride)
    {
    unsigned char * row = pixels + static_cast<size_t> (y - 1) * stride;
    if (swap_alpha_endianness) CopyWithAlphaFirst (row, source, stride);
    else memcpy (row, source, stride);
    }
  }

static bool ReadBitmapRows (BmpInput & input, unsigned char * pixels, size_t stride,
                            unsigned int height, bool top_down, bool swap_alpha_endianness)
  {
  const size_t bytes = stride * height;
  if (input.buffer && bytes <= input.Remaining ())
    {
    const unsigned char * source = input.buffer + static_cast<size_t> (input.position);
    if (!top_down) CopyBottomUpRows (pixels, source, stride, height, swap_alpha_endianness);
    else if (swap_alpha_endianness) CopyWithAlphaFirst (pixels, source, bytes);
    else memcpy (pixels, source, bytes);
    return true;
    }
  if (top_down)
    {
    if (!input.Read (pixels, bytes, true)) return false;
    if (swap_alpha_endianness) CopyWithAlphaFirst (pixels, pixels, bytes);
    return true;
    }

  const unsigned int rows_per_read = (std::max<unsigned int>) (1, BmpReadSize / stride);
  for (unsigned int remaining = height; remaining; )
    {
    // The top rows have not been decoded yet. Read there while the block fits above its destination.
    const unsigned int rows = (std::min) (rows_per_read, (std::max) (1u, remaining / 2));
    remaining -= rows;
    unsigned char * dest = pixels + static_cast<size_t> (remaining) * stride;
    if (rows == 1)
      {
      if (!input.Read (dest, stride, true)) return false;
      if (swap_alpha_endianness) CopyWithAlphaFirst (dest, dest, stride);
      }
    else
      {
      if (!input.Read (pixels, rows * stride, true)) return false;
      CopyBottomUpRows (dest, pixels, stride, rows, swap_alpha_endianness);
      }
    }
  return true;
  }

static bool ReadBitmapBitfields (BmpInput & input, HBITMAP bitmap, BITMAPINFO * source,
                                 size_t stride, unsigned int height, std::vector<unsigned char> & band)
  {
  if (band.empty ())
    return ::SetDIBits (NULL, bitmap, 0, height, input.buffer + static_cast<size_t> (input.position),
                        source, DIB_RGB_COLORS) == static_cast<int> (height) && ::GdiFlush ();

  const unsigned int rows_per_band = static_cast<unsigned int> (band.size () / stride);
  for (unsigned int y = 0; y < height; )
    {
    const unsigned int rows = (std::min) (rows_per_band, height - y);
    // SetDIBits counts the destination scan lines from the bottom.
    const unsigned int start = source->bmiHeader.biHeight < 0 ? height - y - rows : y;
    if (!input.Read (band.data (), rows * stride, true)
        || ::SetDIBits (NULL, bitmap, start, rows, band.data (), source, DIB_RGB_COLORS) != static_cast<int> (rows)
        || !::GdiFlush ()) return false;
    y += rows;
    }
  return true;
  }

// LoadImage can return a bottom-up bitmap. Decode directly into a top-down
// bitmap so drawing uses one row order. This avoids allocating a second
// bitmap to reorder the rows.
static long ReadBmpBitmap (LPCWSTR path, const unsigned char * buffer, size_t length,
                           HBITMAP & bitmap, bool swap_alpha_endianness)
  {
  ImageStream stream (NULL, fclose);
  BmpInput input = {NULL, buffer, length, 0};
  if (path)
    {
    // LoadImage rejected BMPs held open for writing.
    stream.reset (_wfsopen (path, L"rb", _SH_DENYWR));
    if (!stream) return errno == ENOENT || errno == ENOTDIR ? eFileNotFound : eUnableToLoadImage;
    const __int64 file_length = RewindAndGetLength (stream.get ());
    if (file_length < 0) return eUnableToLoadImage;
    input = {stream.get (), NULL, static_cast<unsigned __int64> (file_length), 0};
    }

  BITMAPFILEHEADER file;
  DWORD header_size;
  if (!input.Read (&file, sizeof file) || file.bfType != 0x4D42
      || !input.Read (&header_size, sizeof header_size)) return eUnableToLoadImage;
  const bool core = header_size == sizeof (BITMAPCOREHEADER);
  if (!core && header_size != 40 && header_size != 52 && header_size != 56
      && header_size != 108 && header_size != 124) return eUnableToLoadImage;
  unsigned char header [sizeof (BITMAPV5HEADER)];
  memcpy (header, &header_size, sizeof header_size);
  if (!input.Read (header + sizeof header_size, header_size - sizeof header_size, true)) return eUnableToLoadImage;

  BITMAPINFOHEADER info = {};
  if (core)
    {
    BITMAPCOREHEADER original;
    memcpy (&original, header, sizeof original);
    info.biWidth = original.bcWidth;
    info.biHeight = original.bcHeight;
    info.biPlanes = original.bcPlanes;
    info.biBitCount = original.bcBitCount;
    }
  else
    memcpy (&info, header, sizeof info);
  info.biSize = sizeof info;
  if (info.biPlanes != 1 || info.biWidth <= 0 || !info.biHeight || info.biHeight == LONG_MIN)
    return eUnableToLoadImage;
  const int depth = info.biBitCount;
  if (depth != 1 && depth != 4 && depth != 8 && depth != 16 && depth != 24 && depth != 32)
    return eUnableToLoadImage;
  const bool fields = info.biCompression == BI_BITFIELDS;
  const bool rle = info.biCompression == BI_RLE4 || info.biCompression == BI_RLE8;
  if ((fields && depth != 16 && depth != 32)
      || (rle && (info.biHeight < 0 || (info.biCompression == BI_RLE4 ? depth != 4 : depth != 8)))
      || (!fields && !rle && info.biCompression != BI_RGB)) return eUnableToLoadImage;

  const bool top_down = info.biHeight < 0;
  const unsigned int height = top_down ? -info.biHeight : info.biHeight;
  const unsigned __int64 row_bytes = (static_cast<unsigned __int64> (info.biWidth) * depth + 31) / 32 * 4;
  const unsigned __int64 bytes = row_bytes * height;
  if (row_bytes > MAXLONG || bytes > MAXDWORD) return eUnableToLoadImage;
  const size_t stride = static_cast<size_t> (row_bytes);
  const unsigned int palette_size = depth <= 8 ? 1u << depth : 0;
  const unsigned int colors = palette_size ? (info.biClrUsed ? info.biClrUsed : palette_size) : 0;
  if (colors > palette_size) return eUnableToLoadImage;

  const size_t masks = fields ? 3 * sizeof (DWORD) : 0;
  std::vector<DWORD> description ((sizeof info + masks) / sizeof (DWORD) + palette_size, 0);
  BITMAPINFO * format = reinterpret_cast<BITMAPINFO *> (description.data ());
  DWORD * extra = description.data () + sizeof info / sizeof (DWORD);
  if (fields)
    {
    if (header_size == sizeof info)
      {
      if (!input.Read (extra, masks, true)) return eUnableToLoadImage;
      }
    else
      memcpy (extra, header + sizeof info, masks);
    for (int i = 0; i < 3; ++i)
      {
      if (!extra [i] || (depth == 16 && extra [i] > 0xFFFF)) return eUnableToLoadImage;
      const DWORD shifted = extra [i] / (extra [i] & (0u - extra [i]));
      if (shifted & (shifted + 1)) return eUnableToLoadImage;
      }
    if ((extra [0] & extra [1]) || (extra [0] & extra [2]) || (extra [1] & extra [2])) return eUnableToLoadImage;
    extra += 3;
    }
  if (core)
    {
    for (unsigned int i = 0; i < colors; ++i)
      if (!input.Read (extra + i, sizeof (RGBTRIPLE), true)) return eUnableToLoadImage;
    }
  else if (!input.Read (extra, colors * sizeof (RGBQUAD), true)) return eUnableToLoadImage;

  // Older BMP files can omit the offset when the pixels follow the colour table.
  unsigned __int64 offset = file.bfOffBits;
  if (!offset)
    offset = input.position + (palette_size ? 0 : static_cast<unsigned __int64> (info.biClrUsed) * sizeof (RGBQUAD));
  if (offset < input.position || !input.Seek (offset)) return eUnableToLoadImage;
  if (rle)
    {
    if (info.biSizeImage)
      input.length = (std::min) (input.length, offset + info.biSizeImage);
    // Check the command stream before allocating a bitmap of the declared dimensions.
    if (!ReadBitmapRle (input, NULL, stride, info.biWidth, height, depth == 4)
        || !input.Seek (offset)) return eUnableToLoadImage;
    }

  std::vector<unsigned char> band;
  if (fields && (input.file || bytes > input.Remaining ()))
    {
    const size_t rows = (std::min<size_t>) (height, (std::max<size_t>) (1, BmpReadSize / stride));
    band.resize (rows * stride);
    }
  info.biClrUsed = colors;
  info.biSizeImage = static_cast<DWORD> (bytes);
  format->bmiHeader = info;
  format->bmiHeader.biHeight = -static_cast<LONG> (height);
  format->bmiHeader.biCompression = BI_RGB;
  void * data = NULL;
  HBITMAP loaded = ::CreateDIBSection (NULL, format, DIB_RGB_COLORS, &data, NULL, 0);
  if (!loaded) return eUnableToLoadImage;
  unsigned char * pixels = static_cast<unsigned char *> (data);
  bool ok;
  if (rle)
    {
    memset (pixels, 0, static_cast<size_t> (bytes));
    ok = ReadBitmapRle (input, pixels, stride, info.biWidth, height, depth == 4);
    }
  else if (fields)
    {
    format->bmiHeader = info;
    ok = ReadBitmapBitfields (input, loaded, format, stride, height, band);
    if (ok && swap_alpha_endianness && depth == 32)
      CopyWithAlphaFirst (pixels, pixels, static_cast<size_t> (bytes));
    }
  else
    ok = ReadBitmapRows (input, pixels, stride, height, top_down, swap_alpha_endianness && depth == 32);
  if (!ok)
    {
    ::DeleteObject (loaded);
    return eUnableToLoadImage;
    }
  bitmap = loaded;
  return eOK;
  }

// Read uncompressed 32-bit BGRA TGA data in top-to-bottom row order.
// Require eight alpha bits, no image ID or colour map, and zero origin coordinates.
static long ReadTgaBitmap (FILE * file, const unsigned char * buffer, size_t length,
                           HBITMAP & bitmap, bool swap_alpha_endianness)
  {
  if (file)
    {
    const __int64 file_length = RewindAndGetLength (file);
    if (file_length < 0 || static_cast<unsigned __int64> (file_length) > static_cast<size_t> (-1))
      return eUnableToLoadImage;
    length = static_cast<size_t> (file_length);
    }
  unsigned char header [18];
  if (length < sizeof header) return eUnableToLoadImage;
  if (file)
    {
    if (fread (header, 1, sizeof header, file) != sizeof header) return eUnableToLoadImage;
    }
  else
    {
    if (!buffer) return eUnableToLoadImage;
    memcpy (header, buffer, sizeof header);
    }
  static const unsigned char prefix [12] = {0, 0, 2};
  if (memcmp (header, prefix, sizeof prefix) != 0 || header [16] != 32 || header [17] != 40)
    return eUnableToLoadImage;
  const int width = header [12] | (header [13] << 8);
  const int height = header [14] | (header [15] << 8);
  const unsigned __int64 bytes = static_cast<unsigned __int64> (width) * height * 4;
  if (!width || !height || bytes > MAXDWORD || bytes > length - sizeof header)
    return eUnableToLoadImage;

  const size_t pixel_end = sizeof header + static_cast<size_t> (bytes);
  const bool has_footer = length > pixel_end;
  if (has_footer)
    {
    static const char signature [] = "TRUEVISION-XFILE.";
    DWORD offsets [2];  // Extension area and developer directory, in that order.
    unsigned char footer [sizeof offsets + sizeof signature];
    if (length - pixel_end < sizeof footer) return eUnableToLoadImage;
    const size_t footer_start = length - sizeof footer;
    if (file)
      {
      if (_fseeki64 (file, footer_start, SEEK_SET) != 0
          || fread (footer, 1, sizeof footer, file) != sizeof footer
          || fgetc (file) != EOF || ferror (file)
          || _fseeki64 (file, sizeof header, SEEK_SET) != 0)
        return eUnableToLoadImage;
      }
    else
      memcpy (footer, buffer + footer_start, sizeof footer);
    if (memcmp (footer + sizeof offsets, signature, sizeof signature) != 0)
      return eUnableToLoadImage;

    // Optional metadata starts after the pixels and ends before the footer.
    memcpy (offsets, footer, sizeof offsets);
    const auto fits = [pixel_end, footer_start] (DWORD offset, size_t minimum_size)
      {
      return offset == 0 || (offset >= pixel_end && offset <= footer_start
        && minimum_size <= footer_start - offset);
      };
    const size_t extension_size = 495;
    if (!fits (offsets [0], extension_size) || !fits (offsets [1], sizeof (WORD))
        || (footer_start != pixel_end && offsets [0] == 0 && offsets [1] == 0))
      return eUnableToLoadImage;
    }

  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void * pixels = NULL;
  HBITMAP loaded = ::CreateDIBSection (NULL, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
  if (!loaded) return eUnableToLoadImage;
  if (file)
    {
    if (fread (pixels, 1, static_cast<size_t> (bytes), file) != bytes
        || (!has_footer && fgetc (file) != EOF) || ferror (file))
      {
      ::DeleteObject (loaded);
      return eUnableToLoadImage;
      }
    }
  else if (swap_alpha_endianness)
    CopyWithAlphaFirst (static_cast<unsigned char *> (pixels), buffer + sizeof header, static_cast<size_t> (bytes));
  else
    memcpy (pixels, buffer + sizeof header, static_cast<size_t> (bytes));
  if (file && swap_alpha_endianness)
    CopyWithAlphaFirst (static_cast<unsigned char *> (pixels), static_cast<const unsigned char *> (pixels), static_cast<size_t> (bytes));
  bitmap = loaded;
  return eOK;
  }

static void user_error_fn(png_structp png_ptr,
        png_const_charp error_msg)
  {
//  AfxMessageBox (error_msg);
  }

static void user_warning_fn(png_structp png_ptr,
    png_const_charp warning_msg)
  {
//  AfxMessageBox (warning_msg);

  }

typedef struct
  {
  const unsigned char * BufferPos;
  size_t LengthRemaining;
  } tPngBufferInfo;

static void ReadDataFromInputStream(png_structp png_ptr, png_bytep outBytes,
                      png_size_t byteCountToRead)
{
   if(png_get_io_ptr (png_ptr) == NULL)
     {
     png_error(png_ptr, "No io_ptr structure");
     return;   // oops
     }

   // get our structure with buffer and length left in it
   tPngBufferInfo * PngBufferInfo = (tPngBufferInfo*)png_get_io_ptr (png_ptr);

   if (byteCountToRead > PngBufferInfo->LengthRemaining)
     {
     png_error(png_ptr, "Insufficient data in buffer");
     return;
     }

   // copy wanted bytes
   memcpy (outBytes, PngBufferInfo->BufferPos, byteCountToRead);
   PngBufferInfo->BufferPos += byteCountToRead;
   PngBufferInfo->LengthRemaining -= byteCountToRead;

}  // end of ReadDataFromInputStream

static const int PngSignatureSize = 8;

static long ReadPngBitmap (FILE * file, const unsigned char * buffer, size_t length,
                           HBITMAP & bitmap, bool swap_alpha_endianness)
  {
  png_structp png_ptr = png_create_read_struct (PNG_LIBPNG_VER_STRING, NULL,
                                                user_error_fn, user_warning_fn);
  if (!png_ptr)
    return eUnableToLoadImage;
  png_infop info_ptr = png_create_info_struct (png_ptr);
  if (!info_ptr)
    {
    png_destroy_read_struct (&png_ptr, NULL, NULL);
    return eUnableToLoadImage;
    }

  // libpng uses longjmp on errors. Keep cleanup state volatile and avoid
  // objects that need C++ destructors between setjmp and the last PNG read.
  HBITMAP volatile pending_bitmap = NULL;
  png_bytepp volatile rows = NULL;
  tPngBufferInfo input = {buffer, length};

#pragma warning (push)
#pragma warning (disable : 4611)
  if (setjmp (png_jmpbuf (png_ptr)))
    {
    if (rows) png_free (png_ptr, rows);
    if (pending_bitmap) DeleteObject (pending_bitmap);
    png_destroy_read_struct (&png_ptr, &info_ptr, NULL);
    return eUnableToLoadImage;
    }
#pragma warning (pop)

  if (file)
    png_init_io (png_ptr, file);
  else
    png_set_read_fn (png_ptr, &input, ReadDataFromInputStream);
  // Both inputs start after the signature used to select this decoder.
  png_set_sig_bytes (png_ptr, PngSignatureSize);

  png_read_info (png_ptr, info_ptr);
  png_set_strip_16 (png_ptr);
  png_set_packing (png_ptr);
  png_set_expand (png_ptr);
  png_set_bgr (png_ptr);
  if (swap_alpha_endianness) png_set_swap_alpha (png_ptr);
  png_set_gray_to_rgb (png_ptr);
  png_set_interlace_handling (png_ptr);
  png_read_update_info (png_ptr, info_ptr);

  const png_uint_32 width = png_get_image_width (png_ptr, info_ptr);
  const png_uint_32 height = png_get_image_height (png_ptr, info_ptr);
  const int depth = png_get_color_type (png_ptr, info_ptr) & PNG_COLOR_MASK_ALPHA ? 32 : 24;
  const unsigned __int64 stride = (static_cast<unsigned __int64> (width) * depth + 31) / 32 * 4;
  const unsigned __int64 bytes = stride * height;
  const png_size_t row_bytes = png_get_rowbytes (png_ptr, info_ptr);
  if (!width || !height || width > MAXLONG || height > MAXLONG
      || stride > MAXLONG || bytes > MAXDWORD || row_bytes > stride
      || height > static_cast<size_t> (-1) / sizeof (png_bytep))
    png_error (png_ptr, "Image is too large for a bitmap");

  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -static_cast<LONG> (height);
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = depth;
  info.bmiHeader.biCompression = BI_RGB;
  info.bmiHeader.biSizeImage = static_cast<DWORD> (bytes);
  void * pixels = NULL;
  pending_bitmap = CreateDIBSection (NULL, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
  if (!pending_bitmap)
    {
    png_destroy_read_struct (&png_ptr, &info_ptr, NULL);
    return eUnableToLoadImage;
    }

  rows = static_cast<png_bytepp> (png_malloc (png_ptr, height * sizeof (png_bytep)));
  for (png_uint_32 y = 0; y < height; ++y)
    {
    rows [y] = static_cast<png_bytep> (pixels)
      + static_cast<size_t> (y) * static_cast<size_t> (stride);
    // libpng writes the pixels, but not the bitmap's row padding.
    memset (rows [y] + row_bytes, 0, static_cast<size_t> (stride) - row_bytes);
    }
  png_read_image (png_ptr, rows);
  png_read_end (png_ptr, info_ptr);

  png_free (png_ptr, rows);
  rows = NULL;
  png_destroy_read_struct (&png_ptr, &info_ptr, NULL);
  bitmap = pending_bitmap;
  return eOK;
  }

enum class ImageFormat { Unknown, Bmp, Png, Tga };

static ImageFormat ImageFormatFromHeader (const unsigned char * header, size_t length)
  {
  if (length >= 2 && header [0] == 'B' && header [1] == 'M')
    return ImageFormat::Bmp;
  if (length >= 3 && header [0] == 0 && header [1] == 0 && header [2] == 2)
    return ImageFormat::Tga;
  if (length >= PngSignatureSize && png_sig_cmp (header, 0, PngSignatureSize) == 0)
    return ImageFormat::Png;
  return ImageFormat::Unknown;
  }

static long OpenImageStream (LPCWSTR path, ImageStream & file, ImageFormat & format)
  {
  file.reset (_wfopen (path, L"rb"));
  if (!file)
    return errno == ENOENT || errno == ENOTDIR ? eFileNotFound : eCouldNotOpenFile;
  unsigned char header [PngSignatureSize];
  const size_t length = fread (header, 1, sizeof header, file.get ());
  if (ferror (file.get ()) || (length < sizeof header && !feof (file.get ())))
    return eUnableToLoadImage;
  format = ImageFormatFromHeader (header, length);
  return eOK;
  }

enum class PathLookup { AsGiven, BitmapSearch };

static long ResolveImagePath (LPCWSTR name, PathLookup lookup, std::vector<wchar_t> & path)
  {
  DWORD size = MAX_PATH;
  for (;;)
    {
    path.resize (size);
    const DWORD length = lookup == PathLookup::AsGiven
        ? ::GetFullPathNameW (name, size, path.data (), NULL)
        : ::SearchPathW (NULL, name, NULL, size, path.data (), NULL);
    if (!length)
      {
      const DWORD error = ::GetLastError ();
      return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? eFileNotFound : eCouldNotOpenFile;
      }
    if (length < size)
      return eOK;
    // Both functions return the required capacity when the path does not fit.
    size = length + 1;
    }
  }

long LoadImageFile (LPCTSTR filename, HBITMAP & bitmap)
  {
  bitmap = NULL;
  if (!filename || !*filename)
    return eBadParameter;

  // A relative ANSI name can resolve to a directory outside the ANSI code page.
  const int chars = ::MultiByteToWideChar (CP_ACP, 0, filename, -1, NULL, 0);
  if (!chars)
    return eBadParameter;
  std::vector<wchar_t> name (chars);
  if (!::MultiByteToWideChar (CP_ACP, 0, filename, -1, name.data (), chars))
    return eBadParameter;
  std::vector<wchar_t> localPath;
  const long resolved = ResolveImagePath (name.data (), PathLookup::AsGiven, localPath);
  if (resolved != eOK)
    return resolved;
  ImageStream file (NULL, fclose);
  ImageFormat format = ImageFormat::Unknown;
  const long localResult = OpenImageStream (localPath.data (), file, format);

  // Only legacy BMP requests search beside the executable or on PATH.
  // The header still selects the decoder for every file at the supplied path.
  const wchar_t * end = name.data () + chars - 1;
  while (end != name.data () && std::iswspace (end [-1])) --end;
  if ((format == ImageFormat::Bmp || format == ImageFormat::Unknown)
      && end - name.data () >= 4 && _wcsnicmp (end - 4, L".bmp", 4) == 0
      && wcscmp (name.data (), localPath.data ()) != 0)
    {
    std::vector<wchar_t> searched;
    const long found = ResolveImagePath (name.data (), PathLookup::BitmapSearch, searched);
    if (found != eOK && found != eFileNotFound)
      return found;
    if (found == eOK && wcscmp (localPath.data (), searched.data ()) != 0)
      {
      ImageStream candidate (NULL, fclose);
      ImageFormat candidateFormat;
      const long opened = OpenImageStream (searched.data (), candidate, candidateFormat);
      if (opened != eOK)
        return opened;
      if (candidateFormat == ImageFormat::Bmp)
        return ReadBmpBitmap (searched.data (), NULL, 0, bitmap, false);
      }
    }

  if (localResult != eOK)
    return localResult;
  switch (format)
    {
    case ImageFormat::Bmp:
      return ReadBmpBitmap (localPath.data (), NULL, 0, bitmap, false);
    case ImageFormat::Png:
      return ReadPngBitmap (file.get (), NULL, 0, bitmap, false);
    case ImageFormat::Tga:
      return ReadTgaBitmap (file.get (), NULL, 0, bitmap, false);
    default:
      return eUnableToLoadImage;
    }
  }

long LoadImageMemory (const unsigned char * buffer, size_t length, HBITMAP & bitmap,
                      bool swap_alpha_endianness)
  {
  bitmap = NULL;
  if (!buffer)
    return eUnableToLoadImage;
  switch (ImageFormatFromHeader (buffer, length))
    {
    case ImageFormat::Bmp:
      return ReadBmpBitmap (NULL, buffer, length, bitmap, swap_alpha_endianness);
    case ImageFormat::Png:
      return ReadPngBitmap (NULL, buffer + PngSignatureSize, length - PngSignatureSize,
                            bitmap, swap_alpha_endianness);
    case ImageFormat::Tga:
      return ReadTgaBitmap (NULL, buffer, length, bitmap, swap_alpha_endianness);
    default:
      return eUnableToLoadImage;
    }
  }

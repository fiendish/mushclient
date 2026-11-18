#ifndef MUSHCLIENT_IMAGE_LOADERS_H
#define MUSHCLIENT_IMAGE_LOADERS_H

#include <cstddef>
#include <windows.h>

// The caller owns the top-down bitmap on success. On failure, the bitmap is null.
// File and memory input use the same header check to select a decoder.
long LoadImageFile (LPCTSTR filename, HBITMAP & bitmap);

// The swap option moves the last byte to the first in 32-bit BGRA pixels.
long LoadImageMemory (const unsigned char * buffer, std::size_t length, HBITMAP & bitmap,
                      bool swap_alpha_endianness = false);

#endif // MUSHCLIENT_IMAGE_LOADERS_H

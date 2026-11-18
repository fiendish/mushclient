#include "miniwindow_alpha_blend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
// Convert between encoded sRGB and linear light without per-pixel powers.
struct MiniWindowAlphaSRGB
  {
  double linear [256];
  unsigned char encoded [4097];

  MiniWindowAlphaSRGB ()
    {
    for (int i = 0; i < 256; ++i)
      {
      const double value = i / 255.0;
      linear [i] = value <= 0.04045 ? value / 12.92
        : std::pow ((value + 0.055) / 1.055, 2.4);
      }
    for (int i = 0; i <= 4096; ++i)
      {
      const double value = i / 4096.0;
      const double srgb = value <= 0.0031308 ? value * 12.92
        : 1.055 * std::pow (value, 1 / 2.4) - 0.055;
      encoded [i] = static_cast<unsigned char> (srgb * 255 + 0.5);
      }
    }

  unsigned char Encode (double value) const
    {
    if (value <= 0)
      return 0;
    if (value >= 1)
      return 255;
    return encoded [static_cast<size_t> (value * 4096 + 0.5)];
    }
  };

// Average the source-pixel area covered by each destination pixel.
// The weights stay within the selected source rectangle.
class MiniWindowAlphaFilter
  {
  int m_SourceStart, m_SourceSize, m_DestinationSize, m_DestinationStart;

  public:
  struct Span
    {
    int first, last;
    std::vector<double> weights;

    double Weight (int pixel) const
      {
      return weights.empty () ? 1.0 : weights [pixel - first];
      }
    };

  std::vector<Span> spans;

  MiniWindowAlphaFilter (int source_start, int source_size, int destination_size,
                         int destination_start, int count)
    : m_SourceStart (source_start), m_SourceSize (source_size),
      m_DestinationSize (destination_size), m_DestinationStart (destination_start)
    {
    spans.reserve (count);
    for (int i = 0; i < count; ++i)
      {
      const int pixel = destination_start + i;
      spans.emplace_back ();
      Span & span = spans.back ();
      if (source_size == destination_size)
        {
        span.first = span.last = pixel;
        }
      else
        {
        const double scale = static_cast<double> (source_size) / destination_size;
        const double left = pixel * scale;
        const double right = (pixel + 1.0) * scale;
        span.first = static_cast<int> (std::floor (left));
        const int last = static_cast<int> (std::ceil (right)) - 1;
        span.last = last < source_size ? last : source_size - 1;
        span.weights.reserve (span.last - span.first + 1);
        for (int sample = span.first; sample <= span.last; ++sample)
          {
          const double a = left > sample ? left : sample;
          const double b = right < sample + 1 ? right : sample + 1;
          span.weights.push_back ((b - a) / scale);
          }
        }
      span.first += source_start;
      span.last += source_start;
      }
    }

  bool OutputRange (int source_first, int source_last, int & first, int & last) const
    {
    if (source_first < m_SourceStart) source_first = m_SourceStart;
    if (source_last > m_SourceStart + m_SourceSize) source_last = m_SourceStart + m_SourceSize;
    if (source_first >= source_last) return false;
    first = static_cast<int> (static_cast<std::int64_t> (source_first - m_SourceStart)
      * m_DestinationSize / m_SourceSize) - m_DestinationStart;
    last = static_cast<int> ((static_cast<std::int64_t> (source_last - m_SourceStart)
      * m_DestinationSize + m_SourceSize - 1) / m_SourceSize) - m_DestinationStart;
    if (first < 0) first = 0;
    if (last > static_cast<int> (spans.size ())) last = static_cast<int> (spans.size ());
    return first < last;
    }
  };

template<bool FullOpacity>
inline void BlendSpan (const unsigned char * source, unsigned char * target,
                       size_t pixel_count, double opacity, bool opaque)
  {
  for (size_t x = 0; x < pixel_count; ++x, source += 4, target += 4)
    {
    const int alpha = opaque ? 255 : source [3];
    if (FullOpacity && alpha == 255)
      {
      target [0] = source [0];
      target [1] = source [1];
      target [2] = source [2];
      }
    else
      {
      for (int c = 0; c < 3; ++c)
        target [c] = MiniWindowAlphaImage::BlendChannel<FullOpacity> (source [c], target [c], alpha, opacity);
      }
    }
  }
} // namespace

void MiniWindowAlphaImage::PrepareRuns () const
  {
  if (runs_ready) return;
  rows.clear ();
  runs.clear ();
  has_partial_alpha = false;
  rows.reserve (static_cast<size_t> (height) + 1);
  for (int y = 0; y < height; ++y)
    {
    rows.push_back (runs.size ());
    if (ignore_alpha)
      {
      if (width > 0) runs.push_back (Run (0, width, true));
      continue;
      }
    const unsigned char * row = Row (y);
    for (int x = 0; x < width; )
      {
      while (x < width && row [static_cast<size_t> (x) * 4 + 3] == 0)
        {
        ++x;
        while (width - x >= 4)
          {
          const unsigned char * p = row + static_cast<size_t> (x) * 4;
          if (p [3] | p [7] | p [11] | p [15]) break;
          x += 4;
          }
        }
      if (x == width) break;
      const int start = x;
      const bool opaque = row [static_cast<size_t> (x) * 4 + 3] == 255;
      do { ++x; }
      while (x < width && row [static_cast<size_t> (x) * 4 + 3] != 0
        && (row [static_cast<size_t> (x) * 4 + 3] == 255) == opaque);
      runs.push_back (Run (start, x, opaque));
      if (!opaque) has_partial_alpha = true;
      }
    }
  rows.push_back (runs.size ());
  runs_ready = true;
  }

void MiniWindowAlphaImage::PrepareRectangles () const
  {
  PrepareRuns ();
  if (rectangles_ready) return;
  rectangles.clear ();
  int left = width, top = height, right = 0, bottom = 0;
  size_t active = 0;
  for (int y = 0; y < height; ++y)
    for (size_t i = rows [y]; i < rows [y + 1]; ++i)
      {
      const Run & run = runs [i];
      active += run.right - run.left;
      if (run.left < left) left = run.left;
      if (run.right > right) right = run.right;
      if (y < top) top = y;
      bottom = y + 1;
      }
  if (runs.empty ()) { rectangles_ready = true; return; }

  // Sparse images use a few exact rectangles instead of copying a large
  // transparent bounding box. Dense images use one destination transfer.
  const size_t area = static_cast<size_t> (right - left) * (bottom - top);
  if (active <= area / 16)
    {
    std::vector<size_t> previous, current;

    for (int y = top; y < bottom; ++y)
      {
      current.clear ();

      for (size_t i = rows [y]; i < rows [y + 1]; ++i)
        {
        const Run & run = runs [i];
        size_t match = rectangles.size ();

        for (size_t j = 0; j < previous.size (); ++j)
          {
          Rect & r = rectangles [previous [j]];
          if (r.left == run.left && r.right == run.right && r.bottom == y)
            {
            match = previous [j];
            break;
            }
          }

        if (match == rectangles.size ())
          {
          rectangles.push_back (Rect (run.left, y, run.right, y + 1));
          if (rectangles.size () > 64)
            {
            rectangles.clear ();
            break;
            }
          }
        else
          rectangles [match].bottom = y + 1;

        current.push_back (match);
        }

      if (rectangles.empty ())
        break;
      previous.swap (current);
      }
    }

  if (rectangles.empty ())
    rectangles.push_back (Rect (left, top, right, bottom));
  rectangles_ready = true;
  }

template<bool FullOpacity>
void MiniWindowAlphaImage::BlendPixels (unsigned char * destination, int destination_stride,
                                        int source_left, int source_top, int w, int h, double opacity) const
  {
  const int source_right = source_left + w;

  for (int y = source_top; y < source_top + h; ++y)
    {
    const unsigned char * source_row = Row (y);
    unsigned char * target_row = destination
      + static_cast<size_t> (y - source_top) * destination_stride;

    if (!runs_ready)
      {
      // A one-time crop can blend its rows without building an index.
      BlendSpan<FullOpacity> (source_row + static_cast<size_t> (source_left) * 4,
                              target_row, w, opacity, ignore_alpha);
      continue;
      }
    for (size_t i = rows [y]; i < rows [y + 1]; ++i)
      {
      const Run & run = runs [i];
      if (run.right <= source_left)
        continue;
      if (run.left >= source_right)
        break;

      const int start = run.left > source_left ? run.left : source_left;
      const int end = run.right < source_right ? run.right : source_right;
      const unsigned char * source = source_row + static_cast<size_t> (start) * 4;
      unsigned char * target = target_row + static_cast<size_t> (start - source_left) * 4;

      BlendSpan<FullOpacity> (source, target, end - start, opacity, ignore_alpha);
      }
    }
  }

void MiniWindowAlphaImage::Blend (unsigned char * destination, int destination_stride,
                                  int source_left, int source_top, int w, int h, double opacity) const
  {
  if (opacity == 1.0)
    BlendPixels<true> (destination, destination_stride, source_left, source_top, w, h, opacity);
  else
    BlendPixels<false> (destination, destination_stride, source_left, source_top, w, h, opacity);
  }

void MiniWindowAlphaImage::BlendScaled (unsigned char * destination, int destination_stride,
                                        int source_left, int source_top, int source_width, int source_height,
                                        int destination_width, int destination_height,
                                        int offset_x, int offset_y, int width, int height, double opacity) const
  {
  if (opacity == 0) return;
  if (source_width == destination_width && source_height == destination_height)
    {
    Blend (destination, destination_stride, source_left + offset_x, source_top + offset_y,
      width, height, opacity);
    return;
    }
  PrepareRuns ();
  if (runs.empty ()) return;
  static const MiniWindowAlphaSRGB colour_space;
  const bool full_opacity = opacity == 1;
  const MiniWindowAlphaFilter horizontal (source_left, source_width,
    destination_width, offset_x, width);
  const MiniWindowAlphaFilter vertical (source_top, source_height,
    destination_height, offset_y, height);
  struct FilteredPixel { double colours [3]; double coverage; };
  // Consecutive output rows can share only their boundary source row.
  int filtered_y = -1;
  int filtered_first = width, filtered_last = 0;
  std::vector<FilteredPixel> filtered_row;
  std::vector<FilteredPixel> resized_row;
  for (int y = 0; y < height; ++y)
    {
    const MiniWindowAlphaFilter::Span & ys = vertical.spans [y];
    if (rows [ys.first] == rows [ys.last + 1]) continue;
    resized_row.assign (width, FilteredPixel ());
    if (full_opacity)
    for (int sy = ys.first; sy <= ys.last; ++sy)
      {
      if (rows [sy] == rows [sy + 1]) continue;
      if (filtered_y != sy)
        {
        filtered_first = width;
        filtered_last = 0;
        const unsigned char * source_row = Row (sy);
        for (size_t i = rows [sy]; i < rows [sy + 1]; ++i)
          {
          const Run & run = runs [i];
          if (!run.opaque) continue;
          int first, last;
          if (!horizontal.OutputRange (run.left, run.right, first, last)) continue;
          // Runs are ordered left to right. Clear each newly covered part once,
          // including gaps between runs. Adjacent runs can share an output pixel.
          if (filtered_last == 0)
            {
            filtered_row.resize (width);
            filtered_first = first;
            filtered_last = first;
            }
          if (last > filtered_last)
            {
            std::fill (filtered_row.begin () + filtered_last, filtered_row.begin () + last, FilteredPixel ());
            filtered_last = last;
            }
          for (int x = first; x < last; ++x)
            {
            const MiniWindowAlphaFilter::Span & xs = horizontal.spans [x];
            const int begin = run.left > xs.first ? run.left : xs.first;
            const int end = run.right < xs.last + 1 ? run.right : xs.last + 1;
            const unsigned char * source = source_row + static_cast<size_t> (begin) * 4;
            for (int sx = begin; sx < end; ++sx, source += 4)
              {
              const double weight = xs.Weight (sx);
              filtered_row [x].coverage += weight;
              for (int c = 0; c < 3; ++c)
                filtered_row [x].colours [c] += colour_space.linear [source [c]] * weight;
              }
            }
          }
        filtered_y = sy;
        }
      const double weight = ys.Weight (sy);
      for (int x = filtered_first; x < filtered_last; ++x)
        {
        resized_row [x].coverage += filtered_row [x].coverage * weight;
        for (int c = 0; c < 3; ++c)
          resized_row [x].colours [c] += filtered_row [x].colours [c] * weight;
        }
      }
    unsigned char * target_row = destination + static_cast<size_t> (y) * destination_stride;
    const bool blend_samples = !full_opacity || has_partial_alpha;
    for (int x = 0; x < width; ++x)
      {
      FilteredPixel & pixel = resized_row [x];
      // Mixed-alpha samples below can still touch a pixel with no opaque coverage.
      if (pixel.coverage >= 1 || (!blend_samples && pixel.coverage <= 0)) continue;
      const double background_weight = 1 - pixel.coverage;
      for (int c = 0; c < 3; ++c)
        pixel.colours [c] += colour_space.linear [target_row [x * 4 + c]] * background_weight;
      }
    if (blend_samples)
      for (int sy = ys.first; sy <= ys.last; ++sy)
        {
        const unsigned char * source_row = Row (sy);
        for (size_t i = rows [sy]; i < rows [sy + 1]; ++i)
          {
          const Run & run = runs [i];
          if (full_opacity && run.opaque) continue;
          int first, last;
          if (!horizontal.OutputRange (run.left, run.right, first, last)) continue;
          for (int x = first; x < last; ++x)
            {
            const MiniWindowAlphaFilter::Span & xs = horizontal.spans [x];
            const int begin = run.left > xs.first ? run.left : xs.first;
            const int end = run.right < xs.last + 1 ? run.right : xs.last + 1;
            const unsigned char * source = source_row + static_cast<size_t> (begin) * 4;
            const unsigned char * background = target_row + x * 4;
            for (int sx = begin; sx < end; ++sx, source += 4)
              {
              const double weight = xs.Weight (sx) * ys.Weight (sy);
              resized_row [x].coverage += weight;
              for (int c = 0; c < 3; ++c)
                {
                const int alpha = ignore_alpha ? 255 : source [3];
                const unsigned char blended = full_opacity
                  ? BlendChannel<true> (source [c], background [c], alpha, opacity)
                  : BlendChannel<false> (source [c], background [c], alpha, opacity);
                resized_row [x].colours [c] += weight
                  * (colour_space.linear [blended] - colour_space.linear [background [c]]);
                }
              }
            }
          }
        }
    for (int x = 0; x < width; ++x)
      {
      const FilteredPixel & pixel = resized_row [x];
      if (pixel.coverage <= 0) continue;
      for (int c = 0; c < 3; ++c)
        target_row [x * 4 + c] = colour_space.Encode (pixel.colours [c]);
      }
    }
  }

// Blending for top-down 32-bit BGRA images with straight alpha.
// Source pixels must stay unchanged while their alpha ranges are cached.
#ifndef MUSHCLIENT_MINIWINDOW_ALPHA_BLEND_H
#define MUSHCLIENT_MINIWINDOW_ALPHA_BLEND_H
#include <cstddef>
#include <vector>

class MiniWindowAlphaImage
  {
  public:
  struct Run
    {
    int left, right;
    bool opaque;

    Run (int l, int r, bool full = false) : left (l), right (r), opaque (full)
      { }
    };

  struct Rect
    {
    int left, top, right, bottom;

    Rect (int l, int t, int r, int b)
      : left (l), top (t), right (r), bottom (b)
      { }
    };

  std::vector<unsigned char> owned_pixels;
  const unsigned char * pixels;
  int width, height, stride;
  bool ignore_alpha;
  mutable std::vector<std::size_t> rows;
  mutable std::vector<Run> runs;
  mutable std::vector<Rect> rectangles;
  mutable bool runs_ready, rectangles_ready, has_partial_alpha;

  MiniWindowAlphaImage (const unsigned char * data, int w, int h, int bytes, bool opaque = false)
    : pixels (data), width (w), height (h), stride (bytes), ignore_alpha (opaque),
      runs_ready (false), rectangles_ready (false), has_partial_alpha (false)
    { }

  // Index visible source ranges once. Scaling and unscaled drawing share them.
  void PrepareRuns () const;

  // Destination-transfer rectangles are needed only for unscaled GDI drawing.
  void PrepareRectangles () const;

  const unsigned char * Row (int y) const
    {
    return pixels + static_cast<std::size_t> (y) * stride;
    }

  template<bool FullOpacity>
  static unsigned char BlendChannel (unsigned char source, unsigned char target,
                                      int alpha, double opacity)
    {
    const unsigned char blended = (source * alpha + target * (255 - alpha)) / 255;
    return FullOpacity ? blended : static_cast<unsigned char>
      (opacity * blended + (1 - opacity) * target + 0.5);
    }

  // Apply the ordinary alpha and opacity blend to each source sample,
  // then average the resulting colours in linear light.
  void BlendScaled (unsigned char * destination, int destination_stride,
                    int source_left, int source_top, int source_width, int source_height,
                    int destination_width, int destination_height,
                    int offset_x, int offset_y, int width, int height, double opacity) const;

  void Blend (unsigned char * destination, int destination_stride,
              int source_left, int source_top, int w, int h, double opacity) const;

  private:
  template<bool FullOpacity>
  inline void BlendPixels (unsigned char * destination, int destination_stride,
                           int source_left, int source_top, int w, int h, double opacity) const;
  };
#endif

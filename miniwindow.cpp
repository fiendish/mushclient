// MiniWindow.cpp : implementation of the CMiniWindow class
//
// See: http://www.gammon.com.au/mw

#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "MUSHview.h"
#include "scripting\errors.h"
#include "color.h"
#include "mainfrm.h"
#include "blending.h"
#include "miniwindow_alpha_blend.h"
#include "image_loaders.h"
#include <climits>
#include <cmath>
#include <new>

#define PNG_NO_CONSOLE_IO
#include "png\png.h"

static std::shared_ptr<MiniWindowDrawingResources> SharedDrawingResources ();
static CBrush * CachedSolidBrush (MiniWindowDrawingResources *, long);

// Restore cached objects before another call can evict them, including on exceptions.
class MiniWindowSelectedObject
  {
  HDC m_DC;
  HGDIOBJ m_Previous;
  public:
  MiniWindowSelectedObject (CDC * dc, CGdiObject * object)
    : m_DC (dc->GetSafeHdc ()),
      m_Previous (::SelectObject (m_DC, object->GetSafeHandle ()))
    { }
  ~MiniWindowSelectedObject ()
    {
    if (Valid ()) ::SelectObject (m_DC, m_Previous);
    }
  bool Valid () const { return m_Previous && m_Previous != HGDI_ERROR; }
  };

static BITMAPINFO TopDownBitmapInfo (int width, int height)
  {
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  return info;
  }

class MiniWindowAlphaScratch
  {
  HDC m_DC;
  HBITMAP m_Bitmap;
  HGDIOBJ m_Previous;

  public:
  unsigned char * pixels;
  int width, height;

  MiniWindowAlphaScratch ()
    : m_DC (NULL), m_Bitmap (NULL), m_Previous (NULL),
      pixels (NULL), width (0), height (0)
    { }

  ~MiniWindowAlphaScratch ()
    {
    if (m_Previous)
      ::SelectObject (m_DC, m_Previous);
    if (m_Bitmap)
      ::DeleteObject (m_Bitmap);
    if (m_DC)
      ::DeleteDC (m_DC);
    }

  HDC DC () const
    {
    return m_DC;
    }

  bool Ensure (int w, int h)
    {
    if (w <= width && h <= height)
      return true;
    // Allocate exactly the first rectangle. Round up only when a later rectangle grows.
    if (m_Bitmap)
      {
      if (w > INT_MAX - 63 || h > INT_MAX - 63)
        return false;
      w = (w + 63) & ~63;
      h = (h + 63) & ~63;
      }

    if (w > INT_MAX / 4)
      return false;
    if (static_cast<unsigned __int64> (w) * h * 4 > MAXDWORD)
      return false;

    if (!m_DC)
      m_DC = ::CreateCompatibleDC (NULL);
    if (!m_DC)
      return false;

    BITMAPINFO info = TopDownBitmapInfo (w, h);

    void * data = NULL;
    HBITMAP bitmap = ::CreateDIBSection (m_DC, &info, DIB_RGB_COLORS, &data, NULL, 0);
    if (!bitmap)
      return false;

    HGDIOBJ previous = ::SelectObject (m_DC, bitmap);
    if (!previous || previous == HGDI_ERROR)
      {
      ::DeleteObject (bitmap);
      return false;
      }

    if (!m_Previous)
      m_Previous = previous;
    if (m_Bitmap)
      ::DeleteObject (m_Bitmap);

    m_Bitmap = bitmap;
    pixels = static_cast<unsigned char *> (data);
    width = w;
    height = h;
    return true;
    }

  // The caller flushes pending GDI copies before accessing pixel memory.
  bool CopyFrom (HDC source, int x, int y, int w, int h)
    {
    return Ensure (w, h) && ::BitBlt (m_DC, 0, 0, w, h, source, x, y, SRCCOPY);
    }
  };

struct MiniWindowImage : CBitmap
  {
  std::unique_ptr<MiniWindowAlphaImage> alpha;
  // Window captures share their pixels with a view that ignores alpha when scaled.
  std::unique_ptr<MiniWindowAlphaImage> opaque_alpha;
  };

static const MiniWindowAlphaImage * CacheAlphaImage (CDC * target, MiniWindowImage & bitmap,
                                                      const BITMAP & bi)
  {
  // GetDIBits converts indexed and device-dependent bitmaps to top-down BGRA.
  const int w = bi.bmWidth, h = bi.bmHeight;
  if (w <= 0 || h <= 0 || w > INT_MAX / 4)
    return NULL;
  const unsigned __int64 bytes = static_cast<unsigned __int64> (w) * h * 4;
  if (bytes > static_cast<size_t> (-1))
    return NULL;

  std::vector<unsigned char> data (static_cast<size_t> (bytes));
  BITMAPINFO info = TopDownBitmapInfo (w, h);
  if (::GetDIBits (target->GetSafeHdc (),
                  reinterpret_cast<HBITMAP> (bitmap.GetSafeHandle ()), 0, h,
                  data.data (), &info, DIB_RGB_COLORS) != h)
    return NULL;

  // Device-dependent bitmaps use the fourth byte as padding, not alpha.
  // Treat those and bitmaps below 32 bits per pixel as opaque.
  if (bi.bmBitsPixel != 32 || !bi.bmBits)
    {
    for (size_t i = 3; i < data.size (); i += 4)
      data [i] = 255;
    }

  bitmap.alpha.reset (new MiniWindowAlphaImage (data.data (), w, h, w * 4));
  bitmap.alpha->owned_pixels.swap (data);
  return bitmap.alpha.get ();
  }

// Use indexed pixels when available. A first crop needs only its own rows.
static const MiniWindowAlphaImage * AlphaSource (CDC * target, MiniWindowImage & bitmap,
    const BITMAP & bi, bool scale,
    long & x, long & y, MiniWindowAlphaImage & crop, MiniWindowAlphaScratch & storage)
  {
  const bool whole = crop.width == bi.bmWidth && crop.height == bi.bmHeight;
  const MiniWindowAlphaImage * cached = scale && bitmap.opaque_alpha
    ? bitmap.opaque_alpha.get () : bitmap.alpha.get ();
  // Unscaled drawing must not use an opaque cache of a device-dependent bitmap.
  if ((scale || bi.bmBits) && cached)
    {
    const MiniWindowAlphaImage & image = *cached;
    if (whole || image.runs_ready || image.runs_over_budget)
      return &image;

    // Use the supplied pixels without indexing the entire image for a crop.
    crop.stride = image.stride;
    crop.pixels = image.Row (y) + static_cast<size_t> (x) * 4;
    crop.ignore_alpha = image.ignore_alpha;
    }
  else
    {
    if (whole)
      {
      DIBSECTION section = {};
      if (scale || (bi.bmBits
          && ::GetObject (bitmap.GetSafeHandle (), sizeof section, &section) == sizeof section
          && section.dsBmih.biCompression == BI_RGB && section.dsBmih.biBitCount == 32
          && section.dsBmih.biPlanes == 1 && section.dsBm.bmWidthBytes > 0))
        return CacheAlphaImage (target, bitmap, bi);
      }

    CDC source;
    if (!source.CreateCompatibleDC (target))
      return NULL;
    MiniWindowSelectedObject selected (&source, &bitmap);
    if (!selected.Valid () || !storage.CopyFrom (source.GetSafeHdc (), x, y, crop.width, crop.height))
      return NULL;
    crop.pixels = storage.pixels;
    crop.stride = storage.width * 4;
    if (scale && (!bi.bmBits || bi.bmBitsPixel != 32))
      {
      if (!::GdiFlush ())
        return NULL;
      for (int row = 0; row < crop.height; ++row)
        {
        unsigned char * p = storage.pixels + static_cast<size_t> (row) * crop.stride;
        for (int col = 0; col < crop.width; ++col)
          p [col * 4 + 3] = 255;
        }
      }
    }
  x = y = 0;
  return &crop;
  }

static long StoreLoadedBitmap (ImageMap & images, LPCTSTR ImageId, HBITMAP hBmp)
  {
  try
    {
    std::unique_ptr<MiniWindowImage> pImage (new MiniWindowImage);
    if (!pImage->Attach (hBmp))
      {
      DeleteObject (hBmp);
      return eUnableToLoadImage;
      }

    hBmp = NULL;

    BITMAP bi = {};
    if (!pImage->GetBitmap (&bi))
      AfxThrowResourceException ();
    if (bi.bmBitsPixel == 32)
      {
      if (!bi.bmBits) AfxThrowResourceException ();
      pImage->alpha.reset (new MiniWindowAlphaImage (static_cast<const unsigned char *> (bi.bmBits),
                                                    bi.bmWidth, bi.bmHeight, bi.bmWidthBytes));
      }
    images [ImageId] = std::move (pImage);
    }
  catch (...)
    {
    if (hBmp)
      DeleteObject (hBmp);
    throw;
    }

  return eOK;
  }

// constructor
CMiniWindow::CMiniWindow ()  :
          m_DrawingResources (SharedDrawingResources ()),
          m_hOldBitmap (NULL),
          m_Bitmap (NULL),
          m_iWidth (0), m_iHeight (0),
          m_iPosition (0), m_iFlags (0),
          m_iBackgroundColour (0), m_bShow (false),
          m_rect (0, 0, 0, 0),
          m_bTemporarilyHide (false),
          m_last_mouseposition (0, 0),
          m_last_mouse_update (0),
          m_client_mouseposition (0, 0),
          m_FlagsOnMouseDown (0),
          m_ZOrder (0),
          m_bExecutingScript (false),
          m_pAddingHotspot (new bool (false))
  {
  pdc = new CDC;
  pdc->CreateCompatibleDC(NULL);
  pdc->SetTextAlign (TA_LEFT | TA_TOP);
  m_tDateInstalled = CTime::GetCurrentTime();  // when miniwindow loaded
  }  // end CMiniWindow::CMiniWindow  (constructor)

CMiniWindow::~CMiniWindow ()  // destructor
  {

  // get rid of old one if any
  if (m_Bitmap)
    {
    pdc->SelectObject((HGDIOBJ) m_hOldBitmap);    // swap old one back
    m_Bitmap->DeleteObject ();        // delete the one we made
    delete m_Bitmap;
    }


  // delete our fonts
  for (FontMapIterator fit = m_Fonts.begin ();
       fit != m_Fonts.end ();
       fit++)
         delete fit->second;

  m_Fonts.clear ();

  // delete our hotspots
  for (HotspotMapIterator hit = m_Hotspots.begin ();
       hit != m_Hotspots.end ();
       hit++)
         delete hit->second;

  m_Hotspots.clear ();

  delete pdc;
  }   // end CMiniWindow::~CMiniWindow  (destructor)


// a negative or zero value for the RH side of a rectange is considered offset from the right edge
// eg. -1 is 1 pixel in from right, 0 is the RH edge.
long CMiniWindow::FixRight (const long Right)
  {

  if (Right <= 0)
    return m_iWidth + Right;

  return Right;
  }  // end CMiniWindow::FixRight

static int BytesPerLine (int nWidth, int nBitsPerPixel)
  {
  return ( (nWidth * nBitsPerPixel + 31) & (~31) ) / 8;
  }

// helper function to make normal/geometric pens
// legacy behaviour is to do what we always did
// however if an endcap or join mask bit is on we juse the ExtCreatePen function
static void MakeAPen (CPen & pen, long PenColour, long PenStyle, long PenWidth)
  {

  // legacy behaviour
  if ((PenStyle & PS_ENDCAP_MASK) == 0 &&
      (PenStyle & PS_JOIN_MASK) == 0)
    {
    pen.CreatePen (PenStyle, PenWidth, PenColour);
    }
  else
    {
    LOGBRUSH logbrush;
    logbrush.lbStyle = BS_SOLID;
    logbrush.lbColor = PenColour;
    logbrush.lbHatch = 0;     // not applicable

    pen.Attach (::ExtCreatePen (PenStyle | PS_GEOMETRIC, PenWidth, &logbrush, 0, NULL));
    }
  } // end of MakeAPen

// a negative or zero value for the bottom of a rectange is considered offset from the bottom edge
long CMiniWindow::FixBottom (const long Bottom)
  {

  if (Bottom <= 0)
    return m_iHeight + Bottom;

  return Bottom;

  }  // end  CMiniWindow::FixBottom


/* positions:

  0 = stretch to output view size
  1 = stretch with aspect ratio

  2 = stretch to owner size
  3 = stretch with aspect ratio

  -- going clockwise here:

  -- top
  4 = top left
  5 = center left-right at top
  6 = top right

  -- rh side
  7 = on right, center top-bottom
  8 = on right, at bottom

  -- bottom
  9 = center left-right at bottom

  -- lh side
  10 = on left, at bottom
  11 = on left, center top-bottom

  -- middle
  12 = center all

  13 = tile


  */


// create (or re-create) a mini-window
void CMiniWindow::Create (long Left, long Top, long Width, long Height,
                          short Position, long Flags,
                          COLORREF BackgroundColour)
  {

  CDC * pNewDC = new CDC;
  CBitmap * pNewBitmap = NULL;
  HBITMAP hNewOldBitmap = NULL;
  try
    {
    pNewBitmap = new CBitmap;
    if (!pNewDC->CreateCompatibleDC(NULL))
      AfxThrowResourceException ();
    pNewDC->SetTextAlign (TA_LEFT | TA_TOP);

    // CreateBitmap with zero dimensions creates a monochrome bitmap, so force at least 1x1.
    if (!pNewBitmap->CreateBitmap (MAX (Width, 1), MAX (Height, 1), 1,
                                   GetDeviceCaps((*pNewDC), BITSPIXEL), NULL))
      AfxThrowResourceException ();
    // MFC temporary wrappers are deleted on idle; retain only the native handle.
    hNewOldBitmap = (HBITMAP) pNewDC->SelectObject ((HGDIOBJ) pNewBitmap->GetSafeHandle ());
    if (!hNewOldBitmap)
      AfxThrowResourceException ();
    pNewDC->SetWindowOrg(0, 0);
    pNewDC->FillSolidRect (0, 0, Width, Height, BackgroundColour);
    }
  catch (...)
    {
    if (hNewOldBitmap)
      pNewDC->SelectObject ((HGDIOBJ) hNewOldBitmap);
    delete pNewBitmap;
    delete pNewDC;
    throw;
    }

  // release the old resources only after the replacement is complete
  if (m_Bitmap)
    {
    pdc->SelectObject((HGDIOBJ) m_hOldBitmap);    // swap old one back
    m_Bitmap->DeleteObject ();
    delete m_Bitmap;
    }
  delete pdc;

  pdc = pNewDC;
  m_Bitmap = pNewBitmap;
  m_hOldBitmap = hNewOldBitmap;

  m_Location.x           = Left            ;
  m_Location.y           = Top             ;
  m_iWidth               = Width           ;
  m_iHeight              = Height          ;
  m_iPosition            = Position        ;
  m_iFlags               = Flags           ;
  m_iBackgroundColour    = BackgroundColour;

  m_bShow = false;

  // a newly created window has no hotspots
  if ((Flags & MINIWINDOW_KEEP_HOTSPOTS) == 0)
    DeleteAllHotspots ();

  } // end of MiniWindow::Create


// set/clear the show flag so the window becomes visible
void  CMiniWindow::Show (bool bShow)
  {
  m_bShow = bShow;
  }    // end of CMiniWindow::Show

/*

  Actions:

  1 = FrameRect        ( 1 pixel )
  2 = FillRect
  3 = InvertRect
  4 = 3D Rect    (Colour1 is top and left edge colour, Colour2 is bottom and right edge colour)
  5 = DrawEdge   (draws a 3d-style edge with optional fill)

  Colour1 = style of edge:

  EDGE_RAISED:      // 5
  EDGE_ETCHED:      // 6
  EDGE_BUMP:        // 9
  EDGE_SUNKEN:      // 10

  Colour2 = where to draw it:

  BF_TOPLEFT      0x3
  BF_TOPRIGHT     0x6
  BF_BOTTOMLEFT   0x9
  BF_BOTTOMRIGHT  0xC
  BF_RECT         0xF

  BF_DIAGONAL     0x0010

  // For diagonal lines, the BF_RECT flags specify the end point of the
  // vector bounded by the rectangle parameter.

  BF_DIAGONAL_ENDTOPLEFT      0x13
  BF_DIAGONAL_ENDTOPRIGHT     0x16
  BF_DIAGONAL_ENDBOTTOMLEFT   0x19
  BF_DIAGONAL_ENDBOTTOMRIGHT  0x1C

  Additional Colour2 flags:

  BF_MIDDLE       0x0800   Fill in the middle
  BF_SOFT         0x1000   For softer buttons
  BF_ADJUST       0x2000   Calculate the space left over
  BF_FLAT         0x4000   For flat rather than 3D borders
  BF_MONO         0x8000   For monochrome borders


  6 = Flood Fill Border (fills to border specified by Colour1)

  7 = Flood Fill Surface (fills while on surface specified by Colour1)

*/

// various rectangle operations
long  CMiniWindow::RectOp (short Action, long Left, long Top, long Right, long Bottom, long Colour1, long Colour2)
  {
  switch (Action)

    {
    case 1:       // frame
      {
      CBrush * brush = CachedSolidBrush (m_DrawingResources.get (), Colour1);
      if (!brush) return eBadParameter;
      pdc->FrameRect (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)), brush);
      break;
      }

    case 2:       // fill
      {
      CBrush * brush = CachedSolidBrush (m_DrawingResources.get (), Colour1);
      if (!brush) return eBadParameter;
      pdc->FillRect (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)), brush);
      break;
      }

    case 3:       // invert
      {
      pdc->InvertRect (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)));
      break;
      }

    case 4:       // 3D rect
      {
      pdc->Draw3dRect (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)), Colour1, Colour2);
      break;
      }

    case 5:       // DrawEdge
      {

      switch (Colour1)   //  nEdge
        {
        // must be one of these 4 flags
        case EDGE_RAISED:      // 5
        case EDGE_ETCHED:      // 6
        case EDGE_BUMP:        // 9
        case EDGE_SUNKEN:      // 10
             break;

        default: return eBadParameter;
        }

      if ((Colour2 & 0xFF) > 0x1F)
        return eBadParameter;

      pdc->DrawEdge (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)), Colour1, Colour2);
      break;
      }

    case 6:       // Flood fill border
      {
      // create brush for fill colour
      CBrush * brush = CachedSolidBrush (m_DrawingResources.get (), Colour2);
      if (!brush) return eBadParameter;
      MiniWindowSelectedObject selectedBrush (pdc, brush);
      if (!selectedBrush.Valid ()) return eBadParameter;
      pdc->FloodFill (Left, Top, Colour1);
      break;
      }

    case 7:       // Flood fill surface
      {
      // create brush for fill colour
      CBrush * brush = CachedSolidBrush (m_DrawingResources.get (), Colour2);
      if (!brush) return eBadParameter;
      MiniWindowSelectedObject selectedBrush (pdc, brush);
      if (!selectedBrush.Valid ()) return eBadParameter;
      pdc->ExtFloodFill (Left, Top, Colour1, FLOODFILLSURFACE);
      break;
      }

    default: return eUnknownOption;

    } // end of switch

  return eOK;

  } // end of CMiniWindow::RectOp

static long ValidatePenStyle (const long PenStyle, const long PenWidth)
  {

  switch (PenStyle & PS_STYLE_MASK)
    {
      // must be one of these flags
      case PS_SOLID:           // 0
      case PS_NULL:            // 5
      case PS_INSIDEFRAME:     // 6
        break;

      case PS_DASH:            // 1       /* -------  */
      case PS_DOT:             // 2       /* .......  */
      case PS_DASHDOT:         // 3       /* _._._._  */
      case PS_DASHDOTDOT:      // 4       /* _.._.._  */
         // Wide patterned pens require the geometric path in MakeAPen.
         if (PenWidth > 1 &&
             (PenStyle & (PS_ENDCAP_MASK | PS_JOIN_MASK)) == 0)
           return ePenStyleNotValid;
         break;

    default: return ePenStyleNotValid;
    }

  switch (PenStyle & PS_ENDCAP_MASK)
    {
      // must be one of these flags
      case PS_ENDCAP_ROUND:   // 0x000
      case PS_ENDCAP_SQUARE:  // 0x100
      case PS_ENDCAP_FLAT:    // 0x200
        break;

    default: return ePenStyleNotValid;
    }

  switch (PenStyle & PS_JOIN_MASK)
    {
      // must be one of these flags
      case PS_JOIN_ROUND:     // 0x0000
      case PS_JOIN_BEVEL:     // 0x1000
      case PS_JOIN_MITER:     // 0x2000
        break;

    default: return ePenStyleNotValid;
    }

  return eOK;
  }


long ValidateBrushStyle (const long BrushStyle,
                          const long PenColour,
                          const long BrushColour,
                          CBrush & br)
  {


  LOGBRUSH lb;
  lb.lbColor = PenColour;

  switch (BrushStyle)
    {
      // must be one of these flags
      case BS_SOLID:           // 0
         lb.lbStyle = BS_SOLID;
         lb.lbColor = BrushColour;
         break;

      case BS_NULL:            // 1
         lb.lbStyle = BS_NULL;
         break;

      // hatched styles:
      case 2:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_HORIZONTAL;
         break;

      case 3:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_VERTICAL;
         break;

      case 4:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_FDIAGONAL;
         break;

      case 5:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_BDIAGONAL;
         break;

      case 6:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_CROSS;
         break;

      case 7:
         lb.lbStyle = BS_HATCHED;
         lb.lbHatch = HS_DIAGCROSS;
         break;

         // each byte is a line, so 0xAA would be 10101010   (top line)
         //                         0x55 would be 01010101   (next line)
      case 8:       // fine hatch
        {
        CBitmap bitmap;
        WORD    wBits[] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };

        bitmap.CreateBitmap (8, 8, 1, 1, wBits);
        br.CreatePatternBrush (&bitmap);
        return eOK;
        }

      case 9:      // medium hatch
        {
        CBitmap bitmap;
        WORD    wBits[] = { 0x33, 0x33, 0xCC, 0xCC, 0x33, 0x33, 0xCC, 0xCC, };

        bitmap.CreateBitmap (8, 8, 1, 1, wBits);
        br.CreatePatternBrush (&bitmap);
        return eOK;
        }

      case 10:       // coarse hatch
        {
        CBitmap bitmap;
        WORD    wBits[] = { 0x0F, 0x0F, 0x0F, 0x0F, 0xF0, 0xF0, 0xF0, 0xF0 };

        bitmap.CreateBitmap (8, 8, 1, 1, wBits);
        br.CreatePatternBrush (&bitmap);
        return eOK;
        }

      case 11:       // waves - horizontal
        {
        CBitmap bitmap;
        WORD    wBits[] = { 0xCC, 0x33, 0x00, 0x00, 0xCC, 0x33, 0x00, 0x00  };

        bitmap.CreateBitmap (8, 8, 1, 1, wBits);
        br.CreatePatternBrush (&bitmap);
        return eOK;
        }

      case 12:       // waves - vertical
        {
        CBitmap bitmap;
        WORD    wBits[] = { 0x11, 0x11, 0x22, 0x22, 0x11, 0x11, 0x22, 0x22  };

        bitmap.CreateBitmap (8, 8, 1, 1, wBits);
        br.CreatePatternBrush (&bitmap);
        return eOK;
        }

    default: return eBrushStyleNotValid;
    }

  br.CreateBrushIndirect (&lb);

  return eOK;
  }

/*

  Actions:

  1 = Ellipse
  2 = Rectangle (can have thicker pen style and be filled)
  3 = Round Rectangle
  4 = Chord (a closed figure bounded by the intersection of an ellipse and a line segment)
  5 = Pie (Draws a pie-shaped wedge by drawing an elliptical arc whose center and two endpoints are joined by lines)

  */

// One bounded cache serves the ordinary drawing calls in all miniwindows.
class MiniWindowDrawingResources
  {
  typedef std::tuple<long, long, long> PenKey;
  typedef std::pair<long, long> BrushKey;
  std::map<PenKey, std::unique_ptr<CPen> > m_Pens;
  std::map<BrushKey, std::unique_ptr<CBrush> > m_Brushes;

  public:
  CPen * Pen (long colour, long style, long width)
    {
    // Null pens can share a stock handle. Keep one wrapper for that handle.
    const PenKey key = (style & PS_STYLE_MASK) == PS_NULL ?
                       PenKey (0, PS_NULL, 0) : PenKey (colour, style, width);
    const auto found = m_Pens.find (key);
    if (found != m_Pens.end ()) return found->second.get ();
    // Each drawing method restores its selected objects before the next call.
    // Limit live handles when drawing uses thousands of colours.
    if (m_Pens.size () >= 512) m_Pens.clear ();
    std::unique_ptr<CPen> pen (new CPen);
    MakeAPen (*pen, colour, style, width);
    if (!pen->GetSafeHandle ()) return NULL;
    CPen * result = pen.get ();
    m_Pens.emplace (key, std::move (pen));
    return result;
    }

  CBrush * Brush (long style, long pen_colour, long brush_colour, long & status)
    {
    // Solid brushes use the fill colour. Hatch brushes use the pen colour.
    // Null and monochrome pattern brushes do not contain a colour.
    const long colour = style == 0 ? brush_colour :
                        (style >= 2 && style <= 7 ? pen_colour : 0);
    const BrushKey key (style, colour);
    const auto found = m_Brushes.find (key);
    if (found != m_Brushes.end ())
      {
      status = eOK;
      return found->second.get ();
      }
    if (m_Brushes.size () >= 512) m_Brushes.clear ();
    std::unique_ptr<CBrush> brush (new CBrush);
    status = ValidateBrushStyle (style, pen_colour, brush_colour, *brush);
    if (status != eOK) return NULL;
    if (!brush->GetSafeHandle ())
      {
      status = eBadParameter;
      return NULL;
      }
    CBrush * result = brush.get ();
    m_Brushes.emplace (key, std::move (brush));
    return result;
    }
  };

// Miniwindows own the cache. This weak reference shares it without extending its lifetime.
static std::weak_ptr<MiniWindowDrawingResources> shared_drawing_resources;

static std::shared_ptr<MiniWindowDrawingResources> SharedDrawingResources ()
  {
  std::shared_ptr<MiniWindowDrawingResources> resources = shared_drawing_resources.lock ();
  if (!resources)
    {
    resources = std::make_shared<MiniWindowDrawingResources> ();
    shared_drawing_resources = resources;
    }
  return resources;
  }

static CBrush * CachedSolidBrush (MiniWindowDrawingResources * resources, long colour)
  {
  long status;
  return resources->Brush (0, 0, colour, status);
  }

class MiniWindowPen
  {
  CPen * m_Pen;
  public:
  MiniWindowPen (MiniWindowDrawingResources * resources,
                 long colour, long style, long width)
    : m_Pen (resources->Pen (colour, style, width)) { }
  CPen * Get () const { return m_Pen; }
  };

class MiniWindowBrush
  {
  long m_Status;
  CBrush * m_Brush;
  public:
  MiniWindowBrush (MiniWindowDrawingResources * resources,
                   long style, long pen_colour, long brush_colour)
    : m_Brush (resources->Brush (style, pen_colour, brush_colour, m_Status)) { }
  long Status () const { return m_Status; }
  CBrush * Get () const { return m_Brush; }
  };

// various circle/ellipse/pie operations
long CMiniWindow::CircleOp (short Action,
                            long Left, long Top, long Right, long Bottom,
                            long PenColour, long PenStyle, long PenWidth,
                            long BrushColour, long BrushStyle,
                            long Extra1, long Extra2, long Extra3, long Extra4)
  {
  long iResult = eUnknownOption;

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  // validate and create requested bruch
  MiniWindowBrush br (m_DrawingResources.get (), BrushStyle, PenColour, BrushColour);
  if (br.Status () != eOK) return br.Status ();

  // create requested pen
  MiniWindowPen pen (m_DrawingResources.get (), PenColour, PenStyle, PenWidth);
  if (!pen.Get ()) return eBadParameter;

  // select into DC
  MiniWindowSelectedObject selectedPen (pdc, pen.Get ());
  if (!selectedPen.Valid ()) return eBadParameter;
  MiniWindowSelectedObject selectedBrush (pdc, br.Get ());
  if (!selectedBrush.Valid ()) return eBadParameter;

  if (BrushStyle > 1 && BrushStyle <= 7)
    {
    pdc->SetBkColor (BrushColour);      // for hatched brushes this is the background colour
    }
  else
  if (BrushStyle > 7)  // pattern brushes
    {
    pdc->SetTextColor (BrushColour);      // for patterned brushes
    pdc->SetBkColor (PenColour);      // for hatched brushes and patterned brushes
    }

  if (BrushColour != -1)
    pdc->SetBkMode (OPAQUE);
  else
    pdc->SetBkMode (TRANSPARENT);

  switch (Action)

    {
    case 1:       // ellipse
      {
      pdc->Ellipse (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)));
      iResult = eOK;
      break;
      }

    case 2:       // rectangle
      {
      pdc->Rectangle (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)));
      iResult = eOK;
      break;
      }

    case 3:       // round rectangle
      {
      pdc->RoundRect (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)), CPoint (Extra1, Extra2));
      iResult = eOK;
      break;
      }

    case 4:       // chord
      {
      pdc->Chord (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)),
                CPoint (Extra1, Extra2),
                CPoint (Extra3, Extra4));
      iResult = eOK;
      break;
      }

    case 5:       // pie
      {
      pdc->Pie (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)),
                CPoint (Extra1, Extra2),
                CPoint (Extra3, Extra4));
      iResult = eOK;
      break;
      }

    } // end of switch


  return iResult;

  } // end of CMiniWindow::CircleOp


// add a font to our map of fonts by name (eg. "inventory")
long CMiniWindow::Font (LPCTSTR FontId,        // eg. "inventory"
                        LPCTSTR FontName,      // eg. "Courier New"
                       double Size,
                       BOOL Bold, BOOL Italic, BOOL Underline, BOOL Strikeout,
                       short Charset, short PitchAndFamily)
  {

  FontMapIterator it = m_Fonts.find (FontId);
  CFont * pOldFont = it == m_Fonts.end () ? NULL : it->second;

  // no font name, and zero point size requested - delete old font
  if (FontName [0] == 0 && Size == 0.0)
    {
    if (it != m_Fonts.end ())
      {
      m_Fonts.erase (it);
      delete pOldFont;
      }
    return eOK;
    }

  std::unique_ptr<CFont> pFont (new CFont);

  int lfHeight = -MulDiv(Size ? Size : 10.0, pdc->GetDeviceCaps(LOGPIXELSY), 72);

  if (pFont->CreateFont (lfHeight,
                          0, // int nWidth,
                          0, // int nEscapement,
                          0, // int nOrientation,
                          Bold ? FW_BOLD : FW_NORMAL, // int nWeight,
                          Italic, // BYTE bItalic,
                          Underline, // BYTE bUnderline,
                          Strikeout, // BYTE cStrikeOut,
                          Charset,
                          0, // BYTE nOutPrecision,
                          0, // BYTE nClipPrecision,
                          0, // BYTE nQuality,
                          PitchAndFamily,
                          FontName) == 0)
    {
    return eCannotAddFont;
    }

  m_Fonts [FontId] = pFont.get ();
  pFont.release ();
  delete pOldFont;
  return eOK;

  }   // end of CMiniWindow::Font


static void SetUpVariantLong (VARIANT & tVariant, const long iContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_I4;
  tVariant.lVal = iContents;
  }   // end of SetUpVariantLong

static void SetUpVariantString (VARIANT & tVariant, const CString & strContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_BSTR;
  tVariant.bstrVal = strContents.AllocSysString ();
  }   // end of SetUpVariantString

static void SetUpVariantBool (VARIANT & tVariant, const BOOL iContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_BOOL;
  tVariant.boolVal = iContents;
  }   // end of SetUpVariantBool

static void SetUpVariantDate (VARIANT & tVariant, const COleDateTime iContents)
  {
  VariantClear (&tVariant);
  tVariant.vt = VT_DATE;
  tVariant.date = iContents;
  }   // end of SetUpVariantDate

// return info about the select font
void CMiniWindow::FontInfo (LPCTSTR FontId, long InfoType, VARIANT & vaResult)
  {
  FontMapIterator it = m_Fonts.find (FontId);

  if (it == m_Fonts.end ())
    return;   // no such font

  CFont* oldFont = pdc->SelectObject(it->second);    // select in the requested font

  TEXTMETRIC tm;
  pdc->GetTextMetrics(&tm);

  CString rString;
  pdc->GetTextFace (rString);

  switch (InfoType)
    {

    case  1:  SetUpVariantLong (vaResult, tm.tmHeight);           break;
    case  2:  SetUpVariantLong (vaResult, tm.tmAscent);           break;
    case  3:  SetUpVariantLong (vaResult, tm.tmDescent);          break;
    case  4:  SetUpVariantLong (vaResult, tm.tmInternalLeading);  break;
    case  5:  SetUpVariantLong (vaResult, tm.tmExternalLeading);  break;
    case  6:  SetUpVariantLong (vaResult, tm.tmAveCharWidth);     break;
    case  7:  SetUpVariantLong (vaResult, tm.tmMaxCharWidth);     break;
    case  8:  SetUpVariantLong (vaResult, tm.tmWeight);           break;
    case  9:  SetUpVariantLong (vaResult, tm.tmOverhang);         break;
    case 10:  SetUpVariantLong (vaResult, tm.tmDigitizedAspectX); break;
    case 11:  SetUpVariantLong (vaResult, tm.tmDigitizedAspectY); break;
    case 12:  SetUpVariantLong (vaResult, tm.tmFirstChar);        break;
    case 13:  SetUpVariantLong (vaResult, tm.tmLastChar);         break;
    case 14:  SetUpVariantLong (vaResult, tm.tmDefaultChar);      break;
    case 15:  SetUpVariantLong (vaResult, tm.tmBreakChar);        break;
    case 16:  SetUpVariantLong (vaResult, tm.tmItalic);           break;
    case 17:  SetUpVariantLong (vaResult, tm.tmUnderlined);       break;
    case 18:  SetUpVariantLong (vaResult, tm.tmStruckOut);        break;
    case 19:  SetUpVariantLong (vaResult, tm.tmPitchAndFamily);   break;
    case 20:  SetUpVariantLong (vaResult, tm.tmCharSet);          break;
    case 21:  SetUpVariantString (vaResult, rString);             break;

    default:
      vaResult.vt = VT_NULL;
      break;

    } // end of switch

  pdc->SelectObject(oldFont);
  }  // end of CMiniWindow::FontInfo

// return list of fonts we installed
void CMiniWindow::FontList (VARIANT & vaResult)
  {
  COleSafeArray sa;   // for array list

  long iCount = 0;

  // put the arrays into the array
  if (!m_Fonts.empty ())    // cannot create empty dimension
    {
    sa.CreateOneDim (VT_VARIANT, m_Fonts.size ());

    for (FontMapIterator it = m_Fonts.begin ();
         it != m_Fonts.end ();
         it++)
           {
            // the array must be a bloody array of variants, or VBscript kicks up
            COleVariant v (it->first.c_str ());
            sa.PutElement (&iCount, &v);
            iCount++;
           }

    } // end of having at least one

  vaResult = sa.Detach ();


  } // end of  CMiniWindow::FontList



// helper function to calculate the required number of UTF-16 code units
static long CalculateUTF16length (LPCTSTR Text, size_t length)
  {

  int erroroffset;
  int iBad = _pcre_valid_utf ((unsigned char  *) Text, length, &erroroffset);
  if (iBad > 0)
    return -1;

  return MultiByteToWideChar (CP_UTF8, 0, Text, length, NULL, 0);

  }   // end of CalculateUTF16length

// output text, ordinary or UTF8 - returns length of text
long CMiniWindow::Text (LPCTSTR FontId,  // which previously-created font
                        LPCTSTR Text,    // what to say
                        long Left, long Top, long Right, long Bottom, // where to say it
                        long Colour,       // colour to show it in
                        BOOL Unicode)      // true if UTF8

  {
  FontMapIterator it = m_Fonts.find (FontId);

  if (it == m_Fonts.end ())
    return -2;

  size_t length = strlen (Text);
  long utf16_length = 0;

  // give up if no text
  if (length <= 0)
    return 0;

  // quick sanity check on our UTF8 stuff
  if (Unicode)
    {
    utf16_length = CalculateUTF16length (Text, length);
    if (utf16_length <= 0)
      return -3;    // ohno!
    }


  CFont* oldFont = pdc->SelectObject(it->second);    // select in the requested font

  CSize textsize;

  pdc->SetTextColor (Colour);
  pdc->SetBkMode (TRANSPARENT);

  if (Unicode)
    {
    vector<WCHAR> v (utf16_length);    // get correct size vector
    int iUnicodeCharacters = MultiByteToWideChar (CP_UTF8, 0,
                              Text, length,            // input
                              &v [0], utf16_length);    // output

    ExtTextOutW (pdc->m_hDC, Left, Top, ETO_CLIPPED, CRect (Left, Top, FixRight (Right), FixBottom (Bottom) ),
                  &v [0], iUnicodeCharacters, NULL);

    // now calculate width of Unicode pixels
    GetTextExtentPoint32W(
        pdc->m_hDC,             // handle to device context
        &v [0],               // pointer to text string
        iUnicodeCharacters,   // number of characters in string
        &textsize             // pointer to structure for string size
      );

    }
  else
    {
    pdc->ExtTextOut (Left, Top, ETO_CLIPPED, CRect (Left, Top, FixRight (Right), FixBottom (Bottom)),
                  Text, length, NULL);
    textsize = pdc->GetTextExtent (Text, length);

    }


  pdc->SelectObject(oldFont);

  return min (textsize.cx, FixRight (Right) - Left);  // if clipped, length is width of rectangle
  } // end of CMiniWindow::Text


// measure text, ordinary or UTF8
long CMiniWindow::TextWidth (LPCTSTR FontId,  // which previously-created font
                             LPCTSTR Text,    // what to measure
                             BOOL Unicode)    // true if UTF8
  {

  FontMapIterator it = m_Fonts.find (FontId);

  if (it == m_Fonts.end ())
    return -2;

  size_t length = strlen (Text);
  long utf16_length = 0;

  // give up if no text
  if (length <= 0)
    return 0;

  // quick sanity check on our UTF8 stuff
  if (Unicode)
    {
    utf16_length = CalculateUTF16length (Text, length);
    if (utf16_length <= 0)
      return -3;    // ohno!
    }


  CFont* oldFont = pdc->SelectObject(it->second);    // select in the requested font

  CSize textsize;

  if (Unicode)
    {
    vector<WCHAR> v (utf16_length);    // get correct size vector
    int iUnicodeCharacters = MultiByteToWideChar (CP_UTF8, 0,
                              Text, length,           // input
                              &v [0], utf16_length);   // output

    // now calculate width of Unicode pixels
    GetTextExtentPoint32W(
        pdc->m_hDC,           // handle to device context
        &v [0],             // pointer to text string
        iUnicodeCharacters, // number of characters in string
        &textsize           // pointer to structure for string size
      );

    }
  else
    textsize = pdc->GetTextExtent (Text, length);


  pdc->SelectObject(oldFont);

  return textsize.cx;

  }  // end of  CMiniWindow::TextWidth


// draws a straight line
long CMiniWindow::Line (long x1, long y1, long x2, long y2,
                        long PenColour, long PenStyle, long PenWidth)
  {

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  pdc->SetBkMode (TRANSPARENT);

  // create requested pen
  MiniWindowPen pen (m_DrawingResources.get (), PenColour, PenStyle, PenWidth);
  if (!pen.Get ()) return eBadParameter;

  MiniWindowSelectedObject selectedPen (pdc, pen.Get ());
  if (!selectedPen.Valid ()) return eBadParameter;

  pdc->MoveTo (x1, y1);
  pdc->LineTo (x2, y2);    // note NOT: FixRight (x2), FixBottom (y2) (as at version 4.38)


  return eOK;

  }  // end of CMiniWindow::Line


// draws an arc
long CMiniWindow::Arc (long Left, long Top, long Right, long Bottom,
                        long x1, long y1,
                        long x2, long y2,
                        long PenColour, long PenStyle, long PenWidth)
  {

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  pdc->SetBkMode (TRANSPARENT);

  // create requested pen
  MiniWindowPen pen (m_DrawingResources.get (), PenColour, PenStyle, PenWidth);
  if (!pen.Get ()) return eBadParameter;

  MiniWindowSelectedObject selectedPen (pdc, pen.Get ());
  if (!selectedPen.Valid ()) return eBadParameter;

  pdc->Arc(Left, Top, FixRight (Right), FixBottom (Bottom),
          x1, y1, // from
          FixRight (x2), FixBottom (y2)); // to


  return eOK;

  }  // end of CMiniWindow::Arc


// return info about the window
void CMiniWindow::Info (long InfoType, VARIANT & vaResult)
  {

  switch (InfoType)
    {
    case  1:  SetUpVariantLong    (vaResult, m_Location.x);        break; // left
    case  2:  SetUpVariantLong    (vaResult, m_Location.y);        break; // top
    case  3:  SetUpVariantLong    (vaResult, m_iWidth);            break; // width
    case  4:  SetUpVariantLong    (vaResult, m_iHeight);           break; // height
    case  5:  SetUpVariantBool    (vaResult, m_bShow);             break; // show flag
    case  6:  SetUpVariantBool    (vaResult, m_bTemporarilyHide);  break; // is it hidden right now?
    case  7:  SetUpVariantLong    (vaResult, m_iPosition);         break; // layout mode
    case  8:  SetUpVariantLong    (vaResult, m_iFlags);            break; // flags
    case  9:  SetUpVariantLong    (vaResult, m_iBackgroundColour); break; // background colour

    case 10:  SetUpVariantLong    (vaResult, m_rect.left);         break; // where it is right now
    case 11:  SetUpVariantLong    (vaResult, m_rect.top);          break; //       "
    case 12:  SetUpVariantLong    (vaResult, m_rect.right);        break; //       "
    case 13:  SetUpVariantLong    (vaResult, m_rect.bottom);       break; //       "

    case 14:  SetUpVariantLong    (vaResult, m_last_mouseposition.x);  break; //  last mouse x position
    case 15:  SetUpVariantLong    (vaResult, m_last_mouseposition.y);  break; //  last mouse y position

    case 16:  SetUpVariantLong    (vaResult, m_last_mouse_update);  break;    //  last position update count

    case 17:  SetUpVariantLong    (vaResult, m_client_mouseposition.x); break; //  last client mouse x position
    case 18:  SetUpVariantLong    (vaResult, m_client_mouseposition.y); break; //  last client mouse y position

    case 19:  SetUpVariantString  (vaResult, m_sMouseOverHotspot.c_str ()); break; // mouse-over hotspot
    case 20:  SetUpVariantString  (vaResult, m_sMouseDownHotspot.c_str ()); break; // mouse-down hotspot
    case 21:  SetUpVariantDate    (vaResult, COleDateTime (m_tDateInstalled.GetTime ()));  break;

    case 22:  SetUpVariantLong    (vaResult, m_ZOrder);  break; //  Z-Order
    case 23:  SetUpVariantString  (vaResult, m_sCreatingPlugin.c_str ()); break; // creating plugin ID

    default:
      vaResult.vt = VT_NULL;
      break;

    } // end of switch

  }  // end of CMiniWindow::Info


// loads an image file, ready for drawing into window
long CMiniWindow::LoadImage (LPCTSTR ImageId, LPCTSTR FileName)
  {
  CString strFileName = FileName;

  strFileName.TrimLeft ();
  strFileName.TrimRight ();

  // no file name means get rid of image
  if (strFileName.IsEmpty ())
    {
    m_Images.erase (ImageId);
    return eOK;
    }

  HBITMAP hBmp;
  const long result = ::LoadImageFile (FileName, hBmp);
  if (result != eOK)
    return result;
  return StoreLoadedBitmap (m_Images, ImageId, hBmp);

  } // end of CMiniWindow::LoadImage


// loads an image from memory, ready for drawing into window
long CMiniWindow::LoadImageMemory(LPCTSTR ImageId,
                                 unsigned char * Buffer,
                                 const size_t Length,
                                 const bool swap_alpha_endianness)
  {
  HBITMAP hbmp;

  const long result = ::LoadImageMemory (Buffer, Length, hbmp, swap_alpha_endianness);

  if (result != eOK)
    return result;

  return StoreLoadedBitmap (m_Images, ImageId, hbmp);

  } // end of CMiniWindow::LoadImageMemory


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


// saves an image file
long CMiniWindow::Write (LPCTSTR FileName)
  {

  CString strFileName = FileName;

  strFileName.TrimLeft ();
  strFileName.TrimRight ();

  if (strFileName.IsEmpty ())
    return eNoNameSpecified;

  // have to be long enough to have x.bmp
  if (strFileName.GetLength () < 5)
    return eBadParameter;

  // must be .bmp or .png file
  if (strFileName.Right (4).CompareNoCase (".bmp") != 0 &&
      strFileName.Right (4).CompareNoCase (".png") != 0)
    return eBadParameter;

  // get window data
  CDC gDC;
  gDC.CreateCompatibleDC(&(*pdc));
  CBitmap gbmp;

  BITMAPINFO bmi;
  ZeroMemory (&bmi, sizeof bmi);

  bmi.bmiHeader.biSize = sizeof bmi;
  bmi.bmiHeader.biWidth =          m_iWidth;
  bmi.bmiHeader.biHeight =         m_iHeight;
  bmi.bmiHeader.biPlanes =         1;
  bmi.bmiHeader.biBitCount =       24;
  bmi.bmiHeader.biCompression =    BI_RGB;
  bmi.bmiHeader.biSizeImage =      m_iHeight * BytesPerLine (m_iWidth, 24);

  unsigned char * pA = NULL;

  HBITMAP hbmG = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void**) &pA, NULL, 0);

  if (!hbmG)
    return eUnableToLoadImage;

  HBITMAP hOldAbmp = (HBITMAP) SelectObject(gDC.m_hDC, hbmG);

  // copy image from miniwindow to bitmap
  gDC.BitBlt (0, 0, m_iWidth, m_iHeight, &(*pdc), 0, 0, SRCCOPY);

  long iReturn = eOK;

    // must be .bmp or .png file
  if (strFileName.Right (4).CompareNoCase (".png") == 0)
    iReturn = WritePng (FileName, &bmi, pA);
  else
    {     // write BMP file

    // create requested file
    CFile file;
    if( !file.Open (FileName, CFile::modeWrite | CFile::modeCreate))
      iReturn = eCouldNotOpenFile;
    else
      {

    BITMAPFILEHEADER  hdr;
    ZeroMemory (&hdr, sizeof hdr);

    // Fill in the fields of the file header
    hdr.bfType    = ((WORD) ('M' << 8) | 'B');  // is always "BM"
    hdr.bfOffBits = sizeof hdr  + sizeof bmi.bmiHeader;
    hdr.bfSize    =  hdr.bfOffBits + bmi.bmiHeader.biSizeImage;

    try
      {
      // Write the file header
      file.Write( &hdr, sizeof hdr);

      // Write the bitmap info header
      file.Write( &bmi.bmiHeader, sizeof bmi.bmiHeader);

      // Write the bits
      file.Write( pA,  bmi.bmiHeader.biSizeImage);
      }
    catch (CFileException * e)
      {
      e->Delete ();
      iReturn = eLogFileBadWrite;
      } // end of catching a file exception
      }

    }   // end of writing BMP file

  // finished with bitmap
  SelectObject(gDC.m_hDC, hOldAbmp);
  DeleteObject (hbmG);

  return iReturn;

  } // end of CMiniWindow::Write

long CMiniWindow::WritePng (LPCTSTR FileName, const BITMAPINFO * bmi, unsigned char * pData)
  {
  // open file
  FILE *fp = fopen(FileName, "wb");
  if (!fp)
      return (eCouldNotOpenFile);

  // create PNG structure
  png_structp png_ptr = png_create_write_struct
       (PNG_LIBPNG_VER_STRING, NULL,
        user_error_fn, user_warning_fn);

  if (!png_ptr)
    {
    fclose (fp);
    return eLogFileBadWrite;
    }

  // create info structure
  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
    {
    png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
    fclose (fp);
    return eLogFileBadWrite;
    }


#pragma warning (push)
#pragma warning (disable : 4611)  // interaction between '_setjmp' and C++ object destruction is non-portable

  // if png fails it will longjmp back to here, so we destroy the structure,
  // close the file, and wrap up
  if (setjmp(png_jmpbuf(png_ptr)))
    {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose (fp);
    return eLogFileBadWrite;
    }

#pragma warning (pop)

  // initialize IO
  png_init_io (png_ptr, fp);

  // tell PNG the file format
  png_set_IHDR  (png_ptr,
                 info_ptr,
                 bmi->bmiHeader.biWidth,
                 bmi->bmiHeader.biHeight,
                 8,        // bits per pixel
                 PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);

  // set up pointer to each row of data
  vector<png_byte *> row_pointers;
  row_pointers.resize (bmi->bmiHeader.biHeight);

  long bpl = BytesPerLine (bmi->bmiHeader.biWidth, 24);

  long row;
  unsigned char * p = pData;

  // have to reverse row order
  png_uint_32 iHeight = png_get_image_height (png_ptr, info_ptr);
  for (row = 0; row < iHeight; row++, p += bpl)
     row_pointers [bmi->bmiHeader.biHeight - row - 1] = p;

  // tell png where our pixel data is
  png_set_rows (png_ptr, info_ptr, &row_pointers [0]);

  // write out the image data
  png_write_png (png_ptr, info_ptr, PNG_TRANSFORM_BGR, NULL);

  // wrap up
  png_write_end (png_ptr, info_ptr);

  // done with data
  png_destroy_write_struct (&png_ptr, &info_ptr);

  // done with file
  fclose (fp);

  return eOK;
  } // end of CMiniWindow::WritePng


/*

  for monitors with 256 colours or less, consider:

HDRAWDIB hdd = DrawDibOpen();
DrawDibDraw(hdd, hPaintDC,0,0,BITMAP_WIDTH,BITMAP_HEIGHT,&m_BitmapInfo,m_pBits,0,0,BITMAP_WIDTH,BITMAP_HEIGHT,DDF_HALFTONE);
DrawDibClose(hdd);

  */


// draw a previously-loaded image into the window
long CMiniWindow::DrawImage(LPCTSTR ImageId,
               long Left, long Top, long Right, long Bottom,
               short Mode,
               long SrcLeft, long SrcTop, long SrcRight, long SrcBottom)
  {

  ImageMapIterator it = m_Images.find (ImageId);
  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * bitmap = it->second.get ();
  BITMAP bi = {};
  if (!bitmap->GetBitmap (&bi))
    return eUnableToLoadImage;

  // Zero means the image edge. Negative bounds are offsets from the edge.
  if (SrcRight <= 0) SrcRight += bi.bmWidth;
  if (SrcBottom <= 0) SrcBottom += bi.bmHeight;

  const __int64 source_width = static_cast<__int64> (SrcRight) - SrcLeft;
  const __int64 source_height = static_cast<__int64> (SrcBottom) - SrcTop;
  if (source_width <= 0 || source_height <= 0)
    return eOK;
  if (source_width > INT_MAX || source_height > INT_MAX || Mode < 1 || Mode > 3)
    return eBadParameter;
  const int w = static_cast<int> (source_width), h = static_cast<int> (source_height);

  CDC source;
  if (!source.CreateCompatibleDC (pdc))
    return eUnableToLoadImage;
  MiniWindowSelectedObject selected (&source, bitmap);
  if (!selected.Valid () || !pdc->SetBkMode (TRANSPARENT)
      || !pdc->SetStretchBltMode (HALFTONE) || !::SetBrushOrgEx (pdc->GetSafeHdc (), 0, 0, NULL))
    return eUnableToLoadImage;

  if (Mode == 1)
    return pdc->BitBlt (Left, Top, w, h, &source, SrcLeft, SrcTop, SRCCOPY)
      ? eOK : eUnableToLoadImage;

  if (Mode == 2)
    {
    const __int64 width = static_cast<__int64> (FixRight (Right)) - Left;
    const __int64 height = static_cast<__int64> (FixBottom (Bottom)) - Top;
    if (width == 0 || height == 0)
      return eOK;
    if (width < INT_MIN || width > INT_MAX || height < INT_MIN || height > INT_MAX)
      return eBadParameter;
    return pdc->StretchBlt (Left, Top, static_cast<int> (width), static_cast<int> (height), &source,
                           SrcLeft, SrcTop, w, h, SRCCOPY) ? eOK : eUnableToLoadImage;
    }

  CDC mask;
  CBitmap mask_bitmap;
  if (!mask.CreateCompatibleDC (pdc) || !mask_bitmap.CreateBitmap (w, h, 1, 1, NULL))
    return eUnableToLoadImage;
  MiniWindowSelectedObject selected_mask (&mask, &mask_bitmap);
  // Use the top-left pixel of the original image as the colour key for any crop.
  const COLORREF key = ::GetPixel (source.GetSafeHdc (), 0, 0);
  if (!selected_mask.Valid () || key == CLR_INVALID)
    return eUnableToLoadImage;
  const COLORREF old_source_background = source.SetBkColor (key);
  if (old_source_background == CLR_INVALID)
    return eUnableToLoadImage;
  const bool copied_mask = mask.BitBlt (0, 0, w, h, &source, SrcLeft, SrcTop, SRCCOPY) != 0;
  if (source.SetBkColor (old_source_background) == CLR_INVALID || !copied_mask)
    return eUnableToLoadImage;

  const COLORREF old_background = pdc->SetBkColor (RGB (255, 255, 255));
  const COLORREF old_text = pdc->SetTextColor (RGB (0, 0, 0));
  const bool copied = old_background != CLR_INVALID && old_text != CLR_INVALID
    && pdc->BitBlt (Left, Top, w, h, &source, SrcLeft, SrcTop, SRCINVERT)
    && pdc->BitBlt (Left, Top, w, h, &mask, 0, 0, SRCAND)
    && pdc->BitBlt (Left, Top, w, h, &source, SrcLeft, SrcTop, SRCINVERT);

  const bool restored_background = old_background != CLR_INVALID && pdc->SetBkColor (old_background) != CLR_INVALID;
  const bool restored_text = old_text != CLR_INVALID && pdc->SetTextColor (old_text) != CLR_INVALID;
  return copied && restored_background && restored_text ? eOK : eUnableToLoadImage;
  }  // end of CMiniWindow::DrawImage


// return list of images
void CMiniWindow::ImageList(VARIANT & vaResult)
  {
  COleSafeArray sa;   // for array list

  long iCount = 0;

  // put the arrays into the array
  if (!m_Images.empty ())    // cannot create empty dimension
    {
    sa.CreateOneDim (VT_VARIANT, m_Images.size ());

    for (ImageMapIterator it = m_Images.begin ();
         it != m_Images.end ();
         it++)
           {
            // the array must be a bloody array of variants, or VBscript kicks up
            COleVariant v (it->first.c_str ());
            sa.PutElement (&iCount, &v);
            iCount++;
           }

    } // end of having at least one

  vaResult = sa.Detach ();

  }   // end of CMiniWindow::ImageList



// return info about the selected image
void CMiniWindow::ImageInfo (LPCTSTR ImageId, long InfoType, VARIANT & vaResult)
  {
  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return;   // no such Image

  CBitmap * bitmap = it->second.get ();

  BITMAP  bi;
  bitmap->GetBitmap(&bi);

  switch (InfoType)
    {

    case  1:  SetUpVariantLong (vaResult, bi.bmType);      break;
    case  2:  SetUpVariantLong (vaResult, bi.bmWidth);     break;
    case  3:  SetUpVariantLong (vaResult, bi.bmHeight);    break;
    case  4:  SetUpVariantLong (vaResult, bi.bmWidthBytes);break;
    case  5:  SetUpVariantLong (vaResult, bi.bmPlanes);    break;
    case  6:  SetUpVariantLong (vaResult, bi.bmBitsPixel); break;

     default:
      vaResult.vt = VT_NULL;
      break;

    } // end of switch

  }  // end of CMiniWindow::ImageInfo


// draw bezier curves
long CMiniWindow::Bezier(LPCTSTR Points, long PenColour, long PenStyle, long PenWidth)
  {

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  vector<string> v;

  StringToVector (Points, v, ",");

  int iCount = v.size ();

  // must have at least a start point (2 items), plus one extra (3 points)
  if (iCount < 8)
    return eInvalidNumberOfPoints;

  // and has to be 2 more than 6 items  (1 more than 3n points)
  if ((iCount % 6) != 2)
    return eInvalidNumberOfPoints;

  iCount = iCount / 2;  // number of points

  vector<POINT> points (iCount);

  int iCurrent = 0;

  for (vector<string>::const_iterator i = v.begin (); i != v.end (); i++)
    {
    if (!IsStringNumber (*i, true))
      return eInvalidPoint;
    points [iCurrent].x = atol (i->c_str ());
    i++;  // we know this is safe becaue of earlier check (we must have pairs of numbers)
    if (!IsStringNumber (*i, true))
      return eInvalidPoint;
    points [iCurrent].y = atol (i->c_str ());
    iCurrent++;  // onto next point
    }

  pdc->SetBkMode (TRANSPARENT);

  // create requested pen
  MiniWindowPen pen (m_DrawingResources.get (), PenColour, PenStyle, PenWidth);
  if (!pen.Get ()) return eBadParameter;
  MiniWindowSelectedObject selectedPen (pdc, pen.Get ());
  if (!selectedPen.Valid ()) return eBadParameter;

  pdc->PolyBezier(&points [0], iCount);


  return eOK;


  }   // end of CMiniWindow::Bezier


// draw a polygon (straight lines)
long CMiniWindow::Polygon(LPCTSTR Points,
                         long PenColour, short PenStyle, long PenWidth,
                         long BrushColour, long BrushStyle,
                         BOOL Close,
                         BOOL Winding)
  {

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  // validate and create requested bruch
  MiniWindowBrush br (m_DrawingResources.get (), BrushStyle, PenColour, BrushColour);
  if (br.Status () != eOK) return br.Status ();

  vector<string> v;

  StringToVector (Points, v, ",");

  int iCount = v.size ();

  // must have at least a start point (2 items), plus one extra (1 point)
  if (iCount < 4)
    return eInvalidNumberOfPoints;

  // it has to be divisible by 2
  if ((iCount % 2) != 0)
    return eInvalidNumberOfPoints;

  iCount = iCount / 2;  // number of points

  vector<POINT> points (iCount);

  int iCurrent = 0;

  for (vector<string>::const_iterator i = v.begin (); i != v.end (); i++)
    {
    if (!IsStringNumber (*i, true))
      return eInvalidPoint;
    points [iCurrent].x = atol (i->c_str ());
    i++;  // we know this is safe becaue of earlier check (we must have pairs of numbers)
    if (!IsStringNumber (*i, true))
      return eInvalidPoint;
    points [iCurrent].y = atol (i->c_str ());
    iCurrent++;  // onto next point
    }

  // create requested pen
  MiniWindowPen pen (m_DrawingResources.get (), PenColour, PenStyle, PenWidth);
  if (!pen.Get ()) return eBadParameter;

  // select pen and brush into device context
  MiniWindowSelectedObject selectedPen (pdc, pen.Get ());
  if (!selectedPen.Valid ()) return eBadParameter;
  MiniWindowSelectedObject selectedBrush (pdc, br.Get ());
  if (!selectedBrush.Valid ()) return eBadParameter;

  pdc->SetPolyFillMode (Winding ? WINDING : ALTERNATE);

  if (BrushStyle > 1 && BrushStyle <= 7)
    {
    pdc->SetBkColor (BrushColour);      // for hatched brushes this is the background colour
    }
  else
  if (BrushStyle > 7)  // pattern brushes
    {
    pdc->SetTextColor (BrushColour);      // for patterned brushes
    pdc->SetBkColor (PenColour);      // for hatched brushes and patterned brushes
    }

  if (BrushColour != -1)
    pdc->SetBkMode (OPAQUE);
  else
    pdc->SetBkMode (TRANSPARENT);

  if (Close)
    pdc->Polygon(&points [0], iCount);
  else
    pdc->Polyline(&points [0], iCount);


  return eOK;

  }   // end of CMiniWindow::Polygon

// reposition window
long CMiniWindow::Position(long Left, long Top,
                           short Position,
                           long Flags)
  {
  m_Location.x           = Left     ;
  m_Location.y           = Top      ;
  m_iPosition            = Position ;
  m_iFlags               = Flags    ;

  return eOK;

  } // end of  CMiniWindow::Position


/*

  Cursor values:

  0:  arrow
  1:  hand
  2:  I-beam
  3:  + symbol
  4:  wait (hour-glass)
  5:  up arrow
  6:  arrow nw-se
  7:  arrow ne-sw
  8:  arrow e-w
  9:  arrow n-s
 10:  arrow - all ways
 11:  (X) no, no, I won't do that, but ...
 12:  help  (? symbol)


  Flags:

  0x01 : report all mouseovers, not just when first entering the hotspot

  */

// Check ASCII callback names without a temporary CString. Use the existing
// label check for non-ASCII characters so locale handling stays the same.
static bool InvalidHotspotCallback (LPCTSTR label)
  {
  if (!*label) return false;
  const unsigned char * p = reinterpret_cast<const unsigned char *> (label);
  if (*p >= 128) return CheckLabel (label, true) != FALSE;
  if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')))
    return true;
  for (++p; *p; ++p)
    {
    if (*p >= 128) return CheckLabel (label, true) != FALSE;
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9') || *p == '_' || *p == '.'))
      return true;
    }
  return false;
  }

// add a hotspot for handling mouse-over, mouse up/down events
long CMiniWindow::AddHotspot(CMUSHclientDoc * pDoc,
                             LPCTSTR HotspotId,
                             LPCTSTR sPluginID,
                             long Left, long Top, long Right, long Bottom,
                             LPCTSTR MouseOver,
                             LPCTSTR CancelMouseOver,
                             LPCTSTR MouseDown,
                             LPCTSTR CancelMouseDown,
                             LPCTSTR MouseUp,
                             LPCTSTR TooltipText,
                             long Cursor,
                             long Flags)
  {

  if (InvalidHotspotCallback (MouseOver))
    return eInvalidObjectLabel;
  if (InvalidHotspotCallback (CancelMouseOver))
    return eInvalidObjectLabel;
  if (InvalidHotspotCallback (MouseDown))
    return eInvalidObjectLabel;
  if (InvalidHotspotCallback (CancelMouseDown))
    return eInvalidObjectLabel;
  if (InvalidHotspotCallback (MouseUp))
    return eInvalidObjectLabel;

  // can't switch plugins here :)
  if (!m_sCallbackPlugin.empty () && m_sCallbackPlugin != sPluginID)
    return eHotspotPluginChanged;

  // Own the callback ID only when this window first receives plugin hotspots.
  string replacementPluginID;
  if (m_sCallbackPlugin.empty ())
    replacementPluginID = sPluginID;

  std::unique_ptr<CHotspot> pHotspot (new CHotspot);

  pHotspot->m_sMouseOver        = MouseOver;
  pHotspot->m_sCancelMouseOver  = CancelMouseOver;
  pHotspot->m_sMouseDown        = MouseDown;
  pHotspot->m_sCancelMouseDown  = CancelMouseDown;
  pHotspot->m_sMouseUp          = MouseUp;
  pHotspot->m_sTooltipText      = TooltipText;
  pHotspot->m_Cursor            = Cursor;
  pHotspot->m_Flags             = Flags;

  // if not in a plugin, look in main world for hotspot callbacks, and remember the dispatch ID
  if (!*sPluginID)
    {
    // Resolving a Lua name may run __index or a GC finalizer. As with mouse
    // callbacks, do not allow WindowDelete to destroy this active window.
    CBoolStateGuard executingGuard (m_bExecutingScript, true);
    CString strErrorMessage;

    pHotspot->m_dispid_MouseOver        = pDoc->GetProcedureDispid (MouseOver, "mouse over", "", strErrorMessage);
    pHotspot->m_dispid_CancelMouseOver  = pDoc->GetProcedureDispid (CancelMouseOver, "cancel mouse over", "", strErrorMessage);
    pHotspot->m_dispid_MouseDown        = pDoc->GetProcedureDispid (MouseDown, "mouse down", "", strErrorMessage);
    pHotspot->m_dispid_CancelMouseDown  = pDoc->GetProcedureDispid (CancelMouseDown, "cancel mouse down", "", strErrorMessage);
    pHotspot->m_dispid_MouseUp          = pDoc->GetProcedureDispid (MouseUp, "mouse up", "", strErrorMessage);
    }

  // Lookup may have deleted/replaced hotspots, recreated the window, or let
  // another plugin install its callbacks. Borrow the old hotspot only now.
  if (!m_sCallbackPlugin.empty () && m_sCallbackPlugin != sPluginID)
    return eHotspotPluginChanged;

  pHotspot->m_rect = CRect (Left, Top, FixRight (Right), FixBottom (Bottom));
  string hotspotId (HotspotId);
  HotspotMapIterator it = m_Hotspots.lower_bound (hotspotId);
  CHotspot * pOldHotspot = NULL;
  if (it == m_Hotspots.end () || it->first != hotspotId)
    m_Hotspots.emplace_hint (it, std::move (hotspotId), pHotspot.get ());
  else
    {
    pOldHotspot = it->second;
    it->second = pHotspot.get ();
    }
  pHotspot.release ();
  delete pOldHotspot;

  if (!replacementPluginID.empty ())
    m_sCallbackPlugin.swap (replacementPluginID);

  if (m_sMouseOverHotspot == HotspotId)
    m_sMouseOverHotspot.erase ();

  if (m_sMouseDownHotspot == HotspotId)
    m_sMouseDownHotspot.erase ();

  return eOK;
  }    // end of CMiniWindow::AddHotspot

// remove a previously-installed hotspot
long CMiniWindow::DeleteHotspot(LPCTSTR HotspotId)
  {

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it == m_Hotspots.end ())
    return eHotspotNotInstalled;   // no such hotspot

  delete it->second;

  m_Hotspots.erase (it);

  if (m_sMouseOverHotspot == HotspotId)
    m_sMouseOverHotspot.erase ();

  if (m_sMouseDownHotspot == HotspotId)
    m_sMouseDownHotspot.erase ();

  if (m_Hotspots.empty ())
    {
    m_sCallbackPlugin.erase ();
    }

  return eOK;
  }    // end of CMiniWindow::DeleteHotspot

// return list of all hotspots in this miniwindow
void CMiniWindow::HotspotList(VARIANT & vaResult)
  {
  COleSafeArray sa;   // for array list

  long iCount = 0;

  // put the arrays into the array
  if (!m_Hotspots.empty ())    // cannot create empty dimension
    {
    sa.CreateOneDim (VT_VARIANT, m_Hotspots.size ());

    for (HotspotMapIterator it = m_Hotspots.begin ();
         it != m_Hotspots.end ();
         it++)
           {
            // the array must be a bloody array of variants, or VBscript kicks up
            COleVariant v (it->first.c_str ());
            sa.PutElement (&iCount, &v);
            iCount++;
           }

    } // end of having at least one

  vaResult = sa.Detach ();

  }    // end of CMiniWindow::HotspotList

// delete all hotspots
long CMiniWindow::DeleteAllHotspots()
  {
  // delete our hotspots
  for (HotspotMapIterator hit = m_Hotspots.begin ();
       hit != m_Hotspots.end ();
       hit++)
         delete hit->second;

  m_Hotspots.clear ();
  m_sMouseOverHotspot.erase ();
  m_sMouseDownHotspot.erase ();
  m_sCallbackPlugin.erase ();
  return eOK;
  }    // end of CMiniWindow::DeleteAllHotspots

// get information about a hotspot
void CMiniWindow::HotspotInfo(LPCTSTR HotspotId, long InfoType, VARIANT & vaResult)
  {

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it == m_Hotspots.end ())
    return;   // no such hotspot

  CHotspot * pHotspot = it->second;

  switch (InfoType)
    {

    case  1:  SetUpVariantLong   (vaResult, pHotspot->m_rect.left);                 break; // left
    case  2:  SetUpVariantLong   (vaResult, pHotspot->m_rect.top);                  break; // top
    case  3:  SetUpVariantLong   (vaResult, pHotspot->m_rect.right);                break; // right
    case  4:  SetUpVariantLong   (vaResult, pHotspot->m_rect.bottom);               break; // bottom
    case  5:  SetUpVariantString (vaResult, pHotspot->m_sMouseOver.c_str ());       break;
    case  6:  SetUpVariantString (vaResult, pHotspot->m_sCancelMouseOver.c_str ()); break;
    case  7:  SetUpVariantString (vaResult, pHotspot->m_sMouseDown.c_str ());       break;
    case  8:  SetUpVariantString (vaResult, pHotspot->m_sCancelMouseDown.c_str ()); break;
    case  9:  SetUpVariantString (vaResult, pHotspot->m_sMouseUp.c_str ());         break;
    case 10:  SetUpVariantString (vaResult, pHotspot->m_sTooltipText.c_str ());     break;
    case 11:  SetUpVariantLong   (vaResult, pHotspot->m_Cursor);                    break; // cursor code
    case 12:  SetUpVariantLong   (vaResult, pHotspot->m_Flags);                     break; // flags
    case 13:  SetUpVariantString (vaResult, pHotspot->m_sMoveCallback.c_str ());    break; // drag-and-drop move callback
    case 14:  SetUpVariantString (vaResult, pHotspot->m_sReleaseCallback.c_str ()); break; // drag-and-drop release callback
    case 15:  SetUpVariantLong   (vaResult, pHotspot->m_DragFlags);                 break; // drag-and-drop flags

    default:
      vaResult.vt = VT_NULL;
      break;

    } // end of switch

  }    // end of CMiniWindow::HotspotInfo


long CMiniWindow::ImageOp(short Action,
                          long Left, long Top, long Right, long Bottom,
                          long PenColour, long PenStyle, long PenWidth,
                          long BrushColour, LPCTSTR ImageId,
                          long EllipseWidth, long EllipseHeight)
  {

  long iResult = eUnknownOption;

  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * bitmap = it->second.get ();

  pdc->SetBkMode (OPAQUE);

  // for monochrome bitmaps, 1-bits will come out in pen colour, and 0-bits will come out in background colour
  pdc->SetTextColor (BrushColour);  // for patterned brushes
  pdc->SetBkColor (PenColour);      // for hatched brushes and patterned brushes

  if (ValidatePenStyle (PenStyle, PenWidth))
    return ePenStyleNotValid;

  CBrush br;

  br.CreatePatternBrush (bitmap);

  // create requested pen
  MiniWindowPen pen (m_DrawingResources.get (), PenColour, PenStyle, PenWidth);
  if (!pen.Get ()) return eBadParameter;

  // select into DC
  MiniWindowSelectedObject selectedPen (pdc, pen.Get ());
  if (!selectedPen.Valid ()) return eBadParameter;
  MiniWindowSelectedObject selectedBrush (pdc, &br);
  if (!selectedBrush.Valid ()) return eBadParameter;

  switch (Action)

    {
    case 1:       // ellipse
      {
      pdc->Ellipse (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)));
      iResult = eOK;
      break;
      }

    case 2:       // rectangle
      {
      pdc->Rectangle (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)));
      iResult = eOK;
      break;
      }

    case 3:       // round rectangle
      {
      pdc->RoundRect (CRect (Left, Top, FixRight (Right), FixBottom (Bottom)), CPoint (EllipseWidth, EllipseHeight));
      iResult = eOK;
      break;
      }


    } // end of switch


  return iResult;


  }   // end of  CMiniWindow::ImageOp

long CMiniWindow::CreateImage(LPCTSTR ImageId, long Row1, long Row2, long Row3, long Row4, long Row5, long Row6, long Row7, long Row8)
  {

  std::unique_ptr<MiniWindowImage> pImage (new MiniWindowImage);

  // bottom row comes first in a bitmap
  WORD    wBits[8] = { (WORD) Row8, (WORD) Row7, (WORD) Row6, (WORD) Row5,
                       (WORD) Row4, (WORD) Row3, (WORD) Row2, (WORD) Row1 };

  if (!pImage->CreateBitmap (8, 8, 1, 1, wBits))
    AfxThrowResourceException ();

  m_Images [ImageId] = std::move (pImage);
  return eOK;

  }  // end of CMiniWindow::CreateImage

// see: http://www.nathanm.com/photoshop-blending-math/
// and: http://www.pegtop.net/delphi/articles/blendmodes/

#define Blend_It(mode, name, Op) \
   case mode: \
      if (Opacity < 1.0) \
        for (i = 0; i < count; i++) \
          pB [i]     = Blend_Opacity (pA [i], pB [i], Op, Opacity);\
      else\
        for (i = 0; i < count; i ++)\
          pB [i]      = Op (pA [i], pB [i]);\
   break;

// we have to do this a row at a time, because there might be a filler at the end of each row
// and thus, a discontinuity which throws out the r/g/b sequence
#define Colour_Op(mode, name, fR, fG, fB) \
   case mode: \
     {  \
     for (row = 0; row < iHeight; row++)\
      {\
      const size_t base = static_cast<size_t> (row) * perline;\
      unsigned char rA, gA, bA, rB, gB, bB; \
      if (Opacity < 1.0) \
        for (i = 0; i < perline - 2; ) \
          {\
          bA = pA [base + i];     \
          gA = pA [base + i + 1]; \
          rA = pA [base + i + 2]; \
          bB = pB [base + i];     \
          gB = pB [base + i + 1]; \
          rB = pB [base + i + 2]; \
          pB [base + i] = Simple_Opacity (bB, fB, Opacity); \
          i++;\
          pB [base + i] = Simple_Opacity (gB, fG, Opacity); \
          i++;\
          pB [base + i] = Simple_Opacity (rB, fR, Opacity); \
          i++;\
          }\
      else\
        for (i = 0; i < perline - 2;)\
          {\
          bA = pA [base + i];     \
          gA = pA [base + i + 1]; \
          rA = pA [base + i + 2]; \
          bB = pB [base + i];     \
          gB = pB [base + i + 1]; \
          rB = pB [base + i + 2]; \
          pB [base + i] = fB;  \
          i++;\
          pB [base + i] = fG;  \
          i++;\
          pB [base + i] = fR;  \
          i++;\
          }\
        }\
       }\
   break;



// see also: CMUSHclientDoc::BlendPixel

class MiniWindowBlendTarget
  {
  HDC m_DC;
  HBITMAP m_Bitmap;
  HGDIOBJ m_Previous;

  public:
  unsigned char * pixels;
  int stride;

  MiniWindowBlendTarget ()
    : m_DC (NULL), m_Bitmap (NULL), m_Previous (NULL), pixels (NULL), stride (0)
    { }

  MiniWindowBlendTarget (const MiniWindowBlendTarget &) = delete;
  MiniWindowBlendTarget & operator= (const MiniWindowBlendTarget &) = delete;

  ~MiniWindowBlendTarget ()
    {
    if (m_Previous) ::SelectObject (m_DC, m_Previous);
    if (m_Bitmap) ::DeleteObject (m_Bitmap);
    if (m_DC) ::DeleteDC (m_DC);
    }

  bool CopyFrom (HDC source, int x, int y, int width, int height, bool top_down = true)
    {
    if (width <= 0 || height <= 0) return false;
    const unsigned __int64 bytes_per_row = (static_cast<unsigned __int64> (width) * 3 + 3) / 4 * 4;
    if (bytes_per_row > INT_MAX || bytes_per_row * height > MAXDWORD)
      return false;
    stride = static_cast<int> (bytes_per_row);
    m_DC = ::CreateCompatibleDC (source);
    if (!m_DC) return false;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = top_down ? -height : height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    void * data = NULL;
    m_Bitmap = ::CreateDIBSection (source, &info, DIB_RGB_COLORS, &data, NULL, 0);
    if (!m_Bitmap) return false;
    pixels = static_cast<unsigned char *> (data);
    m_Previous = ::SelectObject (m_DC, m_Bitmap);
    if (!m_Previous || m_Previous == HGDI_ERROR)
      {
      m_Previous = NULL;
      return false;
      }
    return ::BitBlt (m_DC, 0, 0, width, height, source, x, y, SRCCOPY) != 0;
    }

  bool CopyFrom (CDC & source, CBitmap * bitmap, int x, int y, int width, int height)
    {
    MiniWindowSelectedObject selected (&source, bitmap);
    return selected.Valid () && CopyFrom (source.GetSafeHdc (), x, y, width, height, false);
    }

  long Put (HDC target, int x, int y, int width, int height) const
    {
    return ::BitBlt (target, x, y, width, height, m_DC, 0, 0, SRCCOPY)
      ? eOK : eUnableToLoadImage;
    }
  };

// A 24-bit row with three padding bytes consumes one extra Dissolve sample.
static bool DissolveHasPaddingPixel (long width)
  {
  return width % 4 == 3;
  }

#define BLEND_DIRECT_CHANNEL(mode, name, operation) \
  case mode: \
    for (int c = 0; c < 3; ++c) \
      target [c] = FullOpacity ? operation (source [c], target [c]) \
        : Blend_Opacity (source [c], target [c], operation, opacity); \
    break;

#define BLEND_DIRECT_COLOUR(mode, name, red, green, blue) \
  case mode: \
    target [0] = FullOpacity ? (blue) : Simple_Opacity (bB, blue, opacity); \
    target [1] = FullOpacity ? (green) : Simple_Opacity (gB, green, opacity); \
    target [2] = FullOpacity ? (red) : Simple_Opacity (rB, red, opacity); \
    break;

template<int Mode, bool FullOpacity>
static void BlendDirectPixels (unsigned char * pixels, int stride,
    const MiniWindowAlphaImage & image, int left, int top, int width, int height,
    double opacity, const unsigned char * cos_table)
  {
  for (int y = 0; y < height; ++y)
    {
    // Dissolve assigns random values from the bottom row up.
    const int row = Mode == BlendDissolveMode ? height - 1 - y : y;
    const unsigned char * source = image.Row (top + row) + static_cast<size_t> (left) * 4;
    unsigned char * target = pixels + static_cast<size_t> (row) * stride;
    for (int x = 0; x < width; ++x, source += 4, target += 3)
      {
      const unsigned char bA = source [0], gA = source [1], rA = source [2];
      const unsigned char bB = target [0], gB = target [1], rB = target [2];
      switch (Mode)
        {
        MUSHCLIENT_BLEND_MODES (BLEND_DIRECT_CHANNEL, BLEND_DIRECT_CHANNEL, SKIP_BLEND_MODE, SKIP_BLEND_MODE)

        case BlendDissolveMode:
          if (genrand () < opacity)
            {
            target [0] = bA;
            target [1] = gA;
            target [2] = rA;
            }
          break;

        MUSHCLIENT_BLEND_MODES (SKIP_BLEND_MODE, SKIP_BLEND_MODE, BLEND_DIRECT_COLOUR, SKIP_BLEND_MODE)

        default:  // Keep rounding the HSL result at full opacity.
          {
          CColor a, b;
          a.SetRGB (rA, gA, bA);
          b.SetRGB (rB, gB, bB);
          if (Mode == BlendHueMode || Mode == BlendColourMode) b.SetHue (a.GetHue ());
          if (Mode == BlendSaturationMode || Mode == BlendColourMode) b.SetSaturation (a.GetSaturation ());
          if (Mode == BlendLuminanceMode) b.SetLuminance (a.GetLuminance ());
          if (Mode == BlendHslMode)
            {
            target [0] = Simple_Opacity (bB, a.GetLuminance () * 255, opacity);
            target [1] = Simple_Opacity (gB, a.GetSaturation () * 255, opacity);
            target [2] = Simple_Opacity (rB, (a.GetHue () / 360.0) * 255, opacity);
            }
          else
            {
            target [0] = Simple_Opacity (bB, b.GetBlue (), opacity);
            target [1] = Simple_Opacity (gB, b.GetGreen (), opacity);
            target [2] = Simple_Opacity (rB, b.GetRed (), opacity);
            }
          }
          break;
        }
      }
    if (Mode == BlendDissolveMode && DissolveHasPaddingPixel (width))
      genrand ();
    }
  }

#undef BLEND_DIRECT_CHANNEL
#undef BLEND_DIRECT_COLOUR

template<int Mode>
static void BlendDirectMode (MiniWindowBlendTarget & target, const MiniWindowAlphaImage & image,
                            int left, int top, int width, int height, double opacity,
                            const unsigned char * cos_table)
  {
  if (opacity < 1.0)
    BlendDirectPixels<Mode, false> (target.pixels, target.stride, image,
      left, top, width, height, opacity, cos_table);
  else
    BlendDirectPixels<Mode, true> (target.pixels, target.stride, image,
      left, top, width, height, opacity, cos_table);
  }

static void BlendDirectImage (MiniWindowBlendTarget & target, const MiniWindowAlphaImage & image,
                             int left, int top, int width, int height, int mode, double opacity,
                             const unsigned char * cos_table)
  {
#define BLEND_DIRECT_MODE(number, ...) \
    case number: BlendDirectMode<number> (target, image, left, top, width, height, opacity, cos_table); break;
  switch (mode)
    {
    MUSHCLIENT_BLEND_MODES (BLEND_DIRECT_MODE, BLEND_DIRECT_MODE, BLEND_DIRECT_MODE, BLEND_DIRECT_MODE)
    }
#undef BLEND_DIRECT_MODE
  }

template<bool FullOpacity>
static void MergeDirectPixels (MiniWindowBlendTarget & target, const MiniWindowAlphaImage & image,
                               const MiniWindowAlphaImage & mask, int left, int top,
                               int width, int height, int mode, double opacity)
  {
  const unsigned char * key = image.Row (top) + static_cast<size_t> (left) * 4;
  for (int y = 0; y < height; ++y)
    {
    const unsigned char * source = image.Row (top + y) + static_cast<size_t> (left) * 4;
    const unsigned char * alpha = mask.Row (top + y) + static_cast<size_t> (left) * 4;
    unsigned char * pixels = target.pixels + static_cast<size_t> (y) * target.stride;
    for (int x = 0; x < width; ++x, source += 4, alpha += 4, pixels += 3)
      {
      if (mode == 1 && source [0] == key [0] && source [1] == key [1] && source [2] == key [2])
        continue;
      for (int c = 0; c < 3; ++c)
        pixels [c] = MiniWindowAlphaImage::BlendChannel<FullOpacity>
          (source [c], pixels [c], alpha [c], opacity);
      }
    }
  }

long CMiniWindow::BlendImage(LPCTSTR ImageId,
                  long Left, long Top, long Right, long Bottom,
                  short Mode, double Opacity,
                  long SrcLeft, long SrcTop, long SrcRight, long SrcBottom)
  {

  // constrain to what we actually have
  if (Left < 0)
    Left = 0;
  if (Top < 0)
    Top = 0;
  if (Right > m_iWidth)
    Right = m_iWidth;
  if (Bottom > m_iHeight)
    Bottom = m_iHeight;

  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  if (!NormalizeBlendOpacity (Opacity))
    return eBadParameter;

  CBitmap * bitmap = it->second.get ();

  BITMAP bi;
  if (!bitmap->GetBitmap (&bi))
    return eUnableToLoadImage;

  const long right = FixRight (Right);
  const long bottom = FixBottom (Bottom);

  // constrain to what we actually have
  if (SrcLeft < 0)
    SrcLeft = 0;
  if (SrcTop < 0)
    SrcTop = 0;
  if (SrcRight > bi.bmWidth )
    SrcRight = bi.bmWidth ;
  if (SrcBottom > bi.bmHeight)
    SrcBottom = bi.bmHeight;

  // adjust so that -1 means 1 from right
  if (SrcRight <= 0)
    SrcRight = bi.bmWidth + SrcRight;

  if (SrcBottom <= 0)
    SrcBottom =  bi.bmHeight + SrcBottom;

  // Check for empty rectangles before subtracting to avoid signed overflow.
  if (right <= Left || bottom <= Top || SrcRight <= SrcLeft || SrcBottom <= SrcTop)
    return eOK;

  // Both starts are nonnegative, so these positive differences fit in a long.
  const long iWidth = min (right - Left, SrcRight - SrcLeft);
  const long iHeight = min (bottom - Top, SrcBottom - SrcTop);

  if (Mode < 1 || Mode > BlendModeCount)
    return eUnknownOption;
  if (Opacity == 0.0)
    {
    if (Mode == BlendDissolveMode)
      {
      const size_t samples = static_cast<size_t> (iWidth) + DissolveHasPaddingPixel (iWidth);
      for (long y = 0; y < iHeight; ++y)
        for (size_t x = 0; x < samples; ++x)
          genrand ();
      }
    return eOK;
    }

  // precompute cos table
  static unsigned char cos_table [256];
  static bool bTableComputed = false;

  if (!bTableComputed)
    {
    double pi_div255 = 3.1415926535898 / 255.0;
    for (int i = 0; i < 256; i++)
      {
      double a = 64.0 - cos ((double) i * pi_div255) * 64.0;
      cos_table [i] = (uint8) (a + 0.5);  // round
      }
    bTableComputed = true;
    } // table needs computing

  if (it->second->alpha && it->second->alpha->owned_pixels.empty ())
    {
    MiniWindowBlendTarget target;
    if (!target.CopyFrom (pdc->GetSafeHdc (), Left, Top, iWidth, iHeight) || !::GdiFlush ())
      return eUnableToLoadImage;
    BlendDirectImage (target, *it->second->alpha, SrcLeft, SrcTop, iWidth, iHeight,
      Mode, Opacity, cos_table);
    return target.Put (pdc->GetSafeHdc (), Left, Top, iWidth, iHeight);
    }

  CDC source_dc;
  MiniWindowBlendTarget source, target;
  if (!source_dc.CreateCompatibleDC (pdc)
      || !source.CopyFrom (source_dc, bitmap, SrcLeft, SrcTop, iWidth, iHeight)
      || !target.CopyFrom (pdc->GetSafeHdc (), Left, Top, iWidth, iHeight, false)
      || !::GdiFlush ())
    return eUnableToLoadImage;

  const unsigned char * pA = source.pixels;
  unsigned char * pB = target.pixels;
  const size_t perline = target.stride;
  const size_t count = perline * iHeight;
  size_t i;
  long row;
  switch (Mode)
    {

    MUSHCLIENT_BLEND_MODES (Blend_It, SKIP_BLEND_MODE, SKIP_BLEND_MODE, SKIP_BLEND_MODE)

    case BlendDissolveMode:
       {

       for (row = 0; row < iHeight; row++)
        {
        const size_t base = static_cast<size_t> (row) * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          double rnd = genrand ();
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];

          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          pB [base + i] = (rnd < Opacity) ? bA : bB;
          i++;
          pB [base + i] = (rnd < Opacity) ? gA : gB;
          i++;
          pB [base + i] = (rnd < Opacity) ? rA : rB;
          i++;

          }
         }  // end for each row

       }
      break;

    MUSHCLIENT_BLEND_MODES (SKIP_BLEND_MODE, Blend_It, Colour_Op, SKIP_BLEND_MODE)

    case BlendHueMode:
      {
       for (row = 0; row < iHeight; row++)
        {
        const size_t base = static_cast<size_t> (row) * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          CColor cA, cB;    // A = blend, B = base
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetHue (cA.GetHue ());  // hue of blend image, others from base image
          pB [base + i] = Simple_Opacity (bB, cB.GetBlue (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (gB, cB.GetGreen (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (rB, cB.GetRed (), Opacity);
          i++;
          }
         }  // end for each row
       }
      break;

    case BlendSaturationMode:
      {
       for (row = 0; row < iHeight; row++)
        {
        const size_t base = static_cast<size_t> (row) * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          CColor cA, cB;    // A = blend, B = base
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetSaturation (cA.GetSaturation ());  // saturation of blend image, others from base image
          pB [base + i] = Simple_Opacity (bB, cB.GetBlue (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (gB, cB.GetGreen (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (rB, cB.GetRed (), Opacity);
          i++;
          }
         }  // end for each row
       }
      break;

    case BlendColourMode:
      {
       for (row = 0; row < iHeight; row++)
        {
        const size_t base = static_cast<size_t> (row) * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          CColor cA, cB;    // A = blend, B = base
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetHue (cA.GetHue ());  // hue of blend image,
          cB.SetSaturation (cA.GetSaturation ());  // saturation of blend image, luminance from base image
          pB [base + i] = Simple_Opacity (bB, cB.GetBlue (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (gB, cB.GetGreen (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (rB, cB.GetRed (), Opacity);
          i++;
          }
         }  // end for each row
       }
      break;

      /*

    Note: I don't get the same results here as using Photoshop on the same images. Not quite sure why,
    some test code below seems to indicate that for all possible values of RGB, they map to HSL and back
    again without error.

    In the absence of any obvious bug, I am assuming that by plugging the Luminance from one colour into the
    L channel of another, I am creating an out-of-gamut colour, which then cannot be represented properly.

    I can't find, so far, any code that would limit the value to be in-gamut, so for now this Luminance (and
    Colour, Saturation and Hue too, probably) should be regarded as experimental.  - NJG - 2/Aug/2008.

    */

    case BlendLuminanceMode:
      {
       for (row = 0; row < iHeight; row++)
        {
        const size_t base = static_cast<size_t> (row) * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          CColor cA, cB;    // A = blend, B = base
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          // bit of a fudge here, but what can you do?

          // this is going to brighten the really dark pixels, however at least then they
          // should retain their base colour, rather than be promoted up to something garish

//          if (rB < 12) rB = 12;
//          if (gB < 12) gB = 12;
//          if (bB < 12) bB = 12;

          cA.SetRGB (rA, gA, bA);
          cB.SetRGB (rB, gB, bB);

          cB.SetLuminance (cA.GetLuminance ());  // luminance from blend image, others from base image

          pB [base + i] = Simple_Opacity (bB, cB.GetBlue (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (gB, cB.GetGreen (), Opacity);
          i++;
          pB [base + i] = Simple_Opacity (rB, cB.GetRed (), Opacity);
          i++;

          }
         }  // end for each row
       }
      break;

    case BlendHslMode:  // Store hue in red, saturation in green and luminance in blue.
      {
       for (row = 0; row < iHeight; row++)
        {
        const size_t base = static_cast<size_t> (row) * perline;
        unsigned char rA, gA, bA, rB, gB, bB;
        for (i = 0; i < perline - 2; )
          {
          CColor cA, cB;    // A = blend, B = base
          bA = pA [base + i];
          gA = pA [base + i + 1];
          rA = pA [base + i + 2];
          bB = pB [base + i];
          gB = pB [base + i + 1];
          rB = pB [base + i + 2];

          cA.SetRGB (rA, gA, bA);

          pB [base + i] = Simple_Opacity (bB, cA.GetLuminance () * 255, Opacity);
          i++;
          pB [base + i] = Simple_Opacity (gB, cA.GetSaturation () * 255, Opacity);
          i++;
          pB [base + i] = Simple_Opacity (rB, (cA.GetHue () / 360.0) * 255, Opacity);
          i++;

          }
         }  // end for each row
       }
      break;

      /*
    case 65: // testing  conversion to/from RGB to HSL and back again is consistent.
      {
      int r, g, b;
      CColor cA;

      for (r = 0; r < 256; r ++)
        for (g = 0; g < 256; g ++)
          for (b = 0; b < 256; b ++)
            {
            cA.SetRGB (r, g, b);
            cA.GetLuminance ();  // force change to HSL
            ASSERT (r == cA.GetRed ());
            ASSERT (g == cA.GetGreen ());
            ASSERT (b == cA.GetBlue ());

            }


      }
    break;

      */

    default: return eUnknownOption;
    }

  return target.Put (pdc->GetSafeHdc (), Left, Top, iWidth, iHeight);

  }   // end of CMiniWindow::BlendImage

#undef Blend_It
#undef Colour_Op


// copies one miniwindow image as a stand-alone image
long CMiniWindow::ImageFromWindow(LPCTSTR ImageId, CMiniWindow * pSrcWindow)
  {

  std::unique_ptr<MiniWindowImage> image (new MiniWindowImage);
  const int w = pSrcWindow->m_iWidth, h = pSrcWindow->m_iHeight;
  BITMAP surface = {};
  const bool direct = w > 0 && h > 0 && w <= INT_MAX / 4
    && static_cast<unsigned __int64> (w) * h * 4 <= MAXDWORD
    && ::GetObject (::GetCurrentObject (pdc->GetSafeHdc (), OBJ_BITMAP), sizeof surface, &surface) == sizeof surface
    && !surface.bmBits && surface.bmBitsPixel == 32 && surface.bmPlanes == 1
    && ::GetDeviceCaps (pdc->GetSafeHdc (), BITSPIXEL) == 32;
  if (direct)
    {
    // Keep the captured fourth byte for unscaled drawing. The scaled view
    // treats these same pixels as opaque without changing their alpha bytes.
    BITMAPINFO info = TopDownBitmapInfo (w, h);
    void * pixels = NULL;
    HBITMAP bitmap = ::CreateDIBSection (pdc->GetSafeHdc (), &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!bitmap)
      AfxThrowResourceException ();
    if (!image->Attach (bitmap))
      {
      ::DeleteObject (bitmap);
      AfxThrowResourceException ();
      }
    const unsigned char * data = static_cast<const unsigned char *> (pixels);
    image->alpha.reset (new MiniWindowAlphaImage (data, w, h, w * 4));
    image->opaque_alpha.reset (new MiniWindowAlphaImage (data, w, h, w * 4, true));
    }
  else if (!image->CreateCompatibleBitmap (pdc, w, h))
    AfxThrowResourceException ();
  CDC dc;
  if (!dc.CreateCompatibleDC (pdc))
    AfxThrowResourceException ();
  {
  MiniWindowSelectedObject selected (&dc, image.get ());
  if (!selected.Valid () || !dc.BitBlt (0, 0, w, h, pSrcWindow->pdc, 0, 0, SRCCOPY)
      || (direct && !::GdiFlush ()))
    AfxThrowResourceException ();
  }
  m_Images [ImageId] = std::move (image);
  return eOK;

  } // end of CMiniWindow::ImageFromWindow


static void MakeTexture (const COLORREF Multiplier,
                         unsigned char * pBuffer,
                         const long iWidth,
                         const long iHeight)
  {

  long   rval = GetRValue (Multiplier),
         gval = GetGValue (Multiplier),
         bval = GetBValue (Multiplier);

  long row, col;
  long c;

  int increment =  BytesPerLine (iWidth, 24);

   // main loop is columns
   for (col = 0; col < iWidth; col++)
     {
     unsigned char * p = pBuffer + col * 3;
     for (row = 0; row < iHeight; row++)
       {
       c = col ^ row;
       p [0] = c * bval;
       p [1] = c * gval;
       p [2] = c * rval;
       p += increment;
       }  // end of each row
     }  // end of each column


  }  // end of MakeTexture


long CMiniWindow::Gradient(long Left, long Top, long Right, long Bottom,
                      long StartColour, long EndColour,
                      short Mode)
  {
  // calculate size of desired rectangle
  long iWidth = FixRight (Right) - Left;
  long iHeight = FixBottom (Bottom) - Top;

  if (iWidth <= 0 || iHeight <= 0)   // sanity check
    return eOK;

  // Texture Fill requires slower method

  if (Mode == 3)
    {

    // upper layer (from image id)
    CDC gDC;
    gDC.CreateCompatibleDC(&(*pdc));
    CBitmap gbmp;

    BITMAPINFO bmi;
    ZeroMemory (&bmi, sizeof bmi);

    bmi.bmiHeader.biSize = sizeof bmi;
    bmi.bmiHeader.biWidth =          iWidth;
    bmi.bmiHeader.biHeight =         iHeight;
    bmi.bmiHeader.biPlanes =         1;
    bmi.bmiHeader.biBitCount =       24;
    bmi.bmiHeader.biCompression =    BI_RGB;
    bmi.bmiHeader.biSizeImage =      iHeight * BytesPerLine (iWidth, 24);

    unsigned char * pA = NULL;

    HBITMAP hbmG = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void**) &pA, NULL, 0);

    if (!hbmG)
      return eUnableToLoadImage;

    HBITMAP hOldAbmp = (HBITMAP) SelectObject(gDC.m_hDC, hbmG);

    MakeTexture (StartColour, pA, iWidth, iHeight);

    // copy result back

    pdc->BitBlt (Left, Top, iWidth, iHeight, &gDC, 0, 0, SRCCOPY);


    SelectObject(gDC.m_hDC, hOldAbmp);

    DeleteObject (hbmG);

    return eOK;

    }  // end of texture fill


  // for Windows 2000 onwards, use faster GDI fill mode as suggested by Twisol
  // See: http://msdn.microsoft.com/en-us/library/aa925212.aspx

  if (os_version.dwPlatformId > VER_PLATFORM_WIN32_NT ||   // some future version?
     (os_version.dwPlatformId == VER_PLATFORM_WIN32_NT &&  // NT upwards
      os_version.dwMajorVersion >= 5))                     // Windows 2000 upwards
    {

    TRIVERTEX        vert[2] ;
    GRADIENT_RECT    gRect;
    DWORD            dwMode;

    // first vertex
    vert [0] .x      = Left;
    vert [0] .y      = Top;
    vert [0] .Red    = GetRValue (StartColour) << 8;  // higher resolution colours
    vert [0] .Green  = GetGValue (StartColour) << 8;
    vert [0] .Blue   = GetBValue (StartColour) << 8;
    vert [0] .Alpha  = 0;

    // second vertex
    vert [1] .x      = FixRight (Right);
    vert [1] .y      = FixBottom (Bottom);
    vert [1] .Red    = GetRValue (EndColour) << 8;
    vert [1] .Green  = GetGValue (EndColour) << 8;
    vert [1] .Blue   = GetBValue (EndColour) << 8;
    vert [1] .Alpha  = 0;

    // rectange goes from first vertex to second one
    gRect.UpperLeft  = 0;
    gRect.LowerRight = 1;

    switch (Mode)
      {
      case 1 : dwMode = GRADIENT_FILL_RECT_H; break;  // horizontal
      case 2 : dwMode = GRADIENT_FILL_RECT_V; break;  // vertical
      default: return eUnknownOption;
      } // end of switch

    // do the fill
    GradientFill (pdc->m_hDC, vert, 2, &gRect, 1, dwMode);

    return eOK;

    }   // end of Windows 2000 or higher

  // slower method (also suggested by Twisol rather than doing individual bytes):
  double rdiff = GetRValue (EndColour) - GetRValue (StartColour),
         gdiff = GetGValue (EndColour) - GetGValue (StartColour),
         bdiff = GetBValue (EndColour) - GetBValue (StartColour);

  double rval = GetRValue (StartColour),
         gval = GetGValue (StartColour),
         bval = GetBValue (StartColour);

  double rinc = rdiff / (double) (iWidth - 1),
         ginc = gdiff / (double) (iWidth - 1),
         binc = bdiff / (double) (iWidth - 1);

  COLORREF oldColour = pdc->GetBkColor ();
  pdc->SetBkMode (TRANSPARENT);


  switch (Mode)
    {
    case 1 : // horizontal (left to right)

      {
      long iRight = FixRight (Right);
      for (long x = Left; x < iRight; x++)
        {
        pdc->FillSolidRect( x, Top, 1, iHeight, RGB (rval, gval, bval) );
        rval += rinc;
        gval += ginc;
        bval += binc;
       }
      }

      break;  // end of horizontal

    case 2 : // vertical  (top to bottom)

      {
      long iBottom = FixBottom (Bottom);
      for (long y = Top; y < iBottom; y++)
        {
        pdc->FillSolidRect( Left, y, iWidth, 1, RGB (rval, gval, bval) );
        rval += rinc;
        gval += ginc;
        bval += binc;
       }
      }

      break;  // end of vertical


    default: return eUnknownOption;

    } // end of switch


  pdc->SetBkColor (oldColour);  // FillSolidRect changes the background colour
  return eOK; // end of horizontal or vertical

  } // end of CMiniWindow::Gradient

static void GeneralFilter (unsigned char * inbuf, long iWidth, long iHeight,
                            const long iPerLine,
                            const double Options,
                            const double * iMatrix,
                            const double iDivisor)
  {

  unsigned char * pi;
  long total;

  long row, col, rgb;
  int i;
  int iLastByte = iWidth * 3;
  long window [5];

  if (Options != 2)    // 2 = vertical only
    {
    // horizontal
    // main loop is rows
    for (row = 0; row < iHeight; row++)
     {

     pi = inbuf + iPerLine * row;


     for (rgb = 0; rgb < 3; rgb++)
       {

       // preload 4 slots in window for edge case  (-2 to +1)
       for (col = 0; col < 4; col++)
          window [col + 1] = pi [min (max (rgb + (col * 3) - 6, 0), iLastByte - 1)];

       for (col = 0; col < iLastByte - 4; col += 3)
         {
         // we add 3 to get to the next pixel in that colour (R, G or B)

         // insert right-most byte into window

         window [0] = window [1];
         window [1] = window [2];
         window [2] = window [3];
         window [3] = window [4];
         window [4] = pi [min (max (col + rgb + 6, 0), iLastByte - 1)];

         total = 0;

         for (i = 0; i < 5; i++)
            total +=  window [i] * iMatrix [i];

         pi [col + rgb] = (uint8) (min (max (total / iDivisor, 0), 255));
         }   // end of each column
       }     // end of each colour

     }  // end of each row
    }   // horizontal wanted

  if (Options != 1)    // 1 = horizontal only
  {

  // vertical
    // main loop is columns
    for (col = 0; col < iLastByte; col++)
      {

       // preload 4 slots in window for edge case  (-2 to +1)
       for (row = 0; row < 4; row++)
         {
         long from = min (max (row - 2, 0), iHeight - 1);
         pi = inbuf + col + (from * iPerLine);
         window [row + 1] = *pi;
         }

      for (row = 0; row < iHeight; row++)
       {

       long from = min (max (row + 3, 0), iHeight - 1);
       pi = inbuf + col + (from * iPerLine);

       // insert right-most byte into window

       window [0] = window [1];
       window [1] = window [2];
       window [2] = window [3];
       window [3] = window [4];
       window [4] = *pi;

       total = 0;

       for (i = 0; i < 5; i++)
          total +=  window [i] * iMatrix [i];

       pi = inbuf + col + (row * iPerLine);
       pi [0] = (uint8) (min (max (total / iDivisor, 0), 255));

       }  // end of each row
      }  // end of each column
    }  // vertical wanted

  } // end of Blur


static void Noise (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  unsigned char * pi = inbuf;
  long count = iPerLine * iHeight;
  long i, c;
  double threshold = Options / 100.0;
  for (i = 0; i < count; i++)
    {
    c = *pi;
    c += (128 - genrand () * 256) * threshold;
    *pi++  = CLAMP (c);
    }

  } // end of Noise

static void MonoNoise (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  long i, j, c, row;
  double threshold = Options / 100.0;


  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine);
    for (i = 0; i < iWidth; i ++)
      {
      j = (128 - genrand () * 256) * threshold;

      c = *pi + j;
      *pi++  = CLAMP (c);
      c = *pi + j;
      *pi++  = CLAMP (c);
      c = *pi + j;
      *pi++  = CLAMP (c);
      }
     }  // end for each row


  } // end of MonoNoise


static void Brightness (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  unsigned char * pi = inbuf;
  long count = iPerLine * iHeight;
  long i, c;
  for (i = 0; i < count; i++)
    {
    c = *pi;
    c += Options;
    *pi++  = CLAMP (c);
    }

  } // end of Brightness


static void BrightnessMultiply (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  unsigned char * pi = inbuf;
  long count = iPerLine * iHeight;
  long i, c;
  for (i = 0; i < count; i++)
    {
    c = *pi;
    c *= Options;
    *pi++  = CLAMP (c);
    }

  } // end of BrightnessMultiply

static void Contrast (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  unsigned char lookup [256];  // lookup table for speed

  unsigned char * pi = inbuf;
  long count = iPerLine * iHeight;
  long i;
  double c;

  for (i = 0; i < 256; i++)
    {
    c = i - 128;    // center on zero
    c *= Options;  // multiply by contrast
    c += 128;  // put back
    lookup [i] = CLAMP (c);
    }  // end of for loop

  // now convert image using lookup table
  for (i = 0; i < count; i++)
    *pi++ = lookup [*pi];

  } // end of Contrast

static void Gamma (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options)
  {

  unsigned char lookup [256];  // lookup table for speed

  unsigned char * pi = inbuf;
  long count = iPerLine * iHeight;
  long i;
  double c;
  if (Options < 0.0)
    Options = 0.0;

  for (i = 0; i < 256; i++)
    {
    c = ( (double) i) / 255.0;  // normalize it
    c = pow (c, Options);
    c *= 255;
    lookup [i] = CLAMP (c);
    }  // end of for loop

  // now convert image using lookup table
  for (i = 0; i < count; i++)
    *pi++ = lookup [*pi];

  } // end of Gamma


static void ColourBrightness (unsigned char * inbuf, long iWidth, long iHeight,
                              long iPerLine, double Options, long iColour)
  {

  long i, row, c;

  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine) + iColour;
    for (i = 0; i < iWidth; i ++)
      {
      c = *pi;
      c += Options;
      *pi  = CLAMP (c);
      pi += 3;
      }
     }  // end for each row

  } // end of ColourBrightness


static void ColourBrightnessMultiply (unsigned char * inbuf, long iWidth, long iHeight,
                              long iPerLine, double Options, long iColour)
  {

  long i, row, c;

  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine) + iColour;
    for (i = 0; i < iWidth; i ++)
      {
      c = *pi;
      c *= Options;
      *pi  = CLAMP (c);
      pi += 3;
      }
     }  // end for each row

  } // end of ColourBrightnessMultiply


static void ColourContrast (unsigned char * inbuf, long iWidth, long iHeight,
                            long iPerLine, double Options, long iColour)
  {

  unsigned char lookup [256];  // lookup table for speed

  long i, row;
  double c;

  for (i = 0; i < 256; i++)
    {
    c = i - 128;    // center on zero
    c *= Options;  // multiply by contrast
    c += 128;  // put back
    lookup [i] = CLAMP (c);
    }  // end of for loop

  // now convert image using lookup table
  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine) + iColour;
    for (i = 0; i < iWidth; i ++)
      {
      *pi = lookup [*pi];
      pi += 3;
      }
     }  // end for each row

    } // end of ColourContrast

static void ColourGamma (unsigned char * inbuf, long iWidth, long iHeight,
                         long iPerLine, double Options, long iColour)
  {

  unsigned char lookup [256];  // lookup table for speed

  long i, row;
  double c;

  if (Options < 0.0)
    Options = 0.0;

  for (i = 0; i < 256; i++)
    {
    c = ( (double) i) / 255.0;  // normalize it
    c = pow (c, Options);
    c *= 255;
    lookup [i] = CLAMP (c);
    }  // end of for loop

  // now convert image using lookup table
  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine) + iColour;
    for (i = 0; i < iWidth; i ++)
      {
      *pi = lookup [*pi];
      pi += 3;
      }
     }  // end for each row

  } // end of ColourGamma


// see: http://en.wikipedia.org/wiki/Grayscale

static void MakeGreyscale (unsigned char * inbuf, long iWidth, long iHeight, long iPerLine, double Options, const bool bLinear)
  {

  long i, row;
  double c;


  for (row = 0; row < iHeight; row++)
    {
    unsigned char * pi = inbuf + (row * iPerLine);
    for (i = 0; i < iWidth; i ++)
      {

      if (bLinear)
        {
        c =  pi [0] + pi [1] + pi [2];
        c /= 3;
        }
      else
        c = pi [0] * 0.11 + // blue   (perceptual)
            pi [1] * 0.59 + // green
            pi [2] * 0.30;  // red

      *pi++  = CLAMP (c);
      *pi++  = CLAMP (c);
      *pi++  = CLAMP (c);
      }
     }  // end for each row


  } // end of MakeGreyscale


static void Average (unsigned char * pBuffer,
                      const long iWidth,
                      const long iHeight)
  {

  long row, col;
  __int64 r = 0, g = 0, b = 0, count = 0;

  int increment =  BytesPerLine (iWidth, 24);

  // find average

  // main loop is columns
  for (col = 0; col < iWidth; col++)
   {
   unsigned char * p = pBuffer + col * 3;
   for (row = 0; row < iHeight; row++)
     {
     b += p [0];
     g += p [1];
     r += p [2];
     count++;
     p += increment;
     }  // end of each row
   }  // end of each column

  r /= count;
  g /= count;
  b /= count;

  // apply average

  // main loop is columns
  for (col = 0; col < iWidth; col++)
   {
   unsigned char * p = pBuffer + col * 3;
   for (row = 0; row < iHeight; row++)
     {
     p [0] = CLAMP (b);
     p [1] = CLAMP (g);
     p [2] = CLAMP (r);
     p += increment;
     }  // end of each row
   }  // end of each column

  }  // end of Average


// see also:  CMUSHclientDoc::FilterPixel
long CMiniWindow::Filter(long Left, long Top, long Right, long Bottom,
                  short Operation, double Options)
  {

  // constrain to what we actually have
  if (Left < 0)
    Left = 0;
  if (Top < 0)
    Top = 0;
  if (Right > m_iWidth)
    Right = m_iWidth;
  if (Bottom > m_iHeight)
    Bottom = m_iHeight;

  // calculate size of desired rectangle
  long iWidth = FixRight (Right) - Left;
  long iHeight = FixBottom (Bottom) - Top;

  if (iWidth < 1 || iHeight < 1)   // sanity check
    return eOK;

  if (Operation < 1 || Operation > 27)
    return eUnknownOption;

  // upper layer (from image id)
  CDC gDC;
  gDC.CreateCompatibleDC(&(*pdc));
  CBitmap gbmp;

  BITMAPINFO bmi;
  ZeroMemory (&bmi, sizeof bmi);

  bmi.bmiHeader.biSize = sizeof bmi;
  bmi.bmiHeader.biWidth =          iWidth;
  bmi.bmiHeader.biHeight =         iHeight;
  bmi.bmiHeader.biPlanes =         1;
  bmi.bmiHeader.biBitCount =       24;
  bmi.bmiHeader.biCompression =    BI_RGB;
  bmi.bmiHeader.biSizeImage =      iHeight * BytesPerLine (iWidth, 24);

  unsigned char * pA = NULL;

  HBITMAP hbmG = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void**) &pA, NULL, 0);

  if (!hbmG)
    return eUnableToLoadImage;

  HBITMAP hOldAbmp = (HBITMAP) SelectObject(gDC.m_hDC, hbmG);

  // copy base image from miniwindow to bitmap
  gDC.BitBlt (0, 0, iWidth, iHeight, &(*pdc), Left, Top, SRCCOPY);

  // manipulate image here

   switch (Operation)
     {
     case 1: Noise    (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;
     case 2: MonoNoise(pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;
     case 3: // blur
       {
       double iMatrix [5] = { 1, 1, 1, 1, 1 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 5);
       }
       break;
     case 4:  // sharpen
       {
       double iMatrix [5] = { -1, -1, 7, -1, -1 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 3);
       }
       break;
     case 5:  // edge detect
       {
       double iMatrix [5] = { 0, 2.5, -6, 2.5, 0 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 1);
       }
       break;
     case 6:  // emboss
       {
       double iMatrix [5] = { 1, 2, 1, -1, -2 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 1);
       }
       break;
     case  7: Brightness        (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;      // additive brightness
     case  8: Contrast          (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;
     case  9: Gamma             (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;
     case 10: ColourBrightness  (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 2); break;    // red
     case 11: ColourContrast    (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 2); break;    // red
     case 12: ColourGamma       (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 2); break;    // red
     case 13: ColourBrightness  (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 1); break;    // green
     case 14: ColourContrast    (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 1); break;    // green
     case 15: ColourGamma       (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 1); break;    // green
     case 16: ColourBrightness  (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 0); break;    // blue
     case 17: ColourContrast    (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 0); break;    // blue
     case 18: ColourGamma       (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 0); break;    // blue
     case 19: MakeGreyscale     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, true); break;    // linear
     case 20: MakeGreyscale     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, false); break;    // perceptual
     case 21: BrightnessMultiply(pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options); break;          // multiplicative brightness
     case 22: ColourBrightnessMultiply(pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 2); break;   // red
     case 23: ColourBrightnessMultiply(pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 1); break;   // green
     case 24: ColourBrightnessMultiply(pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, 0); break;   // blue
     case 25: // lesser blur
       {
       double iMatrix [5] = { 0, 1, 1, 1, 0 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 3);
       }
       break;
     case 26: // minor blur
       {
       double iMatrix [5] = { 0, 0.5, 1, 0.5, 0 };
       GeneralFilter     (pA, iWidth, iHeight, BytesPerLine (iWidth, 24), Options, iMatrix, 2);
       }
       break;
     case 27: Average             (pA, iWidth, iHeight); break;



     default: return eUnknownOption;

     }

  // copy result back

  pdc->BitBlt (Left, Top, iWidth, iHeight, &gDC, 0, 0, SRCCOPY);


  SelectObject(gDC.m_hDC, hOldAbmp);

  DeleteObject (hbmG);

  return eOK;

  } // end of CMiniWindow::Filter



long CMiniWindow::SetPixel(long x, long y, long Colour)
  {
  pdc->SetPixelV(x, y, Colour);
  return eOK;
  } // end of CMiniWindow::SetPixel


long CMiniWindow::SetZOrder(long Order)
  {
  m_ZOrder = Order;
  return eOK;
  }

long CMiniWindow::GetPixel(long x, long y)
  {
  return pdc->GetPixel(x, y);
  } // end of CMiniWindow::GetPixel


template<bool FullOpacity>
static void MergeMaskedPixels (unsigned char * target, const unsigned char * source,
                               const unsigned char * mask, int width, int height, int stride,
                               double opacity, COLORREF colourKey)
  {
  for (int y = 0; y < height; ++y)
    {
    for (int i = 0; i < width * 3; i += 3)
      {
      const unsigned char b = source [i], g = source [i + 1], r = source [i + 2];
      if (RGB (r, g, b) == colourKey)
        continue;
      const unsigned char db = target [i], dg = target [i + 1], dr = target [i + 2];
      target [i] = MiniWindowAlphaImage::BlendChannel<FullOpacity> (b, db, mask [i], opacity);
      target [i + 1] = MiniWindowAlphaImage::BlendChannel<FullOpacity> (g, dg, mask [i + 1], opacity);
      target [i + 2] = MiniWindowAlphaImage::BlendChannel<FullOpacity> (r, dr, mask [i + 2], opacity);
      }
    target += stride;
    source += stride;
    mask += stride;
    }
  }

long CMiniWindow::MergeImageAlpha(LPCTSTR ImageId, LPCTSTR MaskId,
                                  long Left, long Top, long Right, long Bottom,
                                  short Mode, double Opacity,
                                  long SrcLeft, long SrcTop, long SrcRight, long SrcBottom)
  {

  // constrain to what we actually have
  if (Left < 0)
    Left = 0;
  if (Top < 0)
    Top = 0;
  if (Right > m_iWidth)
    Right = m_iWidth;
  if (Bottom > m_iHeight)
    Bottom = m_iHeight;

  if (!NormalizeBlendOpacity (Opacity))
    return eBadParameter;

  // image to be merged
  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  MiniWindowImage * bitmap = it->second.get ();

  // mask image
  it = m_Images.find (MaskId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  MiniWindowImage * mask_bitmap = it->second.get ();

  BITMAP bi;
  if (!bitmap->GetBitmap (&bi))
    return eUnableToLoadImage;

  const long right = FixRight (Right);
  const long bottom = FixBottom (Bottom);

  // constrain to what we actually have
  if (SrcLeft < 0)
    SrcLeft = 0;
  if (SrcTop < 0)
    SrcTop = 0;
  if (SrcRight > bi.bmWidth )
    SrcRight = bi.bmWidth ;
  if (SrcBottom > bi.bmHeight)
    SrcBottom = bi.bmHeight;

  // adjust so that -1 means 1 from right
  if (SrcRight <= 0)
    SrcRight = bi.bmWidth + SrcRight;

  if (SrcBottom <= 0)
    SrcBottom =  bi.bmHeight + SrcBottom;

  // Check for empty rectangles before subtracting to avoid signed overflow.
  if (right <= Left || bottom <= Top || SrcRight <= SrcLeft || SrcBottom <= SrcTop)
    return eOK;

  // Both starts are nonnegative, so these positive differences fit in a long.
  const long iWidth = min (right - Left, SrcRight - SrcLeft);
  const long iHeight = min (bottom - Top, SrcBottom - SrcTop);

  if (!mask_bitmap->GetBitmap (&bi))
    return eUnableToLoadImage;

  // adjust down in case we don't use whole image
  SrcRight = SrcLeft + iWidth;
  SrcBottom = SrcTop + iHeight;

  // mask must at least be as big as the image we are merging
  if (bi.bmWidth < SrcRight)
    return eBadParameter;
  if (bi.bmHeight < SrcBottom)
    return eBadParameter;

  if (Mode < 0 || Mode > 1)
    return eUnknownOption;
  if (Opacity == 0.0)
    return eOK;


  if (bitmap->alpha && bitmap->alpha->owned_pixels.empty ()
      && mask_bitmap->alpha && mask_bitmap->alpha->owned_pixels.empty ())
    {
    MiniWindowBlendTarget target;
    if (!target.CopyFrom (pdc->GetSafeHdc (), Left, Top, iWidth, iHeight) || !::GdiFlush ())
      return eUnableToLoadImage;
    if (Opacity < 1.0)
      MergeDirectPixels<false> (target, *bitmap->alpha, *mask_bitmap->alpha,
        SrcLeft, SrcTop, iWidth, iHeight, Mode, Opacity);
    else
      MergeDirectPixels<true> (target, *bitmap->alpha, *mask_bitmap->alpha,
        SrcLeft, SrcTop, iWidth, iHeight, Mode, Opacity);
    return target.Put (pdc->GetSafeHdc (), Left, Top, iWidth, iHeight);
    }

  CDC source_dc;
  MiniWindowBlendTarget source, target, mask;
  if (!source_dc.CreateCompatibleDC (pdc)
      || !source.CopyFrom (source_dc, bitmap, SrcLeft, SrcTop, iWidth, iHeight)
      || !target.CopyFrom (pdc->GetSafeHdc (), Left, Top, iWidth, iHeight, false)
      || !mask.CopyFrom (source_dc, mask_bitmap, SrcLeft, SrcTop, iWidth, iHeight)
      || !::GdiFlush ())
    return eUnableToLoadImage;

  const unsigned char * pA = source.pixels, * pM = mask.pixels;
  unsigned char * pB = target.pixels;
  const int perline = target.stride;
  const size_t count = static_cast<size_t> (perline) * iHeight;

  switch (Mode)
    {
    case 0:     // normal
      if (Opacity < 1.0)
        {
        for (size_t i = 0; i < count; i++)
          {
          pB [i] = MiniWindowAlphaImage::BlendChannel<false> (pA [i], pB [i], pM [i], Opacity);
          }
        }
      else
        {
        for (size_t i = 0; i < count; i ++)
          pB [i]  = MiniWindowAlphaImage::BlendChannel<true> (pA [i], pB [i], pM [i], Opacity);
        }
      break;  // end of mode 0

    case 1:  // transparent on pixel at 0,0
      {
      const unsigned char * key = pA + static_cast<size_t> (iHeight - 1) * perline;
      const COLORREF colourKey = RGB (key [2], key [1], key [0]);
      if (Opacity < 1.0)
        MergeMaskedPixels<false> (pB, pA, pM, iWidth, iHeight, perline, Opacity, colourKey);
      else
        MergeMaskedPixels<true> (pB, pA, pM, iWidth, iHeight, perline, Opacity, colourKey);
      }
      break;    // end of mode 1

    default: return eUnknownOption;

    } // end of switch on Mode


  return target.Put (pdc->GetSafeHdc (), Left, Top, iWidth, iHeight);

  } // end of CMiniWindow::MergeImageAlpha

CMenu* GrabAMenu (void)
  {
  CMenu rootMenu;
  if (!rootMenu.LoadMenu (IDR_MXP_MENU))
    return NULL;

  HMENU hPopup = ::GetSubMenu (rootMenu.m_hMenu, 0);
  if (hPopup == NULL)
    return NULL;

  std::unique_ptr<CMenu> pPopup (new CMenu);
  if (!rootMenu.RemoveMenu (0, MF_BYPOSITION))
    return NULL;
  if (!pPopup->Attach (hPopup))
    {
    ::DestroyMenu (hPopup);
    return NULL;
    }
  pPopup->DeleteMenu (0, MF_BYPOSITION);  // get rid of dummy item

  return pPopup.release ();
  }   // end of GrabAMenu

CString CMiniWindow::Menu(long Left, long Top, LPCTSTR Items, CMUSHView* pView)
  {
CString strResult;
int align = TPM_LEFTALIGN | TPM_TOPALIGN;  // these are actually both zero

  // can't menu if not visible
  if (!m_bShow || m_bTemporarilyHide)
    return strResult;

  // can't if outside window
  if (Left < 0 || Left > m_iWidth ||
      Top < 0 || Top > m_iHeight)
      return strResult;

  // make relative to miniwindow
  Left += m_rect.left;
  Top  += m_rect.top;

  // can't have empty string
  if (Items [0] == 0)
    return strResult;

  LPCTSTR p = Items;
  bool bReturnNumber = false;

  if (*p == '!')
    {
    bReturnNumber = true;
    p++;
    }

  // alignment

  if (*p == '~' && strlen (p) > 3)
    {
    p++;  // skip the '~'
    // horizontal alignment
    switch (*p++)
      {
      case 'c':
      case 'C':
         align |= TPM_CENTERALIGN;
         break;
      case 'l':
      case 'L':
         align |= TPM_LEFTALIGN;
         break;
      case 'r':
      case 'R':
         align |= TPM_RIGHTALIGN;
         break;
      default:
         // ignore
         break;
      } // end of switch

    // vertical alignment
    switch (*p++)
      {
      case 'c':
      case 'C':
         align |= TPM_VCENTERALIGN;
         break;
      case 'b':
      case 'B':
         align |= TPM_BOTTOMALIGN;
         break;
      case 't':
      case 'T':
         align |= TPM_TOPALIGN;
         break;
      default:
         // ignore
         break;
      } // end of switch

    } // end of alignment code

  vector<string> v;

  StringToVector (p, v, "|");

  int iCount = v.size ();

  // must have at least one item
  if (iCount < 1)
    return strResult;

CPoint menupoint (Left, Top);


  list<CMenu*> vPopup;    // active nested-menu stack
  vector<CMenu*> allMenus;  // all allocated menu wrappers

  CMenu* pPopup = GrabAMenu ();   // top level
  if (pPopup == NULL)
    return strResult;     // doesn't exist, strange
  std::unique_ptr<CMenu> rootMenuOwner (pPopup);
  allMenus.push_back (pPopup);
  rootMenuOwner.release ();

  class CMenuGuard
    {
    public:
      CMenuGuard (vector<CMenu*> & menus) : m_Menus (menus) { }
      ~CMenuGuard ()
        {
        for (size_t i = 1; i < m_Menus.size (); i++)
          {
          m_Menus [i]->Detach ();
          delete m_Menus [i];
          }
        if (!m_Menus.empty ())
          {
          m_Menus [0]->DestroyMenu ();
          delete m_Menus [0];
          }
        }

    private:
      vector<CMenu*> & m_Menus;
    } menuGuard (allMenus);

  vPopup.push_back (pPopup);

  CWnd* pWndPopupOwner = (CWnd *) pView;
  while (pWndPopupOwner->GetStyle() & WS_CHILD)
    pWndPopupOwner = pWndPopupOwner->GetParent();

  ClientToScreen(pView->m_hWnd, &menupoint);

  int j = 0;
  vector<CString> menuItems;
  menuItems.reserve (MXP_MENU_COUNT);

  for (vector<string>::const_iterator i = v.begin (); i != v.end (); i++)
    {
    CString strItem = i->c_str ();

    if (*i == "-" || *i == "")
      vPopup.back ()->AppendMenu (MF_SEPARATOR, 0, "");
    else if (strItem.Left (1) == ">")    // nested menu
      {
      CMenu* pNestedMenu = GrabAMenu ();   // nested menu
      if (pNestedMenu)
        {
        std::unique_ptr<CMenu> nestedMenuOwner (pNestedMenu);
        allMenus.push_back (pNestedMenu);
        nestedMenuOwner.release ();
        if (!vPopup.back ()->AppendMenu (MF_POPUP | MF_ENABLED,
                                         (UINT) pNestedMenu->m_hMenu,
                                         strItem.Mid (1)))
          {
          allMenus.pop_back ();
          pNestedMenu->DestroyMenu ();
          delete pNestedMenu;
          AfxThrowResourceException ();
          }
        vPopup.push_back (pNestedMenu);  // add to menu stack
        }
      }
    else if (strItem.Left (1) == "<")    // un-nest menu
      {
      if (vPopup.size () > 1)
        vPopup.pop_back ();  // drop last item
      }
    else
      {
      // flags - will always be a string
      UINT iFlags = MF_STRING;

      while (strItem.Left (1) == "+" ||
             strItem.Left (1) == "^")
        {
        // leading + means tick it
        if (strItem.Left (1) == "+")
          iFlags |= MF_CHECKED;
        // and a carat means disabled
        else if (strItem.Left (1) == "^")
          iFlags |= MF_GRAYED;

        strItem = strItem.Mid (1);  // get rid of first character
        } // end of while

      if (iFlags & MF_GRAYED)
        vPopup.back ()->AppendMenu (iFlags, 0, strItem);
      else
        {
        menuItems.push_back (strItem);
        vPopup.back ()->AppendMenu (iFlags, MXP_FIRST_MENU + j, strItem);
        j++;
        if (j >= MXP_MENU_COUNT)
          break;
        }
      }

    }

  // without this line the auto-enable always set "no items" to active
  CValueStateGuard<BOOL> autoMenuEnableGuard (Frame.m_bAutoMenuEnable, FALSE);

  int iResult = vPopup.front ()->TrackPopupMenu(align | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
                          menupoint.x,
                          menupoint.y,
                          pWndPopupOwner);

  if (iResult > 0)
    {
    if (bReturnNumber)
      strResult = CFormat ("%i", iResult - MXP_FIRST_MENU + 1);
    else
      {
      const int iMenuItem = iResult - MXP_FIRST_MENU;
      if (iMenuItem >= 0 && iMenuItem < static_cast<int> (menuItems.size ()))
        strResult = menuItems [iMenuItem];
      }
    }

  return  strResult;  // will be empty for errors or cancelled


  }  // end of CMiniWindow::Menu

long CMiniWindow::DragHandler(CMUSHclientDoc * pDoc, LPCTSTR HotspotId,
                              LPCTSTR sPluginID,
                              LPCTSTR MoveCallback,
                              LPCTSTR ReleaseCallback, long Flags)
  {

  if (InvalidHotspotCallback (MoveCallback))
    return eInvalidObjectLabel;
  if (InvalidHotspotCallback (ReleaseCallback))
    return eInvalidObjectLabel;

  // can't switch plugins here :)
  if (!m_sCallbackPlugin.empty () && m_sCallbackPlugin != sPluginID)
    return eHotspotPluginChanged;

  HotspotMapIterator first = m_Hotspots.find (HotspotId);
  if (first == m_Hotspots.end ())
    return eHotspotNotInstalled;   // no such hotspot

  DISPID dispidMove = DISPID_UNKNOWN;
  DISPID dispidRelease = DISPID_UNKNOWN;

  // if not in a plugin, look in main world for hotspot callbacks, and remember the dispatch ID
  if (!*sPluginID)
    {
    CBoolStateGuard executingGuard (m_bExecutingScript, true);
    CString strErrorMessage;

    dispidMove = pDoc->GetProcedureDispid (MoveCallback, "mouse move", "", strErrorMessage);
    dispidRelease = pDoc->GetProcedureDispid (ReleaseCallback, "mouse release", "", strErrorMessage);
    }

  // Callback lookup can delete or replace the hotspot (including via
  // WindowCreate). Do not carry its pointer across that lookup.
  if (!m_sCallbackPlugin.empty () && m_sCallbackPlugin != sPluginID)
    return eHotspotPluginChanged;
  HotspotMapIterator it = *sPluginID ? first : m_Hotspots.find (HotspotId);
  if (it == m_Hotspots.end ())
    return eHotspotNotInstalled;

  CHotspot * pHotspot = it->second;
  pHotspot->m_sMoveCallback = MoveCallback;
  pHotspot->m_sReleaseCallback = ReleaseCallback;
  pHotspot->m_DragFlags = Flags;
  if (!*sPluginID)
    {
    pHotspot->m_dispid_MoveCallback = dispidMove;
    pHotspot->m_dispid_ReleaseCallback = dispidRelease;
    }

  return eOK;


  } // end of CMiniWindow::DragHandler


// changes hotspot tooltip text
long CMiniWindow::HotspotTooltip(LPCTSTR HotspotId,
                             LPCTSTR TooltipText)
  {

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it == m_Hotspots.end ())
    return eHotspotNotInstalled;

  CHotspot * pHotspot = it->second;

  pHotspot->m_sTooltipText      = TooltipText;

  return eOK;
  }    // end of CMiniWindow::HotspotTooltip

long CMiniWindow::DrawImageAlpha(LPCTSTR ImageId,
                    long Left, long Top, long Right, long Bottom,
                    double Opacity,
                    long SrcLeft, long SrcTop,
                    long SrcRight, long SrcBottom, bool Scale)
  {
  if (!NormalizeBlendOpacity (Opacity))
    return eBadParameter;
  ImageMapIterator it = m_Images.find (ImageId);
  if (it == m_Images.end ())
    return eImageNotInstalled;
  CBitmap * bitmap = it->second.get ();
  BITMAP bi = {};
  if (!bitmap->GetBitmap (&bi))
    return eUnableToLoadImage;

  long width, height, source_width, source_height;
  long left, top, visible_width, visible_height;
  if (Scale)
    {
    if (SrcRight <= 0)
      SrcRight = bi.bmWidth + SrcRight;
    if (SrcBottom <= 0)
      SrcBottom = bi.bmHeight + SrcBottom;
    if (SrcLeft < 0 || SrcTop < 0 || SrcRight > bi.bmWidth || SrcBottom > bi.bmHeight
        || SrcLeft >= SrcRight || SrcTop >= SrcBottom)
      return eBadParameter;
    source_width = SrcRight - SrcLeft;
    source_height = SrcBottom - SrcTop;

    const long right = FixRight (Right), bottom = FixBottom (Bottom);
    const __int64 w = static_cast<__int64> (right) - Left;
    const __int64 h = static_cast<__int64> (bottom) - Top;
    if (w <= 0 || h <= 0)
      return eOK;
    if (w > INT_MAX || h > INT_MAX)
      return eBadParameter;
    width = static_cast<long> (w);
    height = static_cast<long> (h);
    left = max (Left, 0L);
    top = max (Top, 0L);
    const long clipped_right = min (right, static_cast<long> (m_iWidth));
    const long clipped_bottom = min (bottom, static_cast<long> (m_iHeight));
    if (clipped_right <= left || clipped_bottom <= top)
      return eOK;
    visible_width = clipped_right - left;
    visible_height = clipped_bottom - top;
    }
  else
    {
    if (bi.bmBitsPixel != 32)
      return eImageNotInstalled;
    left = Left = max (Left, 0L);
    top = Top = max (Top, 0L);
    SrcLeft = max (SrcLeft, 0L);
    SrcTop = max (SrcTop, 0L);
    const long right = min (FixRight (Right), static_cast<long> (m_iWidth));
    const long bottom = min (FixBottom (Bottom), static_cast<long> (m_iHeight));
    if (left >= right || top >= bottom || SrcLeft >= bi.bmWidth || SrcTop >= bi.bmHeight)
      return eOK;
    width = source_width = visible_width = min (right - left, bi.bmWidth - SrcLeft);
    height = source_height = visible_height = min (bottom - top, bi.bmHeight - SrcTop);
    }
  if (Opacity == 0.0)
    return eOK;

  MiniWindowAlphaScratch storage, target;
  MiniWindowAlphaImage crop (NULL, source_width, source_height, 0);
  const MiniWindowAlphaImage * image = AlphaSource (pdc, *it->second,
    bi, Scale, SrcLeft, SrcTop, crop, storage);
  if (!image)
    return eUnableToLoadImage;

  const bool resized = source_width != width || source_height != height;
  // Do not index a GDI crop until draw() flushes the pending pixel copies.
  if (resized && !storage.pixels && image->PrepareRuns () && image->runs.empty ())
    return eOK;

  const HDC destination = pdc->GetSafeHdc ();
  const auto draw = [&] (long x, long y, long w, long h) -> long
    {
    if (!target.CopyFrom (destination, x, y, w, h) || !::GdiFlush ())
      return eUnableToLoadImage;
    image->BlendScaled (target.pixels, target.width * 4, SrcLeft, SrcTop,
      source_width, source_height, width, height, x - Left, y - Top, w, h, Opacity);
    return ::BitBlt (destination, x, y, w, h, target.DC (), 0, 0, SRCCOPY)
      ? eOK : eUnableToLoadImage;
    };

  if (resized)
    return draw (left, top, visible_width, visible_height);

  const long sx = SrcLeft + (left - Left), sy = SrcTop + (top - Top);
  // At 1:1, transfer only visible rectangles when the index fits.
  if (image == &crop || !image->PrepareRectangles ())
    {
    if (!image->ignore_alpha)
      {
      if (storage.pixels && !::GdiFlush ())
        return eUnableToLoadImage;
      if (!image->HasVisiblePixels (sx, sy, visible_width, visible_height))
        return eOK;
      }
    return draw (left, top, visible_width, visible_height);
    }

  struct Transfer
    {
    long x, y, width, height;
    MiniWindowAlphaScratch * scratch;
    int scratch_y;
    size_t backup_offset;
    };
  const bool multiple = image->rectangles.size () > 1;
  std::vector<Transfer> transfers;
  if (multiple) transfers.reserve (image->rectangles.size ());
  for (const MiniWindowAlphaImage::Rect & r : image->rectangles)
    {
    const long x = max (sx, static_cast<long> (r.left));
    const long y = max (sy, static_cast<long> (r.top));
    const long right = min (sx + visible_width, static_cast<long> (r.right));
    const long bottom = min (sy + visible_height, static_cast<long> (r.bottom));
    if (x < right && y < bottom)
      {
      const Transfer transfer = {left + (x - sx), top + (y - sy), right - x, bottom - y, NULL, 0, 0};
      if (!multiple)
        return image->HasVisiblePixels (x, y, transfer.width, transfer.height)
          ? draw (transfer.x, transfer.y, transfer.width, transfer.height) : eOK;
      transfers.push_back (transfer);
      }
    }

  if (transfers.empty ()) return eOK;
  if (transfers.size () == 1)
    {
    const Transfer & transfer = transfers.front ();
    return draw (transfer.x, transfer.y, transfer.width, transfer.height);
    }

  // Stack rectangles so the drawing bitmap needs at most twice the area
  // it contains. Keep a compact backup of only the pixels to be painted.
  std::sort (transfers.begin (), transfers.end (), [] (const Transfer & a, const Transfer & b)
    { return a.width > b.width; });
  size_t backup_bytes = 0;
  std::vector<std::unique_ptr<MiniWindowAlphaScratch>> extra_scratch;
  for (size_t first = 0; first < transfers.size (); )
    {
    const long stack_width = transfers [first].width;
    const size_t max_height = MAXDWORD / (static_cast<unsigned __int64> (stack_width) * 4);
    size_t last = first, stack_height = 0;
    unsigned __int64 stack_pixels = 0;
    for (; last < transfers.size (); ++last)
      {
      Transfer & t = transfers [last];
      const unsigned __int64 pixels = static_cast<unsigned __int64> (t.width) * t.height;
      if (static_cast<size_t> (t.height) > max_height - stack_height
          || stack_width * (stack_height + t.height) > 2 * (stack_pixels + pixels))
        break;
      if (pixels * 4 > static_cast<size_t> (-1) - backup_bytes)
        return eUnableToLoadImage;
      t.scratch_y = static_cast<int> (stack_height);
      t.backup_offset = backup_bytes;
      stack_height += t.height;
      stack_pixels += pixels;
      backup_bytes += static_cast<size_t> (pixels * 4);
      }
    if (first == last)
      return eUnableToLoadImage;

    MiniWindowAlphaScratch * scratch = &target;
    if (first != 0)
      {
      extra_scratch.push_back (std::make_unique<MiniWindowAlphaScratch> ());
      scratch = extra_scratch.back ().get ();
      }
    if (!scratch->Ensure (stack_width, static_cast<int> (stack_height)))
      return eUnableToLoadImage;
    for (; first < last; ++first)
      transfers [first].scratch = scratch;
    }
  std::unique_ptr<unsigned char[]> backup (new (std::nothrow) unsigned char [backup_bytes]);
  if (!backup)
    return eUnableToLoadImage;

  // Finish every read before changing the window.
  for (const Transfer & t : transfers)
    if (!::BitBlt (t.scratch->DC (), 0, t.scratch_y, t.width, t.height, destination, t.x, t.y, SRCCOPY))
      return eUnableToLoadImage;
  if (!::GdiFlush ())
    return eUnableToLoadImage;

  const auto copy_pixels = [] (unsigned char * destination, int destination_stride,
                               const unsigned char * source, int source_stride, int width, int height)
    {
    const size_t row_bytes = static_cast<size_t> (width) * 4;
    if (row_bytes == static_cast<size_t> (destination_stride) && row_bytes == static_cast<size_t> (source_stride))
      {
      memcpy (destination, source, row_bytes * height);
      return;
      }
    for (int y = 0; y < height; ++y)
      memcpy (destination + static_cast<size_t> (y) * destination_stride,
              source + static_cast<size_t> (y) * source_stride, row_bytes);
    };
  for (const Transfer & t : transfers)
    {
    const int stride = t.scratch->width * 4;
    unsigned char * pixels = t.scratch->pixels + static_cast<size_t> (t.scratch_y) * stride;
    copy_pixels (backup.get () + t.backup_offset, t.width * 4, pixels, stride, t.width, t.height);
    image->Blend (pixels, stride, sx + (t.x - left), sy + (t.y - top), t.width, t.height, Opacity);
    }

  const auto put = [&] (const Transfer & t)
    {
    return ::BitBlt (destination, t.x, t.y, t.width, t.height, t.scratch->DC (), 0, t.scratch_y, SRCCOPY);
    };
  size_t written = 0;
  for (; written < transfers.size (); ++written)
    if (!put (transfers [written]))
      break;
  const bool flushed = ::GdiFlush ();
  if (written == transfers.size () && flushed)
    return eOK;

  // Restore every attempted rectangle, including the one whose write failed.
  const size_t restore = min (written + 1, transfers.size ());
  for (size_t i = 0; i < restore; ++i)
    {
    const Transfer & t = transfers [i];
    const int stride = t.scratch->width * 4;
    unsigned char * pixels = t.scratch->pixels + static_cast<size_t> (t.scratch_y) * stride;
    copy_pixels (pixels, stride, backup.get () + t.backup_offset, t.width * 4, t.width, t.height);
    }
  for (size_t i = 0; i < restore; ++i)
    put (transfers [i]);
  ::GdiFlush ();
  return eUnableToLoadImage;
  } // end of  CMiniWindow::DrawImageAlpha

long CMiniWindow::GetImageAlpha(LPCTSTR ImageId,
                    long Left, long Top, long Right, long Bottom,
                    long SrcLeft, long SrcTop)
  {

  // constrain to what we actually have
  if (Left < 0)
    Left = 0;
  if (Top < 0)
    Top = 0;
  if (Right > m_iWidth)
    Right = m_iWidth;
  if (Bottom > m_iHeight)
    Bottom = m_iHeight;

  // image to be merged
  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * bitmap = it->second.get ();


  BITMAP bi = {};
  if (!bitmap->GetBitmap (&bi))
    return eUnableToLoadImage;

  // can't do it unless we have alpha channel
  if (bi.bmBitsPixel != 32)
    return eImageNotInstalled;

  // calculate size of desired rectangle
  const long right = FixRight (Right), bottom = FixBottom (Bottom);
  if (right <= Left || bottom <= Top)
    return eOK;
  const long iWidth = right - Left, iHeight = bottom - Top;

  // constrain to what we actually have
  if (SrcLeft < 0)
    SrcLeft = 0;
  if (SrcTop < 0)
    SrcTop = 0;

  CDC source;
  if (!source.CreateCompatibleDC (pdc))
    return eUnableToLoadImage;
  MiniWindowSelectedObject selected (&source, bitmap);
  MiniWindowAlphaScratch scratch;
  if (!selected.Valid () || !scratch.Ensure (iWidth, iHeight))
    return eUnableToLoadImage;

  // A fresh DIB is black outside the source crop.
  const long w = min (iWidth, bi.bmWidth - SrcLeft);
  const long h = min (iHeight, bi.bmHeight - SrcTop);
  if (w > 0 && h > 0)
    {
    if (!::BitBlt (scratch.DC (), 0, 0, w, h, source.GetSafeHdc (), SrcLeft, SrcTop, SRCCOPY)
        || !::GdiFlush ())
      return eUnableToLoadImage;
    for (int y = 0; y < h; ++y)
      {
      unsigned char * pixel = scratch.pixels + static_cast<size_t> (y) * scratch.width * 4;
      for (int x = 0; x < w; ++x, pixel += 4)
        {
        pixel [0] = pixel [1] = pixel [2] = pixel [3];
        pixel [3] = 0;
        }
      }
    }
  return ::BitBlt (pdc->GetSafeHdc (), Left, Top, iWidth, iHeight,
                   scratch.DC (), 0, 0, SRCCOPY) ? eOK : eUnableToLoadImage;
  } // end of  CMiniWindow::GetImageAlpha


long CMiniWindow::ScrollwheelHandler(CMUSHclientDoc * pDoc,
                                     LPCTSTR HotspotId,
                                     LPCTSTR sPluginID,
                                     LPCTSTR MoveCallback)
  {

  if (InvalidHotspotCallback (MoveCallback))
    return eInvalidObjectLabel;

  // can't switch plugins here :)
  if (!m_sCallbackPlugin.empty () && m_sCallbackPlugin != sPluginID)
    return eHotspotPluginChanged;

  HotspotMapIterator first = m_Hotspots.find (HotspotId);
  if (first == m_Hotspots.end ())
    return eHotspotNotInstalled;   // no such hotspot

  DISPID dispidScrollwheel = DISPID_UNKNOWN;

  // if not in a plugin, look in main world for hotspot callbacks, and remember the dispatch ID
  if (!*sPluginID)
    {
    CBoolStateGuard executingGuard (m_bExecutingScript, true);
    CString strErrorMessage;

    dispidScrollwheel = pDoc->GetProcedureDispid (MoveCallback, "scroll wheel", "", strErrorMessage);
    }

  // As with drag handlers, lookup must finish before borrowing the hotspot.
  if (!m_sCallbackPlugin.empty () && m_sCallbackPlugin != sPluginID)
    return eHotspotPluginChanged;
  HotspotMapIterator it = *sPluginID ? first : m_Hotspots.find (HotspotId);
  if (it == m_Hotspots.end ())
    return eHotspotNotInstalled;

  CHotspot * pHotspot = it->second;
  pHotspot->m_sScrollwheelCallback = MoveCallback;
  if (!*sPluginID)
    pHotspot->m_dispid_ScrollwheelCallback = dispidScrollwheel;

  return eOK;


  } // end of CMiniWindow::ScrollwheelHandler


// resize a window

long CMiniWindow::Resize(long Width, long Height, long BackgroundColour)
  {

  // no change to size? wow, that was easy ...
  if (Width == m_iWidth && Height == m_iHeight)
    return eOK;


  CDC bmDC;   // for loading bitmaps into
  CBitmap tempBitmap;
  CBitmap * pOldBmDCBitmap = NULL;
  CBitmap * pOldWindowDCBitmap = NULL;
  CBitmap * pTempWindowDCBitmap = NULL;
  CBitmap * previousWindowBitmap = m_Bitmap;
  std::unique_ptr<CBitmap> pNewBitmap (new CBitmap);

  try
    {
    if (!bmDC.CreateCompatibleDC(&(*pdc)))
      AfxThrowResourceException ();
    if (!tempBitmap.CreateCompatibleBitmap (&(*pdc), 1, 1))
      AfxThrowResourceException ();

    // CreateBitmap with zero dimensions creates a monochrome bitmap, so force at least 1x1.
    if (!pNewBitmap->CreateBitmap (MAX (Width, 1), MAX (Height, 1), 1,
                                   GetDeviceCaps((*pdc), BITSPIXEL), NULL))
      AfxThrowResourceException ();

    // Unselect the old bitmap before selecting it into the copy DC.
    pOldWindowDCBitmap = pdc->SelectObject (&tempBitmap);
    if (!pOldWindowDCBitmap)
      AfxThrowResourceException ();
    pOldBmDCBitmap = bmDC.SelectObject (previousWindowBitmap);
    if (!pOldBmDCBitmap)
      AfxThrowResourceException ();

    pTempWindowDCBitmap = pdc->SelectObject (pNewBitmap.get ());
    if (!pTempWindowDCBitmap)
      AfxThrowResourceException ();
    pdc->SetWindowOrg(0, 0);

    // fill with requested fill colour
    pdc->FillSolidRect (0, 0, Width, Height, BackgroundColour);

    // copy old contents back
    if (!pdc->BitBlt (0, 0, m_iWidth, m_iHeight, &bmDC, 0, 0, SRCCOPY))
      AfxThrowResourceException ();

    if (!bmDC.SelectObject(pOldBmDCBitmap))
      AfxThrowResourceException ();
    pOldBmDCBitmap = NULL;
    }
  catch (...)
    {
    if (pOldBmDCBitmap)
      bmDC.SelectObject (pOldBmDCBitmap);
    if (pTempWindowDCBitmap)
      pdc->SelectObject (pTempWindowDCBitmap);
    if (pOldWindowDCBitmap)
      pdc->SelectObject (pOldWindowDCBitmap);
    throw;
    }

  m_Bitmap = pNewBitmap.release ();

  // done with previous bitmap from this miniwindow
  previousWindowBitmap->DeleteObject ();
  delete previousWindowBitmap;

  // remember new width and height

  m_iWidth  = Width ;
  m_iHeight = Height;

  return eOK;

  } // end of CMiniWindow::Resize



// move a hotspot (maybe the window was resized)

long CMiniWindow::MoveHotspot(LPCTSTR HotspotId,
                             long Left, long Top, long Right, long Bottom)
  {

  HotspotMapIterator it = m_Hotspots.find (HotspotId);

  if (it == m_Hotspots.end ())
    return eHotspotNotInstalled;

  it->second->m_rect = CRect (Left, Top, FixRight (Right), FixBottom (Bottom));

  return eOK;
  }    // end of CMiniWindow::MoveHotspot



long CMiniWindow::TransformImage(LPCTSTR ImageId, float Left, float Top, short Mode, float Mxx, float Mxy, float Myx, float Myy)
  {

  ImageMapIterator it = m_Images.find (ImageId);

  if (it == m_Images.end ())
    return eImageNotInstalled;

  CBitmap * bitmap = it->second.get ();

  pdc->SetBkMode (TRANSPARENT);

  BITMAP  bi;
  bitmap->GetBitmap(&bi);

  CDC bmDC;
  bmDC.CreateCompatibleDC(&(*pdc));
  CBitmap *pOldbmp = bmDC.SelectObject(bitmap);

  long iWidth  = bi.bmWidth;
  long iHeight = bi.bmHeight;

  if (iWidth <= 0 || iHeight <= 0)   // Sanity Claus
    {
    bmDC.SelectObject(pOldbmp);
    return eOK;
    }

  // need advanced mode to do SetWorldTransform successfully
  if (SetGraphicsMode (pdc->m_hDC, GM_ADVANCED) == 0)
    {
    bmDC.SelectObject (pOldbmp);
    return eBadParameter;
    }

  // x' = x * Mxx + y * Mxy + Left
  // y' = x * Myx + y * Myy + Top

  XFORM xform;
  xform.eM11 = Mxx;       // cosine
  xform.eM21 = Mxy;       // sine
  xform.eM12 = Myx;       // - sine
  xform.eM22 = Myy;       // cosine
  xform.eDx = Left;
  xform.eDy = Top;

  if (SetWorldTransform (pdc->m_hDC, &xform) == 0)
    {
    bmDC.SelectObject(pOldbmp);
    return eBadParameter;
    }

  // gives smoother rotations, especially with transparency
  pdc->SetStretchBltMode (HALFTONE);
  SetBrushOrgEx(pdc->m_hDC, 0, 0, NULL);  // as recommended after  SetStretchBltMode

  switch (Mode)
    {
    case 1: pdc->BitBlt (0, 0, iWidth, iHeight, &bmDC, 0, 0, SRCCOPY);
            break;      // straight copy

    case 3:        // transparency, nom nom nom!
      {
      COLORREF crOldBack = pdc->SetBkColor (RGB (255, 255, 255));    // white
      COLORREF crOldText = pdc->SetTextColor (RGB (0, 0, 0));        // black
      CDC dcTrans;   // transparency mask


      // Create a memory (*pdc) for the mask
      dcTrans.CreateCompatibleDC(&(*pdc));

      // Create the mask bitmap
      CBitmap bitmapTrans;
      bitmapTrans.CreateBitmap(m_iWidth, m_iHeight, 1, 1, NULL);

      // Select the mask bitmap into the appropriate (*pdc)
      CBitmap* pOldBitmapTrans = dcTrans.SelectObject(&bitmapTrans);

      // Our transparent pixel will be at 0,0 (top left corner) of original image
      COLORREF crOldBackground = bmDC.SetBkColor (::GetPixel (bmDC, 0, 0));

      // Build mask based on transparent colour at location 0, 0
      dcTrans.BitBlt (0, 0, iWidth, iHeight, &bmDC, 0, 0, SRCCOPY);

      // Do the work
      pdc->BitBlt (0, 0, iWidth, iHeight, &bmDC,    0, 0, SRCINVERT);
      pdc->BitBlt (0, 0, iWidth, iHeight, &dcTrans, 0, 0, SRCAND);
      pdc->BitBlt (0, 0, iWidth, iHeight, &bmDC,    0, 0, SRCINVERT);

      // Restore settings
      dcTrans.SelectObject(pOldBitmapTrans);
      pdc->SetBkColor(crOldBack);
      pdc->SetTextColor(crOldText);
      bmDC.SetBkColor(crOldBackground);
      }
      break;

    default:
      bmDC.SelectObject(pOldbmp);
      ModifyWorldTransform(pdc->m_hDC, &xform, MWT_IDENTITY);
      SetGraphicsMode (pdc->m_hDC, GM_COMPATIBLE);
      return eBadParameter;

    } // end of switch

  bmDC.SelectObject(pOldbmp);

  // reset to identity transformation
  ModifyWorldTransform(pdc->m_hDC, &xform, MWT_IDENTITY);
  SetGraphicsMode (pdc->m_hDC, GM_COMPATIBLE);

  return eOK;
  }   // end of CMiniWindow::TransformImage


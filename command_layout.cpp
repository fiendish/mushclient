#include "stdafx.h"
#include "command_layout.h"
#include <usp10.h>
#include <algorithm>

// Keep each nested native message paired with its own editor and action.
static thread_local CommandWordBreakContext commandWordBreakContext = {};

CommandWordBreakScope::CommandWordBreakScope(HWND window, EDITWORDBREAKPROC original,
    UINT message, WPARAM key) : saved(commandWordBreakContext)
  {
  commandWordBreakContext = {window, original, message, key,
      saved.failure ? saved.failure : &failure};
  }
CommandWordBreakScope::~CommandWordBreakScope() { commandWordBreakContext = saved; }

void CommandWordBreakScope::RethrowFailure() const
  {
  if (failure)
    std::rethrow_exception(failure);
  }

static void CheckCommandTextResult(HRESULT result)
  {
  if (FAILED(result))
    AfxThrowOleException(result);
  }

class CommandTextMeasurement
  {
  HWND window;
  HDC dc;
  HGDIOBJ oldFont;
  SCRIPT_TABDEF tabs;
public:
  explicit CommandTextMeasurement(HWND hwnd) : window(hwnd), tabs()
    {
    dc = ::GetDC(window);
    if (!dc)
      AfxThrowResourceException();
    oldFont = ::SelectObject(dc, (HFONT) ::SendMessage(window, WM_GETFONT, 0, 0));
    if (!oldFont || oldFont == HGDI_ERROR)
      {
      ::ReleaseDC(window, dc);
      AfxThrowResourceException();
      }
    // Match the edit control's dialog-unit scale for its default tab stops.
    SIZE alphabet;
    if (!::GetTextExtentPoint32W(dc,
        L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ", 52, &alphabet))
      {
      ::SelectObject(dc, oldFont);
      ::ReleaseDC(window, dc);
      AfxThrowResourceException();
      }
    tabs.iScale = (alphabet.cx / 26 + 1) / 2;
    }
  ~CommandTextMeasurement()
    {
    ::SelectObject(dc, oldFont);
    ::ReleaseDC(window, dc);
    }
  int Width(const WCHAR * text, int length)
    {
    if (!length)
      return 0;
    SCRIPT_STRING_ANALYSIS analysis = NULL;
    CheckCommandTextResult(::ScriptStringAnalyse(dc, text, length,
        length * 3 / 2 + 16, -1, SSA_LINK | SSA_FALLBACK | SSA_GLYPHS | SSA_TAB,
        -1, NULL, NULL, NULL, &tabs, NULL, &analysis));
    int width = ::ScriptString_pSize(analysis)->cx;
    CheckCommandTextResult(::ScriptStringFree(&analysis));
    return width;
    }
  };

static int CommandWordBreak(std::vector<WCHAR> & text, int index, int action,
    LPSTR bytes, int byteCount)
  {
  HWND hwnd = commandWordBreakContext.window;
  ASSERT(hwnd);
  bool navigation = (commandWordBreakContext.message == WM_KEYDOWN &&
      (commandWordBreakContext.key == VK_LEFT || commandWordBreakContext.key == VK_RIGHT)) ||
      commandWordBreakContext.message == WM_LBUTTONDBLCLK;
  int length = (int) text.size() - 1;
  int base = 0, limit = length;
  if (navigation)
    {
    // Wine's default callback searches the whole command. Keep that behavior
    // when a word starts before the current visual line.
    text.resize(::GetWindowTextLengthW(hwnd) + 1);
    length = ::GetWindowTextW(hwnd, &text[0], (int) text.size());
    base = (int) ::SendMessage(hwnd, EM_LINEINDEX, (WPARAM) -1, 0);
    ASSERT(base >= 0);
    index += base;
    limit += base;
    }
  if (!length)
    return 0;
  SCRIPT_ANALYSIS script = {};
  script.eScript = SCRIPT_UNDEFINED;
  std::vector<SCRIPT_LOGATTR> attributes(length);
  CheckCommandTextResult(::ScriptBreak(&text[0], length, &script, &attributes[0]));
  if (navigation || action != WB_LEFT)
    {
    switch (action)
      {
      case WB_LEFT:
        if (index)
          --index;
        while (index > 0 && !attributes[index].fSoftBreak)
          --index;
        break;
      case WB_RIGHT:
        while (index < limit && text[index] && !attributes[index].fSoftBreak)
          ++index;
        break;
      case WB_ISDELIMITER:
        return attributes[index].fWhiteSpace;
      }
    return index - base;
    }

  RECT rect;
  ::SendMessage(hwnd, EM_GETRECT, 0, (LPARAM) &rect);
  CommandTextMeasurement measurement(hwnd);
  // Measure candidate prefixes with the same shaping and tabs as the editor.
  std::vector<int> stops(1, 0);
  for (int position = 1; position <= length; ++position)
    if (position == length || !IS_LOW_SURROGATE(text[position]) ||
        !IS_HIGH_SURROGATE(text[position - 1]))
      stops.push_back(position);
  // Bound the search near one line before measuring larger prefixes.
  int low = 0, high = 1, last = (int) stops.size() - 1;
  while (high < last &&
      measurement.Width(&text[0], stops[high]) <= rect.right - rect.left)
    {
    low = high;
    high = high > last / 2 ? last : high * 2;
    }
  while (low < high)
    {
    int middle = low + (high - low + 1) / 2;
    if (measurement.Width(&text[0], stops[middle]) <= rect.right - rect.left)
      low = middle;
    else
      high = middle - 1;
    }
  int fit = stops[low];
  // A glyph that cannot fit must reach Wine's forced-break path. Returning one
  // repeatedly would prevent its wrapping loop from making progress.
  if (!fit)
    return 0;
  int boundary = fit;
  if (commandWordBreakContext.original)
    boundary = commandWordBreakContext.original(bytes, fit + 1, byteCount, WB_LEFT);
  else
    while (boundary > 0 && boundary < length && !attributes[boundary].fSoftBreak)
      --boundary;
  // A long word has no fitting word boundary. Return its fitting prefix so
  // Wine does not enter the faulty forced-character-wrap path.
  return (std::max)(1, boundary > 0 ? boundary : fit);
  }


int CALLBACK WineCommandWordBreak(LPSTR bytes, int index, int byteCount, int action)
  {
  if (!*commandWordBreakContext.failure)
    {
    try
      {
      bool navigation = (commandWordBreakContext.message == WM_KEYDOWN &&
          (commandWordBreakContext.key == VK_LEFT || commandWordBreakContext.key == VK_RIGHT)) ||
          commandWordBreakContext.message == WM_LBUTTONDBLCLK;
      if (commandWordBreakContext.original && (navigation || action != WB_LEFT))
        return commandWordBreakContext.original(bytes, index, byteCount, action);
      int length = ::MultiByteToWideChar(CP_ACP, 0, bytes, byteCount, NULL, 0);
      if (byteCount && !length)
        AfxThrowResourceException();
      std::vector<WCHAR> text(length + 1);
      if (length && ::MultiByteToWideChar(CP_ACP, 0, bytes, byteCount,
          &text[0], length) != length)
        AfxThrowResourceException();
      return CommandWordBreak(text, index, action, bytes, byteCount);
      }
    catch (...)
      {
      *commandWordBreakContext.failure = std::current_exception();
      }
    }
  // Let Wine finish its native cleanup before the outer message rethrows.
  // Zero selects its forced-break path for wrapping.
  return 0;
  }

// Map native character positions to the legacy bytes returned by GetCommand.
static int PositionFromByte(const std::vector<int> & offsets, int position)
  {
  if (position < 0)
    return position;
  position = (std::min)(position, offsets.back());
  std::vector<int>::const_iterator found =
      std::lower_bound(offsets.begin(), offsets.end(), position);
  int index = (int) (found - offsets.begin());
  if (*found != position)
    --index;
  while (index > 0 && offsets[index] == offsets[index - 1])
    --index;
  return index;
  }

static void EncodeCommandText(const std::wstring & text, std::string & bytes,
    std::vector<int> & offsets)
  {
  bytes.clear();
  offsets.assign(1, 0);
  for (size_t position = 0; position < text.size(); )
    {
    int count = IS_HIGH_SURROGATE(text[position]) && position + 1 < text.size() &&
        IS_LOW_SURROGATE(text[position + 1]) ? 2 : 1;
    char value[8];
    int length = ::WideCharToMultiByte(CP_ACP, 0, &text[position], count,
        value, sizeof(value), NULL, NULL);
    if (!length)
      AfxThrowResourceException();
    if (count == 2)
      offsets.push_back((int) bytes.size());
    bytes.append(value, length);
    offsets.push_back((int) bytes.size());
    position += count;
    }
  }

CCommandLayout::CCommandLayout(HWND window) : source(window),
    nativeByteIndices(!::IsWindowUnicode(window) &&
        !::GetProcAddress(::GetModuleHandleA("ntdll.dll"), "wine_get_version")), nativeBytes(1, 0)
  {
  }

void CCommandLayout::Update()
  {
  int length = ::GetWindowTextLengthA(source);
  std::string newBytes(length + 1, 0);
  newBytes.resize(::GetWindowTextA(source, &newBytes[0], length + 1));
  std::wstring value;
  if (!nativeByteIndices)
    {
    int count = ::GetWindowTextLengthW(source);
    value.resize(count + 1);
    value.resize(::GetWindowTextW(source, &value[0], count + 1));
    }
  if (bytes != newBytes || sourceText != value)
    {
    if (!nativeByteIndices)
      EncodeCommandText(value, newBytes, nativeBytes);
    bytes.swap(newBytes);
    sourceText.swap(value);
    }
  }

int CCommandLayout::ByteFromNative(int position) const
  {
  // Windows ANSI edit messages use byte indices; Wine uses UTF-16 indices.
  if (nativeByteIndices)
    return position < 0 ? position : (std::min)(position, (int) bytes.size());
  return position < 0 ? position : nativeBytes[(std::min)(position, (int) nativeBytes.size() - 1)];
  }

int CCommandLayout::NativeFromByte(int position) const
  {
  if (nativeByteIndices)
    return position < 0 ? position : (std::min)(position, (int) bytes.size());
  return PositionFromByte(nativeBytes, position);
  }

std::vector<int> CCommandLayout::LineStarts() const
  {
  int count = (int) ::SendMessage(source, EM_GETLINECOUNT, 0, 0);
  std::vector<int> starts;
  for (int line = 0; line < count; ++line)
    {
    int position = (int) ::SendMessage(source, EM_LINEINDEX, line, 0);
    int byte = ByteFromNative(position);
    if (starts.empty() || starts.back() != byte)
      starts.push_back(byte);
    }
  return starts;
  }

#pragma once

struct CommandWordBreakContext
  {
  HWND window;
  EDITWORDBREAKPROC original;
  UINT message;
  WPARAM key;
  };

class CommandWordBreakScope
  {
  CommandWordBreakContext saved;
public:
  CommandWordBreakScope(HWND window, EDITWORDBREAKPROC original, UINT message, WPARAM key);
  ~CommandWordBreakScope();
  };

int CALLBACK WineCommandWordBreak(LPSTR bytes, int index, int byteCount, int action);

// The native ANSI editor supplies layout. Public positions use command bytes.
class CCommandLayout
  {
  HWND source;
  bool nativeByteIndices;
  std::wstring sourceText;
  std::string bytes;
  std::vector<int> nativeBytes;
public:
  explicit CCommandLayout(HWND source);
  void Update();
  int ByteFromNative(int position) const;
  int NativeFromByte(int position) const;
  std::vector<int> LineStarts() const;
  int LineCount() const { return (int) LineStarts().size(); }
  };

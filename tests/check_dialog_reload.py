#!/usr/bin/env python3
"""Check deferred reloads with production idle handlers and portable substitutes.

The fixture injects LoadList failures and nested idle messages. It models the
framework exception boundary. It does not run MFC, native controls, or Lua.
Set CXX to select the compiler. Use --revision to check a pinned Git revision.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


FIXTURE = r'''
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
using WPARAM = unsigned long;
using LPARAM = long;
using LRESULT = long;
constexpr unsigned WM_KICKIDLE = 0x036a;
#define CHECK(value) do { if (!(value)) { std::cerr << __LINE__ << ": " << #value << '\n'; std::exit(1); } } while (0)
struct AppType { void* m_pMainWnd = nullptr; } App;
AppType* AfxGetApp() { return &App; }
struct CException {
  int reports = 0, deletes = 0;
  void ReportError() { ++reports; }
  void Delete() { ++deletes; }
};
struct CGenPropertyPage {
  bool m_bReloadList = false;
  int loads = 0, updates = 0, depth = 0, exceptions = 0;
  std::function<void()> duringLoad;
  std::exception_ptr failure, caught;
  LRESULT OnKickIdle(WPARAM, LPARAM);
  void LoadList() {
    CHECK(depth == 0);
    ++loads;
    struct Active {
      int& depth;
      explicit Active(int& value) : depth(value) { ++depth; }
      ~Active() { --depth; }
    } active(depth);
    if (duringLoad) duringLoad();
    if (failure) std::rethrow_exception(failure);
  }
  void UpdateDialogControls(void*, bool) { ++updates; }
  LRESULT SendMessage(unsigned message, WPARAM w, LPARAM l) {
    CHECK(message == WM_KICKIDLE);
    // Model the MFC message boundary, after the handler rethrows the error.
    try { return OnKickIdle(w, l); }
    catch (...) { caught = std::current_exception(); ++exceptions; return 0; }
  }
};
struct CTreePropertySheet {
  CGenPropertyPage* page;
  CGenPropertyPage* GetActivePage() { return page; }
  LRESULT OnKickIdle(WPARAM, LPARAM);
};
struct CPrefsPropertySheet {
  CGenPropertyPage* page;
  CGenPropertyPage* GetActivePage() { return page; }
  LRESULT OnKickIdle(WPARAM, LPARAM);
};
// PRODUCTION METHODS
int main() {
  CGenPropertyPage page;
  CHECK(page.OnKickIdle(0, 0) == 0);
  CHECK(page.loads == 0 && page.updates == 1);
  page.m_bReloadList = true;
  CHECK(page.OnKickIdle(0, 0) == 0);
  CHECK(page.loads == 1 && page.updates == 2 && !page.m_bReloadList);
  std::cout << "Idle and successful reloads keep their existing behavior\n";

  CException resource;
  page.failure = std::make_exception_ptr(&resource);
  page.m_bReloadList = true;
  bool propagated = false;
  try { page.OnKickIdle(0, 0); }
  catch (CException* error) { CHECK(error == &resource); propagated = true; }
  CHECK(propagated && page.m_bReloadList && page.loads == 2);
  CHECK(page.updates == 2 && page.depth == 0);
  CHECK(resource.reports == 0 && resource.deletes == 0);
  page.failure = nullptr;
  page.OnKickIdle(0, 0);
  CHECK(page.loads == 3 && !page.m_bReloadList);
  std::cout << "The original resource exception propagates and a later idle retries\n";

  page.failure = std::make_exception_ptr(std::runtime_error("original failure"));
  page.m_bReloadList = true;
  propagated = false;
  try { page.OnKickIdle(0, 0); }
  catch (const std::runtime_error&) {
    CHECK(std::current_exception() == page.failure);
    propagated = true;
  }
  CHECK(propagated && page.m_bReloadList && page.depth == 0);
  std::cout << "Standard exceptions also retain their identity and reload request\n";

  for (bool fail : {false, true}) {
    CGenPropertyPage nested;
    nested.m_bReloadList = true;
    if (fail) nested.failure = std::make_exception_ptr(&resource);
    nested.duringLoad = [&] {
      CHECK(!nested.m_bReloadList);
      CHECK(nested.OnKickIdle(0, 0) == 0);
      CHECK(nested.loads == 1 && nested.depth == 1);
    };
    nested.SendMessage(WM_KICKIDLE, 0, 0);
    CHECK(nested.loads == 1 && nested.depth == 0);
    CHECK(nested.m_bReloadList == fail && nested.exceptions == int(fail));
    CHECK(!fail || nested.caught == nested.failure);
  }
  std::cout << "Nested idle messages do not repeat the active reload\n";

  CGenPropertyPage requested;
  requested.m_bReloadList = true;
  requested.duringLoad = [&] { requested.m_bReloadList = true; };
  requested.OnKickIdle(0, 0);
  CHECK(requested.loads == 1 && requested.m_bReloadList);
  requested.duringLoad = nullptr;
  requested.OnKickIdle(0, 0);
  CHECK(requested.loads == 2 && !requested.m_bReloadList);
  std::cout << "A new request during a successful reload remains pending\n";

  CGenPropertyPage persistent;
  persistent.m_bReloadList = true;
  persistent.failure = std::make_exception_ptr(&resource);
  CTreePropertySheet tree{&persistent};
  CPrefsPropertySheet tabs{&persistent};
  for (int event = 1; event <= 6; ++event) {
    CHECK((event % 2 ? tree.OnKickIdle(0, 0) : tabs.OnKickIdle(0, 0)) == 0);
    CHECK(persistent.loads == event && persistent.exceptions == event);
    CHECK(persistent.m_bReloadList && persistent.depth == 0);
    CHECK(persistent.caught == persistent.failure);
  }
  persistent.failure = nullptr;
  CHECK(tree.OnKickIdle(0, 0) == 0);
  CHECK(persistent.loads == 7 && !persistent.m_bReloadList);
  CHECK(tabs.OnKickIdle(0, 0) == 0 && persistent.loads == 7);
  CHECK(resource.reports == 0 && resource.deletes == 0);
  std::cout << "Both sheet handlers return zero and try once per delivered idle message\n";
}
'''


def method(source, name):
    start = source.index(f'LRESULT {name}(WPARAM, LPARAM)')
    end = source.index('\n  } // end of ', start) + len('\n  }')
    return source[start:end]


def run(args, output):
    def source(path):
        if args.revision:
            return subprocess.check_output(
                ['git', '-C', str(args.source), 'show', f'{args.revision}:{path}'], text=True)
        return (args.source / path).read_text()

    methods = [method(source(f'dialogs/world_prefs/{file}.cpp'), f'{name}::OnKickIdle')
               for file, name in [('genpropertypage', 'CGenPropertyPage'),
                                  ('TreePropertySheet', 'CTreePropertySheet'),
                                  ('PrefsPropertySheet', 'CPrefsPropertySheet')]]
    cpp = output / 'dialog_reload.cpp'
    cpp.write_text(FIXTURE.replace('// PRODUCTION METHODS', '\n'.join(methods)))
    command = shlex.split(os.environ['CXX']) if 'CXX' in os.environ else [
        shutil.which('clang++') or shutil.which('c++')]
    if not command or not command[0]:
        raise ValueError('No C++ compiler found. Set CXX or install clang++ or c++.')
    executable = output / 'dialog_reload'
    subprocess.run(command + ['-std=c++17', '-O1', '-g', '-Wall', '-Wextra',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-fno-omit-frame-pointer', str(cpp), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    (output / 'dialog_reload.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='', flush=True)
    result.check_returncode()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--revision')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        args.output = args.output.resolve()
        args.output.mkdir(parents=True, exist_ok=True)
        run(args, args.output)
    else:
        with tempfile.TemporaryDirectory(prefix='mushclient-dialog-reload-') as directory:
            run(args, Path(directory))

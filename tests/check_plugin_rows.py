#!/usr/bin/env python3
"""Check plugin row refresh with extracted production methods and row members.

MFC controls, plugin storage, document registration, and file editing are
substitutes. This check does not run native dialogs, XML reloads, or Lua.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def block(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    depth = 0
    for end in range(brace, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[start:end + 1]
    raise ValueError(f'Unterminated source block: {marker}')


FIXTURE = r'''
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
using namespace std;
// Keep fixture checks active when CXX defines NDEBUG.
#define CHECK(value) do { if (!(value)) { cerr << __LINE__ << ": check failed: " << #value << endl; exit(1); } } while (0)
using __int64 = long long;
using LPARAM = intptr_t; using WPARAM = uintptr_t; using LRESULT = intptr_t;
using DWORD = uintptr_t; using POSITION = size_t;
using LPCTSTR = const char*; constexpr bool TRUE=true;
#define CALLBACK
constexpr int IDCANCEL=2, LVIS_FOCUSED=1, LVNI_SELECTED=2;
struct CString : string {
  using string::string;
  bool IsEmpty() const { return empty(); }
  operator const char*() const { return c_str(); }
  int CompareNoCase(const CString& other) const {
    string a=*this, b=other;
    for (auto& c:a) c=tolower(static_cast<unsigned char>(c));
    for (auto& c:b) c=tolower(static_cast<unsigned char>(c));
    return a.compare(b);
  }
};
template<class T, class ARG=T> struct CArray : vector<T> {
  int GetSize() const { return this->size(); }
  void RemoveAll() { this->clear(); }
  int Add(ARG value) { this->push_back(value); return GetSize()-1; }
};
using CStringArray = CArray<CString, const CString&>;
CString CFormat(const char* format, double value) {
  char buffer[64]; snprintf(buffer, sizeof(buffer), format, value); return buffer;
}
CString TFormat(const char* format, const char* name, const char* id) {
  char text[256]; snprintf(text, sizeof(text), format, name, id); return text;
}
struct CPlugin {
  CString m_strID="B", m_strName="Zulu", m_strPurpose="Old purpose";
  CString m_strAuthor="Old author", m_strLanguage="Lua", m_strSource="old.xml";
  bool m_bEnabled=true; double m_dVersion=1;
  __int64 m_iPluginInstanceNumber=(1LL << 40);
};
using PluginListIterator = vector<CPlugin*>::iterator;
struct CMUSHclientDoc {
  __int64 m_iUniqueDocumentNumber=1;
  vector<CPlugin*> m_PluginList;
  function<void(CPlugin*)> onEnable;
  void EnablePlugin(const CString& id, bool enabled) {
    CPlugin* plugin=GetPlugin(id); CHECK(plugin);
    plugin->m_bEnabled=enabled;
    if (onEnable) onEnable(plugin);
  }
  void Note(const CString&) {}
  void PluginListChanged() {}
  CPlugin* GetPlugin(const CString& id) {
    for (auto* plugin:m_PluginList) if (plugin->m_strID==id) return plugin;
    return nullptr;
  }
};
struct DocTemplate {
  CMUSHclientDoc* doc;
  POSITION GetFirstDocPosition() { return doc ? 1 : 0; }
  CMUSHclientDoc* GetNextDoc(POSITION& pos) { pos=0; return doc; }
};
struct AppType { DocTemplate* m_pWorldDocTemplate; void* m_pMainWnd=nullptr; } App;
AppType* AfxGetApp() { return &App; }
struct Row {
  vector<CString> text=vector<CString>(7); DWORD data=0; bool selected=false;
};
struct Control {
  vector<Row> rows; int loads=0;
  int GetItemCount() const { return rows.size(); }
  DWORD GetItemData(int i) const { return rows.at(i).data; }
  void DeleteAllItems() { rows.clear(); ++loads; }
  void InsertItem(int i, const CString& text) {
    rows.insert(rows.begin()+i, Row()); rows.at(i).text[0]=text;
  }
  void SetItemText(int i, int column, const CString& text) { rows.at(i).text.at(column)=text; }
  void SetItemData(int i, DWORD data) { rows.at(i).data=data; }
  void SortItems(int (*compare)(LPARAM, LPARAM, LPARAM), LPARAM context) {
    stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
      return compare(a.data, b.data, context) < 0;
    });
  }
  void SetItemState(int, int, int) {}
  int GetNextItem(int i, int flag) const {
    CHECK(flag==LVNI_SELECTED);
    while (++i<GetItemCount()) if (rows[i].selected) return i;
    return -1;
  }
};
// PRODUCTION COLUMNS
struct CPluginsDlg {
  // PRODUCTION ROW MEMBERS
  int m_last_col=eColumnName; bool m_reverse=false, closed=false;
  Control m_ctlPluginList; vector<CString> edited;
  void EndDialog(int) { closed=true; }
  void UpdateDialogControls(void*, bool) {}
  void EditPlugin(const CString name) { edited.push_back(name); }
  CMUSHclientDoc* GetLiveDocument() const;
  CString GetPluginIDForIndex(int) const;
  CPlugin* GetPluginForIndex(int) const;
  CPlugin* GetPluginForItem(int) const;
  void LoadList(); LRESULT OnKickIdle(WPARAM, LPARAM); void OnEdit(); void OnEnable();
};
// PRODUCTION METHODS
int main() {
  CMUSHclientDoc doc; DocTemplate documents{&doc}; App.m_pWorldDocTemplate=&documents;
  auto old=make_unique<CPlugin>();
  CPlugin other; other.m_strID="A"; other.m_strName="Alpha";
  other.m_strSource="other.xml"; ++other.m_iPluginInstanceNumber;
  doc.m_PluginList={old.get(), &other};
  CPluginsDlg dialog; dialog.m_pDoc=&doc; dialog.m_iDocumentNumber=1;
  auto& list=dialog.m_ctlPluginList;
  dialog.LoadList();
  CHECK(list.GetItemData(0)==1 && list.GetItemData(1)==0);
  CHECK(dialog.GetPluginForItem(0)==&other && dialog.GetPluginForItem(1)==old.get());
  list.rows[0].selected=list.rows[1].selected=true;
  dialog.OnKickIdle(0, 0);
  CHECK(list.loads==1 && list.rows[0].selected && list.rows[1].selected);
  cout << "Unchanged sorted rows preserve selection and do not reload\n" << flush;

  auto replacement=make_unique<CPlugin>(*old);
  replacement->m_iPluginInstanceNumber+=(1LL << 32);
  replacement->m_strName="Aardvark"; replacement->m_strPurpose="New purpose";
  replacement->m_strAuthor="New author"; replacement->m_strLanguage="Python";
  replacement->m_strSource="new.xml"; replacement->m_bEnabled=false; replacement->m_dVersion=2;
  doc.m_PluginList={&other, replacement.get()}; old.reset();
  dialog.OnEdit();
  CHECK((dialog.edited==vector<CString>{"other.xml"}));
  CHECK(list.loads==1); // A stale selection must not act on its replacement before idle.
  CHECK(dialog.GetPluginForItem(1)==nullptr);
  dialog.OnKickIdle(0, 0);
  CHECK((list.rows[0].text==vector<CString>{
    "Aardvark", "New purpose", "New author", "Python", "new.xml", "No", " 2.00"}));
  CHECK(list.loads==2 && list.GetItemData(0)==1);
  CHECK(dialog.GetPluginForItem(0)==replacement.get() && dialog.GetPluginForItem(1)==&other);
  CHECK(dialog.GetPluginForIndex(0)==&other);
  CHECK(list.GetNextItem(-1, LVNI_SELECTED)==-1); // Keep LoadList's selection policy.
  list.rows[0].selected=true; dialog.edited.clear(); dialog.OnEdit();
  CHECK((dialog.edited==vector<CString>{"new.xml"}));
  dialog.OnKickIdle(0, 0);
  CHECK(list.loads==2 && list.rows[0].selected);
  cout << "Same-ID replacement refreshes metadata and sort order; stale selected edits are rejected\n" << flush;

  auto different=make_unique<CPlugin>(*replacement);
  different->m_strID="C"; ++different->m_iPluginInstanceNumber; different->m_dVersion=3;
  doc.m_PluginList={&other, different.get()}; replacement.reset();
  CHECK(dialog.GetPluginForItem(0)==nullptr);
  dialog.OnKickIdle(0, 0);
  CHECK(list.loads==3 && list.rows[0].text[eColumnVersion]==" 3.00");
  CHECK(dialog.GetPluginForItem(0)==different.get());
  cout << "Different-ID replacement still refreshes\n" << flush;

  list.SetItemData(0, 999); dialog.OnKickIdle(0, 0);
  CHECK(list.loads==4 && dialog.GetPluginForItem(0)==different.get());
  list.rows.pop_back(); dialog.OnKickIdle(0, 0);
  CHECK(list.loads==5 && list.GetItemCount()==2);
  CHECK(dialog.GetPluginForIndex(-1)==nullptr);
  CHECK(dialog.GetPluginForIndex(dialog.m_PluginIDs.GetSize())==nullptr);
  dialog.m_PluginInstanceNumbers.pop_back();
  CHECK(dialog.GetPluginForIndex(1)==nullptr);
  dialog.OnKickIdle(0, 0);
  CHECK(list.loads==6 && dialog.GetPluginForIndex(1)==different.get());
  cout << "Missing instance metadata rejects lookup and is rebuilt at idle\n";

  // The first selected plugin's callback replaces the second selected plugin.
  // This occurs inside one command handler, without an intervening idle refresh.
  CPlugin first; first.m_strID="first"; first.m_strName="Alpha"; first.m_bEnabled=false;
  auto second=make_unique<CPlugin>(); second->m_strID="second";
  second->m_strName="Zulu"; second->m_bEnabled=false;
  auto next=make_unique<CPlugin>(*second); ++next->m_iPluginInstanceNumber;
  CPlugin last; last.m_strID="last"; last.m_strName="Zzz"; last.m_bEnabled=false;
  doc.m_PluginList={&first,second.get(),&last}; dialog.LoadList();
  list.rows[0].selected=list.rows[1].selected=list.rows[2].selected=true;
  int calls=0;
  doc.onEnable=[&](CPlugin* plugin) {
    ++calls;
    if (plugin==&first) {doc.m_PluginList[1]=next.get(); second.reset();}
    else CHECK(plugin==&last);
  };
  dialog.OnEnable();
  CHECK(calls==2 && first.m_bEnabled && last.m_bEnabled && !next->m_bEnabled);
  CHECK(dialog.GetPluginForItem(1)==next.get());
  doc.onEnable=nullptr;
  list.rows[1].selected=true; dialog.OnEnable();
  CHECK(next->m_bEnabled); // A fresh selection still acts on the new instance.
  cout << "Enable callback replacement cannot retarget another selected row\n";
  const int loadsBeforeClosing=list.loads;
  ++doc.m_iUniqueDocumentNumber;
  CHECK(dialog.GetPluginForItem(0)==nullptr);
  dialog.OnKickIdle(0, 0);
  CHECK(dialog.closed && list.loads==loadsBeforeClosing);
  cout << "Invalid row data, row count, and document identity retain their checks\n";
}
'''


def run(args, output):
    def source(path):
        if args.revision:
            return subprocess.check_output(
                ['git', '-C', str(args.source), 'show', f'{args.revision}:{path}'], text=True)
        return (args.source / path).read_text()

    cpp = source('dialogs/plugins/PluginsDlg.cpp')
    header = source('dialogs/plugins/PluginsDlg.h')
    members = header[header.index('  CMUSHclientDoc * m_pDoc;'):header.index('  // for sorting the list')]
    methods = [block(cpp, marker) for marker in (
        'static int CALLBACK CompareFunc', 'CMUSHclientDoc * CPluginsDlg::GetLiveDocument',
        'CString CPluginsDlg::GetPluginIDForIndex', 'CPlugin * CPluginsDlg::GetPluginForIndex',
        'CPlugin * CPluginsDlg::GetPluginForItem', 'void CPluginsDlg::LoadList',
        'LRESULT CPluginsDlg::OnKickIdle', 'void CPluginsDlg::OnEdit()',
        'void CPluginsDlg::OnEnable()')]
    program = FIXTURE.replace('// PRODUCTION COLUMNS', block(header, '  enum ') + ';')
    program = program.replace('// PRODUCTION ROW MEMBERS', members)
    program = program.replace('// PRODUCTION METHODS', '\n'.join(methods))
    path = output / 'plugin_rows.cpp'
    path.write_text(program)
    command = shlex.split(os.environ['CXX']) if os.environ.get('CXX') else [
        shutil.which('clang++') or shutil.which('c++')]
    if not command or not command[0]:
        raise ValueError('No C++ compiler found. Set CXX or install clang++ or c++.')
    command += ['-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                str(path), '-o', str(output / 'plugin_rows')]
    subprocess.run(command, check=True)
    result = subprocess.run([str(output / 'plugin_rows')], capture_output=True, text=True)
    (output / 'plugin_rows.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='', flush=True)
    result.check_returncode()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--revision', help='Read production logic from this Git revision')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output:
        args.output = args.output.resolve()
        args.output.mkdir(parents=True, exist_ok=True)
        run(args, args.output)
    else:
        with tempfile.TemporaryDirectory(prefix='mushclient-plugin-rows-') as directory:
            run(args, Path(directory))

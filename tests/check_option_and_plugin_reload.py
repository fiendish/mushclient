#!/usr/bin/env python3
"""Check UTF-8 option changes and plugin reloads with extracted production code.

Requires Python 3 and clang++. MFC, plugin loading, and Windows text conversion
use substitutes. This does not test a native Windows build.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from output_callbacks import line_buffer_header, replace_once, section


def block(text, start):
    begin = text.index(start)
    brace = text.index('{', begin)
    depth = 0
    for end in range(brace, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if depth == 0:
            return text[begin:end + 1]
    raise ValueError(start)



def utf8_program(read, line_buffer):
    source = read('scriptingoptions.cpp')
    doc = read('doc.cpp')
    shim_path = 'tests/output_test_shim.h'
    shim = read(shim_path)

    def patch(expected, replacement):
        nonlocal shim
        shim = replace_once(shim, expected, replacement, shim_path)

    # Use the actual option field size and the Windows MAX macro semantics.
    patch('#define MAX std::max', '#define MAX(a,b) ((a)>(b)?(a):(b))')
    patch('bool m_bUTF_8=false,m_wrap=true,',
          'unsigned short m_bUTF_8=false;bool m_wrap=true,')
    resize = block(shim, ' void ResizeText(int n)')
    patch(resize, ' void ResizeText(int);')
    patch('struct CMUSHView:CView {', '''
struct CMUSHView:CView {
 struct {bool m_hWnd=false;void SendMessage(int,int,int){assert(false);}} m_ToolTip;
''')
    declarations = r'''
 unsigned short otherOption=0;
 int modified=0,windowSizes=0,inputWraps=0;
 struct Config {bool bInclude=false;long iValue=0;} config;
 Config* m_NumericConfiguration[3]={&config,&config,&config};
 void SetModifiedFlag(){++modified;}void SendWindowSizes(long){++windowSizes;}
 void FixUpOutputBuffer(long){assert(false);}void FixInputWrap(){++inputWraps;}
 void SetSpeedWalkDelay(short){assert(false);}
 int m_iUseMXP=0;bool m_bMXP=false;
 void MXP_Off(bool){assert(false);}void MXP_On(bool,bool){assert(false);}
 int m_input_font_height=0,m_input_font_weight=0,m_input_font_charset=0;
 bool m_input_font_italic=false;CString m_input_font_name;
 int m_font_height=0,m_font_weight=0,m_font_charset=0,m_iLineSpacing=0;
 CString m_font_name;bool m_bShowBold=false,m_bShowItalic=false,m_bShowUnderline=false;
 template<class... T>void ChangeInputFont(T...){assert(false);}
 template<class... T>void ChangeFont(T...){assert(false);}
 void UpdateAllViews(void*){assert(false);}
 long SetOptionItem(int,long,bool,bool);
'''
    patch(' struct CTriggerLineSnapshot', declarations + '\n struct CTriggerLineSnapshot')
    header = read('doc.h')
    flags = section(header, '#define OPT_CUSTOM_COLOUR', '// for debug.options', 'doc.h')
    options = r'''
#define NUMITEMS(x) (sizeof(x)/sizeof((x)[0]))
enum {eOK,eUnknownOption,eOptionOutOfRange};
enum {eNoMXP,eUseMXP,TTM_SETDELAYTIME,TTDT_AUTOPOP,TTDT_INITIAL};
struct tConfigurationNumericOption {
 const char* pName;double iDefault;size_t iOffset,iLength;int iMinimum,iMaximum,iFlags;
};
#define O(arg) offsetof(CMUSHclientDoc,arg),sizeof(((CMUSHclientDoc*)0)->arg)
tConfigurationNumericOption OptionsTable[]={
 {"utf_8",false,O(m_bUTF_8),0,0,0},
 {"other",false,O(otherOption),0,0,0},
 {"wrap_column",80,O(m_nWrapColumn),20,500,OPT_FIX_WRAP_COLUMN|OPT_FIX_INPUT_WRAP}
};
bool failResize=false;int resizes=0;
char* allocateLineText(int size) {
 ++resizes;if(failResize)throw new CMemoryException;
 return new char[size];
}
void AfxThrowMemoryException(){throw new CMemoryException;}
'''
    # Inject only the allocator. Keep production ResizeText publication ordering.
    resize = block(read('Line.cpp'), 'void CLine::ResizeText (')
    resize = resize.replace('new char [iNewSize]', 'allocateLineText(iNewSize)')
    base = block(source, 'long SetBaseOptionItem (')
    # Windows long has four bytes; the macOS/Linux host long can have eight.
    base = base.replace('(long *)', '(int32_t *)')
    body = base + block(source, 'long CMUSHclientDoc::SetOptionItem (')
    for start, end in [
        ('bool CMUSHclientDoc::StartNewLine_KeepPreviousStyle', 'COutputAppendTransaction::COutputAppendTransaction'),
        ('bool CMUSHclientDoc::AddToLine (', '// called when starting a new line to get colours right'),
        ('bool CMUSHclientDoc::StartNewLine (', 'const bool CMUSHclientDoc::CheckScriptingAvailable'),
        (' void CMUSHclientDoc::RemoveChunk (void)', 'void CMUSHclientDoc::ShowStatusLine'),
    ]:
        body += section(doc, start, end, 'doc.cpp')
    main = r'''
int main() {
 // At byte 79, each multibyte character must stay on the first line.
 for(const string& character:{string("\xc2\xa2"),string("\xe2\x82\xac"),string("\xf0\x9f\x98\x80")}) {
  CMUSHclientDoc d;string prefix(79,'A');assert(d.AddToLine(prefix.c_str(),0));
  CLine* first=d.m_pCurrentLine;
  assert(d.SetOptionItem(0,1,true,false)==eOK);
  assert(d.AddToLine(character.c_str(),0));
  assert(d.m_pCurrentLine==first && d.m_LineList.GetCount()==1);
  assert(string(first->text,first->len)==prefix+character);
  assert(d.SetOptionItem(0,1,true,false)==eOK);
  assert(d.AddToLine("Z",0));assert(d.m_LineList.GetCount()==2);
  assert(string(first->text,first->len)==prefix+character);
 }
 {
  CMUSHclientDoc d;assert(d.AddToLine("saved",0));
  char* original=d.m_pCurrentLine->text;int size=d.m_pCurrentLine->iMemoryAllocated;
  failResize=true;bool caught=false;
  try{d.SetOptionItem(0,1,true,true);}catch(CMemoryException* e){caught=true;e->Delete();}
  failResize=false;assert(caught && !d.m_bUTF_8 && !d.modified && !d.config.bInclude);
  assert(d.m_pCurrentLine->text==original && d.m_pCurrentLine->iMemoryAllocated==size);
  assert(string(original,d.m_pCurrentLine->len)=="saved");
  assert(d.SetOptionItem(0,1,true,true)==eOK && d.m_bUTF_8);
  assert(d.config.bInclude && d.config.iValue==1);
  int allocations=resizes;d.modified=0;
  assert(d.SetOptionItem(0,1,true,false)==eOK && resizes==allocations && !d.modified);
  assert(d.SetOptionItem(0,0,true,false)==eOK && resizes==allocations);
  assert(d.m_pCurrentLine->iMemoryAllocated==320 && !d.m_bUTF_8);
  assert(d.SetOptionItem(1,1,true,false)==eOK && resizes==allocations);
  assert(d.SetOptionItem(-1,1,true,false)==eUnknownOption);
  assert(d.SetOptionItem(3,1,true,false)==eUnknownOption);
  assert(d.SetOptionItem(0,2,true,false)==eOptionOutOfRange && resizes==allocations);
  assert(d.SetOptionItem(0,1,false,false)==eOK && resizes==allocations);
  assert(d.SetOptionItem(2,40,true,false)==eOK);
  assert(d.m_pCurrentLine->iMemoryAllocated==160 && d.windowSizes==1 && d.inputWraps==1);
 }
 {
  CMUSHclientDoc d;d.m_pCurrentLine=nullptr;
  assert(d.SetOptionItem(0,1,true,false)==eOK && d.m_bUTF_8);
 }
 {
  CMUSHclientDoc d;d.m_pCurrentLine->ResizeText(400);
  int allocations=resizes;assert(d.SetOptionItem(0,1,true,false)==eOK);
  assert(d.m_pCurrentLine->iMemoryAllocated==400 && resizes==allocations);
 }
 cout<<"UTF-8: 2/3/4-byte boundary appends, allocation failure/retry, option validation, disable/repeat, missing line, existing capacity, and wrap change passed\n";
}
'''
    return shim + flags + options + line_buffer + resize + body + main


def plugin_program(read):
    methods = read('scripting/methods/methods_plugins.cpp')
    plugins = read('plugins.cpp')
    prefix = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <list>
#include <stdexcept>
#include <string>
using namespace std;
using LPCTSTR=const char*;
#define ASSERT assert
#define TRUE true
#define NORMAL 0
struct CString:string {
 using string::string;CString(const string& s):string(s){}
 void Empty(){clear();}
};
enum {eOK,eNoSuchPlugin,eBadParameter,ePluginFileNotFound,eProblemsLoadingPlugin};
const string ON_PLUGIN_LIST_CHANGED="OnPluginListChanged";
struct CFileException{static int deleted;void Delete(){++deleted;delete this;}};
struct CArchiveException{static int deleted;void Delete(){++deleted;delete this;}};
int CFileException::deleted=0;int CArchiveException::deleted=0;
struct CPlugin {
 static int deleted;CString m_strSource="plugin.xml",m_strID="id",m_strName="name",m_strCallingPluginID;
 int m_iActiveScriptCalls=0;~CPlugin(){++deleted;}
};
int CPlugin::deleted=0;
using PluginListIterator=list<CPlugin*>::iterator;
struct compare_plugin_name {
 bool operator()(CPlugin* p,LPCTSTR name)const{return p->m_strName==name;}
};
template<class F,class A>auto bind2nd(F f,A a){return [=](CPlugin* p){return f(p,a);};}
struct CMUSHclientDoc {
 CPlugin* m_CurrentPlugin=nullptr;unsigned short m_iNoteStyle=17;
 list<CPlugin*>m_PluginList;int mode=0,modified=0,events=0,loads=0;
 bool m_bInPluginListChanged=false,m_bPluginListChangedPending=false,m_bPluginListChangedDeferred=false;
 int m_iPluginListChangedDeferralDepth=0;function<void()> observer;
 ~CMUSHclientDoc(){for(auto* p:m_PluginList)delete p;}
 CPlugin* GetPlugin(LPCTSTR id){for(auto* p:m_PluginList)if(p->m_strID==id)return p;return nullptr;}
 void InternalLoadPlugin(const CString& name){
  ++loads;assert(m_CurrentPlugin==nullptr && name=="plugin.xml");
  if(mode==1)throw new CFileException;
  if(mode==2)throw new CArchiveException;
  if(mode==3)throw runtime_error("unexpected load failure");
  m_PluginList.push_back(new CPlugin);SetModifiedFlag(TRUE);
 }
 void SetModifiedFlag(bool){++modified;}
 void SendToAllPluginCallbacks(const string& name){assert(name==ON_PLUGIN_LIST_CHANGED);++events;observer();}
 void PluginListChanged();long ReloadPlugin(LPCTSTR);
};
'''
    guard = block(read('plugins.h'), 'class CPluginContextGuard') + ';\n'
    body = block(plugins, 'CPluginContextGuard::CPluginContextGuard (')
    body += block(plugins, 'CPluginContextGuard::~CPluginContextGuard ()')
    body += block(plugins, 'void  CMUSHclientDoc::PluginListChanged (')
    body += block(methods, 'long CMUSHclientDoc::ReloadPlugin(')
    main = r'''
int main(){
 for(int mode=0;mode<3;++mode)for(bool callerContext:{false,true})for(bool byName:{false,true}){
  CMUSHclientDoc d;CPlugin caller;d.m_CurrentPlugin=callerContext?&caller:nullptr;
  auto* expectedContext=d.m_CurrentPlugin;d.mode=mode;d.m_PluginList.push_back(new CPlugin);
  int deleted=CPlugin::deleted,files=CFileException::deleted,archives=CArchiveException::deleted;
  d.observer=[&]{
   assert(d.m_CurrentPlugin==expectedContext && d.m_iNoteStyle==17 && d.modified==1);
   assert(d.m_PluginList.size()==(mode?0:1));assert(CPlugin::deleted==deleted+1);
  };
  long result=d.ReloadPlugin(byName?"name":"id");
  assert(result==(mode==1?ePluginFileNotFound:mode==2?eProblemsLoadingPlugin:eOK));
  assert(d.events==1 && d.loads==1 && d.modified==1 && d.m_CurrentPlugin==expectedContext);
  assert(CFileException::deleted==files+(mode==1));
  assert(CArchiveException::deleted==archives+(mode==2));
 }
 for(int guard=0;guard<3;++guard){
  CMUSHclientDoc d;auto* target=new CPlugin;d.m_PluginList.push_back(target);
  if(guard==1)d.m_CurrentPlugin=target;if(guard==2)target->m_iActiveScriptCalls=1;
  assert(d.ReloadPlugin(guard==0?"missing":"id")== (guard==0?eNoSuchPlugin:eBadParameter));
  assert(d.loads==0 && d.events==0 && !d.modified && d.m_PluginList.size()==1);
 }
 {
  CMUSHclientDoc d;CPlugin caller;d.m_CurrentPlugin=&caller;d.mode=1;
  d.m_PluginList.push_back(new CPlugin);d.m_iPluginListChangedDeferralDepth=1;
  assert(d.ReloadPlugin("id")==ePluginFileNotFound);
  assert(d.modified==1 && d.events==0 && d.m_bPluginListChangedDeferred && d.m_CurrentPlugin==&caller);
 }
 {
  CMUSHclientDoc d;CPlugin caller;d.m_CurrentPlugin=&caller;d.mode=2;d.m_PluginList.push_back(new CPlugin);
  d.observer=[&]{assert(d.m_CurrentPlugin==&caller && d.modified==1);throw runtime_error("notification failure");};
  bool caught=false;try{d.ReloadPlugin("id");}catch(const runtime_error& e){caught=string(e.what())=="notification failure";}
  assert(caught && d.events==1 && !d.m_bInPluginListChanged && d.m_CurrentPlugin==&caller);
 }
 {
  CMUSHclientDoc d;CPlugin caller;d.m_CurrentPlugin=&caller;d.mode=3;d.m_PluginList.push_back(new CPlugin);
  bool caught=false;try{d.ReloadPlugin("id");}catch(const runtime_error& e){caught=string(e.what())=="unexpected load failure";}
  assert(caught && d.m_CurrentPlugin==&caller);
 }
 cout<<"Plugins: 12 success/file/archive context/name cases, self/active/missing guards, deferred notification, and exception propagation passed\n";
}
'''
    return prefix + guard + body + main


def run(args, output):
    def read(path):
        if args.baseline_ref:
            return subprocess.check_output(['git', '-C', str(args.source), 'show',
                                            args.baseline_ref + ':' + path], text=True)
        return (args.source / path).read_text()
    reports = []
    for name, make in [('utf8', utf8_program), ('plugins', plugin_program)]:
        if args.case and args.case != name:
            continue
        cpp = output / (name + '.cpp')
        program = make(read, line_buffer_header(args.source, args.baseline_ref, memory_exception_defined=True)) if name == 'utf8' else make(read)
        cpp.write_text(program)
        command = ['clang++', '-std=c++17', '-fsanitize=address,undefined',
                   '-fno-sanitize-recover=all', '-g', '-O1',
                   str(cpp), '-o', str(output / name)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(output / name)], text=True, capture_output=True)
        (output / (name + '.log')).write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='', flush=True)
        result.check_returncode()
        reports.append({'case': name, 'result': 'pass', 'command': command})
    (output / 'runtime-validation.json').write_text(json.dumps(reports, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--baseline-ref')
    parser.add_argument('--case', choices=['utf8', 'plugins'])
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        run(args, args.output)
    else:
        with tempfile.TemporaryDirectory(prefix='mushclient-option-plugin-reload-') as directory:
            run(args, Path(directory))

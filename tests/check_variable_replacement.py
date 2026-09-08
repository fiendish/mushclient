#!/usr/bin/env python3
"""Compile production variable replacement functions and check callback order and failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
using LPCTSTR = const char *;
struct CString : std::string {
  using std::string::string;
  using std::string::operator=;
  operator const char*() const { return c_str(); }
  void TrimLeft(const char *s) { erase(0, find_first_not_of(s)); }
  void TrimRight(const char *s) { auto p=find_last_not_of(s); if(p==npos) clear(); else erase(p+1); }
};
struct CVariable {
  CString strLabel, strContents;
  long long nUpdateNumber;
  bool bSelected = false;
};
struct CException {};
struct CXMLelement { CString strContent; int iLine = 1; };
constexpr unsigned XML_OVERWRITE = 1;
constexpr const char* DEFAULTS_LIT = "defaults";
constexpr long eOK = 0, eVariableNotFound = 1;
struct Map {
  std::map<std::string,CVariable*> values;
  bool failSet = false;
  bool Lookup(const CString& k, CVariable*& v) {
    auto i=values.find(k); if(i==values.end()) return false; v=i->second; return true;
  }
  void SetAt(const CString& k,CVariable* v) {
    if (failSet) throw std::runtime_error("SetAt failure");
    values[k]=v;
  }
  bool RemoveKey(const CString& k) { return values.erase(k)!=0; }
  ~Map() { for(auto p: values) delete p.second; }
};
struct Application { long long n=0; long long GetUniqueNumber() { return ++n; } } App;
class CMUSHclientDoc {
public:
  Map vars;
  void* m_CurrentPlugin = nullptr;
  bool m_bVariablesChanged = false;
  std::function<void()> warning, afterLoad;
  Map& GetVariableMap() { return vars; }
  long CheckObjectName(CString& k) { return k.empty() ? 2 : eOK; }
  void SetModifiedFlag(bool) {}
  void Get_XML_boolean(CXMLelement&, const char*, bool& v,bool d) { v=d; }
  void Get_XML_string(CXMLelement&, const char*,CString& v,bool) { v="victim"; }
  void ThrowErrorException(const char*,const char*) { throw std::runtime_error("bad name"); }
  void LoadError(const char*,const char*,int) { if(warning) warning(); }
  // Stand-in for the verified warning -> screen callback path.
  void CheckUsed(CXMLelement&) { if(afterLoad) afterLoad(); }
  void Load_One_Variable_XML(CXMLelement&,unsigned long,long,bool,unsigned long);
  long DeleteVariable(LPCTSTR);
  long SetVariable(LPCTSTR,LPCTSTR);
};
constexpr bool TRUE = true;
'''
SUFFIX = r'''
int main(int argc, char** argv) {
  assert(argc==2);
  std::string mode=argv[1];
  CMUSHclientDoc doc;
  CVariable* old=new CVariable;
  old->strLabel="victim"; old->strContents=std::string(128,'o');
  doc.vars.SetAt("victim",old);
  CXMLelement node; node.strContent=std::string(128,'n');
  unsigned long mask=XML_OVERWRITE;
  bool observed=false, caught=false;
  if(mode=="delete_after") doc.afterLoad=[&]{
    assert(doc.vars.values.at("victim")->strContents==node.strContent);
    observed=true; doc.DeleteVariable("victim");
  };
  if(mode=="replace_after") doc.afterLoad=[&]{
    assert(doc.vars.values.at("victim")->strContents==node.strContent);
    observed=true; doc.SetVariable("victim","callback");
  };
  if(mode=="throw_after") doc.afterLoad=[&]{
    assert(doc.vars.values.at("victim")->strContents==node.strContent);
    throw std::runtime_error("CheckUsed callback failure");
  };
  if(mode=="delete_before" || mode=="replace_before" || mode=="throw_before" || mode=="fail_after_callback") {
    mask=0;
    doc.warning=[&]{
      observed=true;
      assert(doc.vars.values.at("victim")==old);
      if(mode=="throw_before") throw std::runtime_error("LoadError callback failure");
      if(mode=="delete_before") doc.DeleteVariable("victim");
      else doc.SetVariable("victim","callback");
      if(mode=="fail_after_callback") doc.vars.failSet=true;
    };
  }
  if(mode=="map_failure" || mode=="set_failure") doc.vars.failSet=true;
  try {
    if(mode=="set_failure" || mode=="set_success") doc.SetVariable("victim",node.strContent);
    else doc.Load_One_Variable_XML(node,mask,0,false,0);
  } catch(const std::runtime_error&) { caught=true; }
  if(mode=="map_failure" || mode=="set_failure" || mode=="throw_before") {
    assert(caught); assert(doc.vars.values.at("victim")==old);
    assert(old->strContents==std::string(128,'o'));
    assert(!doc.m_bVariablesChanged);
  } else if(mode=="fail_after_callback") {
    assert(caught && observed); assert(doc.vars.values.at("victim")->strContents=="callback");
  } else if(mode=="throw_after") {
    assert(caught); assert(doc.vars.values.at("victim")->strContents==node.strContent);
  } else {
    assert(!caught);
    if(mode=="delete_after") assert(observed && doc.vars.values.empty());
    else if(mode=="replace_after") assert(observed && doc.vars.values.at("victim")->strContents=="callback");
    else assert(doc.vars.values.at("victim")->strContents==node.strContent);
    if(mode=="delete_before" || mode=="replace_before") assert(observed);
  }
  std::cout<<"PASS "<<mode<<std::endl;
}
'''

def function(source, begin, end):
    start=source.index(begin)
    return source[start:source.index(end,start)+len(end)]+'\n'

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output',type=Path)
    parser.add_argument('--revision')
    args=parser.parse_args()
    out=args.output or Path(tempfile.mkdtemp(prefix='variable-replacement-'))
    out.mkdir(parents=True,exist_ok=True)
    def read(path):
        if args.revision:
            return subprocess.check_output(['git','show',args.revision+':'+path],cwd=ROOT,text=True)
        return (ROOT/path).read_text()
    xml=read('xml/xml_load_world.cpp');methods=read('scripting/methods/methods_variables.cpp')
    source=PREFIX+function(xml,'void CMUSHclientDoc::Load_One_Variable_XML','} // end of CMUSHclientDoc::Load_One_Variable_XML')
    source+=function(methods,'long CMUSHclientDoc::DeleteVariable','} // end of DeleteVariable')
    source+=function(methods,'long CMUSHclientDoc::SetVariable','}    // end of CMUSHclientDoc::SetVariable')+SUFFIX
    cpp=out/'variables.cpp';cpp.write_text(source);binary=out/'variables'
    subprocess.run(['clang++','-std=c++17','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(cpp),'-o',str(binary)],check=True)
    for mode in ['success','delete_after','replace_after','throw_after','delete_before','replace_before','throw_before','fail_after_callback','map_failure','set_failure','set_success']:
        subprocess.run([str(binary),mode],check=True)

if __name__=='__main__': main()

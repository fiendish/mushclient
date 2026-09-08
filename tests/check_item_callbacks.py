"""Check extracted item warning callbacks and editor keys with MFC substitutes."""
import argparse, json, os, subprocess
from pathlib import Path
OUT=Path(__file__).resolve().parents[1]/'.test-output/item-callbacks'
OUT.mkdir(parents=True,exist_ok=True)
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(); p.add_argument('--revision'); p.add_argument('--phase',choices=['items','all'],default='items'); args=p.parse_args()
def source(path):
    if args.revision:
        return subprocess.check_output(['git','-C',str(ROOT),'show',args.revision+':'+path],text=True)
    return (ROOT/path).read_text()
def block(text,start):
    begin=text.index(start); brace=text.index('{',begin); depth=0
    for end in range(brace,len(text)):
        depth+=(text[end]=='{')-(text[end]=='}')
        if depth==0: return text[begin:end+1]
    raise ValueError(start)
xml=source('xml/xml_load_world.cpp'); doc=source('doc.cpp'); hdr=source('doc.h'); editor=source('dialogs/world_prefs/genpropertypage.cpp')
results=[]
def run(name,program):
    path=OUT/(name+'.cpp'); path.write_text(program)
    cmd=['/usr/bin/clang++','-std=c++17','-O0','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(path),'-o',str(OUT/name)]
    subprocess.run(cmd,check=True)
    env=os.environ.copy()
    r=subprocess.run([str(OUT/name)],env=env,capture_output=True,text=True)
    (OUT/(name+'.log')).write_text(r.stdout+r.stderr)
    print(r.stdout+r.stderr,end='')
    r.check_returncode()
    results.append({'name':name,'result':'pass','command':cmd,'log':str(OUT/(name+'.log'))})
prefix=r'''
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
using CString = std::string;
struct Item {
  static int deleted;
  bool bExecutingScript = false;
  Item *pNextRetired = nullptr;
  long long nUpdateNumber = 0, nCreationNumber = 0;
  CString strInternalName;
  ~Item() { ++deleted; }
};
int Item::deleted=0;
struct ItemMap {
  std::map<CString,Item*> items;
  bool Lookup(const CString& key, Item*& result) {
    auto i=items.find(key); if(i==items.end()) return false;
    result=i->second; return true;
  }
  void SetAt(const CString& key, Item* value) { items[key]=value; }
  void RemoveKey(const CString& key) { items.erase(key); }
};
struct Counter { long long n=1; long long GetUniqueNumber() { return n++; } } App;
const int XML_OVERWRITE=1;
void ThrowErrorException(const char*,const CString&) { throw std::runtime_error("duplicate label"); }
struct Change { CString strName; Item *pOld=nullptr,*pNew=nullptr; long long iNewCreationNumber=0; bool bApplied=false; };
'''
for kind,var in [('Alias','a'),('Trigger','t'),('Timer','t')]:
    one=block(xml,f'bool CMUSHclientDoc::Load_One_{kind}_XML (')
    start=one.index(f'  {var}->nUpdateNumber    = App.GetUniqueNumber ();')
    tail=one[start:one.index('  return true;',start)]
    getter=block(hdr,f'C{kind}Map & Get{kind}Map (void)')
    retire=block(doc,f'void CMUSHclientDoc::Retire{kind} (')
    setup=f'C{kind} *old{kind}=nullptr; objectMap.Lookup(str{kind}Name,old{kind});' if f'old{kind} = NULL;' not in tail else ''
    program=prefix+f'''
using C{kind}=Item; using C{kind}Map=ItemMap;
struct CPlugin {{ C{kind}Map m_{kind}Map; }};
struct CMUSHclientDoc {{
 C{kind}Map m_{kind}Map; CPlugin *m_CurrentPlugin=nullptr;
 C{kind}Map *m_pSetLoad{kind}Map=nullptr;
 C{kind} *m_pRetired{kind+'es' if kind=='Alias' else kind+'s'}=nullptr;
 std::function<void()> callback;
 int warnings=0;
 void CheckUsed(int) {{ ++warnings; callback(); }}
 void ResetOneTimer(Item*) {{}}
 void Retire{kind}(C{kind}*);
 {getter}
 void publish(int iMask) {{
   auto &objectMap=Get{kind}Map();
   CString str{kind}Name="x"; {setup}
   std::unique_ptr<C{kind}> new{kind}(new C{kind});
   auto *{var}=new{kind}.get(); int node=0; Change change;
   {tail}
   Retire{kind}(change.pOld);
 }}
}};
{retire}
'''+r'''
int main() {
  // Each callback runs synchronously inside the extracted CheckUsed call.
  for(int action=0;action<5;++action) for(int overwrite=0;overwrite<2;++overwrite) {
    CMUSHclientDoc d; CPlugin plugin; d.m_CurrentPlugin=&plugin;
    ItemMap &map=d.GETMAP();
    if(action!=3) map.SetAt("x",new Item);
    Item* held=nullptr;
    d.callback=[&] {
      Item* old=nullptr;
      if(action==1 || action==2) {
        assert(map.Lookup("x",old)); map.RemoveKey("x"); delete old;
      }
      if(action==2 || action==3) map.SetAt("x",new Item);
      if(action==4) { assert(map.Lookup("x",held)); held->bExecutingScript=true; }
    };
    bool duplicate=false;
    try { d.publish(overwrite ? XML_OVERWRITE : 0); }
    catch(const std::runtime_error &e) { duplicate=std::string(e.what())=="duplicate label"; }
    assert(d.warnings==1);
    assert(duplicate==(!overwrite && action!=1));
    Item* current=nullptr; assert(map.Lookup("x",current));
    if(!duplicate) { assert(current->nCreationNumber>0); INTERNAL_CHECK }
    if(action==4 && overwrite) {
      assert(d.RETIRED==held); held->bExecutingScript=false; delete held;
    }
    delete current; map.RemoveKey("x");
  }
  std::cout << "KIND: 10 warning deletion, replacement, insertion, duplicate-error, and active-retirement cases passed\n";
}
'''
    program=program.replace('GETMAP',f'Get{kind}Map').replace('RETIRED',f'm_pRetired{kind+"es" if kind=="Alias" else kind+"s"}').replace('KIND',kind).replace('INTERNAL_CHECK','assert(current->strInternalName=="x");' if kind!='Timer' else '')
    run('warning_'+kind.lower(),program)
# Compile the exact final editor key selection and its identity/row assignments.
start=editor.index('  CString strMapOldName = *pstrObjectName;',editor.index('bool CGenPropertyPage::ChangeOneItem'))
stop=editor.index('  std::unique_ptr<CObject> pReplacement',start)
key=editor[start:stop]
assert 'const CString strMapName' in key
assert 'SetInternalName (pUpdatedItem, strMapName);' in editor
assert 'std::unique_ptr<CString> pRowName (new CString (strMapName));' in editor
assert editor.count('m_ObjectMap->SetAt (strMapName, pUpdatedItem);')==2
run('editor_keys',r'''
#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <string>
struct CString:std::string {
 using std::string::string;
 void MakeLower() { std::transform(begin(),end(),begin(),[](unsigned char c){return std::tolower(c);}); }
};
struct Item { CString internal; };
void SetInternalName(Item* item,const CString& name) { item->internal=name; }
int main() {
 for(auto kind:{"alias","trigger","timer"}) for(int action=0;action<5;++action) {
  CString oldKey=action<2?"*generated100":"oldlabel";
  CString *pstrObjectName=&oldKey;
  bool bNameChanged=action==1 || action==3 || action==4;
  CString strObjectName=action==0?"":action==2?"oldlabel":action==4?"*generated101":"newlabel";
'''+key+r'''
  Item item; Item* pUpdatedItem=&item;
  SetInternalName (pUpdatedItem, strMapName);
  std::unique_ptr<CString> pRowName (new CString (strMapName));
  std::map<CString,Item*> objectMap; objectMap[strMapName]=pUpdatedItem;
  assert(!strMapName.empty());
  assert(objectMap.at(*pRowName)==pUpdatedItem);
  assert(objectMap.at(pUpdatedItem->internal)==pUpdatedItem);
  if(!bNameChanged) assert(strMapName==oldKey);
  else assert(strMapName==strObjectName);
  objectMap.erase(*pRowName); assert(objectMap.empty());
 }
 std::cout<<"Editor: 15 generated-key, rename, label removal, row lookup, callback lookup, and deletion cases passed\n";
}
''')
if args.phase=='all':
    # No staging pointer can influence a runtime getter, including world callbacks.
    for path in ['doc.h','doc_construct.cpp','evaluate.cpp']:
        assert 'm_pSetLoad' not in source(path),path
    types=['AliasMap','AliasArray','TriggerMap','TriggerArray','TimerMap']
    scope='#include <cassert>\n#include <iostream>\n'
    scope+='\n'.join(f'struct C{x} {{ int value=0; }};' for x in types)
    members='\n'.join(f'C{x} m_{x};' for x in types)
    scope+='\nstruct CPlugin { '+members+' };\nstruct Doc { '+members+'\nCPlugin* m_CurrentPlugin=nullptr;\n'
    for x in types: scope+=block(hdr,f'C{x} & Get{x} (void)')+'\n'
    scope+='};\nint main() { Doc d; CPlugin plugin;\n'
    for x in types:
        scope+=f'C{x} staged{x}; staged{x}.value=123; d.m_{x}.value=10; plugin.m_{x}.value=20;\n'
        scope+=f'd.m_CurrentPlugin=&plugin; assert(&d.Get{x}()==&plugin.m_{x}); d.Get{x}().value=21;\n'
        scope+=f'd.m_CurrentPlugin=nullptr; assert(&d.Get{x}()==&d.m_{x}); d.Get{x}().value=11;\n'
        scope+=f'assert(staged{x}.value==123 && plugin.m_{x}.value==21 && d.m_{x}.value==11);\n'
    scope+='std::cout<<"Scope: all 5 map and index getters preserve plugin and world scope\\n"; }'
    run('staging_scope',scope)
(OUT/'validation.json').write_text(json.dumps(results,indent=2)+'\n')

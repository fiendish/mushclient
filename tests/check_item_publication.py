"""Compile source excerpts for XML staging and set publication.
Run with Python 3 and clang++. MFC collections and XML tokenization are
substitutes. The source supplies index builders, rollback, and publication.
"""
from pathlib import Path
import argparse, subprocess, os, json, shlex, shutil
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-dir',type=Path,default=ROOT/'.test-output/item-publication')
args=parser.parse_args()
OUT=args.output_dir.resolve()
OUT.mkdir(parents=True,exist_ok=True)
# A failed rerun must not leave an earlier pass report or runtime log.
for name in ['staging-validation.json','staging_integration.log']:
 (OUT/name).unlink(missing_ok=True)

def check(condition, expression):
 if not condition:
  raise AssertionError(f'Check failed: {expression}')

xml=(ROOT/'xml/xml_load_world.cpp').read_text(); doc=(ROOT/'doc.cpp').read_text(); hdr=(ROOT/'doc.h').read_text(); timers=(ROOT/'timers.cpp').read_text()
def compiler_command():
    override = os.environ.get('CXX')
    if override:
        try:
            command = shlex.split(override)
        except ValueError as error:
            raise SystemExit(f'Invalid CXX command: {error}') from error
        if not command:
            raise SystemExit('CXX must name a C++ compiler.')
        executable = shutil.which(command[0])
        if executable is None:
            raise SystemExit(f'CXX compiler not found or not executable: {command[0]}')
        return [executable, *command[1:]]
    executable = shutil.which('clang++') or shutil.which('c++')
    if executable is None:
        raise SystemExit('No C++ compiler found. Set CXX or install clang++ or c++ on PATH.')
    return [executable]
def block(text,start):
 b=text.index(start); i=text.index('{',b); depth=0
 for e in range(i,len(text)):
  depth+=(text[e]=='{')-(text[e]=='}')
  if depth==0:return text[b:e+1]
 raise ValueError(start)
fields=['AliasMap','AliasArray','TriggerMap','TriggerArray','TimerMap']
program=r'''
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdlib>
#include <iterator>
using namespace std;
using CString=string; using POSITION=size_t; using UINT=unsigned int;
#define __int64 long long
// Fixture checks, including extracted ASSERT calls, stay active under NDEBUG.
void Check(bool condition, const char* expression, const char* file, int line) {
 if(!condition) {
  cerr<<file<<":"<<line<<": check failed: "<<expression<<endl;
  std::exit(EXIT_FAILURE);
 }
}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define ASSERT CHECK
const int XML_OVERWRITE=1;
bool failNextAllocation=false;
void* operator new(std::size_t size) {
 if(failNextAllocation) {failNextAllocation=false;throw std::bad_alloc();}
 if(void* p=std::malloc(size ? size : 1)) return p;
 throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
struct CException {
 inline static int deleted=0;
 virtual ~CException() {++deleted;}
 void Delete() {delete this;}
};
using INT_PTR=std::ptrdiff_t;
class CPtrArray {
 protected:
 INT_PTR m_nSize=0; vector<void*> storage;
 public:
 INT_PTR GetSize() const {return m_nSize;}
 void SetSize(INT_PTR size) {
  if(!size) vector<void*>().swap(storage);
  else if(static_cast<size_t>(size)>storage.size()) storage.resize(size);
  m_nSize=size;
 }
 size_t StorageCount() const {return storage.size();}
 void** GetData() {return storage.data();}
};
template<class Base,class Pointer> class CTypedPtrArray : public Base {};
struct Counter { long long n=1; long long GetUniqueNumber() {return n++;} } App;
void ThrowErrorException(const char*,const CString&) { throw runtime_error("duplicate label"); }
template<int N> struct Item {
 inline static int deleted=0; ~Item() {++deleted;}
 bool bExecutingScript=false; Item* pNextRetired=nullptr;
 long long nCreationNumber=0,nUpdateNumber=0;
 int iSequence=0; CString name,trigger,strInternalName;
};
using CAlias=Item<0>; using CTrigger=Item<1>; using CTimer=Item<2>;
template<class T> struct ItemMap {
 map<CString,T*> items;
 int setsUntilFailure=-1;
 bool Lookup(const CString& key,T*& result) {auto i=items.find(key);if(i==items.end())return false;result=i->second;return true;}
 void SetAt(const CString& key,T* value) {if(setsUntilFailure==0){setsUntilFailure=-1;throw bad_alloc();}if(setsUntilFailure>0)--setsUntilFailure;items[key]=value;}
 void RemoveKey(const CString& key) {items.erase(key);}
 void RemoveAll() {items.clear();}
 size_t GetCount() {return items.size();}
 POSITION GetStartPosition() {return items.empty()?0:1;}
 void GetNextAssoc(POSITION& pos,CString& key,T*& value) {auto i=items.begin();advance(i,pos-1);key=i->first;value=i->second;pos=pos<items.size()?pos+1:0;}
};
template<class T> struct ItemArray {
 vector<T*> items;
 bool failGrowth=false;
 void Reserve(size_t size) {items.reserve(size);}
 void SetSize(size_t size) {if(!size){vector<T*>().swap(items);return;}if(failGrowth && size>items.capacity()){failGrowth=false;throw bad_alloc();}items.resize(size);}
 size_t GetSize() {return items.size();}
 void SetAt(size_t i,T* value) {items.at(i)=value;}
 T*& operator[](size_t i) {return items.at(i);}
};
using CAliasMap=ItemMap<CAlias>;using CTriggerMap=ItemMap<CTrigger>;using CTimerMap=ItemMap<CTimer>;
using CAliasArray=ItemArray<CAlias>;using CTriggerArray=ItemArray<CTrigger>;
struct CXMLelement { string kind,key; vector<CXMLelement*> children; bool warning=false; };
#define LOAD_LOOP(parent,type,node) for(auto* node:(parent).children) { if(node->kind==type) {
#define END_LOAD_LOOP } }
#define GET_VERSION_AND_DEFAULTS(node) long iVersion=0; bool bUseDefault=false
class CMUSHclientDoc;
'''
types=(ROOT/'OtherTypes.h').read_text()
program+='template <class T>\n'+block(types,'class CScriptItemArray')+';\n'
program+=block(hdr,'struct CXMLLoadContext')+';\n'
program+='template <class T>\n'+block(hdr,'struct CXMLLoadChange')+';\n'
members='\n'.join(f'C{f} m_{f};' for f in fields)
program+='struct CPlugin { '+members+' };\n'
program+='struct CMUSHclientDoc {\n'+members+'\nCPlugin* m_CurrentPlugin=nullptr;\n'
program+='std::function<void()> callback; int warnings=0; void CheckUsed(CXMLelement& node) {if(node.warning){++warnings;callback();}}\n'
program+='void HandleLoadException(const char*,CException* e) {throw e;} void ResetOneTimer(CTimer*) {}\n'
for f in fields:program+=block(hdr,f'C{f} & Get{f} (void)')+'\n'
for kind,plural in [('Alias','Aliases'),('Trigger','Triggers'),('Timer','Timers')]:
 program+=f'C{kind} *m_pRetired{plural}=nullptr; void Retire{kind}(C{kind}*);\n'
 for name in [f'Load_{plural}_XML',f'Load_One_{kind}_XML'] + ([] if kind=='Timer' else [f'Build{kind}Indexes',f'Sort{plural}']):
  idx=hdr.index(' '+name+' ('); start=hdr.rfind('\n',0,idx)+1;end=hdr.index(';',idx)+1
  program+=hdr[start:end]+'\n'
program+='};\n'
# Compile the actual context constructor, rollback helpers, and change guard.
start=xml.index('CXMLLoadContext::CXMLLoadContext'); end=xml.index('\n/*',start)
program+=xml[start:end]+'\n'
for kind,plural,var in [('Alias','Aliases','a'),('Trigger','Triggers','t'),('Timer','Timers','t')]:
 program+=block(doc,f'void CMUSHclientDoc::Retire{kind} (')+'\n'
 if kind!='Timer': program+=block(doc,('static int CompareAlias (' if kind=='Alias' else 'int CompareTrigger ('))+'\n'
 if kind!='Timer':
  program+=block(doc,f'void CMUSHclientDoc::Build{kind}Indexes (')+'\n'
  program+=block(doc,f'void  CMUSHclientDoc::Sort{plural} (')+'\n'
 program+=block(xml,f'UINT CMUSHclientDoc::Load_{plural}_XML (')+'\n'
 one=block(xml,f'bool CMUSHclientDoc::Load_One_{kind}_XML ('); start=one.index(f'  {var}->nUpdateNumber    = App.GetUniqueNumber ();'); tail=one[start:one.index('  return true;',start)]
 sig=one[:one.index('{')]
 program+=sig+f'''{{
 std::unique_ptr<C{kind}> new{kind}(new C{kind});
 auto* {var}=new{kind}.get(); CString str{kind}Name=node.key;
 {tail}
 return true;
}}
'''
# Extract final set publication too. These helpers have no UI dependencies.
evaluate=(ROOT/'evaluate.cpp').read_text()
start=evaluate.index('template <class TObject>\nstruct CSetPublishChange');end=evaluate.index('BOOL CMUSHclientDoc::Load_Set',start)
publication=evaluate[start:end]
program+=publication+'\n'
program+='int main() {\n'
for kind,plural in [('Alias','Aliases'),('Trigger','Triggers'),('Timer','Timers')]:
 fs=[f for f in fields if f.startswith(kind)]
 for failure in [False,True]:
  program+='''{
 CMUSHclientDoc d; CPlugin plugin;
 CXMLLoadContext context(&d);
'''
  program+='\n'.join(f'C{f} staged{f}; context.p{f}=&staged{f};' for f in fs)+'\n'
  program+=f'''
 auto *live=new C{kind}, *local=new C{kind};
 d.m_{kind}Map.SetAt("x",live); plugin.m_{kind}Map.SetAt("x",local);
 {'' if kind=='Timer' else f'd.Sort{plural}();'} d.m_CurrentPlugin=&plugin; {'' if kind=='Timer' else f'd.Sort{plural}();'} d.m_CurrentPlugin=nullptr;
 CXMLelement x{{"{kind.lower()}","x"}},y{{"{kind.lower()}","y"}};
 CXMLelement section{{"{plural.lower()}","",{{&x,&y}},true}};
 CXMLelement root{{"root","",{{&section}}}};
 d.callback=[&] {{
   // Plugin warning callback deletes its own x, then performs a nested import.
   d.m_CurrentPlugin=&plugin;
   C{kind}* old=nullptr;
   const bool foundPluginItem=d.Get{kind}Map().Lookup("x",old);
   CHECK(foundPluginItem);
   d.Get{kind}Map().RemoveKey("x"); delete old; {'' if kind=='Timer' else f'd.Sort{plural}();'}
   CXMLelement nestedItem{{"{kind.lower()}","nested"}};
   CXMLelement nestedSection{{"{plural.lower()}","",{{&nestedItem}}}};
   CXMLelement nestedRoot{{"root","",{{&nestedSection}}}};
   const UINT pluginLoaded=d.Load_{plural}_XML(nestedRoot,XML_OVERWRITE,0);
   CHECK(pluginLoaded==1);
   // A nested world callback changes the live world, not the staged set.
   d.m_CurrentPlugin=nullptr;
   const bool foundWorldItem=d.Get{kind}Map().Lookup("x",old);
   CHECK(foundWorldItem);
   d.Get{kind}Map().RemoveKey("x"); delete old; {'' if kind=='Timer' else f'd.Sort{plural}();'}
   const UINT worldLoaded=d.Load_{plural}_XML(nestedRoot,XML_OVERWRITE,0);
   CHECK(worldLoaded==1);
   {'throw runtime_error("section warning failure");' if failure else ''}
 }};
 bool failed=false;
 try {{
   const UINT loaded=d.Load_{plural}_XML(root,XML_OVERWRITE,0,&context);
   CHECK(loaded==2);
 }}
 catch(const runtime_error& e) {{ CHECK(string(e.what())=="section warning failure"); failed=true; }}
 CHECK(failed=={str(failure).lower()}); CHECK(d.warnings==1);
 CHECK(plugin.m_{kind}Map.items.count("x")==0 && plugin.m_{kind}Map.items.count("nested")==1);
 CHECK(d.m_{kind}Map.items.count("x")==0 && d.m_{kind}Map.items.count("nested")==1);
 CHECK(staged{kind}Map.GetCount()=={0 if failure else 2});
'''
  if not failure:
   program+=f'''
 PublishLoadedSet<C{kind}>(&d,d.m_{kind}Map,staged{kind}Map,{'NULL' if kind=='Timer' else f'&CMUSHclientDoc::Sort{plural}'},&CMUSHclientDoc::Retire{kind});
 CHECK(d.m_{kind}Map.GetCount()==2 && d.m_{kind}Map.items.count("x")==1 && d.m_{kind}Map.items.count("y")==1);
 CHECK(staged{kind}Map.GetCount()==0);
'''
  program+=f'''
 for(auto &item:d.m_{kind}Map.items) delete item.second;
 for(auto &item:plugin.m_{kind}Map.items) delete item.second;
 std::cout<<"{kind}: {'rollback' if failure else 'publication'} with plugin and world warning reentry and nested imports passed\\n";
}}
'''
 # Exercise direct publication and the guard against a replacement entry.
 for guarded in [False,True]:
  program+='''{ CMUSHclientDoc d; CXMLLoadContext context(&d);
'''
  program+='\n'.join(f'C{f} staged{f}; context.p{f}=&staged{f};' for f in fs)+'\n'
  program+=f'''
 auto* old=new C{kind}; auto* replacement=new C{kind}; replacement->nCreationNumber=App.GetUniqueNumber();
 staged{kind}Map.SetAt("x",replacement);
 vector<CXMLLoadChange<C{kind}>> changes(1);
 auto& c=changes[0]; c.strName="x"; c.pOld=old;c.pNew=replacement;c.iNewCreationNumber=replacement->nCreationNumber;c.bApplied=true;
 ReserveXMLLoadRollback(context,staged{kind}Map);
 {f'CXMLLoadChangeGuard<C{kind},C{kind}Map> guard(&d,staged{kind}Map,context,changes,&CMUSHclientDoc::Retire{kind}); guard.Rollback();' if guarded else 'PublishXMLLoadRollbackWithoutAllocation(context,staged'+kind+'Map,changes);'}
 CHECK(staged{kind}Map.items.at("x")==old);CHECK(c.bRollbackOwnsNew);
 CHECK(d.m_{kind}Map.GetCount()==0);
 delete old;{'' if guarded else 'delete replacement;'}
 std::cout<<"{kind}: {'guarded' if guarded else 'direct'} rollback keeps explicit targets passed\\n";
}}
'''

# Check ordering, excluded objects, and failure before sorted-array publication.
for kind,plural,match in [('Alias','Aliases','name'),('Trigger','Triggers','trigger')]:
 program+=f'''
 {{ CMUSHclientDoc d; C{kind} a,b,c;
 a.iSequence=2; b.iSequence=c.iSequence=1; b.{match}="z"; c.{match}="a";
 d.m_{kind}Map.SetAt("a",&a);d.m_{kind}Map.SetAt("b",&b);d.m_{kind}Map.SetAt("c",&c);
 d.Sort{plural}(); CHECK(d.m_{kind}Array.items==vector<C{kind}*>({{&c,&b,&a}}));
 set<C{kind}*> exclude{{&b}}; d.Sort{plural}(&exclude);
 CHECK(d.m_{kind}Array.items==vector<C{kind}*>({{&c,&a}}));
 C{kind} added; d.m_{kind}Map.SetAt("added",&added);
 d.m_{kind}Array.failGrowth=true; bool failed=false;
 try {{ d.Sort{plural}(); }} catch(const bad_alloc&) {{failed=true;}}
 CHECK(failed && d.m_{kind}Array.items==vector<C{kind}*>({{&c,&a}}));
 cout<<"{kind}: order, exclusion, and array allocation failure passed\\n";
 }}
'''
# Each set publication must restore prior entries if a map insertion fails.
for kind,plural in [('Alias','Aliases'),('Trigger','Triggers'),('Timer','Timers')]:
 for failure in [0,1]:
  program+=f'''
 {{ CMUSHclientDoc d; C{kind}Map staged; C{kind} old,replacement,added;
 d.m_{kind}Map.SetAt("x",&old); staged.SetAt("x",&replacement);staged.SetAt("y",&added);
 {'' if kind=='Timer' else f'd.Sort{plural}();'}
 d.m_{kind}Map.setsUntilFailure={failure}; bool failed=false;
 try {{ PublishLoadedSet<C{kind}>(&d,d.m_{kind}Map,staged,{'NULL' if kind=='Timer' else f'&CMUSHclientDoc::Sort{plural}'},&CMUSHclientDoc::Retire{kind}); }}
 catch(const bad_alloc&) {{failed=true;}}
 CHECK(failed && d.m_{kind}Map.GetCount()==1 && d.m_{kind}Map.items.at("x")==&old);
 CHECK(staged.GetCount()==2);
 {'' if kind=='Timer' else f'CHECK(d.m_{kind}Array.items==vector<C{kind}*>({{&old}}));'}
 cout<<"{kind}: set publication map failure {failure} passed\\n";
 }}
'''

# Exercise real source loaders with the next allocation forbidden during unwind.
for kind,plural in [('Alias','Aliases'),('Trigger','Triggers'),('Timer','Timers')]:
 for original_kind in ['mfc','standard','unknown']:
  for indexed in ([False,True] if kind!='Timer' else [False]):
   original={'mfc':'CException* original=new CException;', 'standard':'runtime_error original("load");','unknown':'int original=73;'}[original_kind]
   caught={'mfc':'catch(CException* e) {CHECK(e==original);e->Delete();caught=true;}', 'standard':'catch(const runtime_error& e) {CHECK(string(e.what())=="load");caught=true;}','unknown':'catch(int e) {CHECK(e==original);caught=true;}'}[original_kind]
   program+=f'''
 {{ CMUSHclientDoc d; int before=C{kind}::deleted;
 int exceptionsBefore=CException::deleted; {original}
 auto* old=new C{kind}; d.m_{kind}Map.SetAt("x",old);
 {f'd.Sort{plural}();' if indexed else ''}
 CXMLelement x{{"{kind.lower()}","x"}},y{{"{kind.lower()}","y"}};
 CXMLelement section{{"{plural.lower()}","",{{&x,&y}},true}};
 CXMLelement root{{"root","",{{&section}}}};
 d.callback=[&] {{failNextAllocation=true;throw original;}};
 bool caught=false;
 try {{d.Load_{plural}_XML(root,XML_OVERWRITE,0);}} {caught}
 bool noAllocation=failNextAllocation; failNextAllocation=false;
 CHECK(caught && noAllocation);
 CHECK(d.m_{kind}Map.GetCount()==1 && d.m_{kind}Map.items.at("x")==old);
 {f'CHECK(d.m_{kind}Array.items==vector<C{kind}*>({{old}}));' if kind!='Timer' else ''}
 CHECK(C{kind}::deleted==before+2);
 CHECK(CException::deleted==exceptionsBefore+{int(original_kind=='mfc')});
 delete old;
 cout<<"{kind}: {original_kind} load error survives allocation-free rollback (indexed={indexed}) passed\\n";
 }}
'''

# Capacity allocation must fail before a loader publishes any changes.
for kind,plural in [('Alias','Aliases'),('Trigger','Triggers')]:
 program+=f'''
 {{ CMUSHclientDoc d; C{kind} old; d.m_{kind}Map.SetAt("x",&old);
 CXMLelement x{{"{kind.lower()}","x"}};
 CXMLelement section{{"{plural.lower()}","",{{&x}},true}};
 CXMLelement root{{"root","",{{&section}}}};
 bool callbackRan=false;d.callback=[&]{{callbackRan=true;}};
 int before=C{kind}::deleted;failNextAllocation=true;bool caught=false;
 try {{d.Load_{plural}_XML(root,XML_OVERWRITE,0);}}
 catch(const bad_alloc&) {{caught=true;}}
 CHECK(caught && !failNextAllocation && !callbackRan);
 CHECK(d.m_{kind}Map.GetCount()==1 && d.m_{kind}Map.items.at("x")==&old);
 CHECK(d.m_{kind}Array.GetSize()==0 && C{kind}::deleted==before);
 cout<<"{kind}: reservation failure leaves the load unpublished passed\\n";
 }}
'''

# A nested loader reserves outer entries without changing their visible index.
for kind,plural in [('Alias','Aliases'),('Trigger','Triggers')]:
 program+=f'''
 {{ CMUSHclientDoc d; int depth=0;int before=C{kind}::deleted;
 CXMLelement x{{"{kind.lower()}","x"}},y{{"{kind.lower()}","y"}};
 CXMLelement outerSection{{"{plural.lower()}","",{{&x}},true}};
 CXMLelement outerRoot{{"root","",{{&outerSection}}}};
 CXMLelement innerSection{{"{plural.lower()}","",{{&y}},true}};
 CXMLelement innerRoot{{"root","",{{&innerSection}}}};
 d.callback=[&] {{
   CHECK(d.m_{kind}Array.GetSize()==0);
   if(depth++) {{failNextAllocation=true;throw 19;}}
   bool innerCaught=false;
   try {{d.Load_{plural}_XML(innerRoot,XML_OVERWRITE,0);}}
   catch(int e) {{CHECK(e==19);innerCaught=true;}}
   bool innerNoAllocation=failNextAllocation;failNextAllocation=false;
   CHECK(innerCaught && innerNoAllocation && d.m_{kind}Map.GetCount()==1);
   CHECK(d.m_{kind}Array.GetSize()==1 && d.m_{kind}Map.items.count("x")==1);
   failNextAllocation=true;throw 23;
 }};
 bool caught=false;
 try {{d.Load_{plural}_XML(outerRoot,XML_OVERWRITE,0);}}
 catch(int e) {{CHECK(e==23);caught=true;}}
 bool noAllocation=failNextAllocation;failNextAllocation=false;
 CHECK(caught && noAllocation && depth==2);
 CHECK(d.m_{kind}Map.GetCount()==0 && d.m_{kind}Array.GetSize()==0);
 CHECK(C{kind}::deleted==before+2);
 cout<<"{kind}: nested unindexed loads preserve both errors without allocation passed\\n";
 }}
'''

# Compile the actual Reserve method against a pointer-array storage model.
program+=r'''
{ CScriptItemArray<int> array;
 array.Reserve(4);CHECK(array.GetSize()==0 && array.StorageCount()==4);
 failNextAllocation=true;array.SetSize(4);
 bool reused=failNextAllocation;failNextAllocation=false;CHECK(reused);
 array.SetSize(2);array.Reserve(8);CHECK(array.GetSize()==2);
 auto* data=array.GetData();failNextAllocation=true;bool failed=false;
 try {array.Reserve(32);} catch(const bad_alloc&) {failed=true;}
 CHECK(failed && !failNextAllocation && array.GetSize()==2 && array.GetData()==data);
 cout<<"Pointer array reservation preserves logical size, storage, and failed growth passed\n";
}
'''

program+='}\n'
(OUT/'staging_integration.cpp').write_text(program)
cmd=[*compiler_command(),'-std=c++17','-O0','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(OUT/'staging_integration.cpp'),'-o',str(OUT/'staging_integration')]
subprocess.run(cmd,check=True)
env=os.environ.copy()
r=subprocess.run([str(OUT/'staging_integration')],env=env,text=True,capture_output=True)
(OUT/'staging_integration.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
cases = sum(line.endswith(' passed') for line in r.stdout.splitlines())
check(cases == 40, f'cases == 40 (actual: {cases})')
# Includes retain targets, while nested callback API imports use the default.
check('piPrinting, bPlugin ? NULL : pLoadContext);' in block(xml,'void CMUSHclientDoc::Load_One_Include_XML'), "'piPrinting, bPlugin ? NULL : pLoadContext);' in block(xml,'void CMUSHclientDoc::Load_One_Include_XML')")
check('piPrinting, pLoadContext);' in block(xml,'void CMUSHclientDoc::Load_Includes_XML'), "'piPrinting, pLoadContext);' in block(xml,'void CMUSHclientDoc::Load_Includes_XML')")
check(block(xml,'UINT CMUSHclientDoc::Load_World_XML').count('piPrinting, pLoadContext);')==2, "block(xml,'UINT CMUSHclientDoc::Load_World_XML').count('piPrinting, pLoadContext);')==2")
for kind,plural in [('Alias','Aliases'),('Trigger','Triggers'),('Timer','Timers')]:
 one=block(xml,f'bool CMUSHclientDoc::Load_One_{kind}_XML')
 check(f'Get{kind}Map ()' not in one, "f'Get{kind}Map ()' not in one")
 section=block(xml,f'UINT CMUSHclientDoc::Load_{plural}_XML')
 check('iFlags, changes.back (), objectMap)' in section, "'iFlags, changes.back (), objectMap)' in section")
 check('objectMap, context, changes' in section, "'objectMap, context, changes' in section")
print('Source contracts: all item parsers, include calls, and rollback guards use explicit targets')
(OUT/'staging-validation.json').write_text(json.dumps({'result':'pass','command':cmd,'cases':cases,'sourceContracts':'pass','limits':'MFC containers and XML tokenization are substitutes. Section loaders, publication tails, indexes, retirement, rollback, and set publication are extracted source.'},indent=2)+'\n')

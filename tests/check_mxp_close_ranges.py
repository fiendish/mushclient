"""Check saved MXP close ranges, action publication, and traversal cost.
Uses production close-action and capture statements with portable MFC substitutes.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <cassert>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>
using namespace std;
using __int64=long long; using COLORREF=unsigned; using POSITION=size_t;
inline constexpr int ACTIONTYPE=3, START_TAG=4, COLOURTYPE=24, COLOUR_RGB=16, UNDERLINE=32;
struct CString: string { using string::string; CString(const string &s):string(s){} CString()=default;
bool IsEmpty()const{return empty();} void Replace(const char *from,const CString &to){size_t pos=0;while((pos=find(from,pos))!=npos){replace(pos,string(from).size(),to);pos+=to.size();}} };
int actionsAlive=0;
struct CAction { CAction(){++actionsAlive;} ~CAction(){--actionsAlive;} CString m_strAction,m_strHint,m_strVariable; int refs=1;
void AddRef(){++refs;} void Release(){if(!--refs) delete this;} };
struct CStyle { __int64 nRangeCreationNumber=0; unsigned short iFlags=0; COLORREF iForeColour=0,iBackColour=0;CAction *pAction=nullptr;~CStyle(){if(pAction)pAction->Release();} };
template<class T>struct List { vector<T*> items; size_t visits=0; POSITION GetHeadPosition(){return items.empty()?0:1;} POSITION GetTailPosition(){return items.size();} T*GetPrev(POSITION &p){++visits;return items.at(--p);} T*GetNext(POSITION &p){++visits;auto v=items.at(p-1);p=p<items.size()?p+1:0;return v;} };
struct CLine { __int64 nCreationNumber=0; List<CStyle> styleList; };
struct CPreparedMXPClose { __int64 iFirstContentLineCreationNumber=0; CString strText;set<__int64> contentStyleRangeNumbers; };
'''
COMMON = r'''
struct Fixture {
int actionCalls=0,failActionAt=0;vector<CString> actionOrder;
List<CLine> m_LineList;
bool m_bUseCustomLinkColour=false,m_bMudCanChangeLinkColour=false,m_bUnderlineHyperlinks=false,m_bMudCanRemoveUnderline=false;
COLORREF m_iHyperlinkColour=0;
void GetStyleRGB(CStyle *s,COLORREF &a,COLORREF &b){a=s->iForeColour;b=s->iBackColour;}
CAction *GetAction(const CString&a,const CString&b,const CString&c){if(++actionCalls==failActionAt)throw runtime_error("action allocation");actionOrder.push_back(a);auto*p=new CAction;p->m_strAction=a;p->m_strHint=b;p->m_strVariable=c;return p;}
void close(const CPreparedMXPClose &preparedClose){
POSITION linepos,stylepos;
const CString &strText=preparedClose.strText;
'''
MAIN = r'''
}};
struct World:Fixture {
vector<unique_ptr<CLine>> lines;vector<unique_ptr<CStyle>> styles;
CLine* line(long long id){auto l=make_unique<CLine>();l->nCreationNumber=id;auto*p=l.get();m_LineList.items.push_back(p);lines.push_back(std::move(l));return p;}
CStyle* style(CLine*l,long long range,bool active=true){auto s=make_unique<CStyle>();s->nRangeCreationNumber=range;s->iFlags=active?ACTIONTYPE:0;auto*p=s.get();l->styleList.items.push_back(p);styles.push_back(std::move(s));return p;}
void resetCounts(){m_LineList.visits=0;for(auto&l:lines)l->styleList.visits=0;}
size_t styleVisits(){size_t n=0;for(auto&l:lines)n+=l->styleList.visits;return n;}
};
void capture(CPreparedMXPClose& preparedClose,CLine*pLine2,CStyle*pStyle2){
auto&contentStyleRangeNumbers=preparedClose.contentStyleRangeNumbers;
@CAPTURE@
}
int main(){
for(int n:{1000,10000,100000}){
 World w;CStyle*target=nullptr;
 for(int i=1;i<=n;++i)target=w.style(w.line(i),i,i==n);
 CPreparedMXPClose p;p.strText="north";capture(p,w.lines.back().get(),target);
 w.resetCounts();w.close(p);
 assert(target->pAction&&target->pAction->m_strAction=="north");
 assert(w.styleVisits()==(EXPECT_HISTORY_SCAN?size_t(n):size_t(1)));
 for(int i=0;i<n-1;++i)assert(w.styles[i]->pAction==nullptr);
 cout<<"history="<<n<<" line_visits="<<w.m_LineList.visits<<" style_visits="<<w.styleVisits()<<'\n';
}
assert(actionsAlive==0);
{
 World w;auto*first=w.line(0);auto*a=w.style(first,5);auto*next=w.line(10);auto*b=w.style(next,6);
 CPreparedMXPClose p;p.strText="zero";capture(p,first,a);capture(p,next,b);
 assert(p.iFirstContentLineCreationNumber==0);w.close(p);assert(a->pAction&&b->pAction);
}
assert(actionsAlive==0);
for(bool prune:{false,true}){
 World w;auto*history=w.line(10);auto*unrelated=w.style(history,90);
 auto*first=w.line(20);auto*a=w.style(first,100);
 auto*second=w.line(30);auto*b=w.style(second,101);
 CPreparedMXPClose p;p.strText="text";capture(p,first,a);capture(p,second,b);assert(p.iFirstContentLineCreationNumber==20);
 // Split a saved logical range onto a later line. New callback ranges stay out.
 auto*later=w.line(40);auto*split=w.style(later,101);auto*callback=w.style(later,102);
 if(prune){w.m_LineList.items.erase(w.m_LineList.items.begin()+1);}
 // Move a saved style forward without changing its logical range.
 second->styleList.items.clear();later->styleList.items.push_back(b);
 w.close(p);
 assert(!unrelated->pAction&&!callback->pAction);
 assert(b->pAction&&split->pAction);
 assert(bool(a->pAction)==!prune);
 assert(w.actionCalls==(prune?2:3));
}
assert(actionsAlive==0);
for(int fail:{1,2}){
 World w;auto*l=w.line(10);auto*a=w.style(l,1);auto*b=w.style(l,2);
 a->pAction=w.GetAction("a &text;","hint &text;","var");b->pAction=w.GetAction("b &text;","hint &text;","var");
 CAction*oldA=a->pAction;CAction*oldB=b->pAction;
 CPreparedMXPClose p;p.strText="ok";capture(p,l,a);capture(p,l,b);
 w.m_bUseCustomLinkColour=true;w.m_bUnderlineHyperlinks=true;w.m_iHyperlinkColour=99;
 w.actionCalls=0;w.actionOrder.clear();w.failActionAt=fail;bool caught=false;
 try{w.close(p);}catch(const runtime_error&){caught=true;}
 assert(caught&&a->pAction==oldA&&b->pAction==oldB&&actionsAlive==2);
 assert(a->iFlags==ACTIONTYPE&&b->iFlags==ACTIONTYPE);
 w.failActionAt=0;w.actionCalls=0;w.actionOrder.clear();w.close(p);
 assert(w.actionOrder.size()==2&&w.actionOrder[0]=="a ok"&&w.actionOrder[1]=="b ok");
 assert(a->pAction->m_strHint=="hint ok"&&a->pAction->m_strVariable=="var");
 assert((a->iFlags&UNDERLINE)&&a->iForeColour==99);
}
assert(actionsAlive==0);
{
 World w;auto*l=w.line(10);auto*s=w.style(l,1);CPreparedMXPClose p;p.strText="none";
 w.close(p);assert(!s->pAction&&w.styleVisits()==(EXPECT_HISTORY_SCAN?1:0));
 // A removed first identity must not prevent a surviving newer range.
 auto*next=w.line(20);auto*survives=w.style(next,2);capture(p,l,s);capture(p,next,survives);
 w.m_LineList.items.erase(w.m_LineList.items.begin());w.close(p);assert(survives->pAction&&!s->pAction);
}
assert(actionsAlive==0);
cout<<"PASS close range capture, split/move, pruning, callback exclusion, staged failure and action order\n";
}
'''

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path);p.add_argument('--baseline-ref');a=p.parse_args()
 out=a.output or Path(tempfile.mkdtemp(prefix='mxp-close-'));out.mkdir(parents=True,exist_ok=True)
 source=(subprocess.check_output(['git','show',a.baseline_ref+':mxp/mxpCloseAtomic.cpp'],cwd=ROOT,text=True) if a.baseline_ref else (ROOT/'mxp/mxpCloseAtomic.cpp').read_text())
 prepare=(ROOT/'mxp/mxpClose.cpp').read_text();header=(ROOT/'OtherTypes.h').read_text()
 assert 'iFirstContentLineCreationNumber (0)' in header and 'preparedClose = CPreparedMXPClose ();' in prepare
 start=prepare.index('        if (contentStyleRangeNumbers.empty ())')
 end=prepare.index('contentStyleRangeNumbers.insert (pStyle2->nRangeCreationNumber);',start)+len('contentStyleRangeNumbers.insert (pStyle2->nRangeCreationNumber);')
 capture=prepare[start:end]
 start=source.index('    case MXP_ACTION_HYPERLINK:')+len('    case MXP_ACTION_HYPERLINK:');end=source.index('      break;  // end of MXP_ACTION_SEND',start)
 block=source[start:end];staged=source[source.index('struct CStagedMXPActionStyle'):source.index('// do the action required')]
 cpp=out/'mxp_close.cpp';cpp.write_text('#define EXPECT_HISTORY_SCAN '+str(int(bool(a.baseline_ref)))+'\n'+PREFIX+staged+COMMON+'const set<__int64> &contentStyleRangeNumbers=preparedClose.contentStyleRangeNumbers;\n'+block+MAIN.replace('@CAPTURE@',capture))
 exe=out/'mxp_close';subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe)],text=True,capture_output=True);(out/'run.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='');result.check_returncode()
 (out/'result.json').write_text(json.dumps({'source':a.baseline_ref or 'working tree','output':result.stdout,'passed':True,'limitation':'Extracted close/capture code with MFC/action substitutes. No native timing claim.'},indent=2)+'\n')
if __name__=='__main__':main()

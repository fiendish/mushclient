#ifdef NDEBUG
#error Output regression checks require assertions to be enabled.
#endif

#define ENDLINE "\r\n"
#define LOG_LINE 4
#define NO_COLOUR 0xffffffff
#define TRIGGER_MATCH_TEXT 0x0080
#define TRIGGER_MATCH_BACK 0x0800
#define TRIGGER_MATCH_HILITE 0x1000
#define TRIGGER_MATCH_BLINK 0x4000
#define TRIGGER_MATCH_INVERSE 0x8000
#define NORMAL 0
#define HILITE 1
#define UNDERLINE 2
#define BLINK 4
#define INVERSE 8
#define CHANGED 0x10
#define COLOURTYPE 0x300
#define COLOUR_RGB 0x200
#define OTHER_CUSTOM 16
#define TRIGGER_COLOUR_CHANGE_BOTH 0
#define TRIGGER_COLOUR_CHANGE_FOREGROUND 1
#define TRIGGER_COLOUR_CHANGE_BACKGROUND 2
// Focused MFC substitutes for the extracted output regression checks.
// These substitutes do not model the Windows UI or transaction rollback.

#include <cassert>
#include <list>
#include <deque>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <functional>
#include <iostream>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
#include <stdexcept>
using namespace std;
using __int64 = long long; using COLORREF = unsigned; using LPCTSTR = const char*;
#define ASSERT assert
#define MAX std::max
#define MIN std::min
#define TRUE true
#define STYLE_BITS 0x0fff
#define START_TAG 0x1000
#define COMMENT 1
#define USER_INPUT 2
#define NOTE_OR_COMMAND 3
#define SAMECOLOUR 65535
#define COLOUR_CUSTOM 0x100
#define COLOUR_ANSI 0
#define WHITE 7
#define BLACK 0
#define JUMP_SIZE 100
#define CP_UTF8 65001
#define NOSOUNDLIT "(No sound)"
#define RUNTIME_CLASS(x) 0
struct CMemoryException { void Delete(){delete this;} };
struct Node {void* value; Node *prev=nullptr,*next=nullptr;}; using POSITION=Node*;
template<class T> struct List {
 Node *head=nullptr,*tail=nullptr; int count=0;
 POSITION GetHeadPosition()const{return head;} POSITION GetTailPosition()const{return tail;}
 T GetAt(POSITION p){assert(p);return (T)p->value;}
 T GetHead(){assert(head);return (T)head->value;} T GetTail(){assert(tail);return (T)tail->value;}
 int GetCount(){return count;} bool IsEmpty(){return !count;}
 T GetNext(POSITION& p)const{assert(p);T v=(T)p->value;p=p->next;return v;}
 T GetPrev(POSITION& p){assert(p);T v=(T)p->value;p=p->prev;return v;}
 POSITION AddTail(T v){auto p=new Node{v,tail,nullptr}; if(tail)tail->next=p;else head=p;tail=p;++count;return p;}
 void RemoveAt(POSITION p){if(p->prev)p->prev->next=p->next;else head=p->next;if(p->next)p->next->prev=p->prev;else tail=p->prev;delete p;--count;}
 POSITION InsertAfter(POSITION p,T v){auto n=new Node{v,p,p->next};if(p->next)p->next->prev=n;else tail=n;p->next=n;++count;return n;}
 T RemoveTail(){T v=GetTail();RemoveAt(tail);return v;} T RemoveHead(){T v=GetHead();RemoveAt(head);return v;}
};
struct CString: string {using string::string; CString(const char*p,int n):string(p,n){} CString(const string&s):string(s){}
 CString Mid(int start,int n)const{return substr(start,n);}
 int Find(char c)const{auto p=find(c);return p==npos?-1:static_cast<int>(p);}
 void Replace(const char* from,const char* to){size_t p=0;while((p=find(from,p))!=npos){replace(p,strlen(from),to);p+=strlen(to);}}
 operator const char*()const{return c_str();} bool IsEmpty()const{return empty();} int GetLength()const{return size();}};
CString CFormat(const char* fmt,...) {char buf[1024];va_list ap;va_start(ap,fmt);vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);return buf;}
int GetRValue(COLORREF c){return c&255;}int GetGValue(COLORREF c){return (c>>8)&255;}int GetBValue(COLORREF c){return (c>>16)&255;}
CString FixHTMLString(CString s){s.Replace("&","&amp;");s.Replace("<","&lt;");s.Replace(">","&gt;");return s;}
CString FormatTime(int,const CString&s,bool){return s;}
struct CPaneStyle {string m_sText;COLORREF m_cText,m_cBack;int m_iStyle;
 CPaneStyle(const char*s,COLORREF a,COLORREF b,int f):m_sText(s),m_cText(a),m_cBack(b),m_iStyle(f){}};
long long seq=0; bool fail_next_line=false; bool fail_text_resize=false;
struct CAction {int refs=1;void AddRef(){++refs;} void Release(){if(!--refs)delete this;}};
struct CStyle {unsigned short iFlags=0,iLength=0;COLORREF iForeColour=7,iBackColour=0;CAction*pAction=nullptr;long long nCreationNumber=++seq,nRangeCreationNumber=nCreationNumber,nOutputAppendCreationNumber=0;~CStyle(){if(pAction)pAction->Release();}};
#define NEWSTYLE new CStyle
#define DELETESTYLE(x) delete (x)
struct CTime {static int GetCurrentTime(){return 0;}};
void QueryPerformanceCounter(int*){}
int MultiByteToWideChar(int,int,char* p,int n,void*,int){int count=0;for(int i=0;i<n;++i)if((p[i]&0xc0)!=0x80)++count;return count;}
struct CLine {bool hard_return=false;int len=0,iMemoryAllocated,m_nLineNumber,m_theTime=0,m_lineHighPerformanceTime=0;long long nCreationNumber=++seq;char*text;unsigned char flags=0;List<CStyle*>styleList;
 CLine(int number,int wrap,unsigned short f,COLORREF a,COLORREF b,bool utf){m_nLineNumber=number;if(fail_next_line){fail_next_line=false;throw new CMemoryException;}iMemoryAllocated=wrap*(utf?4:1);text=new char[iMemoryAllocated];auto s=new CStyle;s->iFlags=f;s->iForeColour=a;s->iBackColour=b;styleList.AddTail(s);}
 ~CLine(){delete[]text;while(!styleList.IsEmpty())delete styleList.RemoveHead();}
 void ResizeText(int n){assert(n>=len);if(fail_text_resize)throw new CMemoryException;auto p=new char[n];memcpy(p,text,len);delete[]text;text=p;iMemoryAllocated=n;}
};
struct CActiveTag{long long nOpeningStyleCreationNumber=0,nOpeningLineCreationNumber=0;};
class COutputLineBuffer;
struct COutputAppendTransaction {int created=0,wraps=0;long long Identity(){return 4242;}void TrackLine(const CLine*){}void RecordCreatedLine(){++created;}size_t PrepareWrap(CLine*,int){return 0;}void PublishWrap(size_t,long long,std::unique_ptr<COutputLineBuffer>){++wraps;}};
struct CView {bool IsKindOf(int){return false;}};struct CMUSHView:CView {void did_jump(){}};
struct AppType {bool m_bUpdateActivity=false;} App;
void TMessageBox(const char*s){cerr<<s<<'\n';}
struct CMUSHclientDoc {
 CLine*m_pCurrentLine=nullptr;List<CLine*>m_LineList;List<CActiveTag*>m_ActiveTagList;
 int m_nWrapColumn=80,m_total_lines=0,m_maxlines=1000,m_iFlags=0,m_echo_colour=SAMECOLOUR,m_iNoteTextColour=SAMECOLOUR,m_new_lines=0;
 COLORREF m_iForeColour=7,m_iBackColour=0;bool m_bUTF_8=false,m_wrap=true,m_indent_paras=false,m_bCustom16isDefaultColour=false;
 void*m_pActiveCommandView=this,*m_pActiveOutputView=this;POSITION m_pLinePositions[11]={};CString m_new_activity_sound;
 function<void()> callback;int processed=0,callbacks=0,disconnects=0;
 CMUSHclientDoc(){m_pCurrentLine=new CLine(++m_total_lines,80,0,7,0,false);m_LineList.AddTail(m_pCurrentLine);}
 ~CMUSHclientDoc(){while(!m_LineList.IsEmpty())delete m_LineList.RemoveHead();}
 void SendLineToPlugin(){++callbacks;if(callback)callback();}bool ProcessPreviousLine(){++processed;m_pCurrentLine->hard_return=true;return false;}
 POSITION GetFirstViewPosition(){return nullptr;}CView*GetNextView(POSITION&){abort();}
 long long m_iOutputGeneration=0; struct {int m_nCurrentLine=0;} m_DisplayFindInfo;
 void RemoveChunk();void OnConnectionDisconnect(){++disconnects;}void PlaySoundFile(const CString&){}
 struct CTriggerLineSnapshot {long long iCreationNumber;int iColumn,iLength;};
 bool m_bLineOmittedFromOutput=false;
 bool m_logfile=false,m_bLogRaw=false,m_bLogHTML=false,m_bLogInColour=false;
 CString m_strLogLinePreambleOutput,m_strLogLinePostambleOutput;
 int m_iStopTriggerEvaluation=0;long m_newlines_received=0;
 static constexpr int eStopEvaluatingTriggersInAllPlugins=2;
 deque<string> m_sRecentLines;list<CPaneStyle> m_OutstandingLines;
 string logText,screenText,replayedText;
 void WriteToLog(const char*s,size_t n){logText.append(s,n);}void WriteToLog(const CString&s){logText+=s;}
 void RefreshMXPMissingTagAnchors(){}
 void Screendraw(int,bool,const CString&s){screenText+=s;}
 void OutputOutstandingLines(){for(const auto& s:m_OutstandingLines)replayedText+=s.m_sText;m_OutstandingLines.clear();}
 void LogLineInHTMLcolour(POSITION,const map<__int64,int>* =nullptr);
 void Finalize(const vector<CTriggerLineSnapshot>&,long long,POSITION,bool,bool,const CString&,long=0);
 bool Match(const vector<CTriggerLineSnapshot>&,const CString&,int,int,function<void()>);
 bool FindStyle(const CLine*,int,int&,CStyle*&,POSITION&)const;
 void GetStyleRGB(CStyle* s,COLORREF& a,COLORREF& b)const{a=s->iForeColour;b=s->iBackColour;}
 void Colour(const vector<CTriggerLineSnapshot>&,const CString&,int,int,function<void()>,int=1,bool=false,int=0);
 bool StartNewLine(bool,int,bool=true,bool * =nullptr);
 bool FinishNewLine(int,bool,bool*);
 bool StartNewLine_KeepPreviousStyle(int,bool * =nullptr,bool=false);
 bool AddToLine(LPCTSTR,int);bool AddToLineInternal(LPCTSTR,int,COutputAppendTransaction*);
 void SimulateNewlineAndFill(){
  const bool started=StartNewLine(true,0);
  assert(started);
  string s(80,'B');
  const bool added=AddToLine(s.c_str(),0);
  assert(added);
 }
};

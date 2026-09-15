#include <chrono>
// The ASCII fixture retains DisplayMsg's newline and final partial-line delivery.
// Packet transformations, MXP, and unrelated triggers are disabled in these tests.
static void simulateASCII(CMUSHclientDoc& d,const string& input) {
 auto original=d.m_pCurrentLine;string old=text(original);
 for(char c:input) {
  if(c=='\n')assert(d.StartNewLine(true,0));
  else {char one[2]={c,0};assert(d.AddToLine(one,0));}
 }
 if(original!=d.m_pCurrentLine || old!=text(d.m_pCurrentLine))d.SendLineToPlugin();
}
static CLine* findLine(CMUSHclientDoc& d,long long id) {
 for(auto line:lines(d))if(line->nCreationNumber==id)return line;
 return nullptr;
}
static void checkPositions(CMUSHclientDoc& d) {
 int index=0;
 for(auto pos=d.m_LineList.GetHeadPosition();pos;++index) {
  auto current=pos;auto line=d.m_LineList.GetNext(pos);
  if(index%JUMP_SIZE==0)assert(d.m_pLinePositions[index/JUMP_SIZE]==current);
  assert(line->m_nLineNumber==d.m_total_lines-d.m_LineList.GetCount()+index+1);
 }
 for(int i=(index+JUMP_SIZE-1)/JUMP_SIZE;i<=d.m_maxlines/JUMP_SIZE;++i)
  assert(!d.m_pLinePositions[i]);
 assert(d.m_pCurrentLine==d.m_LineList.GetTail());checkStyles(d);
}
static vector<CMUSHclientDoc::CTriggerLineSnapshot> capture(CMUSHclientDoc& d,POSITION p) {
 vector<CLine*> result;while(p)result.push_back(d.m_LineList.GetNext(p));return snapshot(result);
}
static void callbackProgressCases() {
 for(bool prune:{false,true}) {
  CMUSHclientDoc d;d.m_maxlines=200;
  if(prune)while(d.m_LineList.GetCount()<200)assert(d.StartNewLine(true,COMMENT));
  assert(d.AddToLine(string(80,'A').c_str(),0));
  bool entered=false;int outerCalls=0;
  vector<string> trace;
  d.callback=[&] {
   trace.push_back(string(entered?"nested ":"outer ")+text(d.m_pCurrentLine));
   assert(trace.size()<=6); // A repeated outer wrap must fail this regression check.
   if(entered)return;
   ++outerCalls;entered=true;simulateASCII(d,"\n"+string(80,'B'));entered=false;
  };
  assert(d.AddToLine("Z",0));
  assert(outerCalls==1);assert(trace.size()==3);assert(text(d.m_pCurrentLine)=="Z");
  d.SendLineToPlugin(); // DisplayMsg completion for the original input.
  vector<string> expected={"outer "+string(80,'A'),"nested "+string(80,'A'),
   "nested "+string(80,'B'),"outer Z","nested Z","nested "+string(80,'B')};
  assert(trace==expected);assert(outerCalls==2);
  assert(allText(d)==string(80,'A')+string(80,'B')+"Z"+string(80,'B'));
  assert(d.m_iOutputGeneration==(prune?1:0));checkStyles(d);
 }
 // The callback's full line must retain its own normal word-wrap boundary.
 for(int originalShape:{0,1,2})for(int callbackShape:{0,1,2})for(bool utf:{false,true}) {
  CMUSHclientDoc d;d.m_bUTF_8=utf;
  string original(80,'A'),replacement(80,'B');
  if(originalShape)original[originalShape==1?72:79]=' ';
  if(callbackShape)replacement[callbackShape==1?72:79]=' ';
  assert(d.AddToLine(original.c_str(),0));bool entered=false,done=false;int deliveries=0;
  CAction* action=nullptr;
  d.callback=[&]{++deliveries;if(entered||done)return;done=true;entered=true;
   assert(d.StartNewLine(true,0));auto style=d.m_pCurrentLine->styleList.GetTail();
   style->iForeColour=13;style->pAction=action=new CAction;
   assert(d.AddToLine(replacement.c_str(),0));d.SendLineToPlugin();entered=false;};
  COutputAppendTransaction transaction;
  assert(d.AddToLineInternal("Z",0,&transaction));
  assert(allText(d)==original+replacement+"Z");assert(deliveries==3);
  assert(text(d.m_pCurrentLine)==(callbackShape==1?string(7,'B')+"Z":"Z"));
  assert(d.m_pCurrentLine->styleList.GetTail()->pAction==action);
  assert(d.m_pCurrentLine->styleList.GetTail()->iForeColour==13);
  assert(transaction.created==1);assert(transaction.wraps==(callbackShape==1?1:0));checkStyles(d);
 }
 // Restoration must retain saved word bytes when the callback throws after shrinking.
 {
  CMUSHclientDoc d;string original=string(72,'A')+" "+string(7,'B');
  assert(d.AddToLine(original.c_str(),0));auto originalLine=d.m_pCurrentLine;bool entered=false;
  d.callback=[&]{if(entered)return;entered=true;assert(d.StartNewLine(true,0));
   throw runtime_error("restore word");};
  bool caught=false;try{d.AddToLine("Z",0);}catch(const runtime_error& e){caught=string(e.what())=="restore word";}
  assert(caught);assert(text(originalLine)==original);checkStyles(d);
 }
 cout<<"Callback progress: exact six-delivery trace, pruning, replacement wrapping, and failure restoration passed\n";
}
static void matchingCases() {
 CMUSHclientDoc d;assert(d.AddToLine("abcd",0));auto first=d.m_pCurrentLine;
 assert(d.StartNewLine(false,0));assert(d.AddToLine("efgh",0));auto second=d.m_pCurrentLine;
 second->styleList.GetTail()->iForeColour=2;
 auto saved=snapshot({first,second});
 assert(d.Match(saved,"abcdefgh",4,TRIGGER_MATCH_TEXT|(2<<4),[]{}));
 assert(d.Match(saved,"abcdefgh",4,TRIGGER_MATCH_TEXT|(2<<4),[&]{
  delete d.m_LineList.RemoveHead();++d.m_iOutputGeneration;
  assert(d.StartNewLine(false,0));assert(d.AddToLine("callback",0));}));
 assert(!d.Match(saved,"abcdefgh",4,TRIGGER_MATCH_TEXT|(3<<4),[]{}));
 assert(!d.Match(saved,"abcdefgh",4,TRIGGER_MATCH_TEXT|(2<<4),[&]{second->text[0]='X';}));
 cout<<"Production matching block: scope, generation refresh, offsets, and text checks passed\n";
}
static void omissionCases() {
 // Modes: no pruning, old history pruned, partial paragraph pruned, all originals pruned.
 for(int mode:{0,1,2,3})for(int kind:{0,COMMENT,USER_INPUT})for(bool ended:{false,true}) {
  CMUSHclientDoc d;d.m_maxlines=200;POSITION first;
  if(mode==2) {
   while(d.m_LineList.GetCount()<100)assert(d.StartNewLine(true,COMMENT));
   first=d.m_LineList.GetTailPosition();d.m_pCurrentLine->flags=0;
   assert(d.AddToLine(string(8080,'A').c_str(),0));assert(d.m_LineList.GetCount()==200);
  } else {
   if(mode)while(d.m_LineList.GetCount()<200)assert(d.StartNewLine(true,COMMENT));
   first=d.m_LineList.GetTailPosition();d.m_pCurrentLine->flags=0;
   assert(d.AddToLine("matched",0));
  }
  d.m_pCurrentLine->hard_return=true;
  auto saved=capture(d,first);auto generation=d.m_iOutputGeneration;
  d.m_sRecentLines={"prior","matched"};d.m_newlines_received=2;
  assert(d.StartNewLine(true,kind));assert(d.AddToLine("callback",kind));
  auto callback=d.m_pCurrentLine;auto id=callback->nCreationNumber;
  auto style=callback->styleList.GetTail();style->pAction=new CAction;style->iForeColour=13;
  callback->hard_return=ended;
  if(ended && kind==0){d.m_sRecentLines.push_back("callback");++d.m_newlines_received;}
  if(mode==3)for(int i=0;i<100;++i)assert(d.StartNewLine(false,0));
  auto last=d.m_pCurrentLine;
  d.m_iStopTriggerEvaluation=d.eStopEvaluatingTriggersInAllPlugins;
  d.m_logfile=true;
  d.Finalize(saved,generation,first,true,false,"matched",2);
  assert(findLine(d,id)==callback);assert(d.m_pCurrentLine==last);
  assert(text(callback)=="callback");assert(callback->hard_return==ended);
  assert(callback->styleList.GetTail()==style);assert(style->iForeColour==13 && style->pAction);
  assert(!(callback->flags&LOG_LINE));assert(d.replayedText.empty());
  for(auto span:saved)assert(!findLine(d,span.iCreationNumber));
  assert(d.logText=="matched\n");
  if(ended && kind==0)assert(d.m_sRecentLines.back()=="callback");
  if(mode!=3)checkPositions(d);else checkStyles(d);
 }
 // Normal omission supplies an empty current line. Allocation failure keeps the old state.
 for(bool fail:{false,true}) {
  CMUSHclientDoc d;assert(d.AddToLine("matched",0));auto original=d.m_pCurrentLine;
  auto first=d.m_LineList.GetTailPosition();auto saved=snapshot({original});original->hard_return=true;
  fail_next_line=fail;bool caught=false;
  try{d.Finalize(saved,0,first,true,true,"matched");}catch(CMemoryException* e){caught=true;e->Delete();}
  assert(caught==fail);
  if(fail){assert(d.m_pCurrentLine==original);assert(text(original)=="matched");assert(d.m_iOutputGeneration==0);}
  else {assert(d.m_pCurrentLine!=original);assert(d.m_pCurrentLine->len==0);checkPositions(d);}
 }
 // The previous unterminated note remains the continuation, as before.
 {
  CMUSHclientDoc d;assert(d.AddToLine("note",COMMENT));auto note=d.m_pCurrentLine;note->flags=COMMENT;
  d.m_pLinePositions[0]=d.m_LineList.GetHeadPosition();
  assert(d.StartNewLine(false,0));assert(d.AddToLine("matched",0));auto original=d.m_pCurrentLine;
  auto first=d.m_LineList.GetTailPosition();auto saved=snapshot({original});original->hard_return=true;
  d.m_OutstandingLines.push_back(CPaneStyle(" deferred",13,0,HILITE));
  d.Finalize(saved,0,first,true,true,"matched");
  assert(d.m_pCurrentLine==note);assert(text(note)=="note");assert(d.replayedText==" deferred");checkPositions(d);
 }
 // Keep the existing replay branch when an original span carries note styles.
 {
  CMUSHclientDoc d;assert(d.AddToLine("note",COMMENT));auto original=d.m_pCurrentLine;
  original->flags=COMMENT;original->hard_return=true;auto saved=snapshot({original});
  d.Finalize(saved,0,d.m_LineList.GetTailPosition(),true,true,"note");
  assert(d.replayedText=="note\r\n");checkPositions(d);
 }
 cout<<"Omission: 24 callback/pruning cases, current lines, positions, history, failures, and deferred replay passed\n";
}
static void omissionTailContinuationCases() {
 for(int kind:{0,COMMENT,USER_INPUT})for(bool ended:{false,true}) {
  CMUSHclientDoc d;assert(d.AddToLine("matched",0));
  auto original=d.m_pCurrentLine;auto first=d.m_LineList.GetTailPosition();
  auto saved=snapshot({original});original->hard_return=true;
  assert(d.StartNewLine(false,kind));assert(d.AddToLine("callback",kind));
  auto callback=d.m_pCurrentLine;
  // Use the production line transition to complete callback output.
  if(ended)assert(d.StartNewLine(true,kind));
  auto continuation=d.m_pCurrentLine;
  assert(!continuation->hard_return);
  d.Finalize(saved,d.m_iOutputGeneration,first,true,true,"matched");
  assert(d.m_pCurrentLine==continuation && continuation==d.m_LineList.GetTail());
  assert(d.AddToLine(" next",kind));
  assert(text(callback)==(ended?"callback":"callback next"));
  assert(text(continuation)==(ended?" next":"callback next"));
  assert(callback->hard_return==ended && !continuation->hard_return);
  checkPositions(d);
 }
 cout<<"Omission continuation: output, note, and command callbacks with production line transitions passed\n";
}
// Build indexed history without running unrelated trigger callbacks.
static void seedOmissionHistory(CMUSHclientDoc& d,int count) {
 d.m_pCurrentLine->flags=COMMENT;
 d.m_pLinePositions[0]=d.m_LineList.GetHeadPosition();
 while(d.m_LineList.GetCount()<count)assert(d.StartNewLine(false,COMMENT));
}
static void omissionIndexBoundaryCases() {
 for(int prefix:{0,1,98,99,100,101,198,199,200,249})
  for(int omitted:{1,2,101})for(int later:{0,1,2,101}) {
   CMUSHclientDoc d;seedOmissionHistory(d,prefix+1);
   auto first=d.m_LineList.GetTailPosition();d.m_pCurrentLine->flags=0;
   vector<CLine*> originals={d.m_pCurrentLine};
   for(int i=1;i<omitted;++i) {
    assert(d.StartNewLine(false,0));originals.push_back(d.m_pCurrentLine);
   }
   auto saved=snapshot(originals);d.m_pCurrentLine->hard_return=true;
   vector<CLine*> callbacks;
   for(int i=0;i<later;++i) {
    assert(d.StartNewLine(false,COMMENT));assert(d.AddToLine("callback",COMMENT));
    callbacks.push_back(d.m_pCurrentLine);
   }
   auto before=lines(d);vector<POSITION> prefixPositions;
   for(int i=0;i*JUMP_SIZE<prefix;++i)prefixPositions.push_back(d.m_pLinePositions[i]);
   auto oldTotal=d.m_total_lines;
   d.Finalize(saved,d.m_iOutputGeneration,first,true,true,"");
   auto after=lines(d);
   // The old partial note remains current only when there is no later output.
   const bool reuseNote=prefix>0 && later==0;
   assert(d.m_LineList.GetCount()==prefix+later+(!reuseNote && later==0?1:0));
   assert(d.m_total_lines==oldTotal-omitted+(!reuseNote && later==0?1:0));
   for(int i=0;i<prefix;++i)assert(after[i]==before[i]);
   for(size_t i=0;i<prefixPositions.size();++i)assert(d.m_pLinePositions[i]==prefixPositions[i]);
   for(int i=0;i<later;++i)assert(after[prefix+i]==callbacks[i] && text(callbacks[i])=="callback");
   checkPositions(d);
  }
 cout<<"Omission index boundaries: 120 prefix, paragraph, and callback suffix cases passed\n";
}
static void omissionBufferScalingCases() {
 for(int history:{1000,100000}) {
  CMUSHclientDoc d;d.m_maxlines=200000;seedOmissionHistory(d,history+1);
  d.m_pCurrentLine->flags=0;
  // Close the history so each omission supplies an empty output continuation.
  auto previous=d.m_LineList.GetTailPosition();d.m_LineList.GetPrev(previous);
  d.m_LineList.GetAt(previous)->hard_return=true;
  size_t reads=0;constexpr int repetitions=256;
  const auto start=chrono::steady_clock::now();
  for(int i=0;i<repetitions;++i) {
   assert(d.AddToLine("omitted",0));auto original=d.m_pCurrentLine;
   auto saved=snapshot({original});original->hard_return=true;
   auto first=d.m_LineList.GetTailPosition();
   listReadCount=0;
   d.Finalize(saved,d.m_iOutputGeneration,first,true,true,"omitted");
   reads+=listReadCount;
   assert(d.m_pCurrentLine!=original && !d.m_pCurrentLine->hard_return);
   assert(d.m_pCurrentLine->len==0 && d.m_LineList.GetCount()==history+1);
  }
  auto elapsed=chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now()-start).count();
  cout<<"Omission buffer scaling: history="<<history<<", repetitions="<<repetitions
      <<", list reads="<<reads<<", microseconds="<<elapsed<<endl;
  assert(reads<=static_cast<size_t>(repetitions)*4);
  checkPositions(d);
 }
}
static void loggingCases() {
 for(int mode:{0,1,2}) {
  CMUSHclientDoc d;assert(d.AddToLine("a&b",0));auto first=d.m_LineList.GetHeadPosition();
  auto original=d.m_pCurrentLine;auto saved=snapshot({original});original->hard_return=true;
  assert(d.AddToLine(" appended",0)); // Same identity, outside the saved byte extent.
  assert(d.StartNewLine(false,COMMENT));assert(d.AddToLine("unlogged note",COMMENT));
  auto note=d.m_pCurrentLine;note->hard_return=true;
  d.m_logfile=true;d.m_bLogHTML=mode!=0;d.m_bLogInColour=mode==2;
  d.m_strLogLinePreambleOutput="[";d.m_strLogLinePostambleOutput="]";
  d.Finalize(saved,0,first,false,false,"a&b");
  assert(original->flags&LOG_LINE);assert(!(note->flags&LOG_LINE));
  assert(d.logText.find("unlogged")==string::npos && d.logText.find("appended")==string::npos);
  assert(d.logText.find(mode?"a&amp;b":"a&b")!=string::npos);
  if(mode<2)assert(d.logText==(mode?"[a&amp;b]\n":"[a&b]\n"));
  assert(d.screenText=="a&b");
  d.logText.clear();d.LogLineInHTMLcolour(first); // Existing unbounded caller contract.
  assert(d.logText.find("appended")!=string::npos);
 }
 // A pruned first span must not make a later callback line part of the HTML log.
 {
  CMUSHclientDoc d;assert(d.AddToLine("abcd",0));auto first=d.m_LineList.GetHeadPosition();
  auto a=d.m_pCurrentLine;assert(d.StartNewLine(false,0));assert(d.AddToLine("efgh",0));auto b=d.m_pCurrentLine;
  auto saved=snapshot({a,b});
  assert(d.StartNewLine(false,COMMENT));assert(d.AddToLine("callback",COMMENT));
  delete d.m_LineList.RemoveHead();++d.m_iOutputGeneration;
  d.m_logfile=d.m_bLogHTML=d.m_bLogInColour=true;
  d.Finalize(saved,0,first,false,false,"abcdefgh");
  assert(d.logText.find("efgh")!=string::npos);assert(d.logText.find("callback")==string::npos);
 }
 cout<<"Logging: plain text, HTML, colour HTML, byte bounds, pruning, and note flags passed\n";
}
static void loggingBoundaryCases() {
 CMUSHclientDoc d;
 assert(d.AddToLine("original",0));
 auto first=d.m_LineList.GetHeadPosition();
 map<__int64,int> lengths={{d.m_pCurrentLine->nCreationNumber,8}};
 // A callback replaces the paragraph end with later output. None of those
 // later lines belongs to the captured log entry, even without a hard return.
 for(int i=0;i<1000;++i) {
  auto line=new CLine(++d.m_total_lines,80,0,7,0,false);
  d.m_LineList.AddTail(line);d.m_pCurrentLine=line;
 }
 listReadCount=0;
 d.LogLineInHTMLcolour(first,&lengths);
 assert(d.logText.find("original")!=string::npos);
 assert(listReadCount<=3);
 d.logText.clear();lengths.clear();listReadCount=0;
 d.LogLineInHTMLcolour(first,&lengths);
 assert(d.logText.empty() && listReadCount==0);
 cout<<"Colour logging stops at the captured paragraph boundary\n";
}
static void followupCases() {
 callbackProgressCases();matchingCases();omissionCases();omissionTailContinuationCases();
 omissionIndexBoundaryCases();
 omissionBufferScalingCases();loggingCases();loggingBoundaryCases();
}

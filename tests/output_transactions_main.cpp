static string txText(CMUSHclientDoc& d) {
 string result;for(auto p=d.m_LineList.GetHeadPosition();p;){auto line=d.m_LineList.GetNext(p);result.append(line->text,line->len);}return result;
}
static void txCheck(CMUSHclientDoc& d) {
 for(auto p=d.m_LineList.GetHeadPosition();p;) {
  auto line=d.m_LineList.GetNext(p);int total=0;
  assert(line->len<=line->iMemoryAllocated);
  for(auto s=line->styleList.GetHeadPosition();s;)total+=line->styleList.GetNext(s)->iLength;
  assert(total==line->len);
 }
}
static bool hasOwner(CMUSHclientDoc& d,long long owner) {
 for(auto p=d.m_LineList.GetHeadPosition();p;){auto line=d.m_LineList.GetNext(p);
  for(auto s=line->styleList.GetHeadPosition();s;)if(line->styleList.GetNext(s)->nOutputAppendCreationNumber==owner)return true;}
 return false;
}
static void history(CMUSHclientDoc& d,int count) {
 d.m_maxlines=count+1000;
 while(d.m_LineList.GetCount()<count){auto line=new CLine(++d.m_total_lines,80,0,7,0,false);d.m_LineList.AddTail(line);d.m_pCurrentLine=line;}
}
static void performanceCases() {
 for (bool rollback : {false, true})
  for(int count:{1000,10000,100000})for(int mode:{0,1,2}) {
   CMUSHclientDoc d;history(d,count);
   if(mode==2) {
    assert(d.AddToLine((string(72,'A')+" ").c_str(),0));
    auto style=new CStyle;style->iForeColour=2;d.m_pCurrentLine->styleList.AddTail(style);
    assert(d.AddToLine(string(7,'B').c_str(),0));
   } else assert(d.AddToLine("base",0));
   auto before=txText(d);
   COutputAppendTransaction tx(&d,1);tx.MarkCurrentLineStyles();tx.PrepareAppendStyle();
   if(mode==1){bool created=false;assert(tx.StartNewLine(false,0,true,&created));assert(created);}
   else assert(d.AddToLineInternal("Z",0,&tx));
   lineReads=styleReads=0;
   if(rollback)tx.Rollback();else tx.Commit();
   auto visitedLines=lineReads,visitedStyles=styleReads;
   cout<<(rollback?"rollback":"commit")<<" history="<<count<<" mode="<<mode
       <<" lines="<<visitedLines<<" styles="<<visitedStyles<<"\n";
#ifdef EXPECT_HISTORY_SCAN
   assert(visitedStyles>=count);
#else
   // Removing a line still rebuilds the sparse position array once. Style
   // processing and individual identity lookups must not scan old history.
   assert(visitedLines <= (rollback && mode != 0 ? count + 20 : 5));
   assert(visitedStyles<=12);
#endif
   assert(!hasOwner(d,tx.Identity()) && !hasOwner(d,-tx.Identity()));
   assert(txText(d)==before+(rollback || mode==1?"":"Z"));txCheck(d);
  }
}
static void unseenLineCases() {
 for(bool commandActive:{false,true})for(bool outputActive:{false,true})
  for(bool rollback:{false,true}) {
   CMUSHclientDoc d;
   d.m_pActiveCommandView=commandActive?&d:nullptr;
   d.m_pActiveOutputView=outputActive?&d:nullptr;
   assert(d.AddToLine("base",0));
   COutputAppendTransaction tx(&d,4);tx.MarkCurrentLineStyles();
   bool created=false;
   assert(tx.StartNewLine(false,0,true,&created) && created);
   assert(d.m_new_lines==(!commandActive && !outputActive?1:0));
   assert(d.AddToLineInternal("new",0,&tx));
   if(rollback)tx.Rollback();else tx.Commit();
   assert(d.m_new_lines==(!rollback && !commandActive && !outputActive?1:0));
   assert(txText(d)==(rollback?"base":"basenew"));txCheck(d);
  }
 cout<<"Unseen-line commit and rollback: all four active-view states passed\n";
}
static void ownerCases() {
 // Inner commit must not clear either sign of the outer owner.
 {
  CMUSHclientDoc d;assert(d.AddToLine("base ",0));
  COutputAppendTransaction outer(&d,16);outer.MarkCurrentLineStyles();outer.PrepareAppendStyle();
  assert(d.AddToLineInternal("outer",0,&outer));
  COutputAppendTransaction inner(&d,16);inner.MarkCurrentLineStyles();inner.PrepareAppendStyle();
  assert(d.AddToLineInternal("inner",0,&inner));inner.Commit();
  assert(hasOwner(d,outer.Identity()) && hasOwner(d,-outer.Identity()));
  assert(!hasOwner(d,inner.Identity()));outer.Rollback();
  assert(txText(d)=="base inner");txCheck(d);
 }
 // An outer commit must leave an active inner owner for its own rollback.
 {
  CMUSHclientDoc d;assert(d.AddToLine("base ",0));
  COutputAppendTransaction outer(&d,16);outer.MarkCurrentLineStyles();outer.PrepareAppendStyle();
  assert(d.AddToLineInternal("outer",0,&outer));
  COutputAppendTransaction inner(&d,16);inner.MarkCurrentLineStyles();inner.PrepareAppendStyle();
  assert(d.AddToLineInternal("inner",0,&inner));outer.Commit();
  assert(hasOwner(d,inner.Identity()));inner.Rollback();assert(txText(d)=="base outer");txCheck(d);
 }
 // Real word wrapping splits one preserved style and moves another completely.
 for(bool rollback:{false,true}) {
  CMUSHclientDoc d;assert(d.AddToLine((string(72,'A')+" ").c_str(),0));
  auto style=new CStyle;style->pAction=new CAction;auto action=style->pAction;
  d.m_pCurrentLine->styleList.AddTail(style);assert(d.AddToLine("BBBBBBB",0));
  string before=txText(d);COutputAppendTransaction tx(&d,4);tx.MarkCurrentLineStyles();tx.PrepareAppendStyle();
  assert(d.AddToLineInternal("Z",0,&tx));assert(hasOwner(d,tx.Identity()) && hasOwner(d,-tx.Identity()));
  if(rollback)tx.Rollback();else tx.Commit();
  assert(txText(d)==before+(rollback?"":"Z"));assert(action->refs==(rollback?1:2));
  assert(!hasOwner(d,tx.Identity()) && !hasOwner(d,-tx.Identity()));txCheck(d);
 }
 // A callback's nested committed output survives the outer append rollback.
 {
  CMUSHclientDoc d;assert(d.AddToLine(string(80,'A').c_str(),0));
  COutputAppendTransaction outer(&d,4);outer.MarkCurrentLineStyles();outer.PrepareAppendStyle();bool done=false;
  long long callbackId=0;
  d.callback=[&]{if(done)return;done=true;assert(d.StartNewLine(true,0));callbackId=d.m_pCurrentLine->nCreationNumber;
   COutputAppendTransaction inner(&d,80);inner.MarkCurrentLineStyles();inner.PrepareAppendStyle();
   assert(d.AddToLineInternal(string(80,'B').c_str(),0,&inner));inner.Commit();};
  assert(d.AddToLineInternal("Z",0,&outer));outer.Rollback();
  assert(txText(d)==string(80,'A')+string(80,'B'));assert(d.m_pCurrentLine->nCreationNumber==callbackId);txCheck(d);
 }
 // Explicit ownership of an older style remains supported.
 {
  CMUSHclientDoc d;auto old=d.m_pCurrentLine->styleList.GetTail();history(d,1000);
  COutputAppendTransaction tx(&d,1);tx.OwnStyle(old);tx.Commit();assert(old->nOutputAppendCreationNumber==0);
 }
 // A style owned before list publication remains supported.
 {
  CMUSHclientDoc d;auto old=d.m_pCurrentLine;history(d,1000);
  COutputAppendTransaction tx(&d,1);auto style=new CStyle;tx.OwnStyle(style);old->styleList.AddTail(style);
  tx.Commit();assert(style->nOutputAppendCreationNumber==0);
 }
 // Losing the original boundary must not retain an invalid pointer or stale owner.
 for(bool appendAgain:{false,true}) {
  CMUSHclientDoc d;history(d,1000);COutputAppendTransaction tx(&d,1);tx.MarkCurrentLineStyles();
  delete d.m_LineList.RemoveTail();--d.m_total_lines;d.m_pCurrentLine=d.m_LineList.GetTail();++d.m_iOutputGeneration;
  if(appendAgain){tx.PrepareAppendStyle();assert(d.AddToLineInternal("Z",0,&tx));}
  tx.Commit();assert(!hasOwner(d,tx.Identity()) && !hasOwner(d,-tx.Identity()));txCheck(d);
 }
 // Callback pruning removes the boundary; later styles and their owners are still visited.
 {
  CMUSHclientDoc d;COutputAppendTransaction tx(&d,4);tx.MarkCurrentLineStyles();
  history(d,200);d.RemoveChunk();tx.PrepareAppendStyle();assert(d.AddToLineInternal("Z",0,&tx));
  tx.Commit();assert(!hasOwner(d,tx.Identity()) && !hasOwner(d,-tx.Identity()));txCheck(d);
 }
 // Colour-style splitting retains ownership on all fragments in the affected line.
 {
  CMUSHclientDoc d;assert(d.AddToLine("abcdef",0));COutputAppendTransaction tx(&d,4);tx.MarkCurrentLineStyles();
  auto first=d.m_pCurrentLine->styleList.GetTail();first->iLength=2;
  auto middle=new CStyle;middle->iLength=2;middle->nRangeCreationNumber=first->nRangeCreationNumber;
  middle->nOutputAppendCreationNumber=first->nOutputAppendCreationNumber;d.m_pCurrentLine->styleList.AddTail(middle);
  auto last=new CStyle;last->iLength=2;last->nRangeCreationNumber=first->nRangeCreationNumber;
  last->nOutputAppendCreationNumber=first->nOutputAppendCreationNumber;d.m_pCurrentLine->styleList.AddTail(last);
  tx.Commit();assert(first->nOutputAppendCreationNumber==0 && middle->nOutputAppendCreationNumber==0 && last->nOutputAppendCreationNumber==0);txCheck(d);
 }
 // Rollback retains line/list state restoration and visible allocation failures.
 {
  CMUSHclientDoc d;assert(d.AddToLine("base",0));auto original=d.m_pCurrentLine;
  COutputAppendTransaction tx(&d,1);tx.SetLineFlags(original,COMMENT);tx.SetListCount(7);
  tx.PrepareAppendStyle();assert(d.AddToLineInternal("x",0,&tx));tx.Rollback();
  assert(txText(d)=="base" && original->flags==0 && d.m_iListCount==0);txCheck(d);
 }
 {
  CMUSHclientDoc d;assert(d.AddToLine(string(80,'A').c_str(),0));COutputAppendTransaction tx(&d,1);
  tx.MarkCurrentLineStyles();tx.PrepareAppendStyle();fail_next_line=true;
  assert(!d.AddToLineInternal("Z",0,&tx));tx.Rollback();assert(txText(d)==string(80,'A'));txCheck(d);
 }
 cout<<"Complete transaction ownership, nested commit/rollback, moved/split styles, callbacks, pruning, and failures passed\n";
}
static void restorationCases() {
 // A partial-line callback can shrink the old line before wrap publication.
 for(bool failCallback:{false,true}) {
  CMUSHclientDoc d;
  const string before=string(72,'A')+" "+string(7,'B');
  assert(d.AddToLine(before.c_str(),0));
  auto old=d.m_pCurrentLine;
  COutputAppendTransaction tx(&d,1);tx.MarkCurrentLineStyles();tx.PrepareAppendStyle();
  bool called=false;
  d.callback=[&]{if(called)return;called=true;
   d.m_nWrapColumn=20;old->ResizeText(old->len);
   if(failCallback){fail_text_resize=true;throw 37;}};
  bool caught=false;
  try {assert(d.AddToLineInternal("Z",0,&tx));}
  catch(int error){assert(error==37);caught=true;}
  assert(called && caught==failCallback);
  // Rollback must also work when another resize allocation cannot succeed.
  fail_text_resize=true;tx.Rollback();fail_text_resize=false;
  assert(txText(d)==before && d.m_pCurrentLine==old);txCheck(d);
 }
 // Restore a nontransactional append before rethrowing the original callback error.
 {
  CMUSHclientDoc d;const string before=string(72,'A')+" "+string(7,'B');
  assert(d.AddToLine(before.c_str(),0));
  d.callback=[&]{d.m_pCurrentLine->ResizeText(d.m_pCurrentLine->len);
   fail_text_resize=true;throw 41;};
  bool caught=false;
  try{d.AddToLine("Z",0);}catch(int error){assert(error==41);caught=true;}
  fail_text_resize=false;assert(caught && txText(d)==before);txCheck(d);
 }
 // The callback can make the continuation narrower than the saved word.
 for(bool failContinuation:{false,true}) {
  CMUSHclientDoc d;const string before=string(20,'A')+" "+string(59,'B');
  assert(d.AddToLine(before.c_str(),0));
  COutputAppendTransaction tx(&d,1);tx.MarkCurrentLineStyles();tx.PrepareAppendStyle();
  bool called=false;
  d.callback=[&]{if(called)return;called=true;d.m_nWrapColumn=20;
   d.m_pCurrentLine->ResizeText(d.m_pCurrentLine->len);
   fail_text_resize=failContinuation;};
  bool caught=false;
  try{assert(d.AddToLineInternal("Z",0,&tx));}
  catch(CMemoryException* error){caught=true;error->Delete();}
  assert(caught==failContinuation);txCheck(d);
  tx.Rollback();fail_text_resize=false;assert(txText(d)==before);txCheck(d);
 }
 // Allocation cleanup can delete every buffered line at exactly JUMP_SIZE.
 for(int count:{JUMP_SIZE-1,JUMP_SIZE,JUMP_SIZE+1}) {
  CMUSHclientDoc d;history(d,count);fail_next_line=true;
  assert(!d.FinishNewLine(0,true,nullptr));
  assert(d.disconnects==1);
  assert(d.m_pCurrentLine==(d.m_LineList.IsEmpty()?nullptr:d.m_LineList.GetTail()));
  if(d.m_pCurrentLine)assert(d.AddToLine("after failure",0));
  else {assert(!d.AddToLine("after failure",0));assert(d.StartNewLine(false,0));
   assert(d.AddToLine("after recovery",0));}
  txCheck(d);
 }
 cout<<"Shrunk-buffer restoration, original exceptions, narrow continuation, and empty-buffer allocation cleanup passed\n";
}
int main(){performanceCases();ownerCases();unseenLineCases();restorationCases();}

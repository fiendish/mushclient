// Regression scenarios for the production append and trigger colour loops.
static string text(CLine* line) { return string(line->text,line->len); }
static vector<CLine*> lines(CMUSHclientDoc& d) {
 vector<CLine*> result;
 for(auto p=d.m_LineList.GetHeadPosition();p;)result.push_back(d.m_LineList.GetNext(p));
 return result;
}
static string allText(CMUSHclientDoc& d) {
 string result;for(auto line:lines(d))result+=text(line);return result;
}
static void checkStyles(CMUSHclientDoc& d) {
 for(auto line:lines(d)) {
  assert(line->len<=line->iMemoryAllocated);int count=0;
  for(auto p=line->styleList.GetHeadPosition();p;)count+=line->styleList.GetNext(p)->iLength;
  assert(count==line->len);
 }
}
static void appendCases() {
 for(int shape=0;shape<3;++shape)for(int fill:{0,7,79,80})for(bool utf:{false,true}) {
  CMUSHclientDoc d;d.m_bUTF_8=utf;
  string original(80,'A');
  if(shape==1)original[72]=' '; // move a word to the next line
  if(shape==2)original[79]=' '; // no saved word
  assert(d.AddToLine(original.c_str(),0));
  CLine* supplied=nullptr;bool done=false;int deliveries=0;
  d.callback=[&]{++deliveries;if(done)return;done=true;
   assert(d.StartNewLine(true,0));supplied=d.m_pCurrentLine;
   auto style=supplied->styleList.GetTail();style->iForeColour=13;
   style->pAction=new CAction;
   assert(d.AddToLine(string(fill,'B').c_str(),0));
  };
  assert(d.AddToLine("Z",0));
  assert(allText(d)==original+string(fill,'B')+"Z");
  assert(d.m_pCurrentLine->styleList.GetTail()->iForeColour==13);
  assert(d.m_pCurrentLine->styleList.GetTail()->pAction==supplied->styleList.GetTail()->pAction);
  assert(text(supplied)==string(fill,'B')+(fill<80?"Z":""));
  assert(deliveries==2+(fill==80?1:0));checkStyles(d);
 }
 cout<<"24 callback width, word-wrap, style, and action cases passed\n";
 for(int shape=0;shape<3;++shape) {
  CMUSHclientDoc d;string original(80,'A');
  if(shape==1)original[72]=' ';
  if(shape==2){original[78]=char(0x81);original[79]=char(0x40);}
  assert(d.AddToLine(original.c_str(),0));assert(d.AddToLine("Z",0));
  assert(allText(d)==original+"Z");checkStyles(d);
 }
 for(int repeats:{1,3}) {
  CMUSHclientDoc d;assert(d.AddToLine(string(80,'A').c_str(),0));
  int remaining=repeats;bool entered=false;
  d.callback=[&]{if(entered||!remaining)return;--remaining;entered=true;
   assert(d.StartNewLine(true,0));assert(d.AddToLine(string(80,'B').c_str(),0));entered=false;};
  assert(d.AddToLine("Z",0));assert(remaining==0);
  assert(allText(d)==string(80,'A')+string(80*repeats,'B')+"Z");checkStyles(d);
 }
 for(int failure:{0,1,2}) {
  CMUSHclientDoc d;assert(d.AddToLine(string(80,'A').c_str(),0));bool done=false;
  d.callback=[&]{if(done)return;done=true;assert(d.StartNewLine(true,0));
   assert(d.AddToLine(string(80,'B').c_str(),0));
   if(failure==0)fail_next_line=true;
   if(failure==1)throw runtime_error("callback failure");
   if(failure==2){while(!d.m_LineList.IsEmpty())delete d.m_LineList.RemoveHead();d.m_pCurrentLine=nullptr;++d.m_iOutputGeneration;}
  };
  if(failure==1) {
   bool caught=false;try{d.AddToLine("Z",0);}catch(const runtime_error& e){caught=string(e.what())=="callback failure";}
   assert(caught);
  } else assert(!d.AddToLine("Z",0));
  if(failure!=2)assert(allText(d)==string(80,'A')+string(80,'B'));
  assert(d.disconnects==(failure==0?1:0));checkStyles(d);
 }
 {
  CMUSHclientDoc d;assert(d.AddToLine(string(80,'A').c_str(),0));bool done=false;
  d.callback=[&]{if(done)return;done=true;assert(d.StartNewLine(true,0));assert(d.AddToLine(string(80,'B').c_str(),0));};
  COutputAppendTransaction transaction;
  assert(d.AddToLineInternal("Z",0,&transaction));assert(transaction.created==1);
  auto output=lines(d);assert(output[1]->styleList.GetTail()->nOutputAppendCreationNumber==0);
  assert(output[2]->styleList.GetTail()->nOutputAppendCreationNumber==4242);checkStyles(d);
 }
 {
  CMUSHclientDoc d;d.m_bUTF_8=true;assert(d.StartNewLine(false,COMMENT));
  string original;for(int i=0;i<80;++i)original+="\xc3\xa9";
  assert(d.AddToLine(original.c_str(),0));bool done=false;
  d.callback=[&]{if(done)return;done=true;assert(d.StartNewLine(true,0));assert(d.AddToLine(original.c_str(),0));};
  assert(d.AddToLine("\xc3\xa9",0));assert(allText(d)==original+original+"\xc3\xa9");
  assert(text(d.m_pCurrentLine)=="\xc3\xa9");checkStyles(d);
 }
 {
  CMUSHclientDoc d;assert(d.AddToLine(string(80,'A').c_str(),0));int callbacks=d.callbacks;
  assert(d.AddToLine("Z",COMMENT));assert(d.callbacks==callbacks);checkStyles(d);
 }
 {
  CMUSHclientDoc d;string prefix=string(72,'A')+" BB";
  assert(d.AddToLine(prefix.c_str(),0));
  auto marker=new CStyle;marker->iFlags=START_TAG;
  d.m_pCurrentLine->styleList.AddTail(marker);
  d.m_pCurrentLine->styleList.AddTail(new CStyle);
  CActiveTag tag;tag.nOpeningStyleCreationNumber=marker->nCreationNumber;
  tag.nOpeningLineCreationNumber=d.m_pCurrentLine->nCreationNumber;
  d.m_ActiveTagList.AddTail(&tag);
  assert(d.AddToLine("BBBBBZ",0));
  assert(allText(d)==prefix+"BBBBBZ");
  assert(tag.nOpeningLineCreationNumber==d.m_pCurrentLine->nCreationNumber);
  bool found=false;for(auto p=d.m_pCurrentLine->styleList.GetHeadPosition();p;) {
   auto style=d.m_pCurrentLine->styleList.GetNext(p);
   if(style->nCreationNumber==tag.nOpeningStyleCreationNumber) {
    assert(style->iFlags&START_TAG);found=true;
   }
  }
  assert(found);checkStyles(d);d.m_ActiveTagList.RemoveHead();
 }
 cout<<"Repeated callbacks, visible failures, null output, transaction ownership, UTF-8, and note controls passed\n";
}
#ifndef APPEND_ONLY
static vector<CMUSHclientDoc::CTriggerLineSnapshot> snapshot(const vector<CLine*>& ls) {
 vector<CMUSHclientDoc::CTriggerLineSnapshot> result;int column=0;
 for(auto line:ls){result.push_back({line->nCreationNumber,column,line->len});column+=line->len;}
 return result;
}
static vector<int> changed(CLine* line) {
 vector<int> result;for(auto p=line->styleList.GetHeadPosition();p;){auto style=line->styleList.GetNext(p);
 for(int i=0;i<style->iLength;++i)result.push_back((style->iFlags&CHANGED)!=0);}
 return result;
}
static void colourCases() {
 // Note reaches the pruning threshold while the matched line survives.
 {
  CMUSHclientDoc d;d.m_maxlines=200;
  while(d.m_LineList.GetCount()<200)assert(d.StartNewLine(true,COMMENT));
  assert(d.AddToLine("matched",0));auto matched=d.m_pCurrentLine;auto saved=snapshot({matched});
  d.Colour(saved,"matched",1,6,[&]{assert(d.StartNewLine(true,COMMENT));assert(d.AddToLine("note",COMMENT));});
  assert(d.m_iOutputGeneration==1);assert(changed(matched)==vector<int>({0,1,1,1,1,1,0}));
  assert(changed(d.m_pCurrentLine)==vector<int>(4,0));checkStyles(d);
 }
 // Prune the first wrapped span. Keep the offsets of the second and third spans.
 for(int removal:{0,1,2,3}) {
  CMUSHclientDoc d;vector<CLine*> matched;
  for(auto word:{"abcd","efgh","ijkl"}){assert(d.AddToLine(word,0));matched.push_back(d.m_pCurrentLine);assert(d.StartNewLine(false,COMMENT));}
  auto saved=snapshot(matched);
  d.Colour(saved,"abcdefghijkl",2,10,[&]{
   for(int i=0;i<removal;++i)delete d.m_LineList.RemoveHead();
   if(removal)++d.m_iOutputGeneration;
   assert(d.AddToLine("callback",COMMENT));
  });
  vector<vector<int>> expected={{0,0,1,1},{1,1,1,1},{1,1,0,0}};
  for(int i=removal;i<3;++i)assert(changed(matched[i])==expected[i]);
  assert(changed(d.m_pCurrentLine)==vector<int>(8,0));checkStyles(d);
 }
 // Repeated matches, subsequent trigger calls, unchanged generation, and tail growth.
 {
  CMUSHclientDoc d;assert(d.AddToLine("ab ab ab",0));auto line=d.m_pCurrentLine;auto saved=snapshot({line});
  for(int offset:{0,3,6})d.Colour(saved,"ab ab ab",offset,offset+2,[&]{if(offset==0)assert(d.AddToLine(" appended",0));});
  assert(changed(line)==vector<int>({1,1,0,1,1,0,1,1,0,0,0,0,0,0,0,0,0}));checkStyles(d);
 }
 // Deleted text and changed text are not replaced by new callback text.
 for(bool truncate:{false,true}) {
  CMUSHclientDoc d;assert(d.AddToLine("abcdef",0));auto line=d.m_pCurrentLine;auto saved=snapshot({line});
  d.Colour(saved,"abcdef",0,6,[&]{++d.m_iOutputGeneration;
   if(truncate){line->len=3;line->styleList.GetTail()->iLength=3;}
   else line->text[0]='X';});
  assert(changed(line)==vector<int>(truncate?3:6,truncate?1:0));checkStyles(d);
 }
 for(int variant:{0,1,2}) {
  CMUSHclientDoc d;assert(d.AddToLine("abcdef",0));auto line=d.m_pCurrentLine;auto saved=snapshot({line});
  d.Colour(saved,"abcdef",1,4,[]{},SAMECOLOUR,variant==0,variant==2?HILITE:NORMAL);
  assert(changed(line)==(variant==2?vector<int>({0,1,1,1,0,0}):vector<int>(6,0)));checkStyles(d);
 }
 {
  CMUSHclientDoc d;assert(d.AddToLine("abcdef",0));auto line=d.m_pCurrentLine;
  auto original=line->styleList.GetTail();auto range=original->nRangeCreationNumber;
  original->nOutputAppendCreationNumber=4242;original->pAction=new CAction;
  auto action=original->pAction;auto saved=snapshot({line});
  d.Colour(saved,"abcdef",1,4,[]{});
  assert(line->styleList.GetCount()==3);
  for(auto p=line->styleList.GetHeadPosition();p;) {
   auto style=line->styleList.GetNext(p);
   assert(style->nRangeCreationNumber==range);
   assert(style->nOutputAppendCreationNumber==4242);
   assert(style->pAction==action);
  }
  assert(action->refs==3);checkStyles(d);
 }
 cout<<"Colour pruning, removed spans, repeated matches, callback output, invalidation, and style-only controls passed\n";
}
#endif
int main() {
 appendCases();
#ifndef APPEND_ONLY
 colourCases();
#endif
 cout<<"All output regression checks passed\n";
}

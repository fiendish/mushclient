/* lrexlib.c - POSIX & PCRE regular expression library */
/* POSIX regexs can use Spencer extensions for matching NULs if available */
/* Reuben Thomas   nov00-25aug04 */
/* Shmuel Zeigerman   may04-25aug04 */

// Implements:

//   rex.flags
//   re = rex.new
//   rex.version

//   re:exec
//   re:match
//   re:gmatch

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LUA_52
    #include "..\..\lua52\src\lua.h"
    #include "..\..\lua52\src\lauxlib.h"
#else
    #include "..\lua.h"
    #include "..\lauxlib.h"
#endif

#define LREXLIB_PCRE      // NJG
#pragma warning( disable: 4057)  // 'const unsigned char *' differs in indirection to slightly different base types from 'const char *'

/* Sanity check */
#if !defined(LREXLIB_POSIX) && !defined(LREXLIB_PCRE)
#error Define LREXLIB_POSIX or LREXLIB_PCRE, otherwise this library is useless!
#endif

static void L_lua_error(lua_State *L, const char *message)
{
  lua_pushstring(L, message);
  lua_error(L);
}

static void *Lmalloc(lua_State *L, size_t size)
{
  void *p = malloc(size);
  if(p == NULL)
    L_lua_error(L, "malloc failed");
  return p;
}

static int get_startoffset(lua_State *L, int stackpos, size_t len)
{
  int startoffset = luaL_optint(L, stackpos, 1);
  if(startoffset > 0)
    startoffset--;
  else if(startoffset < 0) {
    startoffset += len;
    if(startoffset < 0)
      startoffset = 0;
  }
  return startoffset;
}

static int regex_tostring (lua_State *L, const char* type_handle, const char* type_name)
{
  char buf[256];
  void *udata = luaL_checkudata(L, 1, type_handle);
  if(udata) {
    sprintf(buf, "%s (%p)", type_name, udata);
    lua_pushstring(L, buf);
  }
  else {
    sprintf(buf, "must be userdata of type '%s'", type_name);
    luaL_argerror(L, 1, buf);
  }
  return 1;
}

typedef struct { const char* key; int val; } flags_pair;

static int regex_get_flags (lua_State *L, const flags_pair *arr)
{
  const flags_pair *p;
  lua_newtable(L);
  for(p=arr; p->key != NULL; p++) {
    lua_pushstring(L, p->key);
    lua_pushnumber(L, p->val);
    lua_rawset(L, -3);
  }
  return 1;
}

/* POSIX regex methods */

#ifdef LREXLIB_POSIX

#include <regex.h>

/* Test if regex.h corresponds to the extended POSIX library,
   i.e. H.Spencer's
*/
#if defined(REG_BASIC) && defined(REG_STARTEND)
#define LREXLIB_POSIX_EXT
#endif

const char posix_handle[] = "posix_regex_handle";
const char posix_typename[] = "posix_regex";

typedef struct {
  regex_t r;
  regmatch_t *match;
} posix2;      /* a better name is needed */

static int posix_comp(lua_State *L) {
  int res;
  posix2 *p2;
  size_t clen;
  const char *pattern = luaL_checklstring(L, 1, &clen);
  int cflags = luaL_optint(L, 2, REG_EXTENDED);

#ifdef LREXLIB_POSIX_EXT
  if((cflags & REG_EXTENDED) && (cflags & REG_NOSPEC)) {
    L_lua_error(L,
      "flags REG_EXTENDED and REG_NOSPEC must not be specified together");
  }
#endif

  p2 = (posix2 *)lua_newuserdata(L, sizeof(posix2));
  p2->match = NULL;

#ifdef LREXLIB_POSIX_EXT
  if(cflags & REG_PEND)
    p2->r.re_endp = pattern + clen;
#endif

  res = regcomp(&p2->r, pattern, cflags);
  if (res) {
    size_t sz = regerror(res, &p2->r, NULL, 0);
    char *errbuf = (char *) Lmalloc(L, sz);
    regerror(res, &p2->r, errbuf, sz);
    lua_pushstring(L, errbuf);
    free(errbuf);
    lua_error(L);
  }

  p2->match =
    (regmatch_t *) Lmalloc(L, (p2->r.re_nsub + 1) * sizeof(regmatch_t));
  luaL_getmetatable(L, posix_handle);
  lua_setmetatable(L, -2);
  return 1;
}

static void posix_getargs
  (lua_State *L, posix2 **p2, const char **text, size_t *text_len)
{
  *p2 = (posix2 *)luaL_checkudata(L, 1, posix_handle);
  luaL_argcheck(L, *p2 != NULL, 1, "compiled regexp expected");
  *text = luaL_checklstring(L, 2, text_len);
}

typedef void (*posix_push_matches)
  (lua_State *L, const char *text, int startoffset,
  regmatch_t *match, size_t ncapt);

static void posix_push_substrings
  (lua_State *L, const char *text, int startoffset,
  regmatch_t *match, size_t ncapt)
{
  size_t i;
  if(startoffset) {
    /* suppress compiler warning */
  }
  lua_newtable(L);
  for (i = 1; i <= ncapt; i++) {
    if (match[i].rm_so >= 0) {
      lua_pushlstring
        (L, text + match[i].rm_so, match[i].rm_eo - match[i].rm_so);
    }
    else
      lua_pushboolean(L, 0);
    lua_rawseti(L, -2, i);
  }
}

static void posix_push_offsets
  (lua_State *L, const char *text, int startoffset,
  regmatch_t *match, size_t ncapt)
{
  size_t i, j;
  if(text) {
    /* suppress compiler warning */
  }
  lua_newtable(L);
  for (i=1, j=1; i <= ncapt; i++) {
    if (match[i].rm_so >= 0) {
      lua_pushnumber(L, startoffset + match[i].rm_so + 1);
      lua_rawseti(L, -2, j++);
      lua_pushnumber(L, startoffset + match[i].rm_eo);
      lua_rawseti(L, -2, j++);
    }
    else {
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
    }
  }
}

static int posix_match_generic(lua_State *L, posix_push_matches push_matches)
{
  size_t elen;
  const char *text;
  posix2 *p2;
  int startoffset;
  int res;

#ifdef LREXLIB_POSIX_EXT
  int eflags = luaL_optint(L, 4, REG_STARTEND);
#else
  int eflags = luaL_optint(L, 4, 0);
#endif

  posix_getargs(L, &p2, &text, &elen);
  startoffset = get_startoffset(L, 3, elen);

#ifdef LREXLIB_POSIX_EXT
  if(eflags & REG_STARTEND) {
    p2->match[0].rm_so = startoffset;
    p2->match[0].rm_eo = elen;
    startoffset = 0;
  }
  else
    text += startoffset;
#else
  text += startoffset;
#endif

  /* execute the search */
  res = regexec(&p2->r, text, p2->r.re_nsub + 1, p2->match, eflags);
  if (res == 0) {
    lua_pushnumber(L, p2->match[0].rm_so + 1 + startoffset);
    lua_pushnumber(L, p2->match[0].rm_eo + startoffset);
    (*push_matches)(L, text, startoffset, p2->match, p2->r.re_nsub);
    return 3;
  }
  return 0;
}

static int posix_match(lua_State *L)
{
  return posix_match_generic(L, posix_push_substrings);
}

static int posix_exec(lua_State *L)
{
  return posix_match_generic(L, posix_push_offsets);
}

static int posix_gmatch(lua_State *L) {
  int res;
  size_t len;
  size_t nmatch = 0, limit = 0;
  const char *text;
  posix2 *p2;
  size_t maxmatch = (size_t)luaL_optnumber(L, 4, 0);

#ifdef LREXLIB_POSIX_EXT
  int eflags = luaL_optint(L, 5, REG_STARTEND);
#else
  int eflags = luaL_optint(L, 5, 0);
#endif

  posix_getargs(L, &p2, &text, &len);
  luaL_checktype(L, 3, LUA_TFUNCTION);

  if(maxmatch > 0) /* this must be stated in the docs */
    limit = 1;

  while (!limit || nmatch < maxmatch) {

#ifdef LREXLIB_POSIX_EXT
    if(eflags & REG_STARTEND) {
      p2->match[0].rm_so = 0;
      p2->match[0].rm_eo = len;
    }
#endif

    res = regexec(&p2->r, text, p2->r.re_nsub + 1, p2->match, eflags);
    if (res == 0) {
      nmatch++;
      lua_pushvalue(L, 3);
      lua_pushlstring(L, text + p2->match[0].rm_so,
                      p2->match[0].rm_eo - p2->match[0].rm_so);
      posix_push_substrings(L, text, 0, p2->match, p2->r.re_nsub);
      lua_call(L, 2, 1);
      if(lua_toboolean(L, -1))
        break;
      lua_pop(L, 1);
      text += p2->match[0].rm_eo;

#ifdef LREXLIB_POSIX_EXT
      if(eflags & REG_STARTEND)
        len -= p2->match[0].rm_eo;
#endif

    } else
      break;
  }
  lua_pushnumber(L, nmatch);
  return 1;
}

static int posix_gc (lua_State *L) {
  posix2 *p2 = (posix2 *)luaL_checkudata(L, 1, posix_handle);
  if (p2) {
    regfree(&p2->r);
    if(p2->match)
      free(p2->match);
  p2 = NULL;
  }
  return 0;
}

static int posix_tostring (lua_State *L) {
  return regex_tostring(L, posix_handle, posix_typename);
}

static flags_pair posix_flags[] =
{
#ifdef LREXLIB_POSIX_EXT
  { "BASIC",    REG_BASIC },
  { "NOSPEC",   REG_NOSPEC },
  { "PEND",     REG_PEND },
  { "STARTEND", REG_STARTEND },
#endif
  { "EXTENDED", REG_EXTENDED },
  { "ICASE",    REG_ICASE },
  { "NOSUB",    REG_NOSUB },
  { "NEWLINE",  REG_NEWLINE },
  { "NOTBOL",   REG_NOTBOL },
  { "NOTEOL",   REG_NOTEOL },
  { NULL, 0 }
};

static int posix_get_flags (lua_State *L) {
  return regex_get_flags(L, posix_flags);
}

static const luaL_Reg posixmeta[] = {
  {"exec",       posix_exec},
  {"match",      posix_match},
  {"gmatch",     posix_gmatch},
  {"__gc",       posix_gc},
  {"__tostring", posix_tostring},
  {NULL, NULL}
};

#endif /* LREXLIB_POSIX */


/* PCRE methods */

#ifdef LREXLIB_PCRE

#include <locale.h>

#define PCRE_EXP_DECL extern

#include "..\pcre\pcre.h"

const char pcre_handle[] = "pcre_regex_handle";
const char pcre_typename[] = "pcre_regex";

typedef struct {
  pcre *pr;
  pcre_extra *extra;
  int *match;
  int ncapt;
  const unsigned char *tables;
} pcre2;      /* a better name is needed */

static const unsigned char *Lpcre_maketables(lua_State *L, int stackpos)
{
  const unsigned char *tables;
  char old_locale[256];
  const char *locale = luaL_checkstring(L, stackpos);

  strcpy(old_locale, setlocale(LC_CTYPE, NULL)); /* store the locale */
  if(NULL == setlocale(LC_CTYPE, locale))        /* set new locale */
    L_lua_error(L, "cannot set locale");
  tables = pcre_maketables();              /* make tables with new locale */
  setlocale(LC_CTYPE, old_locale);         /* restore the old locale */
  return tables;
}

static int Lpcre_comp(lua_State *L)
{
  char buf[256];
  const char *error;
  int erroffset;
  pcre2 *ud;
  size_t clen;  /* clen isn't used in PCRE */
  const char *pattern = luaL_checklstring(L, 1, &clen);
  int cflags = luaL_optint(L, 2, 0);
  int has_locale = lua_gettop(L) > 2 && !lua_isnil(L, 3);

  ud = (pcre2*)lua_newuserdata(L, sizeof(pcre2));
  memset(ud, 0, sizeof(pcre2));
  luaL_getmetatable(L, pcre_handle);
  lua_setmetatable(L, -2);

  /* Establish GC ownership before allocating locale tables. */
  if(has_locale)
    ud->tables = Lpcre_maketables(L, 3);

  ud->pr = pcre_compile(pattern, cflags, &error, &erroffset, ud->tables);
  if(!ud->pr) 
    {
    sprintf(buf, "%s (pattern offset: %d)", error, erroffset+1);
                     /* show offset 1-based as it's common in Lua */
    L_lua_error(L, buf);
    }

  ud->extra =  pcre_study(ud->pr, 0, &error);        
  if(error) L_lua_error(L, error);

  pcre_fullinfo(ud->pr, ud->extra, PCRE_INFO_CAPTURECOUNT, &ud->ncapt);
  /* need (2 ints per capture, plus one for substring match) * 3/2 */
  ud->match = (int *) Lmalloc(L, (ud->ncapt + 1) * 3 * sizeof(int));

  return 1;
}

static void Lpcre_getargs(lua_State *L, pcre2 **pud, const char **text,
                          size_t *text_len)
{
  *pud = (pcre2 *)luaL_checkudata(L, 1, pcre_handle);
  luaL_argcheck(L, *pud != NULL, 1, "compiled regexp expected");
  *text = luaL_checklstring(L, 2, text_len);
}

typedef void (*Lpcre_push_matches)
  (lua_State *L, const char *text, pcre2 * ud);

static void Lpcre_push_substrings
  (lua_State *L, const char *text, pcre2 * ud)
{
  int i;
  int namecount;
  unsigned char *name_table;
  int name_entry_size;
  unsigned char *tabptr;
  const int *match = ud->match;

  lua_newtable(L);
  for (i = 1; i <= ud->ncapt; i++) {
    int j = i * 2;
    if (match[j] >= 0)
      lua_pushlstring(L, text + match[j], match[j + 1] - match[j]);
    else
      lua_pushboolean(L, 0);
    lua_rawseti(L, -2, i);
  }
  /* now do named subpatterns - NJG */
  pcre_fullinfo(ud->pr, ud->extra, PCRE_INFO_NAMECOUNT, &namecount); 
  if (namecount <= 0)
    return;
  pcre_fullinfo(ud->pr, ud->extra, PCRE_INFO_NAMETABLE, &name_table); 
  pcre_fullinfo(ud->pr, ud->extra, PCRE_INFO_NAMEENTRYSIZE, &name_entry_size); 

  // to handle duplicates - first add every name as a non-match

  tabptr = name_table;
  for (i = 0; i < namecount; i++) 
    {
    int n = (((int)tabptr[0]) << 8) | tabptr[1];
    const unsigned char * name = tabptr + 2;
    if (n >= 0 && n <= ud->ncapt)    // if in range
      {
      lua_pushstring (L, name);  // name
      lua_pushboolean (L, 0);    // false
      lua_settable (L, -3);      // set it
      }
    tabptr += name_entry_size;
  }

  // now add the actual matches

  tabptr = name_table;
  for (i = 0; i < namecount; i++) 
    {
    int n = (((int)tabptr[0]) << 8) | tabptr[1];
    const unsigned char * name = tabptr + 2;
    if (n >= 0 && n <= ud->ncapt) 
      {
      int j = n * 2;
      if (match[j] >= 0)
        {
        lua_pushstring (L, name);  // name
        lua_pushlstring (L, text + match[j], match[j + 1] - match[j]); // contents
        lua_settable (L, -3);      // set it
        }   // this one matched
      }  // n in range of captures
    tabptr += name_entry_size;
  }
}

static void Lpcre_push_offsets
  (lua_State *L, const char *text, pcre2 * ud)
{
  int i, j, k;
  if(text) {
    /* suppress compiler warning */
  }
  lua_newtable(L);
  for (i=1, j=1; i <= ud->ncapt; i++) {
    k = i * 2;
    if (ud->match[k] >= 0) {
      lua_pushnumber(L, ud->match[k] + 1);
      lua_rawseti(L, -2, j++);
      lua_pushnumber(L, ud->match[k+1]);
      lua_rawseti(L, -2, j++);
    }
    else {
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
    }
  }
}

typedef struct {
  lua_State *L;
  int callback;
  int failed;
} pcre_callout_context;

// The whole argument-building and callback operation is protected by lua_pcall.
static int callout_lua (lua_State *L)
  {
  pcre_callout_block * cb = (pcre_callout_block *) lua_touserdata(L, 1);
  int i, j, k;

  lua_pushvalue(L, lua_upvalueindex(1));
  lua_pushnumber(L, cb->callout_number);
  lua_newtable(L);
  for (i = 1, j = 1; i < cb->capture_top; i++)
    {
    k = i * 2;
    if (cb->offset_vector[k] >= 0)
      {
      lua_pushnumber(L, cb->offset_vector[k] + 1);
      lua_rawseti(L, -2, j++);
      lua_pushnumber(L, cb->offset_vector[k+1]);
      lua_rawseti(L, -2, j++);
      }
    else
      {
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
      lua_pushboolean(L, 0);
      lua_rawseti(L, -2, j++);
      }
    }
  lua_pushlstring(L, cb->subject, cb->subject_length);
  lua_pushnumber(L, cb->start_match + 1);
  lua_pushnumber(L, cb->current_position + 1);
  lua_pushnumber(L, cb->capture_top - 1);
  lua_pushnumber(L, cb->capture_last);
  lua_call(L, 7, 1);
  return 1;
  }

static int callout_function (pcre_callout_block * cb)
  {
  pcre_callout_context * context = (pcre_callout_context *) cb->callout_data;
  lua_State *L;
  int (*saved_callout)(pcre_callout_block *) = pcre_callout;
  int status, result;

  if (!context || !context->L)
    return 0;
  L = context->L;
  lua_pushvalue(L, context->callback);
  lua_pushlightuserdata(L, cb);
  status = lua_pcall(L, 1, 1, 0);
  // Nested regex calls and trigger processing may change the global callback.
  pcre_callout = saved_callout;
  if (status != 0)
    {
    context->failed = 1;
    // Let PCRE unwind its recursion buffers before raising the Lua error.
    return PCRE_ERROR_CALLOUT;
    }
  result = (int) lua_tonumber(L, -1);
  lua_pop(L, 1);
  return result < 0 ? PCRE_ERROR_NOMATCH : result;
  }

static int execute_regex (lua_State *L, pcre2 *ud, const char *text,
                          int length, int startoffset, int eflags, int which)
  {
  int (*saved_callout)(pcre_callout_block *) = pcre_callout;
  pcre_callout_context context;
  unsigned long saved_flags = 0;
  void *saved_data = NULL;
  int with_callout = lua_isfunction(L, which);
  int result;

  context.L = L;
  context.failed = 0;
  context.callback = 0;
  if (with_callout)
    {
    // Prepare the protected callout closure before entering the native engine.
    if (!lua_checkstack(L, 3))
      return luaL_error(L, "not enough stack space for PCRE callback");
    lua_pushvalue(L, which);
    lua_pushcclosure(L, callout_lua, 1);
    context.callback = lua_gettop(L);
    if (!ud->extra)
      {
      ud->extra = (pcre_extra *)(pcre_malloc)(sizeof(pcre_extra));
      if (!ud->extra)
        return luaL_error(L, "failed to get memory for PCRE callback");
      memset(ud->extra, 0, sizeof(pcre_extra));
      }
    saved_flags = ud->extra->flags;
    if (saved_flags & PCRE_EXTRA_CALLOUT_DATA)
      saved_data = ud->extra->callout_data;
    ud->extra->flags |= PCRE_EXTRA_CALLOUT_DATA;
    ud->extra->callout_data = &context;
    }
  pcre_callout = with_callout ? callout_function : NULL;
  result = pcre_exec(ud->pr, ud->extra, text, length, startoffset, eflags,
                    ud->match, (ud->ncapt + 1) * 3);
  pcre_callout = saved_callout;
  if (with_callout)
    {
    ud->extra->flags = saved_flags;
    ud->extra->callout_data = saved_data;
    lua_remove(L, context.callback);
    }
  if (context.failed)
    return lua_error(L); // keep the original error object, including non-strings
  return result;
  }

static int Lpcre_match_generic(lua_State *L, Lpcre_push_matches push_matches)
{
  int res;
  const char *text;
  pcre2 *ud;
  size_t elen;
  int startoffset;
  int eflags = luaL_optint(L, 4, 0);

  Lpcre_getargs(L, &ud, &text, &elen);
  startoffset = get_startoffset(L, 3, elen);

  res = execute_regex(L, ud, text, (int)elen, startoffset, eflags, 5);
  if (res >= 0) {
    lua_pushnumber(L, ud->match[0] + 1);
    lua_pushnumber(L, ud->match[1]);
    (*push_matches)(L, text, ud);
    return 3;
  }
  return 0;
}

static int Lpcre_match(lua_State *L)
{
  return Lpcre_match_generic(L, Lpcre_push_substrings);
}

static int Lpcre_exec(lua_State *L)
{
  return Lpcre_match_generic(L, Lpcre_push_offsets);
}

static int Lpcre_gmatch(lua_State *L)
{
  int res;
  size_t len;
  int nmatch = 0, limit = 0;
  const char *text;
  pcre2 *ud;
  int maxmatch = luaL_optint(L, 4, 0);
  int eflags = luaL_optint(L, 5, 0);
  int startoffset = 0;
  Lpcre_getargs(L, &ud, &text, &len);
  luaL_checktype(L, 3, LUA_TFUNCTION);

  if(maxmatch > 0) /* this must be stated in the docs */
    limit = 1;

  while (!limit || nmatch < maxmatch) 
    {

    res = execute_regex(L, ud, text, (int)len, startoffset, eflags, 6);
    if (res >= 0) 
      {
      // warning - the function called may change pcre_callout to NULL
      //   because a line written may trigger "process previous line" which
      //   will probably call regexp to match triggers
      int (*f)(pcre_callout_block *) = pcre_callout;

      nmatch++;
      lua_pushvalue(L, 3);
      lua_pushlstring(L, text + ud->match[0], ud->match[1] - ud->match[0]);
      Lpcre_push_substrings(L, text, ud);
      lua_call(L, 2, 1);
      if(lua_toboolean(L, -1))
        break;
      lua_pop(L, 1);
      startoffset = ud->match[1];
      pcre_callout = f;
    } else
      break;
  }
  lua_pushnumber(L, nmatch);
  return 1;
}

static int Lpcre_gc (lua_State *L)
{
  pcre2 *ud = (pcre2 *)luaL_checkudata(L, 1, pcre_handle);
  if (ud) {
    if(ud->pr)      pcre_free(ud->pr);
    if(ud->extra)   pcre_free(ud->extra);
    if(ud->tables)  pcre_free((void *)ud->tables);
    if(ud->match)   free(ud->match);
  }
  return 0;
}

static int Lpcre_tostring (lua_State *L) {
  return regex_tostring(L, pcre_handle, pcre_typename);
}

static int Lpcre_vers (lua_State *L)
{
  lua_pushstring(L, pcre_version());
  return 1;
}

static flags_pair pcre_flags[] =
{
  { "CASELESS",        PCRE_CASELESS },
  { "MULTILINE",       PCRE_MULTILINE },
  { "DOTALL",          PCRE_DOTALL },
  { "EXTENDED",        PCRE_EXTENDED },
  { "ANCHORED",        PCRE_ANCHORED },
  { "DOLLAR_ENDONLY",  PCRE_DOLLAR_ENDONLY },
  { "EXTRA",           PCRE_EXTRA },
  { "NOTBOL",          PCRE_NOTBOL },
  { "NOTEOL",          PCRE_NOTEOL },
  { "UNGREEDY",        PCRE_UNGREEDY },
  { "NOTEMPTY",        PCRE_NOTEMPTY },
  { "UTF8",            PCRE_UTF8 },

  { "AUTO_CALLOUT",    PCRE_AUTO_CALLOUT     },


#if PCRE_MAJOR >= 4
  { "NO_AUTO_CAPTURE", PCRE_NO_AUTO_CAPTURE },
// NJG  { "NO_UTF8_CHECK",   PCRE_NO_UTF8_CHECK },
#endif

#if PCRE_MAJOR >= 7
  { "PARTIAL",         PCRE_PARTIAL          },
  { "FIRSTLINE",       PCRE_FIRSTLINE        },
  { "DUPNAMES",        PCRE_DUPNAMES         },
  { "NEWLINE_CR",      PCRE_NEWLINE_CR       },
  { "NEWLINE_LF",      PCRE_NEWLINE_LF       },
  { "NEWLINE_CRLF",    PCRE_NEWLINE_CRLF     },
  { "NEWLINE_ANY",     PCRE_NEWLINE_ANY      },  
  { "NEWLINE_ANYCRLF", PCRE_NEWLINE_ANYCRLF  },
                                             
#endif                                           

  { NULL, 0 }
};

static int Lpcre_get_flags (lua_State *L) {
  return regex_get_flags(L, pcre_flags);
}

static const luaL_Reg pcremeta[] = {
  {"exec",       Lpcre_exec},
  {"match",      Lpcre_match},
  {"gmatch",     Lpcre_gmatch},
  {"__gc",       Lpcre_gc},
  {"__tostring", Lpcre_tostring},
  {NULL, NULL}
};

#endif /* defined(LREXLIB_PCRE) */


/* Open the library */

static const luaL_Reg rexlib[] = {
#ifdef LREXLIB_POSIX
  {"newPOSIX",    posix_comp},
  {"flagsPOSIX",  posix_get_flags},
#endif
#ifdef LREXLIB_PCRE
  {"new",     Lpcre_comp},
  {"flags",   Lpcre_get_flags},
  {"version", Lpcre_vers},
#endif
  {NULL, NULL}
};

static void createmeta(lua_State *L, const char *name)
{
  luaL_newmetatable(L, name);   /* create new metatable */
  lua_pushliteral(L, "__index");
  lua_pushvalue(L, -2);         /* push metatable */
  lua_rawset(L, -3);            /* metatable.__index = metatable */
}

LUALIB_API int luaopen_rex(lua_State *L)
{
#ifdef LREXLIB_POSIX
  createmeta(L, posix_handle);
  luaL_register (L, NULL, posixmeta);
  lua_pop(L, 1);
#endif
#ifdef LREXLIB_PCRE
  createmeta(L, pcre_handle);
  luaL_register (L, NULL, pcremeta);
  lua_pop(L, 1);
#endif
  luaL_register (L, "rex", rexlib);
  return 1;
}


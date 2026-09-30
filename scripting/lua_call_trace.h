#pragma once

#include <cstdio>
#include <cstring>
#include <string>

inline std::string LuaTraceString (const char * value, size_t length, bool rawBytes = false)
  {
  const unsigned char * text = reinterpret_cast<const unsigned char *> (value);
  size_t shown = length > 120 ? 120 : length;
  while (!rawBytes && shown < length && shown > 0 && (text [shown] & 0xc0) == 0x80)
    --shown;
  std::string result = "\"";
  for (size_t i = 0; i < shown; ++i)
    {
    if (rawBytes)
      {
      char escape [5];
      snprintf (escape, sizeof escape, "\\%03u", static_cast<unsigned int> (text [i]));
      result += escape;
      continue;
      }
    switch (text [i])
      {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (text [i] < 32 || text [i] == 127)
          {
          char escape [5];
          snprintf (escape, sizeof escape, "\\%03u", static_cast<unsigned int> (text [i]));
          result += escape;
          }
        else
          result += static_cast<char> (text [i]);
      }
    }
  if (shown < length)
    result += "...";
  result += '"';
  return result;
  }

inline std::string LuaTraceValue (lua_State * L, int index, bool rawBytes = false)
  {
  switch (lua_type (L, index))
    {
    case LUA_TNIL: return "nil";
    case LUA_TBOOLEAN: return lua_toboolean (L, index) ? "true" : "false";
    case LUA_TNUMBER:
      {
      char number [64];
      snprintf (number, sizeof number, "%.15g", static_cast<double> (lua_tonumber (L, index)));
      return number;
      }
    case LUA_TSTRING:
      {
      size_t length = 0;
      const unsigned char * text = reinterpret_cast<const unsigned char *> (lua_tolstring (L, index, &length));
      return LuaTraceString (reinterpret_cast<const char *> (text), length, rawBytes);
      }
    default:
      return std::string ("<") + lua_typename (L, lua_type (L, index)) + ">";
    }
  }

inline std::string LuaTraceCall (lua_State * L, const char * procedure, int arguments)
  {
  std::string result = procedure;
  result += '(';
  const int first = lua_gettop (L) - arguments + 1;
  const int shown = arguments > 8 ? 8 : arguments;
  for (int i = 0; i < shown; ++i)
    {
    if (i)
      result += ", ";
    const bool rawBytes = (i == 0 &&
      (strcmp (procedure, "OnPluginPacketReceived") == 0 ||
       strcmp (procedure, "OnPluginPacketSent") == 0 ||
       strcmp (procedure, "OnPluginTelnetOption") == 0)) ||
      (i == 1 && strcmp (procedure, "OnPluginTelnetSubnegotiation") == 0);
    result += LuaTraceValue (L, first + i, rawBytes);
    }
  if (shown < arguments)
    result += ", ...";
  result += ')';
  return result;
  }


// Inline scripts need a probe to find their first Lua function. Named callbacks do not.
class CLuaCallTrace
  {
  public:
    CLuaCallTrace (lua_State * L, bool enabled, const char * procedure,
                   int arguments, const bool * inTrace = NULL,
                   void (*firstCall) (void *) = NULL, void * context = NULL) :
      m_L (L), m_previous (NULL), m_owner (this), m_registry (lua_topointer (L, LUA_REGISTRYINDEX)),
      m_inTrace (inTrace), m_enabled (enabled), m_started (false), m_seenRoot (false),
      m_incomplete (false), m_errorHandler (NULL), m_inErrorHandler (false),
      m_threadsRef (LUA_NOREF), m_firstRef (LUA_NOREF),
      m_firstCall (firstCall), m_context (context)
      {
      m_root = lua_topointer (L, lua_gettop (L) - arguments);
      m_chunk = procedure == NULL;
      if (enabled && !m_chunk)
        m_first = LuaTraceCall (L, procedure, arguments);
      }

    ~CLuaCallTrace ()
      {
      Stop ();
      if (m_firstRef != LUA_NOREF)
        luaL_unref (m_L, LUA_REGISTRYINDEX, m_firstRef);
      }

    void Start (const void * errorHandler)
      {
      m_errorHandler = errorHandler;
      if (!m_enabled)
        return;
      m_previous = Active ();
      Active () = this;
      m_started = true;
      if (!m_chunk)
        return;
      for (CLuaCallTrace * trace = m_previous; trace; trace = trace->m_previous)
        if (trace->m_chunk && trace->m_registry == m_registry)
          m_owner = trace->m_owner;
      Watch (m_L);
      }

    void Stop ()
      {
      if (!m_started)
        return;
      ReleaseHooks ();
      Active () = m_previous;
      m_started = false;
      }

    void ReleaseHooks ()
      {
      if (m_threadsRef != LUA_NOREF)
        {
        const int top = lua_gettop (m_L);
        lua_rawgeti (m_L, LUA_REGISTRYINDEX, m_threadsRef);
        const int table = lua_gettop (m_L);
        for (int pass = 0; pass < 2; ++pass)
          for (lua_pushnil (m_L); lua_next (m_L, table); lua_pop (m_L, 1))
            {
            lua_State * thread = lua_tothread (m_L, -2);
            if (pass == 0 && lua_gethook (thread) != Hook)
              m_incomplete = true;
            // Keep a hook that the script installs during the callback.
            if (pass == 1 && lua_gethook (thread) == Hook)
              lua_sethook (thread, NULL, 0, 0);
            }
        lua_settop (m_L, top);
        luaL_unref (m_L, LUA_REGISTRYINDEX, m_threadsRef);
        m_threadsRef = LUA_NOREF;
        }
      }

    std::string Description ()
      {
      Stop ();
      if (m_firstRef != LUA_NOREF)
        {
        const int top = lua_gettop (m_L);
        lua_rawgeti (m_L, LUA_REGISTRYINDEX, m_firstRef);
        const int table = lua_gettop (m_L);
        lua_rawgeti (m_L, table, 0);
        m_first = lua_tostring (m_L, -1);
        lua_pop (m_L, 1);
        // The saved count also covers nil arguments.
        lua_getfield (m_L, table, "count");
        const int count = static_cast<int> (lua_tointeger (m_L, -1));
        lua_pop (m_L, 1);
        for (int i = 1; i <= count; ++i)
          lua_rawgeti (m_L, table, i);
        m_first = LuaTraceCall (m_L, m_first.c_str (), count);
        lua_settop (m_L, top);
        }
      if (m_first.empty ())
        return m_incomplete ? "Lua call detection unavailable: a Lua debug hook is active." : "";
      return "Function: " + m_first;
      }

    bool Incomplete () const { return m_incomplete; }

  private:
    lua_State * m_L;
    CLuaCallTrace * m_previous;
    CLuaCallTrace * m_owner;
    const void * m_registry;
    const bool * m_inTrace;
    bool m_enabled, m_started, m_seenRoot, m_incomplete;
    const void * m_root;
    const void * m_errorHandler;
    bool m_chunk, m_inErrorHandler;
    int m_threadsRef, m_firstRef;
    std::string m_first;
    void (*m_firstCall) (void *);
    void * m_context;

    static CLuaCallTrace * & Active ()
      {
      static CLuaCallTrace * trace = NULL;
      return trace;
      }

    static bool Suppressed ()
      {
      for (CLuaCallTrace * trace = Active (); trace; trace = trace->m_previous)
        if (trace->m_inTrace && *trace->m_inTrace)
          return true;
      return false;
      }

    void Watch (lua_State * L)
      {
      CLuaCallTrace * owner = m_owner;
      const lua_Hook hook = lua_gethook (L);
      if (hook && hook != Hook)
        {
        m_incomplete = true;
        return;
        }
      if (!lua_checkstack (L, 2) || !lua_checkstack (owner->m_L, 4))
        luaL_error (L, "Cannot allocate Lua trace stack");
      if (owner->m_threadsRef == LUA_NOREF)
        {
        lua_newtable (owner->m_L);
        owner->m_threadsRef = luaL_ref (owner->m_L, LUA_REGISTRYINDEX);
        }
      // Thread keys keep all inherited probe hooks alive until cleanup.
      lua_rawgeti (owner->m_L, LUA_REGISTRYINDEX, owner->m_threadsRef);
      lua_pushthread (L);
      if (L != owner->m_L)
        lua_xmove (L, owner->m_L, 1);
      lua_pushboolean (owner->m_L, true);
      lua_rawset (owner->m_L, -3);
      lua_pop (owner->m_L, 1);
      // LuaJIT shares hooks between threads; Lua 5.1 keeps one per thread.
      if (hook != Hook)
        lua_sethook (L, Hook, LUA_MASKCALL | LUA_MASKRET, 0);
      }

    void WatchThreads (lua_State * L, int top)
      {
      for (int i = 1; i <= top; ++i)
        {
        if (lua_isthread (L, i))
          Watch (lua_tothread (L, i));
        // coroutine.wrap returns a C closure whose upvalue is its thread.
        else if (lua_iscfunction (L, i))
          for (int j = 1; lua_getupvalue (L, i, j); ++j)
            {
            if (lua_isthread (L, -1))
              Watch (lua_tothread (L, -1));
            lua_pop (L, 1);
            }
        }
      }

    void SaveFirst (lua_State * L, lua_Debug * ar)
      {
      lua_getinfo (L, "nS", ar);
      lua_newtable (L);
      const int table = lua_gettop (L);
      if (ar->name)
        lua_pushstring (L, ar->name);
      else
        lua_pushfstring (L, "%s:%d", ar->short_src, ar->linedefined);
      lua_rawseti (L, table, 0);
      int count = 0;
      for (int i = 1; i <= 9; ++i)
        {
        const char * name = lua_getlocal (L, ar, i);
        if (!name)
          break;
        if (name [0] == '(')
          {
          lua_pop (L, 1);
          break;
          }
        lua_rawseti (L, table, ++count);
        }
      lua_pushinteger (L, count);
      lua_setfield (L, table, "count");
      if (L != m_L)
        {
        if (!lua_checkstack (m_L, 1))
          luaL_error (L, "Cannot allocate Lua trace stack");
        lua_xmove (L, m_L, 1);
        }
      m_firstRef = luaL_ref (m_L, LUA_REGISTRYINDEX);
      }

    static void Hook (lua_State * L, lua_Debug * ar)
      {
      const void * registry = lua_topointer (L, LUA_REGISTRYINDEX);
      CLuaCallTrace * trace = Active ();
      while (trace && trace->m_registry != registry)
        trace = trace->m_previous;
      // A coroutine can outlive the callback that created it.
      if (!trace)
        {
        lua_sethook (L, NULL, 0, 0);
        return;
        }
      if (!trace->m_chunk || trace->m_firstRef != LUA_NOREF)
        return;
      const int top = lua_gettop (L);
      if (!lua_checkstack (L, 16))
        luaL_error (L, "Cannot allocate Lua trace stack");
      trace->Watch (L);
      lua_getinfo (L, "fS", ar);
      const void * function = lua_topointer (L, -1);
      const bool cFunction = lua_iscfunction (L, -1) != 0;
      if (cFunction)
        trace->WatchThreads (L, lua_gettop (L));
      lua_settop (L, top);
      if (ar->event != LUA_HOOKCALL || Suppressed () || trace->m_inErrorHandler)
        return;
      if (trace->m_seenRoot && function == trace->m_errorHandler)
        {
        trace->m_inErrorHandler = true;
        return;
        }
      if (!trace->m_seenRoot && function == trace->m_root)
        {
        trace->m_seenRoot = true;
        return;
        }
      if (cFunction || strcmp (ar->what, "Lua") != 0)
        return;
      trace->SaveFirst (L, ar);
      if (trace->m_firstCall)
        trace->m_firstCall (trace->m_context);
      // Keep the entry scope until the callback ends, but stop observing calls.
      trace->ReleaseHooks ();
      }

    CLuaCallTrace (const CLuaCallTrace &);
    CLuaCallTrace & operator= (const CLuaCallTrace &);
  };

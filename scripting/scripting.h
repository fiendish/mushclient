#pragma once

extern "C"
  {
#ifdef LUA_52
    #include "..\..\lua52\src\lua.h"
    #include "..\..\lua52\src\lauxlib.h"
#else
    #include "..\lua.h"
    #include "..\lauxlib.h"
#endif
  }

#include "paneline.h"
#include "idle_gc.h"

#define DOCUMENT_STATE "mushclient.document"
#define WORLD_LIBRARY "world"

void LuaError (lua_State *L, 
               LPCTSTR strEvent = "Run-time error",
               LPCTSTR strProcedure = "",
               LPCTSTR strType = "",
               LPCTSTR strReason = "",
               CMUSHclientDoc * pDoc = NULL);

class CPlugin;

class CScriptEngine : public CObject
  {

  public:
    CScriptEngine (CMUSHclientDoc * pDoc,
                   const CString strLanguage,
                   CPlugin * pPlugin = NULL) // constructor
      {
      m_pDoc = pDoc;
      m_pPlugin = pPlugin;
      m_strLanguage = strLanguage;
      m_IActiveScript = NULL;
      m_IActiveScriptParse = NULL;
      m_site = NULL;
      m_pDispatch = NULL;
      L = NULL;
      };  // end of constructor

    ~CScriptEngine () // destructor
      {
      DisableScripting ();
      }; // end of destuctor

  bool CreateScriptEngine (void);
  bool Parse (const CString & strCode, const CString & strWhat);

  DISPID GetDispid (const CString & strName, bool * pLookupError = NULL);
  DISPID GetLuaDispid (const CString & strName, bool * pLookupError = NULL);

  // returns true if script error
  bool Execute (DISPID & dispid,  // dispatch ID, will be set to DISPID_UNKNOWN on an error
                LPCTSTR szProcedure,  // eg. ON_TRIGGER_XYZ
                const unsigned short iReason,  // value for m_iCurrentActionSource
                LPCTSTR szType,   // eg. trigger, alias
                LPCTSTR szReason, // eg. trigger subroutine XXX
                DISPPARAMS & params,  // parameters
                long & nInvocationCount,  // count of invocations
                COleVariant * result,   // result of call
                const bool pluginCallback = false);
  bool ShowError (const HRESULT result, const CString strMsg);
  void DisableScripting (void);

  void OpenLua ();
  void OpenLuaDelayed ();
  void CloseLua ();
  void LuaActivity () { m_idleGC.Activity (); }
  bool CollectLuaGarbage (DWORD now);
  bool LuaGCReady (DWORD now) const
    { return L && m_idleGC.Ready (now) && lua_gc (L, 9, 0) == 1; }

//  void RegisterLuaRoutines ();

  bool ParseLua (const CString & strCode, const CString & strWhat);
  // returns true if script error
  bool ExecuteLua (DISPID & dispid,          // dispatch ID, will be set to DISPID_UNKNOWN on an error
                   LPCTSTR szProcedure,      // eg. ON_TRIGGER_XYZ
                   const unsigned short iReason,  // value for m_iCurrentActionSource
                   LPCTSTR szType,           // eg. trigger, alias
                   LPCTSTR szReason,         // eg. trigger subroutine XXX
                   list<double> & nparams,   // list of number parameters
                   list<string> & sparams,   // list of string parameters
                   long & nInvocationCount,  // count of invocations
                   const t_regexp * regexp = NULL,  // regular expression (for triggers, aliases)
                   map<string, string> * table = NULL, // map of other things
                   CPaneLine * paneline = NULL,        // and the line (for triggers)
                   bool * result = NULL,               // where to put result
                   bool bMarkActivity = true,
                   const bool pluginCallback = false);

  // returns true if script error
  bool ExecuteLua (DISPID & dispid,          // dispatch ID, will be set to DISPID_UNKNOWN on an error
                   LPCTSTR szProcedure,      // eg. ON_TRIGGER_XYZ
                   const unsigned short iReason,  // value for m_iCurrentActionSource
                   LPCTSTR szType,           // eg. trigger, alias
                   LPCTSTR szReason,         // eg. trigger subroutine XXX
                   CString strParam,         // string parameter
                   long & nInvocationCount,  // count of invocations
                   CString & result,         // where to put result
                   const bool pluginCallback = false);

  // return value is return from call

  const bool IsLua () const { return L != NULL; }
  lua_State           * L;                  // Lua state

  private:

  CLuaIdleGC m_idleGC;

  IActiveScript       * m_IActiveScript;          // VBscript interface
  IActiveScriptParse  * m_IActiveScriptParse;     // parser
  CActiveScriptSite   * m_site;                   // our local site (world object)
  IDispatch           * m_pDispatch;              // script engine dispatch pointer

  CMUSHclientDoc      * m_pDoc;                   // related MUSHclient document
  CPlugin            * m_pPlugin;                // owner, NULL for world scripts

  CString               m_strLanguage;        // language, (vbscript, jscript, perlscript)

  };

int RegisterLuaRoutines (lua_State * L);
int DisableDLLs (lua_State * L);
#include "lua_helpers.h"


// ----------- here used for Lua in choosing from combo-box

class CKeyValuePair
  {

  public:
    CKeyValuePair () :
        bNumber_ (false), iKey_ (0.0) { };  // default constructor

    CKeyValuePair (const string sValue) : 
        bNumber_ (false), iKey_ (0.0), sValue_ (sValue) { };  // constructor

    // copy constructor
    CKeyValuePair (const CKeyValuePair & k)
                    : bNumber_ (k.bNumber_), 
                      sKey_ (k.sKey_),
                      iKey_ (k.iKey_),
                      sValue_ (k.sValue_)
                       {};

    // operator =
    const CKeyValuePair & operator= (const CKeyValuePair & rhs)
      {
      bNumber_ = rhs.bNumber_;
      sKey_  = rhs.sKey_;
      iKey_ = rhs.iKey_;
      sValue_ = rhs.sValue_;
      return *this;
      };


  bool   bNumber_; // true if key a number, false if a string

  string sKey_;    // key if string
  double iKey_;    // key if number?

  string sValue_;  // value 

  };   // end of class  CStringValuePair

// call a C function with the Lua environment
void CallLuaCFunction (lua_State * L, lua_CFunction fn);
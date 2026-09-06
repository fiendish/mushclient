LuaCOM source: https://github.com/fiendish/luacom
Revision: 96f9e5bfacd8aec6c5e98d85e55854188eba7412

Copy the files from src/library into this directory, along with
include/luacom.h and COPYRIGHT.

Local changes in luacom.cpp:

1. In luacom_RegisterObject, disable the InprocServer32 and ScriptFile
   registry entries. MUSHclient does not register LuaCOM as an in-process
   server. Keep LocalServer32 registration enabled.

2. In luacom_open, disable automatic loading of luacom5.lua and the
   generated luacom.loh code. MUSHclient controls loading of Lua helper code.

3. In luacom_StartMessageLoop, reject the call when the mushclient_embedded
   registry flag is set. MUSHclient uses its own message loop.

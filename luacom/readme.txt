LuaCOM source: https://github.com/fiendish/luacom
Revision: af855b4f03f45185e44629c28fee34edf536c08e

Copy the files from src/library and include/luacom.h into this directory.
Copy COPYRIGHT to ../licences/LuaCOM licence.txt.

Local changes in luacom.cpp:

1. In luacom_RegisterObject, disable the InprocServer32 and ScriptFile
   registry entries. MUSHclient does not register LuaCOM as an in-process
   server. Keep LocalServer32 registration enabled.

2. In luacom_open, disable automatic loading of luacom5.lua and the
   generated luacom.loh code. MUSHclient controls loading of Lua helper code.

3. In luacom_StartMessageLoop, reject the call when the mushclient_embedded
   registry flag is set. MUSHclient uses its own message loop.

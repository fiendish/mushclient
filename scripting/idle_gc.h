#pragma once

class CLuaIdleGC
  {
  public:
    CLuaIdleGC () : m_bPending (true), m_iCycles (0), m_dwLastCollection (0) {}

    void Activity () { m_bPending = true; }
    bool Ready (unsigned long now) const
      { return m_iCycles || (m_bPending && now - m_dwLastCollection >= 5000); }
    void Cancel (unsigned long now)
      { m_iCycles = 0; m_bPending = false; m_dwLastCollection = now; }

    // Returns -1 with a Lua error on the stack, 0 if skipped, or 1 if stepped.
    int Collect (lua_State * L, unsigned long now)
      {
      // LuaJIT and Lua 5.2 support option 9 (LUA_GCISRUNNING). Older runtimes
      // return -1; leave their collector alone rather than restart a stopped GC.
      if (!L || !Ready (now) || lua_gc (L, 9, 0) != 1)
        return 0;
      if (!m_iCycles)
        {
        // Objects marked before their last reference was dropped can survive
        // the first cycle. A second cycle collects those too.
        m_iCycles = 2;
        m_bPending = false;
        }
      lua_pushcfunction (L, Step);
      if (lua_pcall (L, 0, 1, 0))
        {
        Cancel (now);
        return -1;
        }
      const bool finished = lua_toboolean (L, -1) != 0;
      lua_pop (L, 1);
      if (finished && --m_iCycles == 0)
        m_dwLastCollection = now;
      return 1;
      }

  private:
    static int Step (lua_State * L)
      {
      lua_pushboolean (L, lua_gc (L, LUA_GCSTEP, 16));
      return 1;
      }
    bool m_bPending;
    unsigned int m_iCycles;
    unsigned long m_dwLastCollection;
  };

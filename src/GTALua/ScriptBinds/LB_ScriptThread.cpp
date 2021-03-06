// =================================================================================
// Includes 
// =================================================================================
#include "Includes.h"
#include "GTALua.h"
#include "lua/Lua.h"
#include "ScriptBinds.h"
#include "thirdparty/ScriptHookV/ScriptHookV.h"
using namespace ScriptBinds::ScriptThread;

// =================================================================================
// ScriptThread 
// =================================================================================
LuaScriptThread::LuaScriptThread(string sName)
{
	m_sName = sName;
	m_bActive = false;
	m_bResetting = false;
	m_bRunsOnMainThread = false;
	m_bIsMainThread = false;
	m_bIdleState = false;
	m_iWaitTime = 0;
	m_iNextRun = 0;
}
LuaScriptThread::~LuaScriptThread()
{
	m_bActive = false;
	m_bResetting = false;
	m_iWaitTime = 0;
}

// =================================================================================
// Wait 
// =================================================================================
void LuaScriptThread::Wait(DWORD uiTime)
{
	// Reset
	if (m_bActive && m_bResetting)
	{
		lua->PushString("ScriptThreadReset");
		throw luabind::error(lua->State());
		return;
	}

	// No longer active
	if (!m_bActive)
	{
		lua->PushString("ScriptThread:Wait called on an invalid thread!");
		throw luabind::error(lua->State());
	}

	// Wait
	//ScriptHook::ScriptWait(uiTime);
	m_iWaitTime = uiTime;
}

// =================================================================================
// IsCallbackPresent 
// =================================================================================
bool LuaScriptThread::IsCallbackPresent(char* sName)
{
	lua->Lock();

	// Check if callback is present
	m_self.get(lua->State());
	if (lua->IsNil())
	{
		lua->Pop(1);
		lua->Unlock();
		return false;
	}
	lua->GetField(sName, -1);
	if (lua->IsNil())
	{
		lua->Pop(2);
lua->Unlock();
return false;
	}
	lua->Pop(2);
	lua->Unlock();
	return true;
}

// =================================================================================
// Call Lua Callback
// Returns true when normal exit
// =================================================================================
bool LuaScriptThread::Call_LuaCallback(char* sName)
{
	try
	{
		call<void>(sName);
	}
	catch (luabind::error& e)
	{
		// Reset
		if (lua->IsString() && strcmp(lua->GetString(), "ScriptThreadReset") == 0)
		{
			lua->Pop(1);
			return true;
		}

		// Error
		printf("[LuaScriptThread] Thread %s:%s caused an error!\n", m_sName.c_str(), sName);

		if (lua->IsString())
		{
			lua->PrintErrorMessage(lua->GetString(), true, true);
			lua->Pop(1);
		}
		else
			lua->PrintErrorMessage(const_cast<char*>(e.what()), true, true);

		return false;
	}
	catch (std::exception& e)
	{
		printf("[LuaScriptThread] Thread %s:%s caused an error: %s\n", m_sName.c_str(), sName, e.what());
		return false;
	}
	catch (...)
	{
		printf("LuaScriptThread] Thread %s:%s caused an error: (unknown exception thrown)\n", m_sName.c_str(), sName);
		return false;
	}

	// Normal Exit
	return true;
}

// =================================================================================
// Start 
// =================================================================================
void LuaScriptThread::Start()
{
	// Message
	printf("[LuaScriptThread] Thread %s started\n", m_sName.c_str());

	// Flag
	m_bActive = true;

	// Run
	while (m_bActive)
	{
		// Normal Threads: Run
		if (!m_bIsMainThread)
			Run();

		// Main Thread: Run
		if (m_bIsMainThread)
			Run_MainThread();

		// Main Thread will never be in idle state
		if (!m_bIsMainThread && m_bActive)
			Run_IdleState();
	}

	// Flag
	m_bActive = false;

	// Message
	printf("[LuaScriptThread] Thread %s quit\n", m_sName.c_str());
}

void LuaScriptThread::Run_MainThread()
{
	lua->Lock();

	// Time
	int game_time = ScriptHook::GetGameTime();

	// Get Thread List
	m_self.get(lua->State());
	lua->GetField("ThreadList");
	luabind::object thread_list(luabind::from_stack(lua->State(), -1));
	
	// Threads
	for (luabind::iterator i(thread_list), end; i != end; i++)
	{
		luabind::object l_thread = *i;
		LuaScriptThread* pThread = luabind::object_cast<LuaScriptThread*>(l_thread);
		pThread->m_bActive = true;

		// Next Run
		if (pThread->m_iNextRun == 0)
			pThread->m_iNextRun = game_time - 1;

		// Reset
		if (pThread->m_bResetting)
		{
			printf("Thread reset: %s\n", pThread->GetName().c_str());
			pThread->Call_LuaCallback("SetupCoroutine");
			pThread->m_bResetting = false;
			pThread->m_iNextRun = game_time - 1;
			pThread->m_bIdleState = false;
		}

		// Tick
		if (game_time >= pThread->m_iNextRun && !pThread->m_bIdleState)
		{
			// Callback
			if (!pThread->Run())
			{
				pThread->m_bIdleState = !pThread->m_bResetting;
			}

			// Wait
			pThread->m_iNextRun = game_time + pThread->m_iWaitTime;
			pThread->m_iWaitTime = 1;
		}
	}

	// Cleanup
	lua->Pop(2);
	lua->Unlock();

	// Yield
	ScriptHook::ScriptWait(0);
}

// =================================================================================
// Run 
// =================================================================================
bool LuaScriptThread::Run()
{
	// Idle State
	m_bIdleState = false;

	// Coroutine
	lua->Lock();
	if (!m_bRunsOnMainThread && !m_bIsMainThread && !Call_LuaCallback("SetupCoroutine"))
	{
		printf("[LuaScriptThread] Thread %s failed to setup its coroutine (lua thread)!", m_sName.c_str());
		lua->Unlock();
		return false;
	}
	lua->Unlock();

	// Call
	bool bNormalExit = true;
	while (bNormalExit && m_bActive && !m_bResetting)
	{
		// Callback
		lua->Lock();
		bNormalExit = Call_LuaCallback("Tick");
		lua->Unlock();

		// Break
		if (m_bRunsOnMainThread)
			break;

		// Wait
		ScriptHook::ScriptWait(m_iWaitTime);
		m_iWaitTime = 1;
	}

	// Quit
	if (!m_bRunsOnMainThread && !m_bActive)
	{
		printf("[LuaScriptThread] Thread %s quit\n", m_sName.c_str());
		return false;
	}

	// OnError
	if (!bNormalExit && IsCallbackPresent("OnError"))
	{
		lua->Lock();
		Call_LuaCallback("OnError");
		lua->Unlock();
		return false;
	}

	// smooth
	return true;
}

// =================================================================================
// Idle State
// Thread is still alive, but idling
// =================================================================================
void LuaScriptThread::Run_IdleState()
{
	// Idle
	m_bIdleState = true;

	// Idle Loop
	while (m_bActive && m_bIdleState)
	{
		// Handle Reset
		if (m_bResetting)
		{
			m_bResetting = false;
			m_bIdleState = false;
			printf("[LuaScriptThread] Thread %s reset\n", m_sName.c_str());

			return;
		}

		// Idle State
		m_bIdleState = true;

		// Wait
		ScriptHook::ScriptWait(5);
	}

	// Idle
	m_bIdleState = false;
}

// =================================================================================
// Resest 
// =================================================================================
void LuaScriptThread::Reset()
{
	m_bResetting = true;
}

// =================================================================================
// Kill 
// =================================================================================
void LuaScriptThread::Kill()
{
	m_bActive = false;
	m_bResetting = false;
}

// =================================================================================
// Bind 
// =================================================================================
void ScriptBinds::ScriptThread::Bind()
{
	luabind::module(lua->State())
	[
		luabind::class_<LuaScriptThread>("CScriptThread")
		.def(luabind::constructor<string>())
		.def("GetName", &LuaScriptThread::GetName)
		.def("IsRunning", &LuaScriptThread::IsRunning) // is running
		.def("IsActive", &LuaScriptThread::IsActive) // is valid in general
		.def("Wait", &LuaScriptThread::Wait, luabind::yield)
		.def("Reset", &LuaScriptThread::Reset)
		.def("internal_Kill", &LuaScriptThread::Kill)
		.def_readonly("m_iWaitTime", &LuaScriptThread::m_iWaitTime)
	];
}
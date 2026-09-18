#include "StdAfx.h"
#include "EventsBase.h"
//
namespace NGlobal
{
////////////////////////////////////////////////////////////////////////////////////////////////////
static void FreeEventHandlersHashMap();
//
static struct SExecutionTracker
{
	bool bIsRunning;
	SExecutionTracker(): bIsRunning(true) {}
	~SExecutionTracker() { bIsRunning = false; FreeEventHandlersHashMap(); }
} tracker;
//
typedef vector<IEventRegister*> CCallInfoHash;
// key = the type_info's address, reused as a per-type identity. int held it fine when
// pointers were 4 bytes (Win32); truncates on 64-bit Linux, so widen to intptr_t there.
static unordered_map< intptr_t, CCallInfoHash > *pEventHandlers = 0;
static int nEventHandlersCount = 0;
////////////////////////////////////////////////////////////////////////////////////////////////////
inline unordered_map< intptr_t, CCallInfoHash > &GetEventHandlers()
{
	if ( pEventHandlers == 0 )
		pEventHandlers = new unordered_map< intptr_t, CCallInfoHash >();
	return *pEventHandlers;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void ThrowEventInner( const type_info &eventID, const void *pStuff )
{
	intptr_t nEventID = (intptr_t)&eventID;
	CCallInfoHash &handlers = GetEventHandlers()[ nEventID ];
	for ( CCallInfoHash::iterator i = handlers.begin(); i != handlers.end(); ++i )
		(*i)->Call( pStuff );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void RegisterEventHandler( IEventRegister *pReg, const type_info &eventID )
{
	intptr_t nEventID = (intptr_t)&eventID;
	GetEventHandlers()[ nEventID ].push_back( pReg );
	++nEventHandlersCount;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void UnregisterEventHandler( IEventRegister *pReg, const type_info &eventID )
{
	intptr_t nEventID = (intptr_t)&eventID;
	vector<IEventRegister*> &handlers = GetEventHandlers()[ nEventID ];
	vector<IEventRegister*>::iterator i = find( handlers.begin(), handlers.end(), pReg );
	if ( i != handlers.end() )
	{
		handlers.erase( i );
		--nEventHandlersCount;
		ASSERT( nEventHandlersCount >= 0 );
	}
	FreeEventHandlersHashMap();
}
////////////////////////////////////////////////////////////////////////////////////////////////////
static void FreeEventHandlersHashMap()
{
	if ( !tracker.bIsRunning && pEventHandlers != 0 && nEventHandlersCount == 0 )
	{
		delete pEventHandlers;
		pEventHandlers = 0;
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////
}
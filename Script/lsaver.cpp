#include "stdafx.h"
#include "lsaver.h"
#include "lstring.h"
//////////////////////////////////////////////////////////////////////////
// The maps key C function pointers against their serialized CLuaFuncID; only
// the ID ever reaches a save file, so widening the in-memory pointer key from
// int to intptr_t (64-bit-safe) does not change the on-disk format.
typedef unordered_map<CLuaFuncID, intptr_t> CLuaIDToFuncMap;
typedef unordered_map<intptr_t, CLuaFuncID> CLuaFuncToIDMap;
CLuaFuncToIDMap luaFuncToIDMap;
CLuaIDToFuncMap luaIDToFuncMap;
lua_State *pLUASaverState;
//////////////////////////////////////////////////////////////////////////
void lua_StartSerialize( lua_State *pL )
{ 
	lua_RegisterFunc( (lua_CFunction)0, "NOFUNC" );
	lua_RegisterFunc( (lua_Hook)0, "NOFUNC" );
	pLUASaverState = pL; 
}
//////////////////////////////////////////////////////////////////////////
static void lua_GetID( CLuaFuncID *pID, lua_CFunction func )
{
	intptr_t nFunc = reinterpret_cast<intptr_t>( func );
	CLuaFuncToIDMap::iterator i = luaFuncToIDMap.find( nFunc );
	ASSERT( i != luaFuncToIDMap.end() );  // unregistered lua C function!
	*pID = i->second;
}
//////////////////////////////////////////////////////////////////////////
static void lua_GetID( CLuaFuncID *pID, lua_Hook func )
{
	intptr_t nFunc = reinterpret_cast<intptr_t>( func );
	CLuaFuncToIDMap::iterator i = luaFuncToIDMap.find( nFunc );
	ASSERT( i != luaFuncToIDMap.end() );  // unregistered lua C function!
	*pID = i->second;
}
//////////////////////////////////////////////////////////////////////////
static lua_CFunction lua_GetFunc( const CLuaFuncID& id )
{ 
	CLuaIDToFuncMap::iterator i = luaIDToFuncMap.find( id );
	ASSERT( i != luaIDToFuncMap.end() );  // unregistered lua C function!
	intptr_t nFunc = i->second;
	return ( lua_CFunction )nFunc;
}
//////////////////////////////////////////////////////////////////////////
static lua_Hook lua_GetHook( const CLuaFuncID& id )
{
	CLuaIDToFuncMap::iterator i = luaIDToFuncMap.find( id );
	ASSERT( i != luaIDToFuncMap.end() );  // unregistered lua C function!
	intptr_t nFunc = i->second;
	return ( lua_Hook )nFunc;
}
//////////////////////////////////////////////////////////////////////////
void lua_RegisterFunc( lua_CFunction func, const CLuaFuncID& id )
{
	intptr_t nFunc = reinterpret_cast<intptr_t>( func );
	luaFuncToIDMap[ nFunc ] = id;
	luaIDToFuncMap[ id ] = nFunc;
}
//////////////////////////////////////////////////////////////////////////
void lua_RegisterFunc( lua_Hook func, const CLuaFuncID& id )
{
	intptr_t nFunc = reinterpret_cast<intptr_t>( func );
	luaFuncToIDMap[ nFunc ] = id;
	luaIDToFuncMap[ id ] = nFunc;
}
//////////////////////////////////////////////////////////////////////////
int lua_AddCFunc( lua_CFunction *pFunc, CStructureSaver &f, int nChunk )
{
	CLuaFuncID id;
	if ( f.IsReading() )
	{
		f.Add( nChunk, &id );
		*pFunc = lua_GetFunc( id );
	}
	else
	{
		lua_GetID( &id, *pFunc );
		f.Add( nChunk, &id );
	}
	return nChunk + 1;
}
//////////////////////////////////////////////////////////////////////////
int lua_AddHook( lua_Hook *pHook, CStructureSaver &f, int nChunk )
{
	CLuaFuncID id;
	if ( f.IsReading() )
	{
		f.Add( nChunk, &id );
		*pHook = lua_GetHook( id );
	}
	else
	{
		lua_GetID( &id, *pHook );
		f.Add( nChunk, &id );
	}
	return nChunk + 1;
}
//////////////////////////////////////////////////////////////////////////
void lua_AddString( CStructureSaver &f, chunk_id idChunk, TString **ppszStr, int nChunk )
{
	if ( f.IsReading() )
	{
		string str;
		f.Add( idChunk, &str, nChunk );
		*ppszStr = luaS_new( pLUASaverState, str.c_str() );
	}
	else
	{
		string str = (*ppszStr)->GetStr();
		f.Add( idChunk, &str, nChunk );
	}
}
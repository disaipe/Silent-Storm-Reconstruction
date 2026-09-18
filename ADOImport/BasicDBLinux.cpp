// ============================================================================
//  BasicDBLinux.cpp -- non-Windows counterpart of BasicDB.cpp.
//
//  BasicDB.cpp mixes two things: (1) the ADO/COM-driven content-import
//  pipeline (NDatabase::Import(), COLETable, EstablishConnection -- used only
//  by the offline DataImport.exe tool) and (2) the platform-neutral runtime
//  read path (NDatabase::Serialize(READ), which loads the pre-baked game.db
//  columnar cache). #import "msado15.dll" and the rest of (1) are MSVC/COM
//  extensions that don't exist on GCC/Clang.
//
//  Traced at runtime: Game.exe/Main never call NDatabase::Import() or
//  NDatabase::Refresh(int) (see docs/linux-port.md, "Разбор этапа «БД»") --
//  only DataImport.exe does, and that tool is Windows-only for now. So this
//  file implements exactly the subset (2) actually reachable when playing
//  the game: table registry, ClearDatabaseTables, Serialize, and the
//  storage-backed CDBTableBase::PreCreate/Import + NDatabase::ImportField*
//  that Serialize's columnar (v1) load path drives.
//
//  This is compiled INSTEAD OF BasicDB.cpp on non-Windows builds (see
//  CMakeLists.txt) -- BasicDB.cpp itself is untouched and still builds the
//  Windows/MSVC target exactly as before.
// ============================================================================
#include "StdAfx.h"
#include "../ADOImport/BasicDB.h"
#include "../Misc/BasicFactory.h"
#include "../FileIO/BasicChunk1.h"

// ----------------------------------------------------------------------------
//  CDBTableDataStorage -- verbatim copy of the release/Steam columnar DB
//  representation from BasicDB.cpp. See the comment there for the format
//  reconstruction notes; kept identical here since Serialize()'s v1 read
//  path depends on its exact layout/saveload id.
// ----------------------------------------------------------------------------
struct SColumnInfo
{
	string szName;
	int eType;
	int operator&( CStructureSaver &f ) { f.Add( 2, &szName ); f.Add( 3, &eType ); return 0; }
};
////////////////////////////////////////////////////////////////////////////////////////////////////
class CDBTableDataStorage: public CObjectBase
{
	OBJECT_BASIC_METHODS( CDBTableDataStorage );
public:
	vector< vector<int> >          records_int;
	vector< vector<float> >        records_float;
	vector< vector<std::wstring> > records_wstring;
	vector< SColumnInfo >          fields;
	vector< string >               intFileds;
	vector< string >               floatFileds;
	vector< string >               stringFileds;
	int nCurrentRecord;

	CDBTableDataStorage(): nCurrentRecord( 0 ) {}

	int operator&( CStructureSaver &f )
	{
		f.Add( 2, &records_int );
		f.Add( 3, &records_float );
		f.Add( 4, &records_wstring );
		f.Add( 5, &fields );
		f.Add( 6, &intFileds );
		f.Add( 7, &floatFileds );
		f.Add( 8, &stringFileds );
		return 0;
	}
	void MoveFirst() { nCurrentRecord = 0; }
	void MoveNext()  { ++nCurrentRecord; }
	bool IsEof()     { return nCurrentRecord >= (int)records_int.size(); }

	static int FindIndex( const vector<string> &names, const char *psz )
	{
		for ( int i = 0; i < (int)names.size(); ++i )
			if ( names[i] == psz )
				return i;
		return -1;
	}
	int GetInt( const char *psz )
	{
		int n = FindIndex( intFileds, psz );
		if ( n < 0 || nCurrentRecord < 0 || nCurrentRecord >= (int)records_int.size() ||
				 n >= (int)records_int[nCurrentRecord].size() )
			return 0;
		return records_int[nCurrentRecord][n];
	}
	bool GetBool( const char *psz ) { return GetInt( psz ) != 0; }
	float GetFloat( const char *psz )
	{
		int n = FindIndex( floatFileds, psz );
		if ( n < 0 || nCurrentRecord < 0 || nCurrentRecord >= (int)records_float.size() ||
				 n >= (int)records_float[nCurrentRecord].size() )
			return 0;
		return records_float[nCurrentRecord][n];
	}
	std::wstring GetWString( const char *psz )
	{
		int n = FindIndex( stringFileds, psz );
		if ( n < 0 || nCurrentRecord < 0 || nCurrentRecord >= (int)records_wstring.size() ||
				 n >= (int)records_wstring[nCurrentRecord].size() )
			return std::wstring();
		return records_wstring[nCurrentRecord][n];
	}
	bool HasIntField( const char *psz )    { return FindIndex( intFileds, psz ) >= 0; }
	bool HasFloatField( const char *psz )  { return FindIndex( floatFileds, psz ) >= 0; }
	bool HasStringField( const char *psz ) { return FindIndex( stringFileds, psz ) >= 0; }
};
REGISTER_SAVELOAD_CLASS( 0xa1843130, CDBTableDataStorage )
// when set, the read path pulls from this columnar storage -- always set on Linux,
// since there is no ADO fallback here (unlike BasicDB.cpp's `table`/COLETable).
static CDBTableDataStorage *pStorageSource = 0;
////////////////////////////////////////////////////////////////////////////////////////////////////
namespace NDatabase
{
	CClassFactory<CDBRecord>& GetRecordTypes()
	{
		static CClassFactory<CDBRecord> recordTypes;
		return recordTypes;
	}
	typedef unordered_map< int, CDBTableBase > CTablesHash;
	CTablesHash& GetTables()
	{
		static CTablesHash tables;
		return tables;
	}
	struct STableDescr
	{
		int nTableID;
		string szTable;
	};
	struct SRelation
	{
		string szTable;
		CDBTableBase *pLeft, *pRight;
		int nTableLeft, nTableRight;
		struct SElement { int nLeft, nRight; };
		vector< SElement > data;
		SRelation(): pLeft(0), pRight(0), nTableLeft(0), nTableRight(0) {}
		int operator&( CStructureSaver &f )
		{
			f.Add( 2, &szTable );
			f.Add( 3, &nTableLeft );
			f.Add( 4, &nTableRight );
			f.Add( 5, &data );
			return 0;
		}
	};
	list<STableDescr>& GetTableDescrs()
	{
		static list<STableDescr> tableDescrs;
		return tableDescrs;
	}
	list<SRelation>& GetRelations()
	{
		static list< SRelation > relations;
		return relations;
	}
	bool bIsDatabaseLoading = false;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void NDatabase::SetSource( const char *pszSource )
{
	// no-op on Linux: there is no live DB connection to point at (see file header).
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void NDatabase::AddTable( int nTableID, const char *pszTableName, RecordCreateFunc newf )
{
	CTablesHash &tables = GetTables();
	list<STableDescr> &tableDescrs = GetTableDescrs();
	CTablesHash::iterator i = tables.find( nTableID );
	if ( i != tables.end() )
	{
		ASSERT(0);
		return;
	}
	ASSERT( pszTableName[ strlen( pszTableName ) - 1 ] == 's' );
	GetRecordTypes().RegisterTypeSafe( nTableID, newf );
	STableDescr &t = *tableDescrs.insert( tableDescrs.end(), STableDescr());
	t.nTableID = nTableID;
	t.szTable = pszTableName;
	tables[nTableID];
}
////////////////////////////////////////////////////////////////////////////////////////////////////
CDBTableBase* NDatabase::GetTable( int nTableID )
{
	CTablesHash &tables = GetTables();
	CTablesHash::iterator i = tables.find( nTableID );
	if ( i != tables.end() )
		return &i->second;
	return 0;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void NDatabase::AddRelation( const char *pszTableName )
{
	list< SRelation > &relations = GetRelations();
	SRelation &rel = *relations.insert( relations.end(), SRelation());
	rel.szTable = pszTableName;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void NDatabase::ClearDatabaseTables()
{
	CTablesHash &tables = GetTables();
	tables.clear();
	GetRelations().clear();
	list<STableDescr> &tableDescrs = GetTableDescrs();
	for ( list<STableDescr>::iterator i = tableDescrs.begin(); i != tableDescrs.end(); ++i )
		tables[ i->nTableID ];
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool NDatabase::ImportField( const char *pszFieldName, int *pData )
{
	if ( !pStorageSource || !pStorageSource->HasIntField( pszFieldName ) )
		return false;
	*pData = pStorageSource->GetInt( pszFieldName );
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool NDatabase::ImportField( const char *pszFieldName, bool *pData )
{
	if ( !pStorageSource || !pStorageSource->HasIntField( pszFieldName ) )
		return false;
	*pData = pStorageSource->GetBool( pszFieldName );
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool NDatabase::ImportField( const char *pszFieldName, float *pData )
{
	if ( !pStorageSource || !pStorageSource->HasFloatField( pszFieldName ) )
		return false;
	*pData = pStorageSource->GetFloat( pszFieldName );
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool NDatabase::ImportField( const char *pszFieldName, std::string *pData )
{
	if ( !pStorageSource || !pStorageSource->HasStringField( pszFieldName ) )
		return false;
	std::wstring ws = pStorageSource->GetWString( pszFieldName );
	pData->resize( ws.size() );
	for ( int i = 0; i < (int)ws.size(); ++i )
		(*pData)[i] = (char)ws[i];
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool NDatabase::ImportField( const char *pszFieldName, std::wstring *pData )
{
	if ( !pStorageSource || !pStorageSource->HasStringField( pszFieldName ) )
		return false;
	*pData = pStorageSource->GetWString( pszFieldName );
	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void NDatabase::ImportField( const char *pszFieldName, CDBRecord **pRef, CDBTableBase *pDestTable )
{
	ASSERT( pDestTable );
	ASSERT( pStorageSource );
	*pRef = 0;
	int nID = pStorageSource ? pStorageSource->GetInt( pszFieldName ) : 0;
	if ( pDestTable )
		*pRef = pDestTable->GetDBRecord( nID );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void NDatabase::ImportRelation( CDBRecord *pSrc, CDBTableBase *pDestTable, std::vector< CPtr<CDBRecord> > *pRefs )
{
	list< SRelation > &relations = GetRelations();
	ASSERT( pDestTable );
	std::vector< CPtr<CDBRecord> > &refs = *pRefs;
	list<SRelation>::iterator k;
	CDBTableBase *pLeft = GetTableByRecord( pSrc );
	CDBTableBase *pRight = pDestTable;
	refs.clear();
	if ( pLeft == 0 || pRight == 0 || pLeft == pRight )
	{
		ASSERT(0);
		return;
	}
	bool bDone = false;
	for ( k = relations.begin(); k != relations.end(); ++k )
	{
		if ( k->pLeft == pLeft && k->pRight == pRight )
		{
			ASSERT( !bDone );
			bDone = true;
			int nLeftID = pSrc->GetRecordID();
			for ( size_t z = 0; z < k->data.size(); z++ )
			{
				if ( k->data[z].nLeft == nLeftID )
				{
					CDBRecord *pAdd = pRight->GetDBRecord( k->data[z].nRight );
					if ( pAdd != 0 )
						refs.push_back( pAdd );
					else
						ASSERT(0);
				}
			}
		}
		if ( k->pLeft == pRight && k->pRight == pLeft )
		{
			ASSERT( !bDone );
			bDone = true;
			int nLeftID = pSrc->GetRecordID();
			for ( size_t z = 0; z < k->data.size(); z++ )
			{
				if ( k->data[z].nRight == nLeftID )
				{
					CDBRecord *pAdd = pRight->GetDBRecord( k->data[z].nLeft );
					if ( pAdd != 0 )
						refs.push_back( pAdd );
					else
						ASSERT(0);
				}
			}
		}
	}
	ASSERT( bDone );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
// Post-load link builder -- see BasicDB.cpp's copy of this comment for context; resolved at
// Game.exe link time against DBFormat/DataMap.cpp.
namespace NDb { void BuildMapLinks( bool bTranslate ); }
////////////////////////////////////////////////////////////////////////////////////////////////////
void NDatabase::Serialize( CDataStream &file, CStructureSaver::EMode mode )
{
	CTablesHash &tables = GetTables();
	NDatabase::bIsDatabaseLoading = true;
	bool bDidColumnarLoad = false;
	{
		CStructureSaver f( file, mode );
		if ( mode == CStructureSaver::READ && f.GetVersion() >= 1 )
		{
			bDidColumnarLoad = true;
			typedef std::unordered_map< int, CObj<CDBTableDataStorage> > CStorageHash;
			CStorageHash storageTables;
			f.Add( 1, &storageTables );
			list<SRelation> &relations = GetRelations();
			f.Add( 2, &relations );
			for ( list<SRelation>::iterator r = relations.begin(); r != relations.end(); ++r )
			{
				r->pLeft = GetTable( r->nTableLeft );
				r->pRight = GetTable( r->nTableRight );
			}
			int nTables = 0;
			for ( CStorageHash::iterator it = storageTables.begin(); it != storageTables.end(); ++it )
			{
				CDBTableBase *pTable = GetTable( it->first );
				CDBTableDataStorage *pStorage = it->second;
				if ( !pTable || !pStorage )
					continue;
				pStorageSource = pStorage;
				pTable->PreCreate( it->first );
				pStorageSource = 0;
			}
			for ( CStorageHash::iterator it = storageTables.begin(); it != storageTables.end(); ++it )
			{
				CDBTableBase *pTable = GetTable( it->first );
				CDBTableDataStorage *pStorage = it->second;
				if ( !pTable || !pStorage )
					continue;
				pStorageSource = pStorage;
				pTable->Import();
				pStorageSource = 0;
				++nTables;
			}
			printf( "DB-STORAGE: loaded %d columnar tables from release/Steam game.db (v%d)\n",
							nTables, f.GetVersion() );
		}
		else
		{
			f.Add( 1, &tables );
		}
	}
	NDatabase::bIsDatabaseLoading = false;
	if ( bDidColumnarLoad )
		NDb::BuildMapLinks( false );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
//  CDBTableBase -- storage-only path (no ADO fallback: Linux never has a live DB connection).
////////////////////////////////////////////////////////////////////////////////////////////////////
using namespace NDatabase;
void CDBTableBase::PreCreate( int nTypeID )
{
	records.clear();
	ASSERT( pStorageSource );
	if ( !pStorageSource )
		return;
	for ( pStorageSource->MoveFirst(); !pStorageSource->IsEof(); pStorageSource->MoveNext() )
	{
		CDBRecord *pRes = GetRecordTypes().CreateObject( nTypeID );
		ASSERT( pRes );
		if ( !pRes )
			break;
		pRes->nID = pStorageSource->GetInt( "ID" );
		records[ pRes->nID ] = pRes;
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void CDBTableBase::Refresh( int nTypeID )
{
	// Only reachable from NDatabase::Refresh(int), which nothing calls on Linux
	// (see file header) -- live-reload from a database connection doesn't apply here.
	ASSERT(0);
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void CDBTableBase::Import()
{
	ASSERT( pStorageSource );
	if ( !pStorageSource )
		return;
	for ( pStorageSource->MoveFirst(); !pStorageSource->IsEof(); pStorageSource->MoveNext() )
	{
		int nID = pStorageSource->GetInt( "ID" );
		CRecordHash::iterator i = records.find( nID );
		if ( i == records.end() )
			continue;
		i->second->Import();
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////
CDBRecord* CDBTableBase::GetDBRecord( int nID )
{
	CRecordHash::iterator i = records.find( nID );
	if ( i == records.end() )
		return 0;
	return i->second;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
string NDatabase::GetDBConnectionStr( const string &szDBName )
{
	string szRet = "DRIVER=SQL Server;SERVER=";
	szRet += szDBName;
	szRet += ";UID=sa;PWD=simple;DATABASE=A5GAME;";
	return szRet;
}

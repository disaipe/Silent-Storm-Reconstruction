// ============================================================================
//  lsgdp.cpp -- minimal OLE2 Compound File reader + LifeStudio .msh decoder.
//
//  FaceGenHead.gdp is a standard Microsoft OLE2 Compound File (the "d0 cf 11 e0"
//  container also used by legacy .doc/.xls). Inside it, 800/object.msh holds the
//  base head geometry in a trivially simple form:
//
//      offset  0  u32   magic 0xe157f2ed
//      offset  6  u16   vertex count
//      offset 32  ...   vertexCount * 3 floats (x, y, z), tightly packed
//
//  Verified against all eight .msh streams in the retail file: the declared
//  count matches (size - 32) / 12 exactly in every case.
//
//  Only what is needed to pull one stream out is implemented: header, FAT chain
//  walking, the directory tree, and the mini-stream for entries below the 4096
//  byte cutoff. No writing, no property sets, no storage semantics beyond
//  matching a stream by its full path.
//
//  This exists because the LifeStudio DLLs that would normally supply head
//  geometry are Windows-only (see lsstub.cpp). Reading the game's own data file
//  gets us real heads instead of the collapsed ones a no-op Process() produced.
// ============================================================================
#include "../../../Misc/PlatformCompat.h"
#include "lsgdp.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace NLSGdp
{
namespace
{

typedef std::vector<unsigned char> TBytes;

const unsigned char OLE_SIGNATURE[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };

const uint32_t SECT_END  = 0xFFFFFFFE;   // ENDOFCHAIN
const uint32_t SECT_FREE = 0xFFFFFFFF;   // FREESECT
const uint32_t N_DIR_ENTRY_SIZE = 128;
const uint32_t N_MINI_CUTOFF = 4096;

inline uint16_t RdU16( const unsigned char *p ) { return (uint16_t)( p[0] | ( p[1] << 8 ) ); }
inline uint32_t RdU32( const unsigned char *p )
{
	return (uint32_t)p[0] | ( (uint32_t)p[1] << 8 ) | ( (uint32_t)p[2] << 16 ) | ( (uint32_t)p[3] << 24 );
}

bool ReadWholeFile( const char *pszPath, TBytes *pRes )
{
	FILE *pF = fopen( pszPath, "rb" );
	if ( !pF )
		return false;
	fseek( pF, 0, SEEK_END );
	const long nSize = ftell( pF );
	fseek( pF, 0, SEEK_SET );
	if ( nSize <= 0 )
	{
		fclose( pF );
		return false;
	}
	pRes->resize( (size_t)nSize );
	const size_t nRead = fread( &(*pRes)[0], 1, (size_t)nSize, pF );
	fclose( pF );
	return nRead == (size_t)nSize;
}

// One parsed compound file: enough state to pull a stream out by name.
struct SCompoundFile
{
	TBytes data;
	uint32_t nSectorSize;
	uint32_t nMiniSectorSize;
	std::vector<uint32_t> fat;        // sector -> next sector
	std::vector<uint32_t> miniFat;
	TBytes miniStream;
	uint32_t nDirStart;

	const unsigned char *Sector( uint32_t nSector ) const
	{
		// Sector 0 starts right after the 512-byte header.
		const size_t nOffset = 512 + (size_t)nSector * nSectorSize;
		if ( nOffset + nSectorSize > data.size() )
			return 0;
		return &data[nOffset];
	}

	// Walk a FAT chain, concatenating sectors until nBytes have been collected.
	bool ReadChain( uint32_t nStart, size_t nBytes, bool bMini, TBytes *pRes ) const
	{
		pRes->clear();
		pRes->reserve( nBytes );
		const std::vector<uint32_t> &chain = bMini ? miniFat : fat;
		const uint32_t nUnit = bMini ? nMiniSectorSize : nSectorSize;
		uint32_t nSector = nStart;
		// Bounded by the chain length so a corrupt file cannot spin forever.
		for ( size_t nGuard = 0; nGuard <= chain.size() && pRes->size() < nBytes; ++nGuard )
		{
			if ( nSector == SECT_END || nSector == SECT_FREE || nSector >= chain.size() )
				break;
			const unsigned char *pSrc = 0;
			if ( bMini )
			{
				const size_t nOffset = (size_t)nSector * nUnit;
				if ( nOffset + nUnit > miniStream.size() )
					break;
				pSrc = &miniStream[nOffset];
			}
			else
			{
				pSrc = Sector( nSector );
				if ( !pSrc )
					break;
			}
			const size_t nTake = ( nBytes - pRes->size() < nUnit ) ? nBytes - pRes->size() : nUnit;
			pRes->insert( pRes->end(), pSrc, pSrc + nTake );
			nSector = chain[nSector];
		}
		return pRes->size() == nBytes;
	}
};

bool ParseHeader( SCompoundFile *pFile )
{
	const TBytes &d = pFile->data;
	if ( d.size() < 512 || memcmp( &d[0], OLE_SIGNATURE, 8 ) != 0 )
		return false;

	pFile->nSectorSize = 1u << RdU16( &d[30] );
	pFile->nMiniSectorSize = 1u << RdU16( &d[32] );
	if ( pFile->nSectorSize < 128 || pFile->nSectorSize > 65536 )
		return false;

	const uint32_t nFatSectors = RdU32( &d[44] );
	pFile->nDirStart = RdU32( &d[48] );
	const uint32_t nMiniFatStart = RdU32( &d[60] );
	const uint32_t nMiniFatSectors = RdU32( &d[64] );
	const uint32_t nDifatStart = RdU32( &d[68] );
	const uint32_t nDifatSectors = RdU32( &d[72] );

	// DIFAT: the first 109 FAT sector numbers live in the header, the rest in a
	// chain of DIFAT sectors. Retail's file fits in the header, but follow the
	// chain anyway rather than silently truncating a larger one.
	std::vector<uint32_t> fatSectors;
	for ( int i = 0; i < 109 && fatSectors.size() < nFatSectors; ++i )
	{
		const uint32_t nSect = RdU32( &d[76 + i * 4] );
		if ( nSect != SECT_FREE )
			fatSectors.push_back( nSect );
	}
	uint32_t nDifat = nDifatStart;
	for ( uint32_t nGuard = 0; nGuard < nDifatSectors && nDifat != SECT_END && nDifat != SECT_FREE; ++nGuard )
	{
		const unsigned char *pSect = pFile->Sector( nDifat );
		if ( !pSect )
			break;
		const uint32_t nPerSector = pFile->nSectorSize / 4 - 1;
		for ( uint32_t i = 0; i < nPerSector && fatSectors.size() < nFatSectors; ++i )
		{
			const uint32_t nSect = RdU32( pSect + i * 4 );
			if ( nSect != SECT_FREE )
				fatSectors.push_back( nSect );
		}
		nDifat = RdU32( pSect + pFile->nSectorSize - 4 );
	}

	// The FAT itself is those sectors concatenated, read as u32 entries.
	pFile->fat.clear();
	for ( size_t i = 0; i < fatSectors.size(); ++i )
	{
		const unsigned char *pSect = pFile->Sector( fatSectors[i] );
		if ( !pSect )
			return false;
		for ( uint32_t j = 0; j < pFile->nSectorSize / 4; ++j )
			pFile->fat.push_back( RdU32( pSect + j * 4 ) );
	}
	if ( pFile->fat.empty() )
		return false;

	// Mini FAT, same idea.
	pFile->miniFat.clear();
	uint32_t nSector = nMiniFatStart;
	for ( uint32_t nGuard = 0; nGuard < nMiniFatSectors && nSector != SECT_END && nSector != SECT_FREE; ++nGuard )
	{
		const unsigned char *pSect = pFile->Sector( nSector );
		if ( !pSect )
			break;
		for ( uint32_t j = 0; j < pFile->nSectorSize / 4; ++j )
			pFile->miniFat.push_back( RdU32( pSect + j * 4 ) );
		if ( nSector >= pFile->fat.size() )
			break;
		nSector = pFile->fat[nSector];
	}
	return true;
}

// Directory entries are a flat array; names are UTF-16LE. Only stream entries
// (type 2) matter here, and only their leaf name -- the retail file has no two
// streams sharing a leaf name that we care about.
struct SDirEntry
{
	std::string szName;
	int nType;
	uint32_t nStart;
	uint64_t nSize;
};

bool ReadDirectory( const SCompoundFile &file, std::vector<SDirEntry> *pRes )
{
	TBytes dir;
	// Directory length is not stored; walk the chain to its end.
	uint32_t nSector = file.nDirStart;
	for ( size_t nGuard = 0; nGuard <= file.fat.size(); ++nGuard )
	{
		if ( nSector == SECT_END || nSector == SECT_FREE || nSector >= file.fat.size() )
			break;
		const unsigned char *pSect = file.Sector( nSector );
		if ( !pSect )
			break;
		dir.insert( dir.end(), pSect, pSect + file.nSectorSize );
		nSector = file.fat[nSector];
	}
	if ( dir.empty() )
		return false;

	const size_t nEntries = dir.size() / N_DIR_ENTRY_SIZE;
	for ( size_t i = 0; i < nEntries; ++i )
	{
		const unsigned char *p = &dir[i * N_DIR_ENTRY_SIZE];
		const uint16_t nNameBytes = RdU16( p + 64 );
		SDirEntry e;
		e.nType = p[66];
		e.nStart = RdU32( p + 116 );
		e.nSize = (uint64_t)RdU32( p + 120 ) | ( (uint64_t)RdU32( p + 124 ) << 32 );
		// UTF-16LE -> ASCII; these names are all plain ASCII in practice.
		for ( uint16_t j = 0; j + 1 < nNameBytes; j += 2 )
		{
			const uint16_t wc = RdU16( p + j );
			if ( !wc )
				break;
			e.szName += ( wc < 128 ) ? (char)wc : '?';
		}
		pRes->push_back( e );
	}
	return true;
}

}   // anonymous namespace

////////////////////////////////////////////////////////////////////////////////////////////////////
bool LoadBaseMesh( const char *pszGdpPath, const char *pszStreamName, std::vector<float> *pVerts )
{
	if ( !pVerts )
		return false;
	pVerts->clear();

	SCompoundFile file;
	if ( !ReadWholeFile( pszGdpPath, &file.data ) )
		return false;
	if ( !ParseHeader( &file ) )
		return false;

	std::vector<SDirEntry> entries;
	if ( !ReadDirectory( file, &entries ) )
		return false;

	// Entry 0 is the root storage; its stream holds the mini-stream pool.
	if ( !entries.empty() && entries[0].nSize > 0 )
		file.ReadChain( entries[0].nStart, (size_t)entries[0].nSize, false, &file.miniStream );

	const SDirEntry *pTarget = 0;
	for ( size_t i = 0; i < entries.size(); ++i )
	{
		if ( entries[i].nType == 2 && entries[i].szName == pszStreamName )
		{
			pTarget = &entries[i];
			break;
		}
	}
	if ( !pTarget || pTarget->nSize < 32 )
		return false;

	TBytes stream;
	const bool bMini = pTarget->nSize < N_MINI_CUTOFF;
	if ( !file.ReadChain( pTarget->nStart, (size_t)pTarget->nSize, bMini, &stream ) )
		return false;

	// --- decode the .msh ---
	if ( RdU32( &stream[0] ) != 0xE157F2ED )
		return false;
	const uint16_t nVerts = RdU16( &stream[6] );
	// The count is authoritative, but cross-check it against the stream length
	// rather than trusting it blindly.
	if ( nVerts == 0 || (size_t)nVerts * 12 + 32 != stream.size() )
		return false;

	pVerts->resize( (size_t)nVerts * 3 );
	memcpy( &(*pVerts)[0], &stream[32], (size_t)nVerts * 12 );
	return true;
}

}   // namespace NLSGdp

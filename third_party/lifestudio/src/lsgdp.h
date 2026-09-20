#ifndef __LSGDP_H__
#define __LSGDP_H__
// ============================================================================
//  lsgdp.h -- read base head geometry out of FaceGenHead.gdp.
//
//  Linux-only helper: the LifeStudio DLLs that would normally produce head
//  vertices are Windows-only, so the Linux build reads the game's own GDP file
//  instead. See lsgdp.cpp for the container and mesh formats.
// ============================================================================
#include <vector>

namespace NLSGdp
{
	// Reads pszStreamName (e.g. "object.msh") out of the OLE2 compound file at
	// pszGdpPath and decodes it as a LifeStudio mesh. On success pVerts holds
	// vertexCount * 3 floats, tightly packed x,y,z. Returns false and leaves
	// pVerts empty if the file is missing, not a compound file, has no such
	// stream, or the stream is not a well-formed mesh.
	bool LoadBaseMesh( const char *pszGdpPath, const char *pszStreamName, std::vector<float> *pVerts );
}

#endif

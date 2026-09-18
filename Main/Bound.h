#ifndef __Bound_H_
#define __Bound_H_
#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000
////////////////////////////////////////////////////////////////////////////////////////////////////
template<class T>
struct SGetSelf
{
	const T& operator()( const T &a ) const { return a; }
};
struct SBoundCalcer
{
	CVec3 ptMin, ptMax;
	SBoundCalcer(): ptMin( 1e38f,1e38f,1e38f ), ptMax(-1e38f,-1e38f,-1e38f) {}
	template<class TSet,class TGetPoint>
		void LookSet( const TSet &a, TGetPoint GetPoint )
	{
		for ( typename TSet::const_iterator i = a.begin(); i != a.end(); ++i )
		{
			const CVec3 &p = GetPoint(*i);
			ptMin.x = Min( ptMin.x, p.x );
			ptMin.y = Min( ptMin.y, p.y );
			ptMin.z = Min( ptMin.z, p.z );
			ptMax.x = Max( ptMax.x, p.x );
			ptMax.y = Max( ptMax.y, p.y );
			ptMax.z = Max( ptMax.z, p.z );
		}
	}
	void Clear() { ptMin = CVec3( 1e38f,1e38f,1e38f ); ptMax = CVec3(-1e38f,-1e38f,-1e38f); }
	void Add( const CVec3 &p, float fRadius )
	{
		ptMin.x = Min( ptMin.x, p.x - fRadius );
		ptMin.y = Min( ptMin.y, p.y - fRadius );
		ptMin.z = Min( ptMin.z, p.z - fRadius );
		ptMax.x = Max( ptMax.x, p.x + fRadius );
		ptMax.y = Max( ptMax.y, p.y + fRadius );
		ptMax.z = Max( ptMax.z, p.z + fRadius );
	}
	void Add( const CVec3 &_p )
	{
		ptMin.x = Min( ptMin.x, _p.x );
		ptMin.y = Min( ptMin.y, _p.y );
		ptMin.z = Min( ptMin.z, _p.z );
		ptMax.x = Max( ptMax.x, _p.x );
		ptMax.y = Max( ptMax.y, _p.y );
		ptMax.z = Max( ptMax.z, _p.z );
	}
	void Add( const SBoundCalcer &bc )
	{
		ptMin.Minimize( bc.ptMin );
		ptMax.Maximize( bc.ptMax );
	}
	void Add( const SBound &bv )
	{
		ptMin.Minimize( bv.s.ptCenter - bv.ptHalfBox );
		ptMax.Maximize( bv.s.ptCenter + bv.ptHalfBox );
	}
	bool IsEmpty() const { return ptMin.x > ptMax.x; }
	void Make( SBound *pRes ) const
	{
		if ( IsEmpty() )
			pRes->BoxInit( CVec3(0,0,0), CVec3(0,0,0) ); 
		else
			pRes->BoxInit( ptMin, ptMax ); 
	}
	void Make( SSphere *pRes ) const
	{
		if ( IsEmpty() )
		{
			pRes->ptCenter = CVec3(0,0,0);
			pRes->fRadius = 0;
		}
		else
		{
			pRes->ptCenter = ( ptMax + ptMin ) * 0.5f;
			pRes->fRadius = fabs( ptMax - ptMin ) * 0.5f;
		}
	}
};
////////////////////////////////////////////////////////////////////////////////////////////////////
template<class TRes, class TSet,class TGetPoint>
inline void CalcBound( TRes *pRes, const TSet &a, TGetPoint GetPoint )
{
	SBoundCalcer b;
	b.LookSet( a, GetPoint );
	b.Make( pRes );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
// should not be mixed with fpu & mmx code from StartMMXBound to StoreMMXBoundResult
#ifdef _MSC_VER
#pragma warning( disable : 4799 )
#endif
#ifdef _MSC_VER
inline void StartMMXBound( CVec3 *pMin, CVec3 *pMax )
{
	__asm
	{
		mov esi, pMin
		movq mm4, [esi]
		movd mm5, [esi+8]
		mov esi, pMax
		movq mm6, [esi]
		movd mm7, [esi+8]
	}
}
inline void AddMMXBoundPoint( const CVec3 *p )
{
	__asm
	{
		mov esi, p
		movq mm0, [esi]
		movd mm1, [esi+8]
		movq mm2, mm6
		movq mm3, mm0
		pcmpgtd mm2, mm0
		pcmpgtd mm3, mm4
		pand mm6, mm2
		pand mm4, mm3
		pandn mm2, mm0
		pandn mm3, mm0
		por mm6, mm2
		por mm4, mm3

		movq mm2, mm7
		movq mm3, mm1
		pcmpgtd mm2, mm1
		pcmpgtd mm3, mm5
		pand mm7, mm2
		pand mm5, mm3
		pandn mm2, mm1
		pandn mm3, mm1
		por mm7, mm2
		por mm5, mm3
	}
}
inline void StoreMMXBoundResult( CVec3 *pMin, CVec3 *pMax )
{
	__asm
	{
		mov esi, pMin
		movq [esi], mm4
		movd [esi+8], mm5
		mov esi, pMax
		movq [esi], mm6
		movd [esi+8], mm7
		emms
	}
}
#else
// Portable fallback -- StartMMXBound/AddMMXBoundPoint/StoreMMXBoundResult share running
// min/max state across calls the same way the MMX registers did on the MSVC path (see
// above): a function-local static in an inline function is a single instance shared by
// every translation unit that includes this header.
inline CVec3 &MMXBoundMinAcc() { static CVec3 v; return v; }
inline CVec3 &MMXBoundMaxAcc() { static CVec3 v; return v; }
inline void StartMMXBound( CVec3 *pMin, CVec3 *pMax )
{
	MMXBoundMinAcc() = *pMin;
	MMXBoundMaxAcc() = *pMax;
}
inline void AddMMXBoundPoint( const CVec3 *p )
{
	CVec3 &vMin = MMXBoundMinAcc();
	CVec3 &vMax = MMXBoundMaxAcc();
	for ( int i = 0; i < 3; ++i )
	{
		if ( p->m[i] < vMin.m[i] ) vMin.m[i] = p->m[i];
		if ( p->m[i] > vMax.m[i] ) vMax.m[i] = p->m[i];
	}
}
inline void StoreMMXBoundResult( CVec3 *pMin, CVec3 *pMax )
{
	*pMin = MMXBoundMinAcc();
	*pMax = MMXBoundMaxAcc();
}
#endif // _MSC_VER
#ifdef _MSC_VER
#pragma warning( default : 4799 )
#endif
////////////////////////////////////////////////////////////////////////////////////////////////////
#endif

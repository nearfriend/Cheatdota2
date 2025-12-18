#pragma once

// Minimal ATL stubs to satisfy BlackBone builds without ATL SDK.
#include <Unknwn.h>

namespace ATL
{
	class CAtlBaseModule {};
	extern CAtlBaseModule _AtlBaseModule;
}

template <class T>
class CComPtr
{
public:
	CComPtr() noexcept : p( nullptr ) {}
	explicit CComPtr( T* ptr ) noexcept : p( ptr )
	{
		if ( p ) p->AddRef();
	}
	CComPtr( const CComPtr& other ) noexcept : p( other.p )
	{
		if ( p ) p->AddRef();
	}
	~CComPtr()
	{
		if ( p ) p->Release();
	}

	CComPtr& operator=( const CComPtr& other ) noexcept
	{
		return operator=( other.p );
	}

	CComPtr& operator=( T* ptr ) noexcept
	{
		if ( ptr ) ptr->AddRef();
		if ( p ) p->Release();
		p = ptr;
		return *this;
	}

	T* operator->() const noexcept { return p; }
	T** operator&() noexcept { return &p; }
	operator T*() const noexcept { return p; }

	void Release() noexcept
	{
		if ( p )
		{
			p->Release();
			p = nullptr;
		}
	}

private:
	T* p;
};

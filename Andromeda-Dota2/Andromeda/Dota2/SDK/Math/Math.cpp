#include "Math.hpp"

#include <cmath>
#include <Windows.h>
#include <ImGui/imgui_internal.h>

#include <Dota2/SDK/SDK.hpp>
#include <Common/MemoryEngine.hpp>

namespace Math
{
	using WorldToScreenFn = bool( __fastcall* )( float* worldPos , int* screenX , int* screenY , float* optionalOffset );
	using GetViewSlotFn = void* ( __fastcall* )( int slot );

	auto ResolveGetViewSlotFn() -> GetViewSlotFn
	{
		static GetViewSlotFn cached = nullptr;
		static bool attempted = false;
		if ( attempted )
			return cached;
		attempted = true;

		// Returns the active view object for a split-screen slot (null until the world is ready).
		if ( auto* found = FindPattern( CLIENT_DLL ,
			"83 F9 FF 75 ? 33 C9 EB ? 85 C9 78 ? 48 63 C1 48 83 F8 01 73 ? 48 8D 0D" ) )
		{
			cached = reinterpret_cast<GetViewSlotFn>( found );
			DEV_LOG( "[w2s] resolved GetViewSlot @ %p\n" , found );
		}
		else
		{
			DEV_LOG( "[w2s] GetViewSlot pattern not found\n" );
		}

		return cached;
	}

	auto ResolveWorldToScreenFn() -> WorldToScreenFn
	{
		static WorldToScreenFn cached = nullptr;
		static bool attempted = false;
		if ( attempted )
			return cached;
		attempted = true;

		static const char* directPatterns[] =
		{
			"40 53 56 57 48 83 EC 60 49 8B F8 48 8B F2 48 8B D9 4D 85 C9 74 2D" ,
			nullptr
		};

		for ( int index = 0; directPatterns[index]; ++index )
		{
			if ( auto* found = FindPattern( CLIENT_DLL , directPatterns[index] ) )
			{
				cached = reinterpret_cast<WorldToScreenFn>( found );
				DEV_LOG( "[w2s] resolved engine WorldToScreen @ %p\n" , found );
				return cached;
			}
		}

		if ( auto* wrapper = FindPattern( CLIENT_DLL , "48 8B C4 48 83 EC 48 4C 8D 48 D8 F3 0F 11 48 E8" ) )
		{
			auto* bytes = reinterpret_cast<uint8_t*>( wrapper );
			for ( int offset = 40; offset < 80; ++offset )
			{
				if ( bytes[offset] != 0xE8 )
					continue;

				cached = reinterpret_cast<WorldToScreenFn>(
					GetCallAddress( reinterpret_cast<intptr_t>( bytes + offset ) ) );
				DEV_LOG( "[w2s] resolved WorldToScreen via panorama wrapper @ %p\n" , cached );
				return cached;
			}
		}

		DEV_LOG( "[w2s] failed to resolve WorldToScreen\n" );
		return nullptr;
	}

	auto IsViewReady() -> bool
	{
		auto* getView = ResolveGetViewSlotFn();
		if ( !getView )
			return false;

		void* view = nullptr;
		__try
		{
			view = getView( 0 );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return false;
		}

		if ( !view )
			return false;

		// WorldToScreen immediately does: mov rcx, [view]; call [rcx+0x80]
		__try
		{
			auto* vtable = *reinterpret_cast<void***>( view );
			if ( !vtable || !vtable[0x80 / sizeof( void* )] )
				return false;
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return false;
		}

		return true;
	}

	auto InvokeEngineWorldToScreen( float* pos , int* screenX , int* screenY ) -> bool
	{
		auto* fn = ResolveWorldToScreenFn();
		if ( !fn )
			return false;

		// Prefer skipping when the active view is clearly missing — that path
		// null-derefs inside client/tier0. If the view helper is unresolved,
		// still attempt under SEH so the ring can work.
		if ( ResolveGetViewSlotFn() && !IsViewReady() )
			return false;

		bool ok = false;
		__try
		{
			ok = fn( pos , screenX , screenY , nullptr );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			ok = false;
		}

		return ok;
	}

	auto ProjectWorldToScreen( const Vector3& vIn , ImVec2& vOut ) -> bool
	{
		if ( !ImGui::GetCurrentContext() )
			return false;

		float pos[3] = { vIn.m_x , vIn.m_y , vIn.m_z };
		int screenX = 0;
		int screenY = 0;
		if ( !InvokeEngineWorldToScreen( pos , &screenX , &screenY ) )
			return false;
		if ( screenX < 0 || screenY < 0 )
			return false;

		vOut.x = static_cast<float>( screenX );
		vOut.y = static_cast<float>( screenY );
		return std::isfinite( vOut.x ) && std::isfinite( vOut.y );
	}

	auto WorldToScreen( const Vector3& vIn , ImVec2& vOut ) -> bool
	{
		return ProjectWorldToScreen( vIn , vOut );
	}

	auto WorldToScreen( const Vector3& vIn , Vector2& vOut ) -> bool
	{
		ImVec2 screen{};
		if ( !ProjectWorldToScreen( vIn , screen ) )
			return false;

		vOut.m_x = screen.x;
		vOut.m_y = screen.y;
		return true;
	}

	auto WorldToScreen( const Vector3& vIn , Vector3& vOut ) -> bool
	{
		ImVec2 screen{};
		if ( !ProjectWorldToScreen( vIn , screen ) )
			return false;

		vOut.m_x = screen.x;
		vOut.m_y = screen.y;
		vOut.m_z = 0.f;
		return true;
	}

	auto AngleNormalize( float angle ) -> float
	{
		angle = std::fmod( angle , 360.f );

		if ( angle > 180.f )
			angle -= 360.f;
		else if ( angle < -180.f )
			angle += 360.f;

		return angle;
	}

	auto NormalizeAngles( QAngle& QAngle ) -> void
	{
		for ( auto i = 0; i < 3; i++ )
		{
			while ( QAngle[i] < -180.0f ) QAngle[i] += 360.0f;
			while ( QAngle[i] > 180.0f ) QAngle[i] -= 360.0f;
		}
	}

	auto ClampAngles( QAngle& QAngle ) -> void
	{
		if ( QAngle.m_x > 89.0f ) QAngle.m_x = 89.0f;
		else if ( QAngle.m_x < -89.0f ) QAngle.m_x = -89.0f;

		if ( QAngle.m_y > 180.0f ) QAngle.m_y = 180.0f;
		else if ( QAngle.m_y < -180.0f ) QAngle.m_y = -180.0f;

		QAngle.m_z = 0;
	}

	auto CalcAngle( const Vector3& src , const Vector3& dst ) -> QAngle
	{
		QAngle vAngle;

		Vector3 delta( ( src.m_x - dst.m_x ) , ( src.m_y - dst.m_y ) , ( src.m_z - dst.m_z ) );
		double hyp = sqrt( delta.m_x * delta.m_x + delta.m_y * delta.m_y );

		vAngle.m_x = float( atanf( float( delta.m_z / hyp ) ) * 57.295779513082f );
		vAngle.m_y = float( atanf( float( delta.m_y / delta.m_x ) ) * 57.295779513082f );
		vAngle.m_z = 0.0f;

		if ( delta.m_x >= 0.0 )
			vAngle.m_y += 180.0f;

		return vAngle;
	}

	auto VectorTransform( const Vector3& vIn1 , matrix3x4_t& vIn2 , Vector3& vOut ) -> void
	{
		vOut.m_x = vIn1.m_x * vIn2[0][0] + vIn1.m_y * vIn2[0][1] + vIn1.m_z * vIn2[0][2] + vIn2[0][3];
		vOut.m_y = vIn1.m_x * vIn2[1][0] + vIn1.m_y * vIn2[1][1] + vIn1.m_z * vIn2[1][2] + vIn2[1][3];
		vOut.m_z = vIn1.m_x * vIn2[2][0] + vIn1.m_y * vIn2[2][1] + vIn1.m_z * vIn2[2][2] + vIn2[2][3];
	}

	auto AngleVectors( const QAngle& QAngle , Vector3& vForward ) -> void
	{
		vec_t sp , sy , cp , cy;

		sp = sinf( DEG2RAD( QAngle[0] ) );
		cp = cosf( DEG2RAD( QAngle[0] ) );

		sy = sinf( DEG2RAD( QAngle[1] ) );
		cy = cosf( DEG2RAD( QAngle[1] ) );

		vForward.m_x = cp * cy;
		vForward.m_y = cp * sy;
		vForward.m_z = -sp;
	}

	auto AngleVectors( const QAngle& QAngle , Vector3& vForward , Vector3& vRight , Vector3& vUp ) -> void
	{
		vec_t sr , sp , sy , cr , cp , cy;

		sp = sinf( DEG2RAD( QAngle[0] ) );
		cp = cosf( DEG2RAD( QAngle[0] ) );

		sy = sinf( DEG2RAD( QAngle[1] ) );
		cy = cosf( DEG2RAD( QAngle[1] ) );

		sr = sinf( DEG2RAD( QAngle[2] ) );
		cr = cosf( DEG2RAD( QAngle[2] ) );

		vForward.m_x = ( cp * cy );
		vForward.m_y = ( cp * sy );
		vForward.m_z = ( -sp );

		vRight.m_x = ( -1 * sr * sp * cy + -1 * cr * -sy );
		vRight.m_y = ( -1 * sr * sp * sy + -1 * cr * cy );
		vRight.m_z = ( -1 * sr * cp );

		vUp.m_x = ( cr * sp * cy + -sr * -sy );
		vUp.m_y = ( cr * sp * sy + -sr * cy );
		vUp.m_z = ( cr * cp );
	}

	auto VectorAngles( const Vector3& vForward , QAngle& QAngle ) -> void
	{
		float tmp , yaw , pitch;

		if ( vForward[1] == 0.f && vForward[0] == 0.f )
		{
			yaw = 0.f;

			if ( vForward[2] > 0 )
				pitch = 270.f;
			else
				pitch = 90.f;
		}
		else
		{
			yaw = ( atan2f( vForward[1] , vForward[0] ) * 180.f / M_PI_F );

			if ( yaw < 0 )
				yaw += 360.f;

			tmp = sqrtf( vForward[0] * vForward[0] + vForward[1] * vForward[1] );
			pitch = ( atan2f( -vForward[2] , tmp ) * 180.f / M_PI_F );

			if ( pitch < 0 )
				pitch += 360.f;
		}

		QAngle[0] = pitch;
		QAngle[1] = yaw;
		QAngle[2] = 0;
	}

	auto SmoothAngles( QAngle QViewAngles , QAngle QAimAngles , QAngle& QOutAngles , float Smoothing ) -> void
	{
		if ( Smoothing < 1.f )
			Smoothing = 1.f;

		QAngle qDiffAngles = QAimAngles - QViewAngles;

		NormalizeAngles( qDiffAngles );
		ClampAngles( qDiffAngles );

		QOutAngles = qDiffAngles / Smoothing + QViewAngles;

		NormalizeAngles( QOutAngles );
		ClampAngles( QOutAngles );
	}
}

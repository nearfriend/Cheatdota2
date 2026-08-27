#pragma once

#include <Common/Common.hpp>

#include <Dota2/CBasePattern.hpp>
#include <Dota2/CHook_Loader.hpp>

#include <AndromedaClient/Heroes/Invoker/CInvokerController.hpp>
#include <AndromedaClient/Heroes/Meepo/CMeepoController.hpp>
#include <AndromedaClient/Features/CKillStealer.hpp>
#include <AndromedaClient/Features/CLastHitAssistant.hpp>
#include <AndromedaClient/Features/CAutoCombo.hpp>
#include <AndromedaClient/Features/CDodger.hpp>

#include <array>
#include <atomic>

class CDOTAInput;
class CUserCmd;
class CHeroDataLoader;
class CEntityInstance;

class IAndromedaClient
{
public:
	virtual void OnRender() = 0;
	virtual void OnCreateMove( CDOTAInput* pCDOTAInput , CUserCmd* pCUserCmd ) = 0;
};

class CAndromedaClient final : public IAndromedaClient
{
public:
	auto OnInit() -> void;

public:
	auto SetCameraDistance( float Distance ) -> void;

private:
	auto ResolveCameraPointers() -> void;

public:
	virtual void OnRender() override;
	virtual void OnCreateMove( CDOTAInput* pCDOTAInput , CUserCmd* pCUserCmd ) override;

public:
	auto GetHeroData() -> CHeroDataLoader*;
	auto RegisterFogController( CEntityInstance* pEntity ) -> void;
	auto UnregisterFogController( CEntityInstance* pEntity ) -> void;
	CMeepoController& GetMeepoController() { return m_MeepoController; }
	const CMeepoController& GetMeepoController() const { return m_MeepoController; }
	CInvokerController& GetInvokerController() { return m_InvokerController; }
	const CInvokerController& GetInvokerController() const { return m_InvokerController; }

private:
	auto ResolveFogOffsets() -> void;
	auto ResolveFogConVars() -> void;
	auto ApplyFogConVars() -> bool;
	auto ApplyNoFogParams( uintptr_t fog ) -> void;
	auto ApplyNoFogCameraServices() -> void;
	auto ApplyNoFog() -> void;
	auto ApplyNoFog( CEntityInstance* pEntity ) -> void;
	auto FogPlanesReady() const -> bool;
	auto DiscoverFogControllers() -> void;

	CBasePattern dota_camera_distance = { XorStr( "dota_camera_distance" ) , XorStr( "F3 0F 11 05 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? BA ? ? ? ? F3 0F 11 05 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? BA" ) , XorStr( CLIENT_DLL ) , 0 , eBasePatternSearchType::SEARCH_TYPE_MOV_PTR };
	CBasePattern dota_camera_farplane = { XorStr( "dota_camera_farplane" ) , XorStr( "F3 0F 11 05 ? ? ? ? 48 83 C4 ? C3" ) , XorStr( CLIENT_DLL ) , 0 , eBasePatternSearchType::SEARCH_TYPE_MOV_PTR };

	auto SearchCameraConvar( CBasePattern& pattern , const char** fallbackPatterns ) -> bool;

private:
	float* m_pCameraDistance = nullptr;
	float* m_pRFarz = nullptr;
	bool m_bCameraWasEnabled = true;

	// Dota RTS camera fog — these drive the zoom haze (not C_FogController).
	float* m_pFogStartZoomedOut = nullptr;
	float* m_pFogEndZoomedOut = nullptr;
	float* m_pFogStartZoomedIn = nullptr;
	float* m_pFogEndZoomedIn = nullptr;
	bool* m_pFogEnable = nullptr;
	int m_FogConVarResolveAttempts = 0;

	std::array<std::atomic<CEntityInstance*> , 16> m_FogControllers{};

	// Shared fogparams_t layout used by controllers and local camera services.
	uint32_t m_FogEnableOffset = 0;
	uint32_t m_FogBlendOffset = 0;
	uint32_t m_FogStartOffset = 0;
	uint32_t m_FogEndOffset = 0;
	uint32_t m_FogFarZOffset = 0;
	uint32_t m_FogMaxDensityOffset = 0;
	uint32_t m_FogStartLerpOffset = 0;
	uint32_t m_FogEndLerpOffset = 0;
	uint32_t m_FogMaxDensityLerpOffset = 0;
	uint32_t m_FogSkyboxFactorOffset = 0;
	uint32_t m_FogSkyboxFactorLerpOffset = 0;
	uint32_t m_FogBlendToBackgroundOffset = 0;
	uint32_t m_FogScatteringOffset = 0;
	uint32_t m_FogControllerFogOffset = 0;
	uint32_t m_FogChangedVariablesOffset = 0;
	uint32_t m_PlayerControllerPawnOffset = 0;
	uint32_t m_PlayerPawnCameraServicesOffset = 0;
	uint32_t m_CameraCurrentFogOffset = 0;
	uint32_t m_CameraOverrideFogColorEnabledOffset = 0;
	uint32_t m_CameraOverrideFogColorOffset = 0;
	uint32_t m_CameraOverrideFogStartEndEnabledOffset = 0;
	uint32_t m_CameraOverrideFogStartOffset = 0;
	uint32_t m_CameraOverrideFogEndOffset = 0;
	bool m_FogParamsResolved = false;
	bool m_FogControllerResolved = false;
	bool m_CameraFogResolved = false;
	int m_FogScanCursor = 0;
	ULONGLONG m_NextFogRescanTick = 0;

public:
	CAutoCombo& GetAutoCombo() { return m_AutoCombo; }
	CDodger& GetDodger() { return m_Dodger; }
	const CDodger& GetDodger() const { return m_Dodger; }
	const CAutoCombo& GetAutoCombo() const { return m_AutoCombo; }

private:
	CKillStealer m_KillStealer;
	CLastHitAssistant m_LastHitAssistant;
	CInvokerController m_InvokerController;
	CMeepoController m_MeepoController;
	CAutoCombo m_AutoCombo;
	CDodger m_Dodger;
};

auto GetAndromedaClient() -> CAndromedaClient*;

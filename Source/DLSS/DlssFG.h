/*
 * CDlssFG -- DLSS Frame Generation (NGX feature 3).
 *
 * Self-contained: manages the nvngx_dlssg.dll runtime over D3D12 via CD3D12Bridge.
 * Follows the proven architecture of CDlssNR.
 */

#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include "NGXTypes.h"
#include "D3D12Interop.h"

class CDlssFG
{
public:
	struct Params {
		int  iMultiplier = 2; // 2 = 2x (1 generated frame)
		int  iIndex      = 1; // 1 to Multiplier-1
		bool bReset      = false;
	};

	struct Timing {
		double inputWaitMs = 0;
		double recordMs    = 0;
		double gpuWaitMs   = 0;
	};
	const Timing& LastTiming() const { return m_Timing; }

	enum class State {
		Off, DllNotFound, DllLoadFailed, ExportsMissing, ShimMissing,
		NoD3D12, ApiInitFailed, ShareFailed, FeatureCreateFailed,
		EvaluateFailed, DeviceLost, Ready
	};

	CDlssFG() = default;
	~CDlssFG();
	CDlssFG(const CDlssFG&) = delete;
	CDlssFG& operator=(const CDlssFG&) = delete;

	bool Init(ID3D11Device* pDevice, const wchar_t* pConfiguredDllPath);
	void Shutdown();

	// Creates the resolution-bound FG feature using shared D3D11 textures.
	// pShared11In: the current real frame (RGBA16F or matching swap format)
	// pShared11InterpOut: where the generated interpolated frame is written
	bool CreateFeature(ID3D11Texture2D* pShared11In, ID3D11Texture2D* pShared11InterpOut,
	                   UINT w, UINT h);
	void ReleaseFeature();

	// Passes motion vectors and depth.
	// pMVec: RG16F per-pixel motion vectors from optical flow
	// pDepth: optional depth buffer (or nullptr)
	bool SetGuides(ID3D11Texture2D* pMVec, ID3D11Texture2D* pDepth = nullptr);

	// Evaluates frame generation. The generated frame is written to pShared11InterpOut.
	bool Evaluate(const Params& p);

	void RequestReset() { m_bResetPending = true; }

	bool IsInitialised() const { return m_bInitialised; }
	bool IsFeatureReady() const { return m_pFeature != nullptr; }
	bool MatchesFeature(UINT w, UINT h) const {
		return m_pFeature && m_featW == w && m_featH == h;
	}
	bool MatchesTextures(ID3D11Texture2D* pIn, ID3D11Texture2D* pInterpOut) const {
		return m_pShared11In == pIn && m_pShared11InterpOut == pInterpOut && m_pTex12In && m_pTex12InterpOut;
	}

	State            GetState() const { return m_State; }
	const wchar_t*   GetStateName() const;
	std::wstring     GetStatusLine() const;
	std::wstring     GetInfoBlock() const;
	const std::deque<std::wstring>& GetLog() const { return m_Log; }

	static std::vector<std::wstring> CandidateDllPaths(const wchar_t* pConfigured);

private:
	bool LoadCore();
	bool LoadSnippet(const wchar_t* pConfiguredDllPath);
	bool LoadShim();
	void QueryRequirements();
	void PushEvaluateParams(NVSDK_NGX_Parameter* p, const Params& s, bool bReset);
	void Log(const wchar_t* fmt, ...);

	NVSDK_NGX_Result CallInit();
	NVSDK_NGX_Result CallPopulate(NVSDK_NGX_Parameter* p);
	NVSDK_NGX_Result CallCreate(NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out);
	NVSDK_NGX_Result CallEvaluate(const NVSDK_NGX_Parameter* p);
	NVSDK_NGX_Result CallRelease(NVSDK_NGX_Handle* h);
	NVSDK_NGX_Result CallShutdown1();

	struct GuideSlot {
		CComPtr<ID3D11Texture2D> p11;
		CComPtr<ID3D12Resource>  p12;
		UINT w = 0, h = 0;
	};
	bool SetGuideSlot(GuideSlot& slot, const wchar_t* which, ID3D11Texture2D* pTex11);

	static const wchar_t s_SnippetName[];
	static const wchar_t s_ShimName[];

	HMODULE m_hSnippet = nullptr;
	HMODULE m_hCore    = nullptr;
	HMODULE m_hShim    = nullptr;

	bool m_bUseShim = false;
	PFN_ShimInit      m_pfnShimInit      = nullptr;
	PFN_ShimPopulate  m_pfnShimPopulate  = nullptr;
	PFN_ShimCreate    m_pfnShimCreate    = nullptr;
	PFN_ShimEval      m_pfnShimEval      = nullptr;
	PFN_ShimRelease   m_pfnShimRelease   = nullptr;
	PFN_ShimShutdown1 m_pfnShimShutdown  = nullptr;

	void* m_pfnCreate12   = nullptr;
	void* m_pfnEval12     = nullptr;
	void* m_pfnRelease12  = nullptr;
	void* m_pfnShutdown12 = nullptr;

	PFN_NGX_D3D11_AllocateParameters m_pfnCoreAlloc   = nullptr;
	PFN_NGX_D3D11_DestroyParameters  m_pfnCoreDestroy = nullptr;

	uint32_t m_SdkVersion = NGX_SDK_VERSION_FALLBACK;
	uint32_t m_SnippetVer = 0;
	unsigned int m_ReqMinArch = 0;

	bool                 m_bInitialised = false;
	bool                 m_bResetPending = true;
	State                m_State = State::Off;
	NVSDK_NGX_Result     m_LastResult = NGX_Result_Success;
	std::wstring         m_DetailError;
	Timing               m_Timing;

	NVSDK_NGX_Parameter* m_pParams = nullptr;
	CNgxParameterStore   m_OwnParams;
	bool                 m_bParamsFromCore = false;
	NVSDK_NGX_Handle*    m_pFeature = nullptr;

	CD3D12Bridge         m_Bridge;

	CComPtr<ID3D12Resource> m_pTex12In;
	CComPtr<ID3D12Resource> m_pTex12InterpOut;
	CComPtr<ID3D11Texture2D> m_pShared11In;
	CComPtr<ID3D11Texture2D> m_pShared11InterpOut;

	GuideSlot m_GuideMVec;
	GuideSlot m_GuideDepth;

	CComPtr<ID3D11Texture2D> m_pTex11DummyMVec;
	CComPtr<ID3D12Resource>  m_pTex12DummyMVec;
	CComPtr<ID3D11Texture2D> m_pTex11DummyDepth;
	CComPtr<ID3D12Resource>  m_pTex12DummyDepth;

	UINT m_featW = 0;
	UINT m_featH = 0;

	std::wstring m_DllPath;
	std::wstring m_DataPath;
	std::wstring m_SnippetDir;

	std::deque<std::wstring> m_Log;
};

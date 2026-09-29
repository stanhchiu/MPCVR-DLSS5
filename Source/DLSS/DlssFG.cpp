/*
 * CDlssFG -- DLSS Frame Generation implementation.
 */

#include "stdafx.h"
#include <dxgi1_4.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <cstdarg>
#include <format>
#include "../Utils/Util.h"
#include "DlssFG.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "d3d12.lib")

const wchar_t CDlssFG::s_SnippetName[] = L"nvngx_dlssg.dll";
const wchar_t CDlssFG::s_ShimName[]    = L"nvngx.dll";

// DLSS-G parameter keys
#define P_WIDTH                  "Width"
#define P_HEIGHT                 "Height"
#define P_COLOR                  "Color"
#define P_OUTPUT                 "Output"
#define P_OUTPUT_INTERP          "DLSSG.OutputInterpolated"
#define P_DEPTH                  "Depth"
#define P_MOTIONVECTORS          "MotionVectors"
#define P_RESET                  "Reset"
#define P_MULTIFRAMECOUNT        "DLSSG.MultiFrameCount"
#define P_MULTIFRAMEINDEX        "DLSSG.MultiFrameIndex"
#define P_DEPTHINVERTED          "DLSSG.DepthInverted"
#define P_SHOWONLYINTERP         "DLSSG.OutputDisableInterpolation"

static void __cdecl FgNgxLogCallback(const char* message, NVSDK_NGX_Logging_Level, uint32_t)
{
	if (!message) return;
	wprintf(L"    [NGX-G] %S\n", message);
	fflush(stdout);
}

CDlssFG::~CDlssFG()
{
	Shutdown();
}

void CDlssFG::Log(const wchar_t* fmt, ...)
{
	wchar_t buf[1024];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s(buf, std::size(buf), _TRUNCATE, fmt, ap);
	va_end(ap);

	wprintf(L"    [CDlssFG] %s\n", buf);
	fflush(stdout);

	DLog(L"CDlssFG: {}", buf);
	m_Log.emplace_back(buf);
	while (m_Log.size() > 32) {
		m_Log.pop_front();
	}
}

static std::wstring DirOf(const std::wstring& path)
{
	const size_t sep = path.find_last_of(L"\\/");
	return (sep == std::wstring::npos) ? std::wstring(L".") : path.substr(0, sep);
}

static std::wstring ThisModuleDir()
{
	wchar_t buf[MAX_PATH] = {};
	HMODULE hMod = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCWSTR)&ThisModuleDir, &hMod);
	GetModuleFileNameW(hMod, buf, std::size(buf));
	return DirOf(buf);
}

std::vector<std::wstring> CDlssFG::CandidateDllPaths(const wchar_t* pConfigured)
{
	std::vector<std::wstring> out;

	if (pConfigured && *pConfigured) {
		std::wstring c = pConfigured;
		if (PathIsDirectoryW(c.c_str())) {
			if (c.back() != L'\\' && c.back() != L'/') {
				c += L'\\';
			}
			c += s_SnippetName;
		}
		out.push_back(c);
	}

	const std::wstring dir = ThisModuleDir();
	out.push_back(dir + L"\\" + s_SnippetName);
	out.push_back(dir + L"\\dlss\\" + s_SnippetName);
	out.push_back(dir + L"\\..\\" + s_SnippetName);
	out.push_back(dir + L"\\..\\..\\" + s_SnippetName);
	return out;
}

bool CDlssFG::LoadCore()
{
	m_hCore = LoadLibraryW(L"_nvngx.dll");

	if (!m_hCore) {
		WIN32_FIND_DATAW fd = {};
		const std::wstring root = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\";
		HANDLE hFind = FindFirstFileW((root + L"nv_disp*").c_str(), &fd);
		if (hFind != INVALID_HANDLE_VALUE) {
			do {
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
					continue;
				}
				const std::wstring cand = root + fd.cFileName + L"\\_nvngx.dll";
				if (!PathFileExistsW(cand.c_str())) {
					continue;
				}
				m_hCore = LoadLibraryExW(cand.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
				if (m_hCore) {
					break;
				}
			} while (FindNextFileW(hFind, &fd));
			FindClose(hFind);
		}
	}

	if (!m_hCore) {
		Log(L"_nvngx.dll not found");
		return false;
	}

	m_pfnCoreAlloc   = (PFN_NGX_D3D11_AllocateParameters)GetProcAddress(m_hCore, "NVSDK_NGX_D3D12_AllocateParameters");
	m_pfnCoreDestroy = (PFN_NGX_D3D11_DestroyParameters) GetProcAddress(m_hCore, "NVSDK_NGX_D3D12_DestroyParameters");
	return true;
}

bool CDlssFG::LoadSnippet(const wchar_t* pConfiguredDllPath)
{
	for (const auto& cand : CandidateDllPaths(pConfiguredDllPath)) {
		if (!PathFileExistsW(cand.c_str())) {
			continue;
		}
		m_hSnippet = LoadLibraryExW(cand.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (m_hSnippet) {
			wchar_t full[MAX_PATH] = {};
			GetModuleFileNameW(m_hSnippet, full, std::size(full));
			m_DllPath    = full[0] ? full : cand;
			m_SnippetDir = DirOf(m_DllPath);
			Log(L"snippet: %s", m_DllPath.c_str());
			return true;
		}
		m_State = State::DllLoadFailed;
		Log(L"LoadLibrary failed for %s (err %lu)", cand.c_str(), GetLastError());
		return false;
	}

	m_State = State::DllNotFound;
	Log(L"%s not found in any candidate location", s_SnippetName);
	return false;
}

bool CDlssFG::LoadShim()
{
	const std::wstring dir = ThisModuleDir();
	const std::wstring cands[] = {
		dir + L"\\dlss\\" + s_ShimName,
		dir + L"\\" + s_ShimName,
	};

	for (const auto& c : cands) {
		if (!PathFileExistsW(c.c_str())) {
			continue;
		}
		m_hShim = LoadLibraryExW(c.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (m_hShim) {
			Log(L"shim: %s", c.c_str());
			break;
		}
	}

	if (!m_hShim) {
		m_State = State::ShimMissing;
		Log(L"%s not found", s_ShimName);
		return false;
	}

	m_pfnShimInit     = (PFN_ShimInit)     GetProcAddress(m_hShim, "shim_init");
	m_pfnShimPopulate = (PFN_ShimPopulate) GetProcAddress(m_hShim, "shim_populate");
	m_pfnShimCreate   = (PFN_ShimCreate)   GetProcAddress(m_hShim, "shim_create");
	m_pfnShimEval     = (PFN_ShimEval)     GetProcAddress(m_hShim, "shim_eval");
	m_pfnShimRelease  = (PFN_ShimRelease)  GetProcAddress(m_hShim, "shim_release");
	m_pfnShimShutdown = (PFN_ShimShutdown1)GetProcAddress(m_hShim, "shim_shutdown1");

	if (!m_pfnShimInit || !m_pfnShimPopulate || !m_pfnShimCreate || !m_pfnShimEval) {
		m_State = State::ShimMissing;
		Log(L"shim loaded but exports missing");
		FreeLibrary(m_hShim);
		m_hShim = nullptr;
		return false;
	}

	m_bUseShim = true;
	return true;
}

NVSDK_NGX_Result CDlssFG::CallPopulate(NVSDK_NGX_Parameter* p)
{
	auto pfn = (PFN_NGX_D3D12_PopulateParameters)GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
	if (!pfn) pfn = (PFN_NGX_D3D12_PopulateParameters)GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_PopulateParameters");
	if (!pfn) return NGX_Result_FAIL_NotImplemented;
	return (m_bUseShim && m_pfnShimPopulate) ? m_pfnShimPopulate((void*)pfn, p) : pfn(p);
}

NVSDK_NGX_Result CDlssFG::CallInit()
{
	auto pfn = (PFN_NGX_D3D12_Init_Ext)GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_Init_Ext");
	if (!pfn) pfn = (PFN_NGX_D3D12_Init_Ext)GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_Init");
	if (!pfn) return NGX_Result_FAIL_NotImplemented;

	NVSDK_NGX_FeatureCommonInfo fci = {};
	const wchar_t* paths[1] = { m_SnippetDir.c_str() };
	fci.PathListInfo.Path = paths;
	fci.PathListInfo.Length = 1;
	fci.LoggingInfo.LoggingCallback = FgNgxLogCallback;
	fci.LoggingInfo.MinimumLoggingLevel = NGX_LOGGING_VERBOSE;
	fci.LoggingInfo.DisableOtherLoggingSinks = false;

	NVSDK_NGX_Result r = NGX_Result_Fail;
	const uint32_t versions[] = { m_SdkVersion, 0x15, 0x14, 0x13 };
	for (uint32_t v : versions) {
		r = (m_bUseShim && m_pfnShimInit)
			? m_pfnShimInit((void*)pfn, NGX_DLSSNR_APPID, m_DataPath.c_str(), m_Bridge.GetDev12(), v, nullptr)
			: pfn(NGX_DLSSNR_APPID, m_DataPath.c_str(), m_Bridge.GetDev12(), v, nullptr);
		Log(L"CallInit (0x%02X): 0x%08X %s", v, r, NgxResultName(r));
		if (r != NGX_Result_FAIL_OutOfDate) {
			m_SdkVersion = v;
			break;
		}
	}
	return r;
}

NVSDK_NGX_Result CDlssFG::CallCreate(NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out)
{
	if (!m_pfnCreate12) return NGX_Result_FAIL_NotImplemented;
	return (m_bUseShim && m_pfnShimCreate)
		? m_pfnShimCreate(m_pfnCreate12, m_Bridge.GetList(), NGX_FEATURE_INTERPOLATION, p, (void**)out)
		: ((PFN_NGX_D3D12_CreateFeature)m_pfnCreate12)(m_Bridge.GetList(), NGX_FEATURE_INTERPOLATION, p, out);
}

NVSDK_NGX_Result CDlssFG::CallEvaluate(const NVSDK_NGX_Parameter* p)
{
	if (!m_pfnEval12) return NGX_Result_FAIL_NotImplemented;
	return (m_bUseShim && m_pfnShimEval)
		? m_pfnShimEval(m_pfnEval12, m_Bridge.GetList(), m_pFeature, p, nullptr)
		: ((PFN_NGX_D3D12_EvaluateFeature)m_pfnEval12)(m_Bridge.GetList(), m_pFeature, p, nullptr);
}

NVSDK_NGX_Result CDlssFG::CallRelease(NVSDK_NGX_Handle* h)
{
	if (!m_pfnRelease12 || !h) return NGX_Result_Success;
	return (m_bUseShim && m_pfnShimRelease)
		? m_pfnShimRelease(m_pfnRelease12, h)
		: ((PFN_NGX_D3D12_ReleaseFeature)m_pfnRelease12)(h);
}

NVSDK_NGX_Result CDlssFG::CallShutdown1()
{
	if (!m_pfnShutdown12) return NGX_Result_Success;
	return (m_bUseShim && m_pfnShimShutdown)
		? m_pfnShimShutdown(m_pfnShutdown12, m_Bridge.GetDev12())
		: ((PFN_NGX_D3D12_Shutdown1)m_pfnShutdown12)(m_Bridge.GetDev12());
}

static NVSDK_NGX_Result SafeReqCall(void* pReq, IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* fdi, NVSDK_NGX_FeatureRequirement* req)
{
	__try {
		typedef NVSDK_NGX_Result(__cdecl* PFN_Req)(IDXGIAdapter*, const NVSDK_NGX_FeatureDiscoveryInfo*, NVSDK_NGX_FeatureRequirement*);
		return ((PFN_Req)pReq)(adapter, fdi, req);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return NGX_Result_Fail;
	}
}

void CDlssFG::QueryRequirements()
{
	void* pReq = GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_GetFeatureRequirements");
	if (!pReq) {
		pReq = GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D11_GetFeatureRequirements");
	}
	if (!pReq) return;

	CComPtr<IDXGIDevice> dxgiDev;
	CComPtr<IDXGIAdapter> adapter;
	if (SUCCEEDED(m_Bridge.GetDev11()->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
		dxgiDev->GetAdapter(&adapter);
	}
	if (!adapter) return;

	NVSDK_NGX_FeatureCommonInfo fci = {};
	const wchar_t* paths[1] = { m_SnippetDir.c_str() };
	fci.PathListInfo.Path = paths;
	fci.PathListInfo.Length = 1;

	NVSDK_NGX_FeatureDiscoveryInfo fdi = {};
	fdi.SDKVersion = m_SdkVersion;
	fdi.FeatureID  = NGX_FEATURE_INTERPOLATION;
	fdi.Identifier.IdentifierType = NGX_AppIdType_Application;
	fdi.Identifier.v.ApplicationId = NGX_DLSSNR_APPID;
	fdi.ApplicationDataPath = m_DataPath.c_str();
	fdi.FeatureInfo = &fci;

	NVSDK_NGX_FeatureRequirement req = {};
	const NVSDK_NGX_Result r = SafeReqCall(pReq, adapter, &fdi, &req);
	if (NGX_SUCCEED(r)) {
		m_ReqMinArch = req.MinHWArchitecture;
		Log(L"requirements: support 0x%X, min arch 0x%X", req.FeatureSupported, req.MinHWArchitecture);
	}
}

bool CDlssFG::Init(ID3D11Device* pDevice, const wchar_t* pConfiguredDllPath)
{
	if (m_bInitialised) return true;

	wchar_t appData[MAX_PATH] = {};
	if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData))) {
		m_DataPath = std::wstring(appData) + L"\\MPC-BE Filters\\MPC Video Renderer\\ngx\\";
		SHCreateDirectoryExW(nullptr, m_DataPath.c_str(), nullptr);
	}

	if (!m_Bridge.Init(pDevice)) {
		m_State = State::NoD3D12;
		Log(L"could not initialize D3D12 interop bridge");
		return false;
	}

	if (!LoadSnippet(pConfiguredDllPath)) {
		Shutdown();
		return false;
	}

	m_pfnCreate12   = GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_CreateFeature");
	m_pfnEval12     = GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_EvaluateFeature");
	m_pfnRelease12  = GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_ReleaseFeature");
	m_pfnShutdown12 = GetProcAddress(m_hSnippet, "NVSDK_NGX_D3D12_Shutdown1");

	if (!m_pfnCreate12 || !m_pfnEval12) {
		m_State = State::ExportsMissing;
		Log(L"required D3D12 exports missing from %s", s_SnippetName);
		Shutdown();
		return false;
	}

	if (auto pfnApiVer = (PFN_NGX_GetU32)GetProcAddress(m_hSnippet, "NVSDK_NGX_GetAPIVersion")) {
		const uint32_t v = pfnApiVer();
		if (v >= 0x10 && v <= 0x20) {
			m_SdkVersion = v;
		}
	}
	if (auto pfnSnipVer = (PFN_NGX_GetU32)GetProcAddress(m_hSnippet, "NVSDK_NGX_GetSnippetVersion")) {
		m_SnippetVer = pfnSnipVer();
	}
	Log(L"snippet 0x%08X, SDK version 0x%02X", m_SnippetVer, m_SdkVersion);

	if (!LoadShim()) {
		Shutdown();
		return false;
	}

	NVSDK_NGX_FeatureCommonInfo fci = {};
	const wchar_t* pathList[1] = { m_SnippetDir.c_str() };
	fci.PathListInfo.Path = pathList;
	fci.PathListInfo.Length = 1;
	fci.LoggingInfo.LoggingCallback = FgNgxLogCallback;
	fci.LoggingInfo.MinimumLoggingLevel = NGX_LOGGING_VERBOSE;
	fci.LoggingInfo.DisableOtherLoggingSinks = false;

	if (LoadCore()) {
		typedef NVSDK_NGX_Result(__cdecl* PFN_CoreInit12Ext)(unsigned long long, const wchar_t*, ID3D12Device*, uint32_t, const void*);
		typedef NVSDK_NGX_Result(__cdecl* PFN_CoreInit12)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, uint32_t);
		auto pfnCoreInitExt = (PFN_CoreInit12Ext)GetProcAddress(m_hCore, "NVSDK_NGX_D3D12_Init_Ext");
		auto pfnCoreInit = (PFN_CoreInit12)GetProcAddress(m_hCore, "NVSDK_NGX_D3D12_Init");

		NVSDK_NGX_Result rc = NGX_Result_Fail;
		constexpr uint32_t kSdkVersions[] = { 0x15, 0x14, 0x13 };
		for (uint32_t v : kSdkVersions) {
			if (pfnCoreInitExt) {
				rc = pfnCoreInitExt(NGX_DLSSNR_APPID, m_DataPath.c_str(), m_Bridge.GetDev12(), v, &fci);
				Log(L"core Init_Ext (0x%02X): 0x%08X %s", v, rc, NgxResultName(rc));
				if (NGX_SUCCEED(rc)) {
					m_SdkVersion = v;
					break;
				}
			}
			if (pfnCoreInit) {
				rc = pfnCoreInit(NGX_DLSSNR_APPID, m_DataPath.c_str(), m_Bridge.GetDev12(), &fci, v);
				Log(L"core Init (0x%02X): 0x%08X %s", v, rc, NgxResultName(rc));
				if (NGX_SUCCEED(rc)) {
					m_SdkVersion = v;
					break;
				}
			}
		}
		if (m_pfnCoreAlloc) {
			NVSDK_NGX_Parameter* p = nullptr;
			if (NGX_SUCCEED(m_pfnCoreAlloc(&p)) && p) {
				m_pParams = p;
				m_bParamsFromCore = true;
			}
		}
	}

	if (!m_pParams) {
		m_pParams = &m_OwnParams;
		m_bParamsFromCore = false;
		Log(L"using built-in parameter block");
	}

	QueryRequirements();

	const NVSDK_NGX_Result r = CallInit();
	m_LastResult = r;
	if (NGX_FAILED(r)) {
		m_State = State::ApiInitFailed;
		Log(L"Init failed: 0x%08X %s", r, NgxResultName(r));
		Shutdown();
		return false;
	}

	CallPopulate(m_pParams);

	m_bInitialised = true;
	m_bResetPending = true;
	m_State = State::Ready;
	Log(L"initialised");
	return true;
}

void CDlssFG::Shutdown()
{
	m_Bridge.DrainGpu();
	ReleaseFeature();

	if (m_bInitialised) {
		CallShutdown1();
	}
	m_bInitialised = false;

	if (m_pParams && m_bParamsFromCore && m_pfnCoreDestroy) {
		m_pfnCoreDestroy(m_pParams);
	}
	m_pParams = nullptr;
	m_bParamsFromCore = false;
	m_OwnParams.Reset();

	m_pTex12In.Release();
	m_pTex12InterpOut.Release();
	m_GuideMVec = {};
	m_GuideDepth = {};

	m_Bridge.Shutdown();

	if (m_hShim) {
		FreeLibrary(m_hShim);
		m_hShim = nullptr;
	}
	if (m_hSnippet) {
		FreeLibrary(m_hSnippet);
		m_hSnippet = nullptr;
	}
	if (m_hCore) {
		FreeLibrary(m_hCore);
		m_hCore = nullptr;
	}

	m_State = State::Off;
}

bool CDlssFG::SetGuideSlot(GuideSlot& slot, const wchar_t* which, ID3D11Texture2D* pTex11)
{
	if (!pTex11) {
		slot = {};
		return true;
	}
	if (slot.p11 == pTex11 && slot.p12) {
		return true;
	}

	D3D11_TEXTURE2D_DESC d = {};
	pTex11->GetDesc(&d);

	slot = {};
	if (!m_Bridge.OpenOnD3D12(which, pTex11, slot.p12, m_DetailError)) {
		Log(L"%s", m_DetailError.c_str());
		return false;
	}
	slot.p11 = pTex11;
	slot.w = d.Width;
	slot.h = d.Height;
	return true;
}

bool CDlssFG::SetGuides(ID3D11Texture2D* pMVec, ID3D11Texture2D* pDepth)
{
	bool ok = true;
	ok = SetGuideSlot(m_GuideMVec, L"MotionVectors", pMVec) && ok;
	ok = SetGuideSlot(m_GuideDepth, L"Depth", pDepth) && ok;
	return ok;
}

bool CDlssFG::CreateFeature(ID3D11Texture2D* pShared11In, ID3D11Texture2D* pShared11InterpOut,
                            UINT w, UINT h)
{
	if (!m_bInitialised) return false;

	if (m_pFeature && MatchesFeature(w, h) && MatchesTextures(pShared11In, pShared11InterpOut)) {
		return true;
	}

	ReleaseFeature();

	m_pShared11In = pShared11In;
	m_pShared11InterpOut = pShared11InterpOut;

	if (!m_Bridge.OpenOnD3D12(L"input", pShared11In, m_pTex12In, m_DetailError) ||
	    !m_Bridge.OpenOnD3D12(L"output", pShared11InterpOut, m_pTex12InterpOut, m_DetailError)) {
		m_State = State::ShareFailed;
		Log(L"CreateFeature failed: %s", m_DetailError.c_str());
		ReleaseFeature();
		return false;
	}

	D3D11_TEXTURE2D_DESC descIn = {};
	pShared11In->GetDesc(&descIn);

	m_pParams->Set(P_WIDTH,  (unsigned int)w);
	m_pParams->Set(P_HEIGHT, (unsigned int)h);
	m_pParams->Set("DLSSG.Width",  (unsigned int)w);
	m_pParams->Set("DLSSG.Height", (unsigned int)h);
	m_pParams->Set("DLSSG.InternalWidth",  (unsigned int)w);
	m_pParams->Set("DLSSG.InternalHeight", (unsigned int)h);
	m_pParams->Set("DLSSG.BackbufferFormat", (int)descIn.Format);
	m_pParams->Set("CreationNodeMask", (unsigned int)1);
	m_pParams->Set("VisibilityNodeMask", (unsigned int)1);
	m_pParams->Set("DLSSG.MultiFrameCountMax", (int)3); // allows up to 4x

	// Ensure MVec and Depth guide textures exist
	if (!m_GuideMVec.p12 && (!m_pTex12DummyMVec || m_featW != w || m_featH != h)) {
		m_pTex11DummyMVec.Release();
		m_pTex12DummyMVec.Release();
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R16G16_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
		if (SUCCEEDED(m_Bridge.GetDev11()->CreateTexture2D(&td, nullptr, &m_pTex11DummyMVec))) {
			m_Bridge.OpenOnD3D12(L"dummy_mvec", m_pTex11DummyMVec, m_pTex12DummyMVec, m_DetailError);
		}
	}
	if (!m_GuideDepth.p12 && (!m_pTex12DummyDepth || m_featW != w || m_featH != h)) {
		m_pTex11DummyDepth.Release();
		m_pTex12DummyDepth.Release();
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
		if (SUCCEEDED(m_Bridge.GetDev11()->CreateTexture2D(&td, nullptr, &m_pTex11DummyDepth))) {
			m_Bridge.OpenOnD3D12(L"dummy_depth", m_pTex11DummyDepth, m_pTex12DummyDepth, m_DetailError);
		}
	}

	NVSDK_NGX_Handle* hFeature = nullptr;
	const NVSDK_NGX_Result r = CallCreate(m_pParams, &hFeature);
	m_LastResult = r;

	if (NGX_FAILED(r) || !hFeature) {
		m_State = State::FeatureCreateFailed;
		Log(L"CreateFeature failed: 0x%08X %s", r, NgxResultName(r));
		m_Bridge.ExecuteAndWait();
		ReleaseFeature();
		return false;
	}

	if (!m_Bridge.ExecuteAndWait()) {
		m_State = State::FeatureCreateFailed;
		Log(L"could not submit CreateFeature work");
		ReleaseFeature();
		return false;
	}

	m_pFeature = hFeature;
	m_featW = w;
	m_featH = h;
	m_bResetPending = true;
	m_State = State::Ready;
	Log(L"feature created %ux%u", w, h);
	return true;
}

void CDlssFG::ReleaseFeature()
{
	if (m_pFeature) {
		m_Bridge.DrainGpu();
		CallRelease(m_pFeature);
		m_Bridge.ExecuteAndWait();
		m_pFeature = nullptr;
		Log(L"feature released");
	}
	m_pTex12In.Release();
	m_pTex12InterpOut.Release();
	m_pTex11DummyMVec.Release();
	m_pTex12DummyMVec.Release();
	m_pTex11DummyDepth.Release();
	m_pTex12DummyDepth.Release();
	m_pShared11In = nullptr;
	m_pShared11InterpOut = nullptr;
	m_featW = 0;
	m_featH = 0;
}

void CDlssFG::PushEvaluateParams(NVSDK_NGX_Parameter* p, const Params& s, bool bReset)
{
	const UINT w = m_featW, h = m_featH;

	p->Set(P_WIDTH,  (unsigned int)w);
	p->Set(P_HEIGHT, (unsigned int)h);
	p->Set("DLSSG.Width",  (unsigned int)w);
	p->Set("DLSSG.Height", (unsigned int)h);
	p->Set("DLSSG.Reset", (int)(bReset ? 1 : 0));
	p->Set(P_RESET, (int)(bReset ? 1 : 0));
	p->Set("DLSSG.MultiFrameCount", (int)(s.iMultiplier > 1 ? s.iMultiplier - 1 : 1));
	p->Set("DLSSG.MultiFrameIndex", (int)s.iIndex);
	p->Set("DLSSG.DepthInverted", (int)1);
	p->Set("DLSSG.MvecScaleX", (float)1.0f);
	p->Set("DLSSG.MvecScaleY", (float)1.0f);

	static const float s_IdentityMatrix[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f,
	};
	p->Set("DLSSG.ClipToPrevClip", (void*)s_IdentityMatrix);
	p->Set("DLSSG.PrevClipToClip", (void*)s_IdentityMatrix);

	p->Set("DLSSG.Backbuffer",        (ID3D12Resource*)m_pTex12In);
	p->Set("DLSSG.OutputInterpolated",(ID3D12Resource*)m_pTex12InterpOut);
	p->Set(P_COLOR,                   (ID3D12Resource*)m_pTex12In);
	p->Set(P_OUTPUT,                  (ID3D12Resource*)m_pTex12InterpOut);

	ID3D12Resource* pMVec12 = m_GuideMVec.p12 ? m_GuideMVec.p12.p : m_pTex12DummyMVec.p;
	ID3D12Resource* pDepth12 = m_GuideDepth.p12 ? m_GuideDepth.p12.p : m_pTex12DummyDepth.p;

	if (pMVec12) {
		p->Set("DLSSG.MVecs", pMVec12);
		p->Set(P_MOTIONVECTORS, pMVec12);
	}
	if (pDepth12) {
		p->Set("DLSSG.Depth", pDepth12);
		p->Set(P_DEPTH, pDepth12);
	}

	static const char* const prefixes[] = {
		"DLSSG.InputBackbufferSubrect",
		"DLSSG.BackbufferSubrect",
		"DLSSG.OutputInterpolatedSubrect",
		"DLSSG.DepthSubrect",
		"DLSSG.MVecsSubrect"
	};
	for (const char* pre : prefixes) {
		char name[64];
		sprintf_s(name, "%sBaseX", pre);  p->Set(name, (unsigned int)0);
		sprintf_s(name, "%sBaseY", pre);  p->Set(name, (unsigned int)0);
		sprintf_s(name, "%sWidth", pre);  p->Set(name, (unsigned int)w);
		sprintf_s(name, "%sHeight", pre); p->Set(name, (unsigned int)h);
	}
}

bool CDlssFG::Evaluate(const Params& p)
{
	if (!m_bInitialised || !m_pFeature || !m_pTex12In || !m_pTex12InterpOut) {
		return false;
	}

	if (m_Bridge.CheckDeviceLost(m_DetailError)) {
		Log(L"%s", m_DetailError.c_str());
		m_State = State::DeviceLost;
		return false;
	}

	if (!m_Bridge.SyncD3D11ToD3D12()) {
		Log(L"input never became ready");
		return false;
	}

	PushEvaluateParams(m_pParams, p, m_bResetPending || p.bReset);
	const NVSDK_NGX_Result r = CallEvaluate(m_pParams);

	if (NGX_FAILED(r)) {
		m_State = State::EvaluateFailed;
		Log(L"Evaluate failed: 0x%08X %s", r, NgxResultName(r));
		m_Bridge.ExecuteAndWait();
		return false;
	}

	if (!m_Bridge.ExecuteAndWait()) {
		Log(L"could not submit Evaluate work");
		return false;
	}

	m_bResetPending = false;
	return true;
}

const wchar_t* CDlssFG::GetStateName() const
{
	switch (m_State) {
	case State::Off:                 return L"off";
	case State::DllNotFound:         return L"DLL not found";
	case State::DllLoadFailed:       return L"DLL load failed";
	case State::ExportsMissing:      return L"exports missing";
	case State::ShimMissing:         return L"nvngx.dll shim missing";
	case State::NoD3D12:             return L"Direct3D 12 device creation failed";
	case State::ApiInitFailed:       return L"initialisation failed";
	case State::ShareFailed:         return L"texture sharing failed";
	case State::FeatureCreateFailed: return L"feature creation failed";
	case State::EvaluateFailed:      return L"execution failed";
	case State::DeviceLost:          return L"device lost";
	case State::Ready:               return L"ready";
	default:                         return L"unknown";
	}
}

std::wstring CDlssFG::GetStatusLine() const
{
	if (m_State == State::Ready && m_pFeature) {
		return std::format(L"ready {}x{} 2x", m_featW, m_featH);
	}
	return GetStateName();
}

std::wstring CDlssFG::GetInfoBlock() const
{
	std::wstring out = L"DLSS Frame Generation (Interpolation):\n";
	out += std::format(L"  DLL       : {}\n", m_DllPath.empty() ? L"(none)" : m_DllPath.c_str());
	out += std::format(L"  Snippet   : 0x{:08X}, SDK version 0x{:02X}\n", m_SnippetVer, m_SdkVersion);
	out += std::format(L"  State     : {}\n", GetStatusLine().c_str());
	if (!m_Log.empty()) {
		out += L"  Log       :\n";
		size_t skip = m_Log.size() > 16 ? m_Log.size() - 16 : 0;
		for (const auto& line : m_Log) {
			if (skip) { skip--; continue; }
			out += L"    " + line + L"\n";
		}
	}
	return out;
}

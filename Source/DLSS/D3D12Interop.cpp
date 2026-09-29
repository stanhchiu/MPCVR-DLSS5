#include "stdafx.h"
#include <dxgi1_4.h>
#include <format>
#include "../Utils/Util.h"
#include "D3D12Interop.h"

#pragma comment(lib, "d3d12.lib")

const wchar_t* NgxResultName(NVSDK_NGX_Result r)
{
	switch (r) {
	case NGX_Result_Success:                        return L"Success";
	case NGX_Result_Fail:                           return L"Fail";
	case NGX_Result_FAIL_FeatureNotSupported:       return L"FeatureNotSupported";
	case NGX_Result_FAIL_PlatformError:             return L"PlatformError";
	case NGX_Result_FAIL_FeatureAlreadyExists:      return L"FeatureAlreadyExists";
	case NGX_Result_FAIL_FeatureNotFound:           return L"FeatureNotFound";
	case NGX_Result_FAIL_InvalidParameter:          return L"InvalidParameter";
	case NGX_Result_FAIL_ScratchBufferTooSmall:     return L"ScratchBufferTooSmall";
	case NGX_Result_FAIL_NotInitialized:            return L"NotInitialized";
	case NGX_Result_FAIL_UnsupportedInputFormat:    return L"UnsupportedInputFormat";
	case NGX_Result_FAIL_RWFlagMissing:             return L"RWFlagMissing";
	case NGX_Result_FAIL_MissingInput:              return L"MissingInput";
	case NGX_Result_FAIL_UnableToInitializeFeature: return L"UnableToInitializeFeature";
	case NGX_Result_FAIL_OutOfDate:                 return L"OutOfDate";
	case NGX_Result_FAIL_OutOfGPUMemory:            return L"OutOfGPUMemory";
	case NGX_Result_FAIL_UnsupportedFormat:         return L"UnsupportedFormat";
	case NGX_Result_FAIL_UnableToWriteToAppDataPath:return L"UnableToWriteToAppDataPath";
	case NGX_Result_FAIL_UnsupportedParameter:      return L"UnsupportedParameter";
	case NGX_Result_FAIL_Denied:                    return L"Denied";
	case NGX_Result_FAIL_NotImplemented:            return L"NotImplemented";
	}
	return L"Unknown";
}

// ============================================================================
// CNgxParameterStore Implementation
// ============================================================================

CNgxParameterStore::Slot& CNgxParameterStore::Put(const char* n)
{
	return m_map[n ? n : ""];
}

const CNgxParameterStore::Slot* CNgxParameterStore::Find(const char* n) const
{
	auto it = m_map.find(n ? n : "");
	return (it == m_map.end()) ? nullptr : &it->second;
}

void CNgxParameterStore::Set(const char* n, unsigned long long v) { auto& s = Put(n); s.kind = Kind::U64;   s.u64 = v; s.f64 = (double)v; }
void CNgxParameterStore::Set(const char* n, float v)              { auto& s = Put(n); s.kind = Kind::F32;   s.f64 = v; s.u64 = (uint64_t)v; }
void CNgxParameterStore::Set(const char* n, double v)             { auto& s = Put(n); s.kind = Kind::F64;   s.f64 = v; s.u64 = (uint64_t)v; }
void CNgxParameterStore::Set(const char* n, unsigned int v)       { auto& s = Put(n); s.kind = Kind::U32;   s.u64 = v; s.f64 = (double)v; }
void CNgxParameterStore::Set(const char* n, int v)                { auto& s = Put(n); s.kind = Kind::I32;   s.u64 = (uint64_t)(int64_t)v; s.f64 = (double)v; }
void CNgxParameterStore::Set(const char* n, ID3D11Resource* v)    { auto& s = Put(n); s.kind = Kind::Res11; s.ptr = v; }
void CNgxParameterStore::Set(const char* n, ID3D12Resource* v)    { auto& s = Put(n); s.kind = Kind::Res12; s.ptr = v; }
void CNgxParameterStore::Set(const char* n, void* v)              { auto& s = Put(n); s.kind = Kind::Ptr;   s.ptr = v; }

#define STORE_GET(expr)                                        \
	if (!o) return NGX_Result_FAIL_InvalidParameter;           \
	const Slot* v = Find(n);                                   \
	if (!v) return NGX_Result_FAIL_FeatureNotFound;            \
	*o = (expr);                                               \
	return NGX_Result_Success;

NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, unsigned long long* o) const { STORE_GET(v->u64) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, float* o) const              { STORE_GET((float)v->f64) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, double* o) const             { STORE_GET(v->f64) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, unsigned int* o) const       { STORE_GET((unsigned int)v->u64) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, int* o) const                { STORE_GET((int)v->u64) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, ID3D11Resource** o) const    { STORE_GET((ID3D11Resource*)v->ptr) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, ID3D12Resource** o) const    { STORE_GET((ID3D12Resource*)v->ptr) }
NVSDK_NGX_Result CNgxParameterStore::Get(const char* n, void** o) const              { STORE_GET(v->ptr) }

#undef STORE_GET

void CNgxParameterStore::Reset() { m_map.clear(); }

// ============================================================================
// CD3D12Bridge Implementation
// ============================================================================

CD3D12Bridge::~CD3D12Bridge()
{
	Shutdown();
}

bool CD3D12Bridge::Init(ID3D11Device* pDevice)
{
	if (m_pDev12) {
		return true;
	}
	if (!pDevice) {
		return false;
	}

	// Fences need the 11.4 interfaces.
	if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&m_pDev11)))) {
		DLog(L"CD3D12Bridge: ID3D11Device5 unavailable; shared fences need Windows 10 1703+");
		return false;
	}
	{
		CComPtr<ID3D11DeviceContext> pCtx;
		pDevice->GetImmediateContext(&pCtx);
		if (!pCtx || FAILED(pCtx->QueryInterface(IID_PPV_ARGS(&m_pCtx11)))) {
			DLog(L"CD3D12Bridge: ID3D11DeviceContext4 unavailable");
			return false;
		}
	}

	// Same physical adapter as the renderer, matched by LUID -- a shared handle
	// cannot cross adapters.
	CComPtr<IDXGIDevice> pDXGIDevice;
	CComPtr<IDXGIAdapter> pAdapter;
	if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pDXGIDevice)))
			|| FAILED(pDXGIDevice->GetAdapter(&pAdapter))) {
		DLog(L"CD3D12Bridge: could not reach the renderer's DXGI adapter");
		return false;
	}
	DXGI_ADAPTER_DESC desc = {};
	pAdapter->GetDesc(&desc);

	CComPtr<IDXGIFactory4> pFactory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&pFactory)))) {
		DLog(L"CD3D12Bridge: CreateDXGIFactory1 failed");
		return false;
	}
	CComPtr<IDXGIAdapter1> pAdapter1;
	if (FAILED(pFactory->EnumAdapterByLuid(desc.AdapterLuid, IID_PPV_ARGS(&pAdapter1)))) {
		DLog(L"CD3D12Bridge: EnumAdapterByLuid failed");
		return false;
	}

	HRESULT hr = D3D12CreateDevice(pAdapter1, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_pDev12));
	if (FAILED(hr)) {
		DLog(L"CD3D12Bridge: D3D12CreateDevice failed 0x{:08X}", (unsigned)hr);
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(m_pDev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_pQueue)))
			|| FAILED(m_pDev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_pAlloc)))
			|| FAILED(m_pDev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pAlloc, nullptr, IID_PPV_ARGS(&m_pList)))) {
		DLog(L"CD3D12Bridge: could not build the D3D12 command objects");
		return false;
	}

	// Two shared fences, one per direction.
	if (FAILED(m_pDev11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_pFenceUp11)))) {
		DLog(L"CD3D12Bridge: ID3D11Device5::CreateFence failed");
		return false;
	}
	HANDLE hUp = nullptr;
	if (FAILED(m_pFenceUp11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hUp)) || !hUp) {
		DLog(L"CD3D12Bridge: could not share the D3D11 fence");
		return false;
	}
	hr = m_pDev12->OpenSharedHandle(hUp, IID_PPV_ARGS(&m_pFenceUp12));
	CloseHandle(hUp);
	if (FAILED(hr)) {
		DLog(L"CD3D12Bridge: OpenSharedHandle(fence) failed 0x{:08X}", (unsigned)hr);
		return false;
	}

	if (FAILED(m_pDev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_pFenceDown12)))) {
		DLog(L"CD3D12Bridge: ID3D12Device::CreateFence failed");
		return false;
	}
	HANDLE hDown = nullptr;
	if (FAILED(m_pDev12->CreateSharedHandle(m_pFenceDown12, nullptr, GENERIC_ALL, nullptr, &hDown)) || !hDown) {
		DLog(L"CD3D12Bridge: could not share the D3D12 fence");
		return false;
	}
	hr = m_pDev11->OpenSharedFence(hDown, IID_PPV_ARGS(&m_pFenceDown11));
	CloseHandle(hDown);
	if (FAILED(hr)) {
		DLog(L"CD3D12Bridge: OpenSharedFence failed 0x{:08X}", (unsigned)hr);
		return false;
	}

	DLog(L"CD3D12Bridge: private D3D12 device on {}", desc.Description);
	return true;
}

void CD3D12Bridge::Shutdown()
{
	DrainGpu();

	m_pFenceUp11.Release();
	m_pFenceUp12.Release();
	m_pFenceDown12.Release();
	m_pFenceDown11.Release();
	m_pList.Release();
	m_pAlloc.Release();
	m_pQueue.Release();
	m_pDev12.Release();
	m_pCtx11.Release();
	m_pDev11.Release();

	if (m_hFenceEvent) {
		CloseHandle(m_hFenceEvent);
		m_hFenceEvent = nullptr;
	}
	m_FenceValue = 0;
}

void CD3D12Bridge::DrainGpu()
{
	if (m_pQueue && m_pDev12 && SUCCEEDED(m_pDev12->GetDeviceRemovedReason())) {
		m_FenceValue++;
		if (m_pFenceDown12 && SUCCEEDED(m_pQueue->Signal(m_pFenceDown12, m_FenceValue))) {
			WaitForFence(m_pFenceDown12, m_FenceValue);
		}
	}
	if (m_pCtx11) {
		m_pCtx11->Flush();
	}
}

bool CD3D12Bridge::ExecuteAndWait()
{
	if (!m_pList || !m_pQueue || !m_pAlloc) {
		return false;
	}
	if (FAILED(m_pList->Close())) {
		return false;
	}
	ID3D12CommandList* lists[] = { m_pList };
	m_pQueue->ExecuteCommandLists(1, lists);

	m_FenceValue++;
	if (FAILED(m_pQueue->Signal(m_pFenceDown12, m_FenceValue))) {
		return false;
	}
	if (!WaitForFence(m_pFenceDown12, m_FenceValue)) {
		return false;
	}

	if (FAILED(m_pAlloc->Reset()) || FAILED(m_pList->Reset(m_pAlloc, nullptr))) {
		return false;
	}
	return true;
}

bool CD3D12Bridge::WaitForFence(ID3D12Fence* pFence, UINT64 value)
{
	if (!pFence) {
		return false;
	}
	if (pFence->GetCompletedValue() >= value) {
		return true;
	}
	if (!m_hFenceEvent) {
		m_hFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!m_hFenceEvent) {
			return false;
		}
	}
	if (FAILED(pFence->SetEventOnCompletion(value, m_hFenceEvent))) {
		return false;
	}
	if (WaitForSingleObject(m_hFenceEvent, 2000) != WAIT_OBJECT_0) {
		DLog(L"CD3D12Bridge: GPU wait timed out");
		return false;
	}
	return true;
}

bool CD3D12Bridge::CheckDeviceLost(std::wstring& detailError)
{
	if (!m_pDev12) {
		return true;
	}
	const HRESULT hr = m_pDev12->GetDeviceRemovedReason();
	if (SUCCEEDED(hr)) {
		return false;
	}
	detailError = std::format(L"Direct3D 12 device removed (0x{:08X})", (unsigned)hr);
	DLog(L"CD3D12Bridge: {}", detailError);
	return true;
}

bool CD3D12Bridge::OpenOnD3D12(const wchar_t* which, ID3D11Texture2D* pTex11, CComPtr<ID3D12Resource>& out, std::wstring& detailError)
{
	out.Release();
	if (!pTex11 || !m_pDev12) {
		return false;
	}

	D3D11_TEXTURE2D_DESC td = {};
	pTex11->GetDesc(&td);
	if (!(td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
		detailError = std::format(L"{} texture has no share handle (misc 0x{:X})", which ? which : L"unknown", td.MiscFlags);
		DLog(L"CD3D12Bridge: {}", detailError);
		return false;
	}

	CComPtr<IDXGIResource1> pRes1;
	HRESULT hr = pTex11->QueryInterface(IID_PPV_ARGS(&pRes1));
	if (FAILED(hr)) {
		detailError = std::format(L"{}: no IDXGIResource1 (0x{:08X})", which ? which : L"unknown", (unsigned)hr);
		DLog(L"CD3D12Bridge: {}", detailError);
		return false;
	}

	HANDLE h11 = nullptr;
	hr = pRes1->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h11);
	if (FAILED(hr) || !h11) {
		detailError = std::format(L"{}: CreateSharedHandle failed (0x{:08X})", which ? which : L"unknown", (unsigned)hr);
		DLog(L"CD3D12Bridge: {}", detailError);
		return false;
	}

	hr = m_pDev12->OpenSharedHandle(h11, IID_PPV_ARGS(&out));
	CloseHandle(h11);
	if (FAILED(hr)) {
		detailError = std::format(L"{}: OpenSharedHandle failed (0x{:08X})", which ? which : L"unknown", (unsigned)hr);
		DLog(L"CD3D12Bridge: {}", detailError);
		return false;
	}
	return true;
}

bool CD3D12Bridge::SyncD3D11ToD3D12()
{
	if (!m_pCtx11 || !m_pFenceUp11 || !m_pFenceUp12) {
		return false;
	}
	m_FenceValue++;
	m_pCtx11->Signal(m_pFenceUp11, m_FenceValue);
	m_pCtx11->Flush();
	return WaitForFence(m_pFenceUp12, m_FenceValue);
}

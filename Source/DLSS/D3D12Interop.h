#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atlbase.h>
#include <string>
#include <vector>
#include <map>
#include "NGXTypes.h"

// ----------------------------------------------------------------------------
// CNgxParameterStore: fallback parameter store for NGX
// ----------------------------------------------------------------------------
class CNgxParameterStore final : public NVSDK_NGX_Parameter
{
public:
	enum class Kind : uint8_t { None, U64, F32, F64, U32, I32, Res11, Res12, Ptr };

	void Set(const char* n, unsigned long long v) override;
	void Set(const char* n, float v) override;
	void Set(const char* n, double v) override;
	void Set(const char* n, unsigned int v) override;
	void Set(const char* n, int v) override;
	void Set(const char* n, ID3D11Resource* v) override;
	void Set(const char* n, ID3D12Resource* v) override;
	void Set(const char* n, void* v) override;

	NVSDK_NGX_Result Get(const char* n, unsigned long long* o) const override;
	NVSDK_NGX_Result Get(const char* n, float* o) const override;
	NVSDK_NGX_Result Get(const char* n, double* o) const override;
	NVSDK_NGX_Result Get(const char* n, unsigned int* o) const override;
	NVSDK_NGX_Result Get(const char* n, int* o) const override;
	NVSDK_NGX_Result Get(const char* n, ID3D11Resource** o) const override;
	NVSDK_NGX_Result Get(const char* n, ID3D12Resource** o) const override;
	NVSDK_NGX_Result Get(const char* n, void** o) const override;

	void Reset() override;
	size_t Count() const { return m_map.size(); }

private:
	struct Slot { Kind kind = Kind::None; uint64_t u64 = 0; double f64 = 0; void* ptr = nullptr; };
	Slot& Put(const char* n);
	const Slot* Find(const char* n) const;

	std::map<std::string, Slot> m_map;
};

// ----------------------------------------------------------------------------
// CD3D12Bridge: Manages private D3D12 device, command objects, shared fences,
// and shared NT texture handles between D3D11 and D3D12.
// ----------------------------------------------------------------------------
class CD3D12Bridge
{
public:
	CD3D12Bridge() = default;
	~CD3D12Bridge();
	CD3D12Bridge(const CD3D12Bridge&) = delete;
	CD3D12Bridge& operator=(const CD3D12Bridge&) = delete;

	// Initializes the bridge on the same physical adapter as pDevice.
	bool Init(ID3D11Device* pDevice);
	void Shutdown();

	bool IsReady() const { return m_pDev12 != nullptr; }

	// Texture sharing: Opens a D3D11 texture with D3D11_RESOURCE_MISC_SHARED_NTHANDLE on D3D12.
	bool OpenOnD3D12(const wchar_t* which, ID3D11Texture2D* pTex11, CComPtr<ID3D12Resource>& out, std::wstring& detailError);

	// Execution & Synchronization helpers
	bool ExecuteAndWait();
	void DrainGpu();
	bool WaitForFence(ID3D12Fence* pFence, UINT64 value);
	bool CheckDeviceLost(std::wstring& detailError);

	// Signals D3D11 queue and waits on CPU until D3D12 side can read inputs
	bool SyncD3D11ToD3D12();

	// Direct accessor to devices & command objects
	ID3D11Device5*              GetDev11()   const { return m_pDev11; }
	ID3D11DeviceContext4*       GetCtx11()   const { return m_pCtx11; }
	ID3D12Device*               GetDev12()   const { return m_pDev12; }
	ID3D12CommandQueue*         GetQueue()   const { return m_pQueue; }
	ID3D12CommandAllocator*     GetAlloc()   const { return m_pAlloc; }
	ID3D12GraphicsCommandList*  GetList()    const { return m_pList; }
	ID3D12Fence*                GetFenceUp12() const { return m_pFenceUp12; }
	ID3D12Fence*                GetFenceDown12() const { return m_pFenceDown12; }

	UINT64                      GetFenceValue() const { return m_FenceValue; }
	void                        IncrementFenceValue() { m_FenceValue++; }

private:
	CComPtr<ID3D11Device5>              m_pDev11;
	CComPtr<ID3D11DeviceContext4>       m_pCtx11;

	CComPtr<ID3D12Device>              m_pDev12;
	CComPtr<ID3D12CommandQueue>        m_pQueue;
	CComPtr<ID3D12CommandAllocator>    m_pAlloc;
	CComPtr<ID3D12GraphicsCommandList> m_pList;

	// Shared fences between D3D11 and D3D12
	CComPtr<ID3D11Fence> m_pFenceUp11;     // created on D3D11, signalled there
	CComPtr<ID3D12Fence> m_pFenceUp12;     // opened on D3D12
	CComPtr<ID3D12Fence> m_pFenceDown12;   // created on D3D12, signalled there
	CComPtr<ID3D11Fence> m_pFenceDown11;   // opened on D3D11
	UINT64               m_FenceValue = 0;
	HANDLE               m_hFenceEvent = nullptr;
};

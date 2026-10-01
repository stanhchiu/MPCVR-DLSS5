/*
 * (C) 2026 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-BE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "resource.h"
#include "Helper.h"
#include "DlssStabilizer.h"

namespace {

// Measured in tools/dlssnr_probe --tstab.
constexpr float kTolerance    = 0.02f;   // how far the history may stray from the current 3x3 neighbourhood
constexpr float kReprojection = 0.03f;   // input luma error where trust in a vector starts to fall
constexpr float kConsistency  = 1.0f;    // forward/backward mismatch, flow pixels, where trust starts to fall
constexpr float kCostLow      = 48.0f;   // matching cost where trust starts to fall
constexpr float kCostHigh     = 160.0f;  // and where it is gone
constexpr float kTrustedShare = 0.05f;   // fewer trusted blocks than this share: no global motion (--tsrstill)

// Optical Flow failing this many pictures in a row gives way to the detector.
constexpr int kFlowFailuresBeforeFallback = 30;

// The layout of the renderer's VERTEX.
struct QuadVertex {
	float x, y, z;
	float u, v;
};

// About 540 lines of flow whatever the video: 1.6 ms on an RTX 3050 (--tflow).

} // namespace
HRESULT CDlssStabilizer::CreateTarget(ID3D11Device* pDevice, UINT width, UINT height, DXGI_FORMAT format, Target& target)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width            = width;
	desc.Height           = height;
	desc.MipLevels        = 1;
	desc.ArraySize        = 1;
	desc.Format           = format;
	desc.SampleDesc.Count = 1;
	desc.Usage            = D3D11_USAGE_DEFAULT;
	desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	target = Target{};
	HRESULT hr = pDevice->CreateTexture2D(&desc, nullptr, &target.pTexture);
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateShaderResourceView(target.pTexture, nullptr, &target.pShaderResource);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateRenderTargetView(target.pTexture, nullptr, &target.pRenderTarget);
	}
	return hr;
}

bool CDlssStabilizer::Matches(UINT width, UINT height) const
{
	return m_width == width && m_height == height && m_pPSStabilize;
}

HRESULT CDlssStabilizer::CreateResources(ID3D11Device* pDevice, UINT width, UINT height)
{
	HRESULT hr = S_OK;
	const struct { UINT resid; ID3D11PixelShader** ppShader; } shaders[] = {
		{ IDF_PS_11_DLSS_STAB_STABILIZE,  &m_pPSStabilize  },
	};
	for (const auto& s : shaders) {
		LPVOID data = nullptr;
		DWORD size = 0;
		hr = GetDataFromResource(data, size, s.resid);
		if (SUCCEEDED(hr)) {
			hr = pDevice->CreatePixelShader(data, size, nullptr, s.ppShader);
		}
		if (FAILED(hr)) {
			DLog(L"CDlssStabilizer::Create() : shader {} failed with error {}", s.resid, HR2Str(hr));
			return hr;
		}
	}

	D3D11_BUFFER_DESC bufferDesc = { 32 * sizeof(float), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	hr = pDevice->CreateBuffer(&bufferDesc, nullptr, &m_pConstants);
	if (SUCCEEDED(hr)) {
		static const QuadVertex quad[4] = {
			{ -1, -1, 0,  0, 1 },
			{ -1, +1, 0,  0, 0 },
			{ +1, -1, 0,  1, 1 },
			{ +1, +1, 0,  1, 0 },
		};
		bufferDesc = { sizeof(quad), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
		const D3D11_SUBRESOURCE_DATA init = { quad, 0, 0 };
		hr = pDevice->CreateBuffer(&bufferDesc, &init, &m_pQuad);
	}
	for (int i = 0; i < 2 && SUCCEEDED(hr); i++) {
		hr = CreateTarget(pDevice, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_History[i]);
		if (SUCCEEDED(hr)) {
			hr = CreateTarget(pDevice, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, m_Input[i]);
		}
	}
	if (SUCCEEDED(hr)) {
		hr = m_TexResult.CheckCreate(pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, Tex2D_DefaultShaderRTarget);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateRenderTargetView(m_TexResult.pTexture, nullptr, &m_pResultTarget);
	}
	return hr;
}



HRESULT CDlssStabilizer::Create(ID3D11Device* pDevice, UINT width, UINT height,
	ID3D11InputLayout* pInputLayout, ID3D11VertexShader* pVertexShader,
	ID3D11SamplerState* pSamplerPoint, ID3D11SamplerState* pSamplerLinear)
{
	CheckPointer(pDevice, E_POINTER);
	if (Matches(width, height)) {
		return S_OK;
	}
	// What failed once fails again: no new attempt on every picture until the size
	// or the source changes, or the owner calls Release.
	if (width == m_failedWidth && height == m_failedHeight) {
		return E_FAIL;
	}
	Release();
	if (width < 16 || height < 16) {
		return E_INVALIDARG;
	}

	HRESULT hr = CreateResources(pDevice, width, height);
	if (FAILED(hr)) {
		DLog(L"CDlssStabilizer::Create() : {}x{} failed with error {}", width, height, HR2Str(hr));
		Release();
		m_failedWidth  = width;
		m_failedHeight = height;
		return hr;
	}

	m_pDevice        = pDevice;
	m_pInputLayout   = pInputLayout;
	m_pVertexShader  = pVertexShader;
	m_pSamplerPoint  = pSamplerPoint;
	m_pSamplerLinear = pSamplerLinear;
	m_width          = width;
	m_height         = height;
	Reset();
	return S_OK;
}

void CDlssStabilizer::Release()
{
	m_pPSStabilize.Release();
	m_pConstants.Release();
	m_pQuad.Release();
	m_History[0] = Target{};
	m_History[1] = Target{};
	m_Input[0] = Target{};
	m_Input[1] = Target{};
	m_pResultTarget.Release();
	m_TexResult.Release();
	m_pDevice.Release();
	m_pInputLayout   = nullptr;
	m_pVertexShader  = nullptr;
	m_pSamplerPoint  = nullptr;
	m_pSamplerLinear = nullptr;
	m_width  = 0;
	m_height = 0;
	m_iHistory      = 0;
	m_iInput        = 0;
	m_bHistoryValid = false;
	m_bHaveResult   = false;
	m_bLastReset    = true;
	m_failedWidth   = 0;
	m_failedHeight  = 0;
}



void CDlssStabilizer::Reset()
{
	m_bHistoryValid = false;
	m_bHaveResult   = false;
	m_bLastReset    = true;
}

void CDlssStabilizer::Draw(ID3D11DeviceContext* pContext, ID3D11PixelShader* pShader,
	std::initializer_list<ID3D11RenderTargetView*> targets, UINT width, UINT height,
	std::initializer_list<ID3D11ShaderResourceView*> inputs, const float* constants, size_t count)
{
	ID3D11RenderTargetView* rtvs[2] = {};
	const UINT numTargets = (UINT)std::min<size_t>(targets.size(), std::size(rtvs));
	std::copy_n(targets.begin(), numTargets, rtvs);
	ID3D11ShaderResourceView* srvs[8] = {};
	std::copy_n(inputs.begin(), std::min<size_t>(inputs.size(), std::size(srvs)), srvs);

	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (constants && count && SUCCEEDED(pContext->Map(m_pConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
		float block[32] = {};
		std::copy_n(constants, std::min<size_t>(count, std::size(block)), block);
		memcpy(mr.pData, block, sizeof(block));
		pContext->Unmap(m_pConstants, 0);
	}

	const D3D11_VIEWPORT viewport = { 0, 0, (FLOAT)width, (FLOAT)height, 0, 1 };
	const UINT stride = sizeof(QuadVertex);
	const UINT offset = 0;
	ID3D11SamplerState* samplers[2] = { m_pSamplerPoint, m_pSamplerLinear };
	ID3D11Buffer* pConstants = m_pConstants;
	ID3D11Buffer* pQuad = m_pQuad;

	pContext->IASetInputLayout(m_pInputLayout);
	pContext->OMSetRenderTargets(numTargets, rtvs, nullptr);
	pContext->RSSetViewports(1, &viewport);
	pContext->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
	pContext->VSSetShader(m_pVertexShader, nullptr, 0);
	pContext->PSSetShader(pShader, nullptr, 0);
	pContext->PSSetShaderResources(0, (UINT)std::size(srvs), srvs);
	pContext->PSSetSamplers(0, (UINT)std::size(samplers), samplers);
	pContext->PSSetConstantBuffers(0, 1, &pConstants);
	pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	pContext->IASetVertexBuffers(0, 1, &pQuad, &stride, &offset);
	pContext->Draw(4, 0);

	// Unbound, so the next pass can write what this one read.
	ID3D11ShaderResourceView* noViews[8] = {};
	pContext->PSSetShaderResources(0, (UINT)std::size(noViews), noViews);
	ID3D11RenderTargetView* noTargets[2] = {};
	pContext->OMSetRenderTargets((UINT)std::size(noTargets), noTargets, nullptr);
}

void CDlssStabilizer::Stabilize(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pInput,
	ID3D11ShaderResourceView* pNetwork, ID3D11ShaderResourceView* pMotionVectors,
	ID3D11ShaderResourceView* pConfidence,
	float strength, bool bNewPicture, bool bIsOpticalFlow)
{
	if (!m_width || !pContext || !pInput || !pNetwork) {
		return;
	}

	// A new picture reads the last history and input and writes the other slots.
	// A redraw reads what its picture read and rewrites that picture's slots, so
	// both move on with the pictures only.
	const bool bAdvance = bNewPicture || !m_bHaveResult;
	if (bAdvance) {
		m_bLastReset = !m_bHistoryValid || !pMotionVectors;
	}
	const int iRead     = bAdvance ? m_iHistory : 1 - m_iHistory;
	const int iPrevious = bAdvance ? m_iInput : 1 - m_iInput;

	const float constants[8] = {
		1.0f - 0.75f * std::clamp(strength, 0.0f, 1.0f),     // weight of the current frame where trusted
		kTolerance,
		bIsOpticalFlow ? 1.0f : 0.0f,
		bIsOpticalFlow ? kReprojection : 0.0f,
		m_bLastReset ? 1.0f : 0.0f,                           // no history for this picture
		0.0f,                                                 // EFFECT: only out - in is steadied
		0.25f,                                                // detector age map: a quarter of the size
		0.0f
	};

	Draw(pContext, m_pPSStabilize, { m_History[1 - iRead].pRenderTarget, m_pResultTarget }, m_width, m_height,
		{ pInput, pNetwork, m_History[iRead].pShaderResource, pMotionVectors, m_Input[iPrevious].pShaderResource, pConfidence },
		constants, std::size(constants));

	// This input is the previous one for the next picture's reprojection check.
	const int iStore = 1 - iPrevious;
	CComPtr<ID3D11Resource> pInputResource;
	pInput->GetResource(&pInputResource);
	if (pInputResource) {
		pContext->CopyResource(m_Input[iStore].pTexture, pInputResource);
	}

	m_iHistory = 1 - iRead;
	m_iInput = iStore;
	m_bHistoryValid = true;
	m_bHaveResult = true;
}

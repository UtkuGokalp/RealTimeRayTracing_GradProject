//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#include "DXSample.h"
#include <stdexcept>
#include <dxcapi.h>
#include <vector>
#include "nv_helpers_dx12/TopLevelASGenerator.h"

using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().
using Microsoft::WRL::ComPtr;

class D3D12HelloTriangle : public DXSample
{
public:
	D3D12HelloTriangle(UINT width, UINT height, std::wstring name);

	virtual void OnInit();
	virtual void OnUpdate();
	virtual void OnRender();
	virtual void OnDestroy();

private:
	static const UINT FrameCount = 2;

	struct Vertex
	{
		XMFLOAT3 position;
		XMFLOAT4 color;
	};

	// Pipeline objects.
	CD3DX12_VIEWPORT m_viewport;
	CD3DX12_RECT m_scissorRect;
	ComPtr<IDXGISwapChain3> m_swapChain;
	ComPtr<ID3D12Device5> m_device;
	ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
	ComPtr<ID3D12CommandAllocator> m_commandAllocator;
	ComPtr<ID3D12CommandQueue> m_commandQueue;
	ComPtr<ID3D12RootSignature> m_rootSignature;
	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12PipelineState> m_pipelineState;
	ComPtr<ID3D12GraphicsCommandList4> m_commandList;
	UINT m_rtvDescriptorSize;

	// App resources.
	ComPtr<ID3D12Resource> m_vertexBuffer;
	D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;

	// Synchronization objects.
	UINT m_frameIndex;
	HANDLE m_fenceEvent;
	ComPtr<ID3D12Fence> m_fence;
	UINT64 m_fenceValue;

	//Rendering mode flag
	bool m_raster = true;

	void LoadPipeline();
	void LoadAssets();
	void PopulateCommandList();
	void WaitForPreviousFrame();
	void CheckRaytracingSupport();

	virtual void OnKeyUp(UINT8 key);

	// #DXR
	struct AccelerationStructureBuffers
	{
		ComPtr<ID3D12Resource> pScratch;      // Scratch memory for AS builder
		ComPtr<ID3D12Resource> pResult;       // Where the AS is
		ComPtr<ID3D12Resource> pInstanceDesc; // Hold the matrices of the instances
	};

	ComPtr<ID3D12Resource> m_bottomLevelAS; // Storage for the bottom Level AS

	nv_helpers_dx12::TopLevelASGenerator m_topLevelASGenerator;
	AccelerationStructureBuffers m_topLevelASBuffers;
	std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>> m_instances;

	/// <summary>
	/// Create the acceleration structure of an instance
	/// </summary>
	/// <param name="vVertexBuffers">Pair of vertex buffers and vertex count. The vertex buffers are assumed to contain Vertex structures.</param>
	/// <returns>AccelerationStructureBuffers for TLAS</returns>
	AccelerationStructureBuffers CreateBottomLevelAS(std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vVertexBuffers);

	/// <summary>
	/// Create the main acceleration structure that holds all instances of the scene
	/// </summary>
	/// <param name="instances">Pair of BLAS and transform</param>
	void CreateTopLevelAS(const std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>> &instances);

	/// <summary>
	/// Create all acceleration structures, bottom and top
	/// </summary>
	void CreateAccelerationStructures();

	ComPtr<ID3D12RootSignature> CreateRayGenSignature();
	ComPtr<ID3D12RootSignature> CreateMissSignature();
	ComPtr<ID3D12RootSignature> CreateHitSignature();

	void CreateRaytracingPipeline();

	ComPtr<IDxcBlob> m_rayGenLibrary;
	ComPtr<IDxcBlob> m_hitLibrary;
	ComPtr<IDxcBlob> m_missLibrary;

	ComPtr<ID3D12RootSignature> m_rayGenSignature;
	ComPtr<ID3D12RootSignature> m_hitSignature;
	ComPtr<ID3D12RootSignature> m_missSignature;

	// Ray tracing pipeline state
	ComPtr<ID3D12StateObject> m_rtStateObject;

	//Ray tracing pipeline state properties, retaining the shader identifiers to use in the Shader Binding Table
	ComPtr<ID3D12StateObjectProperties> m_rtStateObjectProperties;

	void CreateRaytracingOutputBuffer();
	void CreateShaderResourceHeap();

	ComPtr<ID3D12Resource> m_outputResource;
	ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;
};

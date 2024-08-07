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

#include "stdafx.h"
#include "D3D12HelloTriangle.h"
#include "DXRHelper.h"
#include "nv_helpers_dx12/BottomLevelASGenerator.h"
#include "nv_helpers_dx12/RaytracingPipelineGenerator.h"
#include "nv_helpers_dx12/RootSignatureGenerator.h"
#include "glm/gtc/type_ptr.hpp"
#include "manipulator.h"
#include "windowsx.h"

D3D12HelloTriangle::D3D12HelloTriangle(UINT width, UINT height, std::wstring name) :
    DXSample(width, height, name),
    m_frameIndex(0),
    m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
    m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
    m_rtvDescriptorSize(0)
{
}

void D3D12HelloTriangle::OnInit()
{
    //Setup for camera movement and rotation
    nv_helpers_dx12::CameraManip.setWindowSize(GetWidth(), GetHeight());
    nv_helpers_dx12::CameraManip.setLookat(glm::vec3(1.5f, 1.5f, 1.5f), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));

    LoadPipeline();
    LoadAssets();
    CheckRaytracingSupport();
    // Setup the acceleration structures (AS) for raytracing. When setting up
    // geometry, each bottom-level AS has its own transform matrix.
    CreateAccelerationStructures();
    // Create the raytracing pipeline, associating the shader code to symbol names
    // and to their root signatures, and defining the amount of memory carried by
    // rays (ray payload)
    CreateRaytracingPipeline();
    // #DXR Extra: Per-Instance Data
    CreatePerInstanceConstantBuffers();
    // #DXR Extra: Per-Instance Data
    // Create a constant buffer, with a color for each vertex of the triangle, for each triangle instance
    CreateGlobalConstantBuffer();
    // Allocate the buffer storing the raytracing output
    CreateRaytracingOutputBuffer();
    // #DXR Extra: Perspective Camera
    // Create a buffer to store the modelview and perspective camera matrices
    CreateCameraBuffer();
    // Create the buffer containing the raytracing result (always output in a
    // UAV), and create the heap referencing the resources used by the raytracing,
    // such as the acceleration structure
    CreateShaderResourceHeap();
    // Create the shader binding table and indicating which shaders
    // are invoked for each instance in the  AS
    CreateShaderBindingTable();
    // Command lists are created in the recording state, but there is nothing
    // to record yet. The main loop expects it to be closed, so close it now.
    ThrowIfFailed(m_commandList->Close());
}

// Load the rendering pipeline dependencies.
void D3D12HelloTriangle::LoadPipeline()
{
    UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    // NOTE: Enabling the debug layer after device creation will invalidate the active device.
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            // Enable additional debug layers.
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    if (m_useWarpDevice)
    {
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

        ThrowIfFailed(D3D12CreateDevice(
            warpAdapter.Get(),
            D3D_FEATURE_LEVEL_12_1,
            IID_PPV_ARGS(&m_device)
        ));
    }
    else
    {
        ComPtr<IDXGIAdapter1> hardwareAdapter;
        GetHardwareAdapter(factory.Get(), &hardwareAdapter);

        ThrowIfFailed(D3D12CreateDevice(
            hardwareAdapter.Get(),
            D3D_FEATURE_LEVEL_12_1,
            IID_PPV_ARGS(&m_device)
        ));
    }

    // Describe and create the command queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));

    // Describe and create the swap chain.
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),		// Swap chain needs the queue so that it can force a flush on it.
        Win32Application::GetHwnd(),
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain
    ));

    // This sample does not support fullscreen transitions.
    ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(swapChain.As(&m_swapChain));
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Create descriptor heaps.
    {
        // Describe and create a render target view (RTV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // Create frame resources.
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

        // Create a RTV for each frame.
        for (UINT n = 0; n < FrameCount; n++)
        {
            ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
            m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvHandle);
            rtvHandle.Offset(1, m_rtvDescriptorSize);
        }
    }

    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
}

// Load the sample assets.
void D3D12HelloTriangle::LoadAssets()
{
    // #DXR Extra: Perspective Camera
    // The root signature describes which data is accessed by the shader. The camera matrices are held
    // in a constant buffer, itself referenced the heap. To do this we reference a range in the heap,
    // and use that range as the sole parameter of the shader. The camera buffer is associated in the
    // index 0, making it accessible in the shader in the b0 register.
    {
        CD3DX12_ROOT_PARAMETER constantParameter;
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        constantParameter.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(1, &constantParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
    }

    // Create the pipeline state, which includes compiling and loading shaders.
    {
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;

#if defined(_DEBUG)
        // Enable better shader debugging with the graphics debugging tools.
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = 0;
#endif

        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(), nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr));
        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(), nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr));

        // Define the vertex input layout.
        D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        // Describe and create the graphics pipeline state object (PSO).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
    }

    // Create the command list.
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), m_pipelineState.Get(), IID_PPV_ARGS(&m_commandList)));

    // Create the vertex buffer.
    {
        // Define the geometry for a triangle.
        Vertex triangleVertices[] =
        {
            {{std::sqrtf(8.f / 9.f), 0.f, -1.f / 3.f}, {1.f, 0.f, 0.f, 1.f}},
            {{-std::sqrtf(2.f / 9.f), std::sqrtf(2.f / 3.f), -1.f / 3.f}, {0.f, 1.f, 0.f, 1.f}},
            {{-std::sqrtf(2.f / 9.f), -std::sqrtf(2.f / 3.f), -1.f / 3.f}, {0.f, 0.f, 1.f, 1.f}},
            {{0.f, 0.f, 1.f}, {1, 0, 1, 1}}
        };

        const UINT vertexBufferSize = sizeof(triangleVertices);

        // Note: using upload heaps to transfer static data like vert buffers is not 
        // recommended. Every time the GPU needs it, the upload heap will be marshalled 
        // over. Please read up on Default Heap usage. An upload heap is used here for 
        // code simplicity and because there are very few verts to actually transfer.
        ThrowIfFailed(m_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_vertexBuffer)));

        // Copy the triangle data to the vertex buffer.
        UINT8* pVertexDataBegin;
        CD3DX12_RANGE readRange(0, 0);		// We do not intend to read from this resource on the CPU.
        ThrowIfFailed(m_vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
        memcpy(pVertexDataBegin, triangleVertices, sizeof(triangleVertices));
        m_vertexBuffer->Unmap(0, nullptr);

        // Initialize the vertex buffer view.
        m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vertexBufferView.StrideInBytes = sizeof(Vertex);
        m_vertexBufferView.SizeInBytes = vertexBufferSize;

        //Initialize indices for tetrahedron.
        std::vector<UINT> indices = { 0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2 };
        const UINT indexBufferSizeInBytes = (UINT)indices.size() * sizeof(UINT);

        CD3DX12_HEAP_PROPERTIES heapProperty = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferResource = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSizeInBytes);
        ThrowIfFailed(m_device->CreateCommittedResource(&heapProperty, D3D12_HEAP_FLAG_NONE, &bufferResource, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_indexBuffer)));

        // Copy the triangle data to the index buffer.
        UINT8* pIndexDataBegin;
        ThrowIfFailed(m_indexBuffer->Map(0, &readRange, (void**)&pIndexDataBegin));
        memcpy(pIndexDataBegin, indices.data(), indexBufferSizeInBytes);
        m_indexBuffer->Unmap(0, nullptr);

        // Initialize the index buffer view.
        m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
        m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        m_indexBufferView.SizeInBytes = indexBufferSizeInBytes;

        // #DXR - Per Instance
        // Create a vertex buffer for a ground plane, similarly to the triangle definition above
        CreatePlaneVB();
    }

    // Create synchronization objects and wait until assets have been uploaded to the GPU.
    {
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_fenceValue = 1;

        // Create an event handle to use for frame synchronization.
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_fenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }

        // Wait for the command list to execute; we are reusing the same command 
        // list in our main loop but for now, we just want to wait for setup to 
        // complete before continuing.
        WaitForPreviousFrame();
    }
}

// Update frame-based values. This method is called before each render.
void D3D12HelloTriangle::OnUpdate()
{
    // #DXR Extra: Perspective Camera
    UpdateCameraBuffer();
    // #DXR Extra - Refitting
    // Increment the time counter at each frame, and update the corresponding instance matrix of the
    // first triangle to animate its position
    //m_time++;
    //m_instances[0].second = XMMatrixRotationAxis({ 0.0f, 1.0f, 0.0f }, m_time / 50.0f) * XMMatrixTranslation(0.0f, 0.1f * cosf(m_time / 20.0f), 0.0f);
}

// Render the scene.
void D3D12HelloTriangle::OnRender()
{
    // Record all the commands we need to render the scene into the command list.
    PopulateCommandList();

    // Execute the command list.
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Present the frame.
    ThrowIfFailed(m_swapChain->Present(1, 0));

    WaitForPreviousFrame();
}

void D3D12HelloTriangle::OnDestroy()
{
    // Ensure that the GPU is no longer referencing resources that are about to be
    // cleaned up by the destructor.
    WaitForPreviousFrame();

    CloseHandle(m_fenceEvent);
}

void D3D12HelloTriangle::PopulateCommandList()
{
    // Command list allocators can only be reset when the associated 
    // command lists have finished execution on the GPU; apps should use 
    // fences to determine GPU execution progress.
    ThrowIfFailed(m_commandAllocator->Reset());

    // However, when ExecuteCommandList() is called on a particular command 
    // list, that command list can then be reset at any time and must be before 
    // re-recording.
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));

    // Set necessary state.
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    // Indicate that the back buffer will be used as a render target.
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Record commands.
    // #DXR
    if (m_raster)
    {
        // #DXR Extra: Perspective Camera
        std::vector<ID3D12DescriptorHeap*> heaps = { m_constHeap.Get() };
        m_commandList->SetDescriptorHeaps((UINT)heaps.size(), heaps.data());
        // set the root descriptor table 0 to the constant buffer descriptor heap
        m_commandList->SetGraphicsRootDescriptorTable(0, m_constHeap->GetGPUDescriptorHandleForHeapStart());
        const float clearColor[] = { 0.03f, 0.35f, 0.43f, 1.0f };
        m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
        m_commandList->IASetIndexBuffer(&m_indexBufferView);
        m_commandList->DrawIndexedInstanced(12, 1, 0, 0, 0);
        m_commandList->IASetVertexBuffers(0, 1, &m_planeBufferView);
        m_commandList->DrawInstanced(6, 1, 0, 0);
    }
    else
    {
        const float clearColor[] = { 0.6f, 0.8f, 0.4f, 1.0f };
        m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        // #DXR Extra - Refitting
        // Refit the top-level acceleration structure to account for the new transform matrix of the
        // triangle. Note that the build contains a barrier, hence we can do the rendering in the
        // same command list
        //CreateTopLevelAS(m_instances, true);

        // #DXR
        // Bind the descriptor heap giving access to the top-level acceleration
        // structure, as well as the raytracing output
        std::vector<ID3D12DescriptorHeap*> heaps = { m_srvUavHeap.Get() };
        m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());

        // On the last frame, the raytracing output was used as a copy source, to
        // copy its contents into the render target. Now we need to transition it to
        // a UAV so that the shaders can write in it.
        CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(m_outputResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->ResourceBarrier(1, &transition);
        // Setup the raytracing task
        D3D12_DISPATCH_RAYS_DESC desc = {};
        // The layout of the SBT is as follows: ray generation shader, miss
        // shaders, hit groups. As described in the CreateShaderBindingTable method,
        // all SBT entries of a given type have the same size to allow a fixed stride.

        // The ray generation shaders are always at the beginning of the SBT.
        uint32_t rayGenSectionSizeInBytes = m_sbtHelper.GetRayGenSectionSize();
        desc.RayGenerationShaderRecord.StartAddress = m_sbtStorage->GetGPUVirtualAddress();
        desc.RayGenerationShaderRecord.SizeInBytes = rayGenSectionSizeInBytes;

        // The miss shaders are in the second SBT section, right after the ray
        // generation shader. We have one miss shader for the camera rays and one
        // for the shadow rays, so this section has a size of 2*m_sbtEntrySize. We
        // also indicate the stride between the two miss shaders, which is the size
        // of a SBT entry
        uint32_t missSectionSizeInBytes = m_sbtHelper.GetMissSectionSize();
        desc.MissShaderTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() + rayGenSectionSizeInBytes;
        desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
        desc.MissShaderTable.StrideInBytes = m_sbtHelper.GetMissEntrySize();

        // The hit groups section start after the miss shaders. In this sample we
        // have one 1 hit group for the triangle
        uint32_t hitGroupSectionSize = m_sbtHelper.GetHitGroupSectionSize();
        desc.HitGroupTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() + rayGenSectionSizeInBytes + missSectionSizeInBytes;
        desc.HitGroupTable.SizeInBytes = hitGroupSectionSize;
        desc.HitGroupTable.StrideInBytes = m_sbtHelper.GetHitGroupEntrySize();

        // Dimensions of the image to render, identical to a kernel launch dimension
        desc.Width = GetWidth();
        desc.Height = GetHeight();
        desc.Depth = 1;

        // Bind the raytracing pipeline
        m_commandList->SetPipelineState1(m_rtStateObject.Get());
        m_commandList->DispatchRays(&desc);

        // The raytracing output needs to be copied to the actual render target used
        // for display. For this, we need to transition the raytracing output from a
        // UAV to a copy source, and the render target buffer to a copy destination.
        // We can then do the actual copy, before transitioning the render target
        // buffer into a render target, that will be then used to display the image
        transition = CD3DX12_RESOURCE_BARRIER::Transition(m_outputResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_commandList->ResourceBarrier(1, &transition);
        transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->ResourceBarrier(1, &transition);

        m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(), m_outputResource.Get());

        transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &transition);
    }

    // Indicate that the back buffer will now be used to present.
    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    ThrowIfFailed(m_commandList->Close());
}

void D3D12HelloTriangle::WaitForPreviousFrame()
{
    // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
    // This is code implemented as such for simplicity. The D3D12HelloFrameBuffering
    // sample illustrates how to use fences for efficient resource usage and to
    // maximize GPU utilization.

    // Signal and increment the fence value.
    const UINT64 fence = m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
    m_fenceValue++;

    // Wait until the previous frame is finished.
    if (m_fence->GetCompletedValue() < fence)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12HelloTriangle::CheckRaytracingSupport()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    ThrowIfFailed(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)));
    if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
    {
        throw std::runtime_error("Raytracing is not supported on device.");
    }
}

void D3D12HelloTriangle::OnKeyUp(UINT8 key)
{
    // Alternate between rasterization and raytracing using the spacebar
    if (key == VK_SPACE)
    {
        m_raster = !m_raster;
    }
}

D3D12HelloTriangle::AccelerationStructureBuffers D3D12HelloTriangle::CreateBottomLevelAS(std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vVertexBuffers, std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vIndexBuffers)
{
    // Create a bottom-level acceleration structure based on a list of vertex
    // buffers in GPU memory along with their vertex count. The build is then done
    // in 3 steps: gathering the geometry, computing the sizes of the required
    // buffers, and building the actual AS

    //Step one: Gathering the geometry
    nv_helpers_dx12::BottomLevelASGenerator bottomLevelAS;

    // #DXR Extra: Indexed Geometry
    for (size_t i = 0; i < vVertexBuffers.size(); i++)
    {
        if (i < vIndexBuffers.size() && vIndexBuffers[i].second > 0)
        {
            bottomLevelAS.AddVertexBuffer(vVertexBuffers[i].first.Get(), 0,
                                          vVertexBuffers[i].second, sizeof(Vertex),
                                          vIndexBuffers[i].first.Get(), 0,
                                          vIndexBuffers[i].second, nullptr, 0, true);
        }
        else
        {
            bottomLevelAS.AddVertexBuffer(vVertexBuffers[i].first.Get(), 0,
                                          vVertexBuffers[i].second, sizeof(Vertex), 0,
                                          0);
        }
    }

    //Step two: Computing the sizes for the buffers
    // The AS build requires some scratch space to store temporary information.
    // The amount of scratch memory is dependent on the scene complexity.
    UINT64 scratchSizeInBytes = 0;
    // The final AS also needs to be stored in addition to the existing vertex
    // buffers. It size is also dependent on the scene complexity.
    UINT64 resultSizeInBytes = 0;
    bottomLevelAS.ComputeASBufferSizes(m_device.Get(), false, &scratchSizeInBytes, &resultSizeInBytes);

    //Step three: building the actual AS
    // Once the sizes are obtained, the application is responsible for allocating
    // the necessary buffers. Since the entire generation will be done on the GPU,
    // we can directly allocate those on the default heap
    AccelerationStructureBuffers buffers;
    buffers.pScratch = nv_helpers_dx12::CreateBuffer(m_device.Get(), scratchSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, nv_helpers_dx12::kDefaultHeapProps);
    buffers.pResult = nv_helpers_dx12::CreateBuffer(m_device.Get(), resultSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nv_helpers_dx12::kDefaultHeapProps);

    // Build the acceleration structure. Note that this call integrates a barrier
    // on the generated AS, so that it can be used to compute a top-level AS right
    // after this method.
    bottomLevelAS.Generate(m_commandList.Get(), buffers.pScratch.Get(), buffers.pResult.Get(), false, nullptr);

    return buffers;
}

void D3D12HelloTriangle::CreateTopLevelAS(const std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>>& instances, bool updateOnly)
{
    if (!updateOnly)
    {
        // Create the main acceleration structure that holds all instances of the scene.
        // Similarly to the bottom-level AS generation, it is done in 3 steps: gathering
        // the instances, computing the memory requirements for the AS, and building the
        // AS itself

        //Step one: Gather the instances
        for (size_t i = 0; i < instances.size(); i++)
        {
            m_topLevelASGenerator.AddInstance(instances[i].first.Get(), instances[i].second, (UINT)i, (UINT)i);
        }

        //Step two: Compute the memory requirements

        // As for the bottom-level AS, the building the AS requires some scratch space
        // to store temporary data in addition to the actual AS. In the case of the
        // top-level AS, the instance descriptors also need to be stored in GPU
        // memory. This call outputs the memory requirements for each (scratch,
        // results, instance descriptors) so that the application can allocate the
        // corresponding memory
        UINT64 scratchSize, resultSize, instanceDescSize;
        m_topLevelASGenerator.ComputeASBufferSizes(m_device.Get(), true, &scratchSize, &resultSize, &instanceDescSize);

        //Step three: Create the buffers and build the TLAS

        // Create the scratch and result buffers. Since the build is all done on GPU,
        // those can be allocated on the default heap
        m_topLevelASBuffers.pScratch = nv_helpers_dx12::CreateBuffer(m_device.Get(), scratchSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nv_helpers_dx12::kDefaultHeapProps);
        m_topLevelASBuffers.pResult = nv_helpers_dx12::CreateBuffer(m_device.Get(), resultSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nv_helpers_dx12::kDefaultHeapProps);
        // The buffer describing the instances: ID, shader binding information,
        // matrices ... Those will be copied into the buffer by the helper through
        // mapping, so the buffer has to be allocated on the upload heap.
        m_topLevelASBuffers.pInstanceDesc = nv_helpers_dx12::CreateBuffer(m_device.Get(), instanceDescSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
    }

    m_topLevelASGenerator.Generate(m_commandList.Get(), m_topLevelASBuffers.pScratch.Get(), m_topLevelASBuffers.pResult.Get(), m_topLevelASBuffers.pInstanceDesc.Get(), updateOnly, m_topLevelASBuffers.pResult.Get());
}

void D3D12HelloTriangle::CreateAccelerationStructures()
{
    // Build the BLAS from triangle vertex buffer
    AccelerationStructureBuffers bottomLevelBuffers = CreateBottomLevelAS({ { m_vertexBuffer.Get(), 4 } }, { {m_vertexBuffer.Get(), 12 } });
    AccelerationStructureBuffers planeBottomLevelBuffers = CreateBottomLevelAS({ { m_planeBuffer.Get(), 6 } });

    // 3 instances of the triangle + a plane
    // Note: The error in here is just in Visual Studio, the program compiles and runs without a problem
    m_instances = { { bottomLevelBuffers.pResult, XMMatrixIdentity() },
                    { bottomLevelBuffers.pResult, XMMatrixTranslation(-0.6f, 0.0f, 0.0f) },
                    { bottomLevelBuffers.pResult, XMMatrixTranslation(+0.6f, 0.0f, 0.0f) },
                    { planeBottomLevelBuffers.pResult, XMMatrixTranslation(0.0f, 0.0f, 0.0f) } };
    CreateTopLevelAS(m_instances);

    //Flush the command list and wait for it to finish
    m_commandList->Close();
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, ppCommandLists);
    m_fenceValue++;
    m_commandQueue->Signal(m_fence.Get(), m_fenceValue);

    m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);

    // Once the command list is finished executing, reset it to be reused for rendering
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));

    // Store the AS buffers. The rest of the buffers will be released once we exit the function
    m_bottomLevelAS = bottomLevelBuffers.pResult;
}

ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateRayGenSignature()
{
    //RayGen shader needs to access 2 resources: the raytracing output and the TLAS
    nv_helpers_dx12::RootSignatureGenerator rsg;
    //Add the external data needed for the shader program
    rsg.AddHeapRangesParameter({ {0 /*u0*/, 1 /*1 descriptor*/, 0 /*use the implicit register space 0*/, D3D12_DESCRIPTOR_RANGE_TYPE_UAV /*UAV representing the output buffer*/, 0 /*heap slot where the UAV is defined*/},
                                 {0 /*t0*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV /*TLAS*/, 1},
                                 {0 /*b0*/, 1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_CBV /*Camera parameters*/, 2} });

    return rsg.Generate(m_device.Get(), true);
}

ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateHitSignature()
{
    //Hit shader only needs the hit info, which comes from the shaders themselves
    //therefore it doesn't need any external data.
    nv_helpers_dx12::RootSignatureGenerator rsg;
    rsg.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV);
    // #DXR Extra: Per-Instance Data
    // The vertex colors may differ for each instance, so it is not possible to
    // point to a single buffer in the heap. Instead we use the concept of root
    // parameters, which are defined directly by a pointer in memory. In the
    // shader binding table we will associate each hit shader instance with its
    // constant buffer. Here we bind the buffer to the first slot, accessible in
    // HLSL as register(b0)
    rsg.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_CBV, 0);
    return rsg.Generate(m_device.Get(), true);
}

ComPtr<ID3D12RootSignature> D3D12HelloTriangle::CreateMissSignature()
{
    //Miss shader only needs the hit info, which comes from the shaders themselves
    //therefore it doesn't need any external data.
    nv_helpers_dx12::RootSignatureGenerator rsg;
    return rsg.Generate(m_device.Get(), true);
}

void D3D12HelloTriangle::CreateRaytracingPipeline()
{
    // The raytracing pipeline binds the shader code, root signatures and pipeline
    // characteristics in a single structure used by DXR to invoke the shaders and
    // manage temporary memory during raytracing
    nv_helpers_dx12::RayTracingPipelineGenerator pipeline(m_device.Get());

    //First we compile the HLSL shaders to DXIL so that they can be used in GPUs.
    //The raytracing pipeline contains all the shaders that may be executed during the raytracing process.
    //The codes are separated semantically to raygen, miss and hit for clarity. Any code layout can be used.
    m_rayGenLibrary = nv_helpers_dx12::CompileShaderLibrary(L"RayGen.hlsl");
    m_missLibrary = nv_helpers_dx12::CompileShaderLibrary(L"Miss.hlsl");
    m_hitLibrary = nv_helpers_dx12::CompileShaderLibrary(L"Hit.hlsl");

    //Secondly, we add the libraries to the pipeline
    // In a way similar to DLLs, each library is associated with a number of
    // exported symbols. This has to be done explicitly in the lines below. Note that a single library
    // can contain an arbitrary number of symbols, whose semantic is given in HLSL using the [shader("xxx")] syntax
    pipeline.AddLibrary(m_rayGenLibrary.Get(), { L"RayGen" });
    pipeline.AddLibrary(m_missLibrary.Get(), { L"Miss" });
    //L"PlaneClosestHit" is from #DXR Extra: Per-Instance Data
    pipeline.AddLibrary(m_hitLibrary.Get(), { L"ClosestHit", L"PlaneClosestHit" });

    //Third, we generate the root signatures of the shaders so that we can define which parameters and buffers will be accessed.
    m_rayGenSignature = CreateRayGenSignature();
    m_hitSignature = CreateHitSignature();
    m_missSignature = CreateMissSignature();

    //Fourth, we need to define what happens when a ray hits our geometry. There are three types of hits: Intersection shader, any-hit shader and a closest-hit shader.
    //All these shaders are stored in what's called a HitGroup. An intersection shader is used when a ray hits a non-triangular geometry. Non-triangular geometries aren't used
    //so this doesn't apply to this project. We can simply use the default intersection shader which is defined in DX12. Any-hit shader is used for any geometry that the ray hits.
    //An empty any-hit shader is also defined in DX12 and that will be used for now as well. The closest-hit shader is the one that's called on the geometry that the ray first hits,
    //therefore the one that is going to be actually visible to the camera. This isn't defined in DX12 and is defined by the developer. Right now, the Hit.hlsl shader defines the ClosestHit
    //shader and that will be fed into the pipeline as the closest-hit shader.
    pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");
    pipeline.AddHitGroup(L"PlaneHitGroup", L"PlaneClosestHit");

    //Fifth, we need to associate the shaders imported from DXIL libraries with exactly one root signature.
    // The following section associates the root signature to each shader. Note
    // that we can explicitly show that some shaders share the same root signature
    // (eg. Miss and ShadowMiss). Note that the hit shaders are now only referred
    // to as hit groups, meaning that the underlying intersection, any-hit and
    // closest-hit shaders share the same root signature.
    pipeline.AddRootSignatureAssociation(m_rayGenSignature.Get(), { L"RayGen" });
    pipeline.AddRootSignatureAssociation(m_missSignature.Get(), { L"Miss" });
    pipeline.AddRootSignatureAssociation(m_hitSignature.Get(), { L"HitGroup", L"PlaneHitGroup" });

    //Sixth, we need to define the memory sizes and recursions allowed to the shaders.
    // The payload size defines the maximum size of the data carried by the rays,
    // ie. the data exchanged between shaders, such as the HitInfo structure in the HLSL code.
    // It is important to keep this value as low as possible as a too high value
    // would result in unnecessary memory consumption and cache trashing.
    pipeline.SetMaxPayloadSize(4 * sizeof(float)); // RGB + distance (float4 type in HLSL)

    // Upon hitting a surface, DXR can provide several attributes to the hit.
    // We just use the barycentric coordinates defined by the weights u,v
    // of the last two vertices of the triangle. The actual barycentrics can
    // be obtained using float3 barycentrics = float3(1.f-u-v, u, v);
    pipeline.SetMaxAttributeSize(2 * sizeof(float)); // barycentric coordinates (float2 type in HLSL)

    // The raytracing process can shoot rays from existing hit points, resulting
    // in nested TraceRay calls. Our sample code traces only primary rays, which
    // then requires a trace depth of 1. Note that this recursion depth should be
    // kept to a minimum for best performance. Path tracing algorithms can be
    // easily flattened into a simple loop in the ray generation.
    pipeline.SetMaxRecursionDepth(1);

    //Seventh, finally we generate the pipeline to be executed on the GPU and then cast the state object to a properties object
    //so that later we can access the shader pointers by name.
    m_rtStateObject = pipeline.Generate();
    ThrowIfFailed(m_rtStateObject->QueryInterface(IID_PPV_ARGS(&m_rtStateObjectProperties)));
}

void D3D12HelloTriangle::CreateRaytracingOutputBuffer()
{
    //Allocate the buffer for the raytracing output, which is the same size as the output image
    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.DepthOrArraySize = 1;
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    // The backbuffer is actually DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, but sRGB
    // formats cannot be used with UAVs. For accuracy we should convert to sRGB
    // ourselves in the shader
    resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    resDesc.Width = GetWidth();
    resDesc.Height = GetHeight();
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resDesc.MipLevels = 1;
    resDesc.SampleDesc.Count = 1;
    ThrowIfFailed(m_device->CreateCommittedResource(&nv_helpers_dx12::kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&m_outputResource)));
}

//Create the main heap used by shaders, which allows access to the raytracing output and the TLAS
void D3D12HelloTriangle::CreateShaderResourceHeap()
{
    //3 entries needed: 1 UAV for the raytracing output, 1 SRV for TLAS and 1 CBV for camera matrices
    m_srvUavHeap = nv_helpers_dx12::CreateDescriptorHeap(m_device.Get(), 3, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

    //Get a handle to te heap memory on the CPU side so that descriptors can be directly written to
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle_cpu = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();

    //UAV is the first entry based on what we defined in the root signature.
    //Create*View() methods write the view information directly into srvHandle.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_outputResource.Get(), nullptr, &uavDesc, srvHandle_cpu);

    //Add the TLAS SRV right after the raytracing output buffer
    srvHandle_cpu.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = m_topLevelASBuffers.pResult->GetGPUVirtualAddress();
    //Write the AS view in heap
    m_device->CreateShaderResourceView(nullptr, &srvDesc, srvHandle_cpu);

    // #DXR Extra: Perspective Camera
    // Add the constant buffer for the camera after the TLAS
    srvHandle_cpu.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Describe and create a constant buffer view for the camera
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = m_cameraBufferSize;
    m_device->CreateConstantBufferView(&cbvDesc, srvHandle_cpu);
}

void D3D12HelloTriangle::CreateShaderBindingTable()
{
    //The resources are bound to shaders in this function.

    // The SBT helper class collects calls to Add*Program.  If called several
    // times, the helper must be emptied before re-adding shaders.
    m_sbtHelper.Reset();
    // The pointer to the beginning of the heap is the only parameter required by
    // shaders without root parameters
    D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    // The helper treats both root parameter pointers and heap pointers as void*, while DX12 uses the
    // D3D12_GPU_DESCRIPTOR_HANDLE to define heap pointers. The pointer in this struct is a UINT64,
    // which then has to be reinterpreted as a pointer.
    auto heapPointer = reinterpret_cast<UINT64*>(srvUavHeapHandle.ptr);

    // The ray generation only uses heap data
    m_sbtHelper.AddRayGenerationProgram(L"RayGen", { heapPointer });
    // The miss and hit shaders do not access any external resources: instead they
    // communicate their results through the ray payload
    m_sbtHelper.AddMissProgram(L"Miss", {});

    // Adding the triangle hit shader
    m_sbtHelper.AddHitGroup(L"HitGroup", { (void*)m_vertexBuffer->GetGPUVirtualAddress(), (void*)m_globalConstantBuffer->GetGPUVirtualAddress() });
    // #DXR Extra: Per-Instance Data
    // We have 3 triangles, each of which needs to access its own constant buffer
    // as a root parameter in its primary hit shader. The shadow hit only sets a
    // boolean visibility in the payload, and does not require external data
    std::vector<void*> inputData;
    for (int i = 0; i < 3; i++)
    {
        inputData.push_back((void*)m_perInstanceConstantBuffers[i]->GetGPUVirtualAddress());
    }
    // The plane also uses a constant buffer for its vertex colors (for simplicity the plane uses the same buffer as the first instance triangle)
    inputData.push_back((void*)(m_perInstanceConstantBuffers[0]->GetGPUVirtualAddress()));
    m_sbtHelper.AddHitGroup(L"HitGroup", inputData);
    
    // #DXR Extra: Per-Instance Data
    m_sbtHelper.AddHitGroup(L"PlaneHitGroup", {});
    // #DXR Extra: Per-Instance Data
    // Adding the triangle hit shader and constant buffer data
    //m_sbtHelper.AddHitGroup(L"HitGroup", { (void*)m_globalConstantBuffer->GetGPUVirtualAddress() });

    // Compute the size of the SBT given the number of shaders and their parameters
    uint32_t sbtSize = m_sbtHelper.ComputeSBTSize();

    // Create the SBT on the upload heap. This is required as the helper will use
    // mapping to write the SBT contents. After the SBT compilation it could be
    // copied to the default heap for performance.
    m_sbtStorage = nv_helpers_dx12::CreateBuffer(m_device.Get(), sbtSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

    if (!m_sbtStorage)
    {
        throw std::logic_error("Could not allocate the shader binding table.");
    }
    // Compile the SBT from the shader and parameters info
    m_sbtHelper.Generate(m_sbtStorage.Get(), m_rtStateObjectProperties.Get());
}

void D3D12HelloTriangle::CreateCameraBuffer()
{
    //The 4 matrices are the classical ones used in the rasterization process, projecting the world - space positions of the vertices into a unit cube.
    //However, to obtain a raytracing result consistent with the rasterization, we need to do the opposite:
    //the rays are initialized as if we had an orthographic camera located at the origin.
    //We then need to transform the ray origin and direction into world space, using the inverse view and projection matrices.
    //The camera buffer stores all 4 matrices, where the raster and raytracing paths will access only the ones needed.
    uint32_t nbMatrix = 4; //view, perspective, viewInv, perspectiveInv
    m_cameraBufferSize = nbMatrix * sizeof(XMMATRIX);

    // Create the constant buffer for all matrices
    m_cameraBuffer = nv_helpers_dx12::CreateBuffer(m_device.Get(), m_cameraBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
    //Descriptor heap that will be used by the rasterization shaders
    m_constHeap = nv_helpers_dx12::CreateDescriptorHeap(m_device.Get(), 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

    // Describe and create the constant buffer view.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = m_cameraBufferSize;

    // Get a handle to the heap memory on the CPU side, to be able to write the descriptors directly
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_constHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateConstantBufferView(&cbvDesc, srvHandle);
}

void D3D12HelloTriangle::UpdateCameraBuffer()
{
    std::vector<XMMATRIX> matrices(4);
    // Initialize the view matrix, ideally this should be based on user
    // interactions The lookat and perspective matrices used for rasterization are
    // defined to transform world-space vertices into a [0,1]x[0,1]x[0,1] camera
    // space
    const glm::mat4& mat = nv_helpers_dx12::CameraManip.getMatrix();
    memcpy(&matrices[0].r->m128_f32[0], glm::value_ptr(mat), 16 * sizeof(float));

    float fovAngleY_degrees = 45.0f;
    float fovAngleY = fovAngleY_degrees * XM_PI / 180.0f; //Convert fov angle degrees to radians
    matrices[1] = XMMatrixPerspectiveFovRH(fovAngleY, m_aspectRatio, 0.1f, 1000.0f);

    // Raytracing has to do the contrary of rasterization: rays are defined in
    // camera space, and are transformed into world space. To do this, we need to
    // store the inverse matrices as well.
    XMVECTOR det;
    matrices[2] = XMMatrixInverse(&det, matrices[0]);
    matrices[3] = XMMatrixInverse(&det, matrices[1]);

    // Copy matrix contents
    uint8_t* pData;
    ThrowIfFailed(m_cameraBuffer->Map(0, nullptr, (void**)&pData));
    memcpy(pData, matrices.data(), m_cameraBufferSize);
    m_cameraBuffer->Unmap(0, nullptr);
}

void D3D12HelloTriangle::OnButtonDown(UINT32 lParam)
{
    nv_helpers_dx12::CameraManip.setMousePosition(-GET_X_LPARAM(lParam), -GET_Y_LPARAM(lParam));
}

void D3D12HelloTriangle::OnMouseMove(UINT8 wParam, UINT32 lParam)
{
    using nv_helpers_dx12::Manipulator;
    Manipulator::Inputs inputs;
    inputs.lmb = wParam & MK_LBUTTON;
    inputs.mmb = wParam & MK_MBUTTON;
    inputs.rmb = wParam & MK_RBUTTON;
    if (!inputs.lmb && !inputs.rmb && !inputs.mmb)
    {
        return; //No mouse buttons pressed
    }

    inputs.ctrl = GetAsyncKeyState(VK_CONTROL);
    inputs.shift = GetAsyncKeyState(VK_SHIFT);
    inputs.alt = GetAsyncKeyState(VK_MENU);

    CameraManip.mouseMove(-GET_X_LPARAM(lParam), -GET_Y_LPARAM(lParam), inputs);
}

// #DXR Extra: Per-Instance Data
void D3D12HelloTriangle::CreatePlaneVB()
{
    // Define the geometry for a plane.
    Vertex planeVertices[] = {
        {{-1.5f, -.8f, 01.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}, // 0
        {{-1.5f, -.8f, -1.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}, // 1
        {{01.5f, -.8f, 01.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}, // 2
        {{01.5f, -.8f, 01.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}, // 2
        {{-1.5f, -.8f, -1.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}, // 1
        {{01.5f, -.8f, -1.5f}, {1.0f, 1.0f, 1.0f, 1.0f}}  // 4
    };
    const UINT planeBufferSize = sizeof(planeVertices);

    // Note: using upload heaps to transfer static data like vert buffers is not
    // recommended. Every time the GPU needs it, the upload heap will be
    // marshalled over. Please read up on Default Heap usage. An upload heap is
    // used here for code simplicity and because there are very few verts to
    // actually transfer.
    CD3DX12_HEAP_PROPERTIES heapProperty = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferResource = CD3DX12_RESOURCE_DESC::Buffer(planeBufferSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&heapProperty, D3D12_HEAP_FLAG_NONE, &bufferResource, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_planeBuffer)));

    // Copy the triangle data to the vertex buffer.
    UINT8* pVertexDataBegin;
    CD3DX12_RANGE readRange(0, 0); //This resource is not intended to be read from the CPU, so its read size is 0.
    ThrowIfFailed(m_planeBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
    memcpy(pVertexDataBegin, planeVertices, planeBufferSize);
    m_planeBuffer->Unmap(0, nullptr);

    // Initialize the vertex buffer view.
    m_planeBufferView.BufferLocation = m_planeBuffer->GetGPUVirtualAddress();
    m_planeBufferView.StrideInBytes = sizeof(Vertex);
    m_planeBufferView.SizeInBytes = planeBufferSize;
}

void D3D12HelloTriangle::CreateGlobalConstantBuffer()
{
    // Due to HLSL packing rules, we create the CB with 9 float4 (each needs to start on a 16-byte boundary)
    XMVECTOR bufferData[] =
    {
        //A matrix
        XMVECTOR{1.0f, 0.0f, 0.0f, 1.0f},
        XMVECTOR{0.7f, 0.4f, 0.0f, 1.0f},
        XMVECTOR{0.4f, 0.7f, 0.0f, 1.0f},

        //B matrix
        XMVECTOR{0.0f, 1.0f, 0.0f, 1.0f},
        XMVECTOR{0.0f, 0.7f, 0.4f, 1.0f},
        XMVECTOR{0.0f, 0.4f, 0.7f, 1.0f},

        //C matrix
        XMVECTOR{0.0f, 0.0f, 1.0f, 1.0f},
        XMVECTOR{0.4f, 0.0f, 0.7f, 1.0f},
        XMVECTOR{0.7f, 0.0f, 0.4f, 1.0f},
    };

    // Create our buffer
    m_globalConstantBuffer = nv_helpers_dx12::CreateBuffer(m_device.Get(), sizeof(bufferData), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);

    // Copy CPU memory to GPU
    uint8_t* pData;
    //ID3D12Resource.Map() maps a pointer created to the GPU side, meaning that pointer now represents
    //an address in GPU. Then, memcpy allows us to copy arbitrary data that we have on the CPU side to the GPU
    //using the mapped pointer. Finally, since we don't need the GPU pointer anymore (and probably for preventing memory leaks)
    //we use ID3D12Resource.Unmap(), unmaps the pointer from the GPU side.
    ThrowIfFailed(m_globalConstantBuffer->Map(0, nullptr, (void**)&pData));
    memcpy(pData, bufferData, sizeof(bufferData));
    m_globalConstantBuffer->Unmap(0, nullptr);
}

void D3D12HelloTriangle::CreatePerInstanceConstantBuffers()
{
    // Due to HLSL packing rules, we create the CB with 9 float4 (each needs to start on a 16-byte boundary)
    XMVECTOR bufferData[] = {
        // A
        XMVECTOR{1.0f, 0.0f, 0.0f, 1.0f},
        XMVECTOR{1.0f, 0.4f, 0.0f, 1.0f},
        XMVECTOR{1.f, 0.7f, 0.0f, 1.0f},

        // B
        XMVECTOR{0.0f, 1.0f, 0.0f, 1.0f},
        XMVECTOR{0.0f, 1.0f, 0.4f, 1.0f},
        XMVECTOR{0.0f, 1.0f, 0.7f, 1.0f},

        // C
        XMVECTOR{0.0f, 0.0f, 1.0f, 1.0f},
        XMVECTOR{0.4f, 0.0f, 1.0f, 1.0f},
        XMVECTOR{0.7f, 0.0f, 1.0f, 1.0f},
    };
    m_perInstanceConstantBuffers.resize(3);
    int i = 0;
    for (auto& cb : m_perInstanceConstantBuffers)
    {
        const uint32_t bufferSize = sizeof(XMVECTOR) * 3;
        cb = nv_helpers_dx12::CreateBuffer(m_device.Get(), bufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, nv_helpers_dx12::kUploadHeapProps);
        uint8_t* pData;
        ThrowIfFailed(cb->Map(0, nullptr, (void**)&pData));
        memcpy(pData, &bufferData[i * 3], bufferSize);
        cb->Unmap(0, nullptr);
        i++;
    }
}

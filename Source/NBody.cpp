#include "Pch.h"
#include <malloc.h> // for _aligned_offset_malloc

#define VHR(hr) if (FAILED(hr)) { assert(0); }
#define SAFE_RELEASE(obj) if ((obj)) { (obj)->Release(); (obj) = nullptr; }
#define DX12_ENABLE_DEBUG_LAYER 0

#define kResolutionX 1280
#define kResolutionY 720
#define kDemoName "MusicVis"

struct GraphicsContext
{
    ID3D12Device*               device;
    ID3D12CommandQueue*         cmdQueue;
    ID3D12CommandAllocator*     cmdAlloc[2];
    ID3D12GraphicsCommandList*  cmdList;
    ID3D12Resource*             swapBuffers[4];
    ID3D12DescriptorHeap*       swapBuffersHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE swapBuffersHeapStart;
    uint32_t                    descriptorSize;
    uint32_t                    descriptorSizeRtv;
    uint32_t                    backBufferIndex;
    uint32_t                    frameIndex;
    IDXGIFactory4*              factory;
    IDXGISwapChain3*            swapChain;
    ID3D12Fence*                frameFence;
    HANDLE                      frameFenceEvent;
};

struct GraphicsResources
{
    ID3D12Resource*         vb[2];
    ID3D12PipelineState*    pso;
    ID3D12RootSignature*    rs;
};

void*
operator new[](size_t size, const char* /*name*/, int32_t /*flags*/, uint32_t /*debugFlags*/, const char* /*file*/, int32_t /*line*/)
{
    return malloc(size);
}

void*
operator new[](size_t size, size_t alignment, size_t alignmentOffset, const char* /*name*/, int32_t /*flags*/, uint32_t /*debugFlags*/,
               const char* /*file*/, int32_t /*Line*/)
{
    return _aligned_offset_malloc(size, alignment, alignmentOffset);
}

static eastl::vector<uint8_t>
LoadFile(const char* fileName)
{
    FILE* file = fopen(fileName, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    assert(size != -1);
    eastl::vector<uint8_t> content(size);
    fseek(file, 0, SEEK_SET);
    fread(&content[0], 1, content.size(), file);
    fclose(file);
    return content;
}

static double
GetTime()
{
    static LARGE_INTEGER startCounter;
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0)
    {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&startCounter);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart - startCounter.QuadPart) / (double)frequency.QuadPart;
}

static void
UpdateFrameTime(double& outTime, float& outDeltaTime, HWND window)
{
    static double lastTime = -1.0;
    static double lastFpsTime = 0.0;
    static uint32_t fpsFrame = 0;

    if (lastTime < 0.0)
    {
        lastTime = GetTime();
        lastFpsTime = lastTime;
    }

    outTime = GetTime();
    outDeltaTime = (float)(outTime - lastTime);
    lastTime = outTime;

    if ((outTime - lastFpsTime) >= 1.0)
    {
        double fps = fpsFrame / (outTime - lastFpsTime);
        double avgFrameTime = (1.0 / fps) * 1000.0;
        char text[256];
        snprintf(text, sizeof(text), "[%f fps  %f ms] %s", fps, avgFrameTime, kDemoName);
        SetWindowText(window, text);
        lastFpsTime = outTime;
        fpsFrame = 0;
    }
    fpsFrame++;
}

static void
PresentFrame(GraphicsContext& gr)
{
    gr.swapChain->Present(0, 0);

    static uint64_t cpuCompletedFrames = 0;
    gr.cmdQueue->Signal(gr.frameFence, ++cpuCompletedFrames);

    const uint64_t gpuCompletedFrames = gr.frameFence->GetCompletedValue();

    if ((cpuCompletedFrames - gpuCompletedFrames) >= 2)
    {
        gr.frameFence->SetEventOnCompletion(gpuCompletedFrames + 1, gr.frameFenceEvent);
        WaitForSingleObject(gr.frameFenceEvent, INFINITE);
    }

    gr.backBufferIndex = gr.swapChain->GetCurrentBackBufferIndex();
    gr.frameIndex = !gr.frameIndex;
}

static void
DrawScene(const GraphicsContext& gr, const GraphicsResources& res)
{
    ID3D12CommandAllocator* cmdAlloc = gr.cmdAlloc[gr.frameIndex];
    cmdAlloc->Reset();

    ID3D12GraphicsCommandList* cmdList = gr.cmdList;
    cmdList->Reset(cmdAlloc, nullptr);

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)kResolutionX, (float)kResolutionY, 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, (LONG)kResolutionX, (LONG)kResolutionY };
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissorRect);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = gr.swapBuffers[gr.backBufferIndex];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmdList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = gr.swapBuffersHeapStart;
    rtvHandle.ptr += gr.backBufferIndex * gr.descriptorSizeRtv;

    cmdList->OMSetRenderTargets(1, &rtvHandle, 0, nullptr);

    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    cmdList->SetPipelineState(res.pso);
    cmdList->SetGraphicsRootSignature(res.rs);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmdList->ResourceBarrier(1, &barrier);

    cmdList->Close();

    gr.cmdQueue->ExecuteCommandLists(1, (ID3D12CommandList**)&cmdList);
}

static void
UpdateScene(double time, float deltaTime)
{
}

static LRESULT CALLBACK
ProcessWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            PostQuitMessage(0);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(window, message, wParam, lParam);
}

static HWND
SetupWindow(const char* name, uint32_t resolutionX, uint32_t resolutionY, WNDPROC winproc)
{
    WNDCLASS winclass = {};
    winclass.lpfnWndProc = winproc;
    winclass.hInstance = GetModuleHandle(nullptr);
    winclass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    winclass.lpszClassName = name;
    if (!RegisterClass(&winclass))
    {
        assert(0);
    }

    RECT rect = { 0, 0, (int32_t)resolutionX, (int32_t)resolutionY };
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX, 0);

    HWND window = CreateWindowEx(0, name, name, WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX | WS_VISIBLE,
                                 CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                 nullptr, nullptr, nullptr, 0);
    assert(window);
    return window;
}

static bool
SetupGraphics(GraphicsContext& gr, HWND window)
{
    assert(gr.device == nullptr);
    assert(window != nullptr);

    VHR(CreateDXGIFactory1(IID_PPV_ARGS(&gr.factory)));

#if DX12_ENABLE_DEBUG_LAYER == 1
    ID3D12Debug* dbg = nullptr;
    D3D12GetDebugInterface(IID_PPV_ARGS(&dbg));
    if (dbg)
    {
        dbg->EnableDebugLayer();
        dbg->Release();
    }
#endif

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gr.device))))
    {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC cmdQueueDesc = {};
    cmdQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    cmdQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    VHR(gr.device->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(&gr.cmdQueue)));

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 4;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = window;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.Windowed = 1;

    IDXGISwapChain* tempSwapChain;
    VHR(gr.factory->CreateSwapChain(gr.cmdQueue, &swapChainDesc, &tempSwapChain));
    VHR(tempSwapChain->QueryInterface(IID_PPV_ARGS(&gr.swapChain)));
    tempSwapChain->Release();

    for (uint32_t i = 0; i < 2; ++i)
    {
        VHR(gr.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gr.cmdAlloc[i])));
    }

    gr.descriptorSize = gr.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gr.descriptorSizeRtv = gr.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 4;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        VHR(gr.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&gr.swapBuffersHeap)));
        gr.swapBuffersHeapStart = gr.swapBuffersHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_CPU_DESCRIPTOR_HANDLE handle = gr.swapBuffersHeapStart;

        for (uint32_t i = 0; i < 4; ++i)
        {
            VHR(gr.swapChain->GetBuffer(i, IID_PPV_ARGS(&gr.swapBuffers[i])));

            gr.device->CreateRenderTargetView(gr.swapBuffers[i], nullptr, handle);
            handle.ptr += gr.descriptorSizeRtv;
        }
    }

    VHR(gr.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gr.frameFence)));

    gr.frameFenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    assert(gr.frameFenceEvent);

    VHR(gr.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gr.cmdAlloc[0], nullptr, IID_PPV_ARGS(&gr.cmdList)));
    gr.cmdList->Close();

    return true;
}

static bool
SetupGraphicsResources(GraphicsResources& res, const GraphicsContext& gr)
{
    eastl::vector<uint8_t> vsBytecode = LoadFile("Assets/Shaders/TriangleVS.cso");
    eastl::vector<uint8_t> psBytecode = LoadFile("Assets/Shaders/SolidPS.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.VS = { vsBytecode.data(), vsBytecode.size() };
    psoDesc.PS = { psBytecode.data(), psBytecode.size() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = 0xffffffff;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    VHR(gr.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&res.pso)));
    VHR(gr.device->CreateRootSignature(0, vsBytecode.data(), vsBytecode.size(), IID_PPV_ARGS(&res.rs)));

    return true;
}

int CALLBACK
WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    SetProcessDPIAware();

    HWND window = SetupWindow(kDemoName, kResolutionX, kResolutionY, ProcessWindowMessage);
    bool run = false;

    GraphicsContext gr = {};
    GraphicsResources grRes = {};

    if (SetupGraphics(gr, window) &&
        SetupGraphicsResources(grRes, gr))
    {
        run = true;
    }

    while (run)
    {
        MSG message = {};
        if (PeekMessage(&message, 0, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
            if (message.message == WM_QUIT)
            {
                break;
            }
        }
        else
        {
            double time;
            float deltaTime;
            UpdateFrameTime(time, deltaTime, window);
            UpdateScene(time, deltaTime);
            DrawScene(gr, grRes);
            PresentFrame(gr);
        }
    }

    //ShutdownDirectX12(dxctx);
    return 0;
}

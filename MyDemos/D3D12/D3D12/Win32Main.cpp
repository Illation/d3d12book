// Main window creation
#include <windows.h>
#include <wrl.h>
#include <windowsx.h>

#include <dxgi1_4.h>
#include <d3d12.h>
#include <DirectXColors.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <d3dcompiler.h>

#include <string>
#include <assert.h>
#include <vector>
#include <unordered_map>
#include <array>

#include "../../../Common/d3dx12.h"

// Link necessary d3d12 libraries.
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "dxgi.lib")

// util
template <typename T>
using T_ComPtr = Microsoft::WRL::ComPtr<T>;

using namespace DirectX;
using namespace DirectX::PackedVector;


// fwd structs
class GameTimer;


// fwd functions

// init
bool InitWindowsApp(HINSTANCE instanceHandle, int show);
bool InitD3D();
bool CreateCommandObjects();
bool CreateSwapChain();
bool CreateDescriptorHeaps();

// framework
void OnResize();
void OnInitialize();
void Update(GameTimer const& gt);
void Draw();

// utility
void FlushCommandQueue();
void SetMsaaEnabled(bool const enabled);
void OnMouseDown(WPARAM btnState, int x, int y);
void OnMouseUp(WPARAM btnState, int x, int y);
void OnMouseMove(WPARAM btnState, int x, int y);
void CalculateFrameStats();
void LogAdapters();
void LogAdapterOutputs(IDXGIAdapter* adapter);
void LogOutputModes(IDXGIOutput* output, DXGI_FORMAT format);
T_ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* device,
	ID3D12GraphicsCommandList* cmdList,
	void const* data,
	UINT64 size,
	T_ComPtr<ID3D12Resource>& uploadBuffer);
UINT CalcConstantBufferByteSize(UINT byteSize);
T_ComPtr<ID3DBlob> CompileShader(std::wstring const& filename, D3D_SHADER_MACRO const* defines, std::string const& entrypoint, std::string const& target);
T_ComPtr<ID3DBlob> CompileShader(std::string const& source, 
	std::string const& name, 
	D3D_SHADER_MACRO const* defines, 
	std::string const& entrypoint, 
	std::string const& target);

int Run();
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// app specific
void BuildDescriptorHeaps();
void BuildConstantBuffers();
void BuildRootSignature();
void BuildShadersAndInputLayout();
void BuildBoxGeometry();
void BuildPSO();

// accessors
ID3D12Resource* CurrentBackBuffer();
D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView();
D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView();

//---------------------
// MathHelper
//
class MathHelper
{
public:
	template<typename T>
	static T Clamp(const T& x, const T& low, const T& high)
	{
		return x < low ? low : (x > high ? high : x);
	}

	static DirectX::XMFLOAT4X4 Identity4x4()
	{
		static DirectX::XMFLOAT4X4 I(
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f);

		return I;
	}

	static float const Pi;
};

float const MathHelper::Pi = 3.1415926535f;


//---------------------
// GameTimer
//
class GameTimer
{
	// construct destruct
	//--------------------
public:
	GameTimer()
	{
		__int64 countsPerSec;
		QueryPerformanceFrequency((LARGE_INTEGER*)&countsPerSec);
		m_SecondsPerCount = 1.0 / (double)countsPerSec;
	}

	// accessors
	//-----------
	float TotalTime() const
	{
		if (m_IsStopped)
		{
			return (float)(((m_StopTime - m_PausedTime) - m_BaseTime) * m_SecondsPerCount);
		}
		else
		{
			return (float)(((m_CurrTime - m_PausedTime) - m_BaseTime) * m_SecondsPerCount);
		}
	}

	float DeltaTime() const { return static_cast<float>(m_DeltaTime); }

	// functionality
	//---------------
	void Reset()
	{
		__int64 currTime;
		QueryPerformanceCounter((LARGE_INTEGER*)&currTime);

		m_BaseTime = currTime;
		m_PrevTime = currTime;
		m_StopTime = 0;
		m_IsStopped = false;
	}

	void Start()
	{
		__int64 startTime;
		QueryPerformanceCounter((LARGE_INTEGER*)&startTime);

		if (m_IsStopped)
		{
			m_PausedTime += (startTime - m_StopTime);

			m_PrevTime = startTime;

			m_StopTime = 0;
			m_IsStopped = false;
		}
	}

	void Stop()
	{
		if (!m_IsStopped)
		{
			__int64 currTime;
			QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
		
			m_StopTime = currTime;
			m_IsStopped = true;
		}
	}

	void Tick()
	{
		if (m_IsStopped)
		{
			m_DeltaTime = 0.0;
			return;
		}

		// time this frame
		__int64 currTime;
		QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
		m_CurrTime = currTime;

		// time diff
		m_DeltaTime = (m_CurrTime - m_PrevTime) * m_SecondsPerCount;

		// for next frame
		m_PrevTime = m_CurrTime;

		// avoid errors (during power save mode apparently)
		if (m_DeltaTime < 0.0)
		{
			m_DeltaTime = 0.0;
		}
	}

	// Data
	///////
private:
	double m_SecondsPerCount = 0.0;
	double m_DeltaTime = -1.0;

	__int64 m_BaseTime = 0;
	__int64 m_PausedTime = 0;
	__int64 m_StopTime = 0;
	__int64 m_PrevTime = 0;
	__int64 m_CurrTime = 0;

	bool m_IsStopped = false;
};

//---------------------
// UploadBuffer
//
// Helpful wrapper around a DX12 resource that lives on the upload heap for easy updating of data
//
template <typename T>
class UploadBuffer
{
	// construct destruct
	//--------------------
public:
	UploadBuffer(ID3D12Device* device, UINT elementCount, bool isConstantBuffer) 
		: m_IsConstantBuffer(isConstantBuffer)
	{
		if (m_IsConstantBuffer)
		{
			m_ElementByteSize = CalcConstantBufferByteSize(sizeof(T));
		}
		else
		{
			m_ElementByteSize = sizeof(T);
		}

		if (FAILED(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(m_ElementByteSize * elementCount),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_UploadBuffer))))
		{
			MessageBox(0, L"Creating resource for wrapped upload buffer failed!", L"ERROR", MB_OK);
			return;
		}

		if (FAILED(m_UploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_MappedData))))
		{
			MessageBox(0, L"Mapping wrapped upload buffer to data pointer failed!", L"ERROR", MB_OK);
			return;
		}

		// don't need to unmap until we're done with the resource, but can't write while the GPU is using it so must ensure proper sync
	}

	// no copy
	UploadBuffer(UploadBuffer const& rhs) = delete;
	UploadBuffer& operator=(UploadBuffer const& rhs) = delete;

	~UploadBuffer() // unmap on destruction
	{
		if (m_UploadBuffer != nullptr)
		{
			m_UploadBuffer->Unmap(0, nullptr);
		}

		m_MappedData = nullptr;
	}

	// accessors
	//-----------
	ID3D12Resource* Resource() const { return m_UploadBuffer.Get(); }

	// functionality
	//---------------
	void CopyData(int elementIndex, T const& data)
	{
		memcpy(&m_MappedData[elementIndex * m_ElementByteSize], &data, sizeof(T));
	}

	// Data
	///////
private:
	T_ComPtr<ID3D12Resource> m_UploadBuffer;
	BYTE* m_MappedData = nullptr;
	UINT m_ElementByteSize = 0u;
	bool m_IsConstantBuffer = false;
};

//---------------------
// Vertex
//
// Layout for a basic vertex
//
struct Vertex
{
	XMFLOAT3 m_Pos;
	XMFLOAT4 m_Color;
};

//---------------------
// SubmeshGeometry
//
// Subrange of geometry within a mesh, multiple of these can be used to store a number of meshes in a single vertex buffer
//  - Access to offsets and counts
//
struct SubmeshGeometry
{
	UINT m_IndexCount = 0u;
	UINT m_StartIndexLocation = 0u;
	INT m_BaseVertexLocation = 0;

	// Bounding box (for culling)
	//DirectX::BoundingBox Bounds;
};

//---------------------
// MeshGeometry
//
// Can store a number of submeshes with access to vertex and index buffers
//
struct MeshGeometry
{
	// accessors
	//------------
	D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const 
	{
		D3D12_VERTEX_BUFFER_VIEW vbv;
		vbv.BufferLocation = m_VertexBufferGPU->GetGPUVirtualAddress();
		vbv.StrideInBytes = m_VertexByteStride;
		vbv.SizeInBytes = m_VertexBufferByteSize;

		return vbv;
	}

	D3D12_INDEX_BUFFER_VIEW IndexBufferView() const 
	{
		D3D12_INDEX_BUFFER_VIEW ibv;
		ibv.BufferLocation = m_IndexBufferGPU->GetGPUVirtualAddress();
		ibv.Format = m_IndexFormat;
		ibv.SizeInBytes = m_IndexBufferByteSize;

		return ibv;
	}

	// functionality
	//---------------
	void DisposeUploaders() // this memory can be freed after uploading data to GPU is completed
	{
		m_VertexBufferUploader = nullptr;
		m_IndexBufferUploader = nullptr;
	}


	// Data
	///////

	std::string m_Name; // for lookup

	// system memory vertex data stored in generic blobs
	T_ComPtr<ID3DBlob> m_VertexBufferCPU;
	T_ComPtr<ID3DBlob> m_IndexBufferCPU;

	// GPU memory
	T_ComPtr<ID3D12Resource> m_VertexBufferGPU;
	T_ComPtr<ID3D12Resource> m_IndexBufferGPU;

	T_ComPtr<ID3D12Resource> m_VertexBufferUploader;
	T_ComPtr<ID3D12Resource> m_IndexBufferUploader;

	// layout
	UINT m_VertexByteStride = 0u;
	UINT m_VertexBufferByteSize = 0u;
	DXGI_FORMAT m_IndexFormat = DXGI_FORMAT_R16_UINT;
	UINT m_IndexBufferByteSize = 0u;

	// geometries
	std::unordered_map<std::string, SubmeshGeometry> m_DrawArgs;
};

//---------------------
// ObjectConstants
//
// Constant buffer to upload to vertex shader (like uniforms)
//
struct ObjectConstants
{
	XMFLOAT4X4 m_WorldViewProj = MathHelper::Identity4x4();
};

//---------------------
// settings
//
struct Settings
{
	// static
	static int const s_SwapChainBufferCount = 2;

	// members
	std::wstring m_MainWindowTitle = L"D3D12 App";

	D3D_DRIVER_TYPE m_D3dDriverType = D3D_DRIVER_TYPE_HARDWARE;
	DXGI_FORMAT m_BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT m_DepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
};

//---------------------
// DxApp
//
// contains all root directX 12 objects
//
struct DxApp
{
	// accessors
	//-----------
	float AspectRatio() const { return static_cast<float>(m_ClientWidth) / m_ClientHeight; }

	// Data
	///////

	// main components
	T_ComPtr<IDXGIFactory4> m_DxgiFactory;
	T_ComPtr<ID3D12Device> m_D3dDevice;

	// synchronize
	T_ComPtr<ID3D12Fence> m_Fence;
	UINT64 m_CurrentFence = 0u;

	// issue commands
	T_ComPtr<ID3D12CommandQueue> m_CommandQueue;
	T_ComPtr<ID3D12CommandAllocator> m_DirectCommandListAllocator;
	T_ComPtr<ID3D12GraphicsCommandList> m_CommandList;

	// standard buffers, link to window
	T_ComPtr<IDXGISwapChain> m_SwapChain;
	int m_CurrentBackBuffer;

	T_ComPtr<ID3D12Resource> m_SwapChainBuffer[Settings::s_SwapChainBufferCount];
	T_ComPtr<ID3D12Resource> m_DepthStencilBuffer;

	D3D12_VIEWPORT m_ScreenViewport;
	D3D12_RECT m_ScissorRect;

	// memory managment
	T_ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
	T_ComPtr<ID3D12DescriptorHeap> m_DsvHeap;

	UINT m_RtvDescriptorSize = 0u;
	UINT m_DsvDescriptorSize = 0u;
	UINT m_CbvSrvUavDescriptorSize = 0u;

	// MSAA
	UINT m_4xMsaaQuality = 0u; // quality level of 4X MSAA
	bool m_MsaaEnabled = false;

	// game state
	GameTimer m_Timer;
	bool m_IsPaused = false;

	// window
	int m_ClientWidth = 800;
	int m_ClientHeight = 600;

	bool m_IsResizing = false;
	bool m_IsMinimized = false;
	bool m_IsMaximized = false;


	// app specific
	//---------------
	T_ComPtr<ID3D12RootSignature> m_RootSignature = nullptr;
	T_ComPtr<ID3D12DescriptorHeap> m_CbvHeap = nullptr;

	std::unique_ptr<UploadBuffer<ObjectConstants>> m_ObjectCB = nullptr;

	std::unique_ptr<MeshGeometry> m_BoxGeo = nullptr;

	T_ComPtr<ID3DBlob> m_VsByteCode = nullptr;
	T_ComPtr<ID3DBlob> m_PsByteCode = nullptr;

	std::vector<D3D12_INPUT_ELEMENT_DESC> m_InputLayout;

	T_ComPtr<ID3D12PipelineState> m_PSO = nullptr;

	XMFLOAT4X4 m_World = MathHelper::Identity4x4();
	XMFLOAT4X4 m_View = MathHelper::Identity4x4();
	XMFLOAT4X4 m_Proj = MathHelper::Identity4x4();

	float m_Theta = 1.5f*XM_PI;
	float m_Phi = XM_PIDIV4;
	float m_Radius = 5.0f;

	POINT m_LastMousePos;
};


// global
HWND g_MainWnd = 0;
DxApp g_DxApp;
Settings g_Settings;

std::string const g_Shader = "									\n\
cbuffer cbPerObject : register(b0)								\n\
{																\n\
	float4x4 gWorldViewProj;									\n\
};																\n\
																\n\
struct VertexIn													\n\
{																\n\
	float3 PosL : POSITION;										\n\
	float4 Color : COLOR;										\n\
};																\n\
																\n\
struct VertexOut												\n\
{																\n\
	float4 PosH : SV_POSITION;									\n\
	float4 Color : COLOR;										\n\
};																\n\
																\n\
VertexOut VS(VertexIn vin)										\n\
{																\n\
	VertexOut vout;												\n\
																\n\
	// Transform to homogeneous clip space.						\n\
	vout.PosH = mul(float4(vin.PosL, 1.0f), gWorldViewProj);	\n\
																\n\
	// Just pass vertex color into the pixel shader.			\n\
	vout.Color = vin.Color;										\n\
																\n\
	return vout;												\n\
}																\n\
																\n\
float4 PS(VertexOut pin) : SV_Target							\n\
{																\n\
	return pin.Color;											\n\
}																";


//---------------
// main
//
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nShowCmd)
{
	// init window etc
	if (!InitWindowsApp(hInstance, nShowCmd))
	{
		return 0;
	}

	// main message loop until we quit
	return Run();
}


//------------
// InitWindowsApp
//
// Create and initialize main window, pass instance and show command
//
bool InitWindowsApp(HINSTANCE instanceHandle, int show)
{
	// Describe (shared?) window properties with a new window instance
	//-----------------------------------------------------------------
	WNDCLASS wc;
	
	wc.style = CS_HREDRAW | CS_VREDRAW; // redraw the window when it is resized
	wc.lpfnWndProc = WndProc; // link event processing fn
	wc.cbClsExtra = 0; // bytes for extra user data behind this class struct
	wc.cbWndExtra = 0; // bytes for extra user data behind the class instance
	wc.hInstance = instanceHandle; // program instance

	// looks
	wc.hIcon = LoadIcon(0, IDI_APPLICATION); // toolbar icon
	wc.hCursor = LoadCursor(0, IDC_CROSS); // cursor
	wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH); // background colour alt: WHITE_BRUSH | DKGRAY_BRUSH | BLACK_BRUSH
	wc.lpszMenuName = 0; // no menu bar

	wc.lpszClassName = L"BasicWndClass"; // ID for creating windows from

	// register window instance with Windows
	if (!RegisterClass(&wc))
	{
		MessageBox(0, L"RegisterClass FAILED", 0, 0);
		return false;
	}

	// Create the window given the window instance
	//---------------------------------------------
	CreateWindow(
		L"BasicWndClass", // instance class name
		g_Settings.m_MainWindowTitle.c_str(), // window title
		WS_OVERLAPPEDWINDOW, // style flags - CAPTION(titlebar) | OVERLAPPED(titlebar+border) | SYSMENU(x) | THICKFRAME(resize) | MINIMIZEBOX | MAXIMIZEBOX
		CW_USEDEFAULT, // x pos
		CW_USEDEFAULT, // y pos
		g_DxApp.m_ClientWidth, // y width
		g_DxApp.m_ClientHeight, // y height
		0, // parent window
		0, // menu handle ( no menu )
		instanceHandle, // program instance
		nullptr); // extra user data forwarded during window creation event

	if (g_MainWnd == 0)
	{
		MessageBox(0, L"CreateWindow FAILED", 0, 0);
		return false;
	}

	// show and update the window
	ShowWindow(g_MainWnd, show);
	UpdateWindow(g_MainWnd);

	return true;
}

//------------
// Run
//
// Main loop until we get the quit message
//
int Run()
{
	g_DxApp.m_Timer.Reset();

	MSG msg = { 0 };
	while (msg.message != WM_QUIT)
	{
		if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) // if there are messages we handle them -> messages are removed from the queue after being processed
		{
			TranslateMessage(&msg); // convert to charset
			DispatchMessage(&msg); // handle message by the correct WndProc
		}
		else // game updates
		{
			g_DxApp.m_Timer.Tick();
			if (!g_DxApp.m_IsPaused)
			{
				CalculateFrameStats();
				Update(g_DxApp.m_Timer);
				Draw();
			}
			else
			{
				Sleep(100);
			}
		}
	}

	return (int)msg.wParam;
}

//------------
// WndProc
//
// Event handling
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// handle specific messages, and return 0 (given they where handled successfully, so we don't call the default procedure)
	switch (msg)
	{
	case WM_CREATE: // init DX12 before window is shown
		g_MainWnd = hWnd;
		if (!InitD3D())
		{
			MessageBox(0, L"Initializing D3D 12 failed!", L"ERROR", MB_OK);
			DestroyWindow(hWnd);
			return 0;
		}

		OnResize();

		OnInitialize();
		return 0;

	case WM_ACTIVATE: // make sure we don't render when the window isn't focused
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			g_DxApp.m_IsPaused = true;
			g_DxApp.m_Timer.Stop();
		}
		else
		{
			g_DxApp.m_IsPaused = false;
			g_DxApp.m_Timer.Start();
		}

		return 0;

	case WM_SIZE:
		g_DxApp.m_ClientWidth = LOWORD(lParam);
		g_DxApp.m_ClientHeight = HIWORD(lParam);

		// check minimized // maximized
		if (g_DxApp.m_D3dDevice != nullptr)
		{
			if (wParam == SIZE_MINIMIZED)
			{
				g_DxApp.m_IsPaused = true;
				g_DxApp.m_IsMinimized = true;
				g_DxApp.m_IsMaximized = false;
			}
			else if (wParam == SIZE_MAXIMIZED)
			{
				g_DxApp.m_IsPaused = false;
				g_DxApp.m_IsMinimized = false;
				g_DxApp.m_IsMaximized = true;
				OnResize();
			}
			else if (wParam == SIZE_RESTORED)
			{
				if (g_DxApp.m_IsMinimized)
				{
					g_DxApp.m_IsPaused = false;
					g_DxApp.m_IsMinimized = false;
					OnResize();
				}
				else if (g_DxApp.m_IsMaximized)
				{
					g_DxApp.m_IsPaused = false;
					g_DxApp.m_IsMaximized = false;
					OnResize();
				}
				else if (g_DxApp.m_IsResizing)
				{
					// no action required, we will resize buffer when drag is finished
				}
				else
				{
					OnResize();
				}
			}
		}

		return 0;

	case WM_ENTERSIZEMOVE:
		g_DxApp.m_IsPaused = true;
		g_DxApp.m_IsResizing = true;
		g_DxApp.m_Timer.Stop();
		return 0;

	case WM_EXITSIZEMOVE:
		g_DxApp.m_IsPaused = false;
		g_DxApp.m_IsResizing = false;
		g_DxApp.m_Timer.Start();
		OnResize();
		return 0;

	case WM_GETMINMAXINFO: // don't let the window get smaller than this:
		((MINMAXINFO*)lParam)->ptMinTrackSize.x = 200;
		((MINMAXINFO*)lParam)->ptMinTrackSize.y = 200;
		return 0;

		//case WM_CLOSE: // confirm
		//	if (MessageBox(hWnd, L"Would you like to quit?", 0, MB_OKCANCEL) == IDOK)
		//	{
		//		DestroyWindow(ghMainWnd);
		//	}

		//	return 0;

		// Mouse events
	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
		OnMouseDown(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
		OnMouseUp(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;
	case WM_MOUSEMOVE:
		OnMouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;

		// handle key presses
	case WM_KEYUP: 
		if (wParam == VK_ESCAPE) // destroy window on escape -> leads to destroy event next
		{
			DestroyWindow(g_MainWnd);
		}
		else if (wParam == VK_F2)
		{
			SetMsaaEnabled(!g_DxApp.m_MsaaEnabled);
		}
		
		return 0;

	case WM_DESTROY: // terminate the message loop when the window is destroyed
		PostQuitMessage(0); // -> WM_QUIT handled in main loop
		return 0;
	}

	// any unhandled messages are passed on to default event handling
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

//-------------
// OnMouseDown
//
void OnMouseDown(WPARAM btnState, int x, int y)
{
	g_DxApp.m_LastMousePos.x = x;
	g_DxApp.m_LastMousePos.y = y;

	SetCapture(g_MainWnd);
}

//-------------
// OnMouseUp
//
void OnMouseUp(WPARAM btnState, int x, int y)
{
	ReleaseCapture();
}

//-------------
// OnMouseMove
//
void OnMouseMove(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0) // rotate around
	{
		// 1 pixel = 0.25°
		float dx = XMConvertToRadians(0.25f * static_cast<float>(x - g_DxApp.m_LastMousePos.x));
		float dy = XMConvertToRadians(0.25f * static_cast<float>(y - g_DxApp.m_LastMousePos.y));

		g_DxApp.m_Theta += dx;
		g_DxApp.m_Phi += dy;

		g_DxApp.m_Phi = MathHelper::Clamp(g_DxApp.m_Phi, 0.1f, MathHelper::Pi - 0.1f);
	}
	else if ((btnState & MK_RBUTTON) != 0) // change radius of rotation
	{
		// 1 pixel = 0.005 units
		float dx = 0.005f * static_cast<float>(x - g_DxApp.m_LastMousePos.x);
		float dy = 0.005f * static_cast<float>(y - g_DxApp.m_LastMousePos.y);

		g_DxApp.m_Radius += dx - dy;

		g_DxApp.m_Radius = MathHelper::Clamp(g_DxApp.m_Radius, 3.f, 15.f);
	}

	g_DxApp.m_LastMousePos.x = x;
	g_DxApp.m_LastMousePos.y = y;
}

//------------
// InitD3D
//
// All the initialization steps for DirectX 12 in one step
//
bool InitD3D()
{
	// debug layers
	//---------------
#if defined(DEBUG) || defined(_DEBUG) 
	{
		T_ComPtr<ID3D12Debug> debugController;
		if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			MessageBox(0, L"Couldn't create debug layer for DX12!", L"WARNING", MB_OK);
		}
		else
		{
			debugController->EnableDebugLayer();
		}
	}
#endif

	// DXGI
	//------
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g_DxApp.m_DxgiFactory))))
	{
		MessageBox(0, L"Couldn't create DXGI factory!", L"ERROR", MB_OK);
		return false;
	}

	// create device
	//---------------
	// attempt creating a hardware device
	HRESULT hardwareResult = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_DxApp.m_D3dDevice)); // nullptr means default device

	// fallback to software renderer
	if (FAILED(hardwareResult))
	{
		T_ComPtr<IDXGIAdapter> warpAdapter;
		if (FAILED(g_DxApp.m_DxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter))))
		{
			MessageBox(0, L"Couldn't find WARP adapter, falling back to software rendering failed!", L"ERROR", MB_OK);
			return false;
		}

		if (FAILED(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_DxApp.m_D3dDevice))))
		{
			MessageBox(0, L"Couldn't init D3D12 with default or warp adapter", L"ERROR", MB_OK);
			return false;
		}

		MessageBox(0, L"Couldn't find phyical graphics adapter, fallback to software rendering!", L"WARNING", MB_OK);
	}

	// Fence for synchronizing CPU and GPU
	//-------------------------------------
	if (FAILED(g_DxApp.m_D3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_DxApp.m_Fence))))
	{
		MessageBox(0, L"Couldn't create a D3D12 Fence", L"ERROR", MB_OK);
		return false;
	}

	// Descriptor sizes
	//------------------
	// render target view
	g_DxApp.m_RtvDescriptorSize = g_DxApp.m_D3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV); 
	// depth stencil view
	g_DxApp.m_DsvDescriptorSize = g_DxApp.m_D3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV); 
	// constant buffer, shader resource, unordered access view
	g_DxApp.m_CbvSrvUavDescriptorSize = g_DxApp.m_D3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); 

	// 4x MSAA Quality
	//-----------------
	{
		D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
		msQualityLevels.Format = g_Settings.m_BackBufferFormat;
		msQualityLevels.SampleCount = 4u; // 4x MSAA should always be supported
		msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
		msQualityLevels.NumQualityLevels = 0u;
		if (FAILED(g_DxApp.m_D3dDevice->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msQualityLevels, sizeof(msQualityLevels))))
		{
			MessageBox(0, L"Couldn't check MSAA quality levels", L"ERROR", MB_OK);
			return false;
		}

		g_DxApp.m_4xMsaaQuality = msQualityLevels.NumQualityLevels;
		assert(g_DxApp.m_4xMsaaQuality > 0u && "Unexpected MSAA quality level!");
	}

#ifdef _DEBUG
	LogAdapters();
#endif

	// other
	//-------
	if (!CreateCommandObjects())
	{
		return false;
	}

	if (!CreateSwapChain())
	{
		return false;
	}

	if (!CreateDescriptorHeaps())
	{
		return false;
	}

	return true;
}

//----------------------
// CreateCommandObjects
//
// Create command queue, allocator and list
//
bool CreateCommandObjects()
{
	// queue
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	if (FAILED(g_DxApp.m_D3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_DxApp.m_CommandQueue))))
	{
		MessageBox(0, L"Failed to create command queue!", L"ERROR", MB_OK);
		return false;
	}

	// allocator
	if (FAILED(g_DxApp.m_D3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_DxApp.m_DirectCommandListAllocator))))
	{
		MessageBox(0, L"Failed to create command allocator!", L"ERROR", MB_OK);
		return false;
	}

	// list
	if (FAILED(g_DxApp.m_D3dDevice->CreateCommandList(0u, 
		D3D12_COMMAND_LIST_TYPE_DIRECT, 
		g_DxApp.m_DirectCommandListAllocator.Get(), 
		nullptr, // initial pipeline state
		IID_PPV_ARGS(g_DxApp.m_CommandList.GetAddressOf()))))
	{
		MessageBox(0, L"Failed to create command list!", L"ERROR", MB_OK);
		return false;
	}

	// close the command list from the start, so that we can reset it
	g_DxApp.m_CommandList->Close();

	return true;
}

//-----------------
// CreateSwapChain
//
// Sets up back buffers etc
//
bool CreateSwapChain()
{
	g_DxApp.m_SwapChain.Reset(); // release the previous swap chain

	DXGI_SWAP_CHAIN_DESC sd;

	// buffer descriptor
	sd.BufferDesc.Width = g_DxApp.m_ClientWidth;
	sd.BufferDesc.Height = g_DxApp.m_ClientHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60u;
	sd.BufferDesc.RefreshRate.Denominator = 1u;
	sd.BufferDesc.Format = g_Settings.m_BackBufferFormat;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

	// msaa on off
	sd.SampleDesc.Count = g_DxApp.m_MsaaEnabled ? 4u : 1u;
	sd.SampleDesc.Quality = g_DxApp.m_MsaaEnabled ? (g_DxApp.m_4xMsaaQuality - 1) : 0u;

	// other
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = Settings::s_SwapChainBufferCount;
	sd.OutputWindow = g_MainWnd;
	sd.Windowed = true;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	// create it
	if (FAILED(g_DxApp.m_DxgiFactory->CreateSwapChain(g_DxApp.m_CommandQueue.Get(), &sd, g_DxApp.m_SwapChain.GetAddressOf())))
	{
		MessageBox(0, L"Failed to create swap chain!", L"ERROR", MB_OK);
		return false;
	}

	return true;
}

//-----------------------
// CreateDescriptorHeaps
//
// 2 for backbuffer render targets and one for DSV
//
bool CreateDescriptorHeaps()
{
	// render target views
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = Settings::s_SwapChainBufferCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0u;
	if (FAILED(g_DxApp.m_D3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(g_DxApp.m_RtvHeap.GetAddressOf()))))
	{
		MessageBox(0, L"Failed to create RTV heap!", L"ERROR", MB_OK);
		return false;
	}

	// depth stencil views
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 1u;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0u;
	if (FAILED(g_DxApp.m_D3dDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(g_DxApp.m_DsvHeap.GetAddressOf()))))
	{
		MessageBox(0, L"Failed to create DSV heap!", L"ERROR", MB_OK);
		return false;
	}

	return true;
}

//-------------------
// CurrentBackBuffer
//
// when the buffer flips we need to get the correct back buffer accordingly
//
ID3D12Resource* CurrentBackBuffer()
{
	return g_DxApp.m_SwapChainBuffer[g_DxApp.m_CurrentBackBuffer].Get();
}

//-----------------------
// CurrentBackBufferView
//
// when the buffer flips we need to get the correct back buffer accordingly
//
D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView()
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(g_DxApp.m_RtvHeap->GetCPUDescriptorHandleForHeapStart(), g_DxApp.m_CurrentBackBuffer, g_DxApp.m_RtvDescriptorSize);
}

//-----------------------
// DepthStencilView
//
D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView()
{
	return g_DxApp.m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
}

//----------
// OnResize
//
// Update render target views, depth stencil texture and view
//
void OnResize()
{
	assert(g_DxApp.m_D3dDevice != nullptr);
	assert(g_DxApp.m_SwapChain != nullptr);
	assert(g_DxApp.m_DirectCommandListAllocator != nullptr);

	// cleanup
	FlushCommandQueue(); // need to flush before overwriting resources the GPU might be reading from
	if (FAILED(g_DxApp.m_CommandList->Reset(g_DxApp.m_DirectCommandListAllocator.Get(), nullptr)))
	{
		MessageBox(0, L"Failed to reset the command buffer!", L"ERROR", MB_OK);
		return;
	}

	// Release resources that will be recreated if they where previously created
	for (UINT i = 0u; i < Settings::s_SwapChainBufferCount; ++i)
	{
		g_DxApp.m_SwapChainBuffer[i].Reset();
	}

	g_DxApp.m_DepthStencilBuffer.Reset();

	// Resize swap chain
	if (FAILED(g_DxApp.m_SwapChain->ResizeBuffers(Settings::s_SwapChainBufferCount,
		g_DxApp.m_ClientWidth,
		g_DxApp.m_ClientHeight,
		g_Settings.m_BackBufferFormat,
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)))
	{
		MessageBox(0, L"Failed to resize swap chain buffers!", L"ERROR", MB_OK);
		return;
	}

	g_DxApp.m_CurrentBackBuffer = 0u;

	// Render Target View
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(g_DxApp.m_RtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (UINT i = 0u; i < Settings::s_SwapChainBufferCount; ++i)
	{
		// access ith swap chain buffer resource
		if (FAILED(g_DxApp.m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&g_DxApp.m_SwapChainBuffer[i]))))
		{
			MessageBox(0, L"Failed to get buffer resource from swap chain!", L"ERROR", MB_OK);
			return;
		}

		// create rtv
		g_DxApp.m_D3dDevice->CreateRenderTargetView(g_DxApp.m_SwapChainBuffer[i].Get(), nullptr, rtvHeapHandle);
		rtvHeapHandle.Offset(1, g_DxApp.m_RtvDescriptorSize);
	}

	// Depth stencil texture
	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0u;
	depthStencilDesc.Width = g_DxApp.m_ClientWidth;
	depthStencilDesc.Height = g_DxApp.m_ClientHeight;
	depthStencilDesc.DepthOrArraySize = 1u;
	depthStencilDesc.MipLevels = 1u;
	depthStencilDesc.Format = g_Settings.m_DepthStencilFormat;
	depthStencilDesc.SampleDesc.Count = g_DxApp.m_MsaaEnabled ? 4u : 1u;
	depthStencilDesc.SampleDesc.Quality = g_DxApp.m_MsaaEnabled ? (g_DxApp.m_4xMsaaQuality - 1) : 0u;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear;
	optClear.Format = g_Settings.m_DepthStencilFormat;
	optClear.DepthStencil.Depth = 1.f;
	optClear.DepthStencil.Stencil = 0u;
	if (FAILED(g_DxApp.m_D3dDevice->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(g_DxApp.m_DepthStencilBuffer.GetAddressOf()))))
	{
		MessageBox(0, L"Failed to get buffer resource from swap chain!", L"ERROR", MB_OK);
		return;
	}

	// create depth stencil view
	g_DxApp.m_D3dDevice->CreateDepthStencilView(g_DxApp.m_DepthStencilBuffer.Get(), nullptr, DepthStencilView());

	// Transition resource from initial state to use as a depth buffer
	g_DxApp.m_CommandList->ResourceBarrier(1u, &CD3DX12_RESOURCE_BARRIER::Transition(g_DxApp.m_DepthStencilBuffer.Get(), 
		D3D12_RESOURCE_STATE_COMMON, 
		D3D12_RESOURCE_STATE_DEPTH_WRITE));

	// execute commands and wait for them to complete
	if (FAILED(g_DxApp.m_CommandList->Close())) // must close before actually pushing to command queue
	{
		MessageBox(0, L"Command list failed to close!", L"ERROR", MB_OK);
		return;
	}

	ID3D12CommandList* cmdLists[] = { g_DxApp.m_CommandList.Get() };
	g_DxApp.m_CommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);
	FlushCommandQueue();

	// reset viewport
	g_DxApp.m_ScreenViewport.TopLeftX = 0.f;
	g_DxApp.m_ScreenViewport.TopLeftY = 0.f;
	g_DxApp.m_ScreenViewport.Width = static_cast<float>(g_DxApp.m_ClientWidth);
	g_DxApp.m_ScreenViewport.Height = static_cast<float>(g_DxApp.m_ClientHeight);
	g_DxApp.m_ScreenViewport.MinDepth = 0.f;
	g_DxApp.m_ScreenViewport.MaxDepth = 1.f;

	g_DxApp.m_ScissorRect = { 0, 0, g_DxApp.m_ClientWidth, g_DxApp.m_ClientHeight };

	// app specific resize stuff
	//----------------------------

	// recalc projection matrix
	XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, g_DxApp.AspectRatio(), 1.f, 1000.f);
	XMStoreFloat4x4(&g_DxApp.m_Proj, P);
}

//-------------------
// FlushCommandQueue
//
// Make sure all commands in the command queue are executed before the function exits
//
void FlushCommandQueue()
{
	// increment to the fence value we need the GPU to catch up to
	g_DxApp.m_CurrentFence++;

	// instruct the command queue to set the fence point 
	// like any other command this doesn't happen until the instruction reaches the enf of the queue 
	//  - this ensures all previous commands get executed before
	if (FAILED(g_DxApp.m_CommandQueue->Signal(g_DxApp.m_Fence.Get(), g_DxApp.m_CurrentFence)))
	{
		MessageBox(0, L"Failed to signal new fence value to command queue!", L"ERROR", MB_OK);
		return;
	}

	// give the GPU a chance to actually complete before we wait for the event
	if (g_DxApp.m_Fence->GetCompletedValue() < g_DxApp.m_CurrentFence)
	{
		// set up an event that makes the CPU wait until the fence is set on the GPU
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		if (FAILED(g_DxApp.m_Fence->SetEventOnCompletion(g_DxApp.m_CurrentFence, eventHandle)))
		{
			MessageBox(0, L"Failed to set up event waiting for fence value completion!", L"ERROR", MB_OK);
			return;
		}

		// put the CPU thread to sleep until this happens
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

//----------------
// SetMsaaEnabled
//
void SetMsaaEnabled(bool const enabled)
{
	if (g_DxApp.m_MsaaEnabled != enabled)
	{
		g_DxApp.m_MsaaEnabled = enabled;

		// recreate swapchain and buffers with new settings
		CreateSwapChain();
		OnResize();
	}
}

//---------------------
// CalculateFrameStats
//
// FPS and frame time in ms
//
void CalculateFrameStats()
{
	static int s_FrameCount = 0;
	static float s_TimeElapsed = 0.f;

	s_FrameCount++;

	// average per second to not be too spammy
	if ((g_DxApp.m_Timer.TotalTime() - s_TimeElapsed) >= 1.f)
	{
		float fps = static_cast<float>(s_FrameCount); // fps = frame count / 1
		float mspf = 1000.f / fps;

		std::wstring fpsStr = std::to_wstring(fps);
		std::wstring mspfStr = std::to_wstring(mspf);

		std::wstring windowText = g_Settings.m_MainWindowTitle + L"   fps: " + fpsStr + L"   mspf: " + mspfStr;
		SetWindowText(g_MainWnd, windowText.c_str());

		// reset
		s_FrameCount = 0;
		s_TimeElapsed += 1.f;
	}
}

//-------------
// LogAdapters
//
void LogAdapters()
{
	UINT i = 0u;
	IDXGIAdapter* adapter = nullptr;
	std::vector<IDXGIAdapter*> adapterList;
	while (g_DxApp.m_DxgiFactory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND)
	{
		DXGI_ADAPTER_DESC desc;
		adapter->GetDesc(&desc);

		std::wstring text = L"+++Adapter: ";
		text += desc.Description;
		text += L"\n";
		OutputDebugString(text.c_str());

		adapterList.push_back(adapter);
		++i;
	}

	for (IDXGIAdapter*& adapter : adapterList)
	{
		LogAdapterOutputs(adapter);
		adapter->Release();
	}
}

//-------------------
// LogAdapterOutputs
//
void LogAdapterOutputs(IDXGIAdapter* adapter)
{
	UINT i = 0u;
	IDXGIOutput* output = nullptr;
	while (adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND)
	{
		DXGI_OUTPUT_DESC desc;
		output->GetDesc(&desc);

		std::wstring text = L"+++Output: ";
		text += desc.DeviceName;

		text += L"\n";
		OutputDebugString(text.c_str());

		LogOutputModes(output, g_Settings.m_BackBufferFormat);

		output->Release();

		++i;
	}
}

//----------------
// LogOutputModes
//
void LogOutputModes(IDXGIOutput* output, DXGI_FORMAT format)
{
	UINT count = 0u;
	UINT flags = 0u;

	output->GetDisplayModeList(format, flags, &count, nullptr); // nullptr makes it output the count instead of normal operation

	std::vector<DXGI_MODE_DESC> modeList(count);
	output->GetDisplayModeList(format, flags, &count, &modeList[0]); // second call fills the correctly sized array

	for (DXGI_MODE_DESC& mode : modeList)
	{
		UINT n = mode.RefreshRate.Numerator;
		UINT d = mode.RefreshRate.Denominator;
		std::wstring text =
			L"Width = " + std::to_wstring(mode.Width) + L" " +
			L"Height = " + std::to_wstring(mode.Height) + L" " +
			L"Refresh = " + std::to_wstring(n) + L"/" + std::to_wstring(d) + L"\n";

		::OutputDebugString(text.c_str());
	}
}

//---------------------
// CreateDefaultBuffer
//
// Create the buffer on the default (GPU access only) heap along with automatic creation of an upload heap. 
//  - The upload buffer can be released after the copy command has been executed
//
T_ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device* device, 
	ID3D12GraphicsCommandList* cmdList, 
	void const* data,
	UINT64 byteSize,
	T_ComPtr<ID3D12Resource>& uploadBuffer)
{
	T_ComPtr<ID3D12Resource> defaultBuffer;

	// create the resource
	if (FAILED(device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(byteSize),
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(defaultBuffer.GetAddressOf()))))
	{
		MessageBox(0, L"Failed to create buffer on default heap!", L"ERROR", MB_OK);
		return nullptr;
	}

	// create intermediate upload heap to copy our data from
	if (FAILED(device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(byteSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(uploadBuffer.GetAddressOf()))))
	{
		MessageBox(0, L"Failed to create buffer on upload heap for intermediate data transfer to a default buffer!", L"ERROR", MB_OK);
		return nullptr;
	}

	// describe data to upload
	D3D12_SUBRESOURCE_DATA subResourceData = {};
	subResourceData.pData = data;
	subResourceData.RowPitch = (LONG_PTR)byteSize;
	subResourceData.SlicePitch = subResourceData.RowPitch;

	// schedule copy of data to the default buffer resource
	//  - in UpdateSubresources, data will first be copied to the intermediate upload heap
	//  - then with ID3D12CommandList::CopySubresourceRegion, from upload to default buffer
	cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_COPY_DEST));
	UpdateSubresources<1>(cmdList, defaultBuffer.Get(), uploadBuffer.Get(), 0, 0, 1, &subResourceData); // from d3dX12
	cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_GENERIC_READ));

	return defaultBuffer;
}

//----------------------------
// CalcConstantBufferByteSize
//
// Constant buffers must be a multiple of hardware allocation size (256) so we round up to that
//
UINT CalcConstantBufferByteSize(UINT byteSize)
{
	return (byteSize + 255) & ~255;
}

//---------------
// CompileShader
//
// Utility wrapper around d3d shader compilation from a file
//
T_ComPtr<ID3DBlob> CompileShader(std::wstring const& filename, D3D_SHADER_MACRO const* defines, std::string const& entrypoint, std::string const& target)
{
	UINT compileFlags = 0u;
#if defined(DEBUG) || defined(_DEBUG)
	compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	HRESULT hr = S_OK;

	T_ComPtr<ID3DBlob> byteCode = nullptr;
	T_ComPtr<ID3DBlob> errors = nullptr;
	hr = D3DCompileFromFile(filename.c_str(),
		defines,
		D3D_COMPILE_STANDARD_FILE_INCLUDE,
		entrypoint.c_str(),
		target.c_str(),
		compileFlags,
		0,
		&byteCode,
		&errors);

	if (errors != nullptr)
	{
		OutputDebugStringA((char*)errors->GetBufferPointer());
	}

	if (FAILED(hr))
	{
		MessageBox(0, L"Failed to compile shader!", L"ERROR", MB_OK);
		return nullptr;
	}

	return byteCode;
}

//---------------
// CompileShader
//
// Utility wrapper around d3d shader compilation from a memory
//
T_ComPtr<ID3DBlob> CompileShader(std::string const& source, 
	std::string const& name, 
	D3D_SHADER_MACRO const* defines, 
	std::string const& entrypoint, 
	std::string const& target)
{
	UINT compileFlags = 0u;
#if defined(DEBUG) || defined(_DEBUG)
	compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	HRESULT hr = S_OK;

	T_ComPtr<ID3DBlob> byteCode = nullptr;
	T_ComPtr<ID3DBlob> errors = nullptr;
	hr = D3DCompile(reinterpret_cast<void const*>(source.c_str()),
		source.size(),
		name.c_str(),
		defines,
		D3D_COMPILE_STANDARD_FILE_INCLUDE,
		entrypoint.c_str(),
		target.c_str(),
		compileFlags,
		0,
		&byteCode,
		&errors);

	if (errors != nullptr)
	{
		OutputDebugStringA((char*)errors->GetBufferPointer());
	}

	if (FAILED(hr))
	{
		MessageBox(0, L"Failed to compile shader!", L"ERROR", MB_OK);
		return nullptr;
	}

	return byteCode;
}

//--------------
// OnInitialize
//
// Chance for project specific initialization code
//
void OnInitialize()
{
	if (FAILED(g_DxApp.m_CommandList->Reset(g_DxApp.m_DirectCommandListAllocator.Get(), nullptr)))
	{
		MessageBox(0, L"Failed to reset command list during init!", L"ERROR", MB_OK);
		return;
	}

	BuildDescriptorHeaps();
	BuildConstantBuffers();
	BuildRootSignature();
	BuildShadersAndInputLayout();
	BuildBoxGeometry();
	BuildPSO();

	// execute all those commands
	if (FAILED(g_DxApp.m_CommandList->Close()))
	{
		MessageBox(0, L"Failed to close command list on init!", L"ERROR", MB_OK);
		return;
	}

	ID3D12CommandList* cmdsLists[] = { g_DxApp.m_CommandList.Get() };
	g_DxApp.m_CommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// sync with CPU (wait)
	FlushCommandQueue();
}

//--------
// Update
//
// per frame gameplay and system logic
//
void Update(GameTimer const& gt)
{
	// calculate cam pos
	float x = g_DxApp.m_Radius * sinf(g_DxApp.m_Phi) * cosf(g_DxApp.m_Theta);
	float y = g_DxApp.m_Radius * sinf(g_DxApp.m_Phi) * sinf(g_DxApp.m_Theta);
	float z = g_DxApp.m_Radius * cosf(g_DxApp.m_Phi);

	// virw matrix
	XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&g_DxApp.m_View, view);

	XMMATRIX world = XMLoadFloat4x4(&g_DxApp.m_World);
	XMMATRIX proj = XMLoadFloat4x4(&g_DxApp.m_Proj);
	XMMATRIX worldViewProj = world * view * proj;
	
	// update cbuffer with that matrix
	ObjectConstants objConstants;
	XMStoreFloat4x4(&objConstants.m_WorldViewProj, XMMatrixTranspose(worldViewProj));
	g_DxApp.m_ObjectCB->CopyData(0, objConstants);

}

//------
// Draw
//
// per frame render logic
//
void Draw()
{
	// prepare
	//---------
	if (FAILED(g_DxApp.m_DirectCommandListAllocator->Reset())) // reuse command list memory (all prior commands finished executing before)
	{
		MessageBox(0, L"Failed to reset command list allocator!", L"ERROR", MB_OK);
		return;
	}

	// also sets PSO
	if (FAILED(g_DxApp.m_CommandList->Reset(g_DxApp.m_DirectCommandListAllocator.Get(), g_DxApp.m_PSO.Get()))) // now we reset the command list itself
	{
		MessageBox(0, L"Failed to reset command list!", L"ERROR", MB_OK);
		return;
	}

	// we need to reset viewport and scissor rect because command list reset
	g_DxApp.m_CommandList->RSSetViewports(1, &g_DxApp.m_ScreenViewport);
	g_DxApp.m_CommandList->RSSetScissorRects(1, &g_DxApp.m_ScissorRect);

	// convert the old front buffer (read access) to a render target (write access)
	g_DxApp.m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET));

	// Clear back and depth buffer
	//-----------------------------
	g_DxApp.m_CommandList->ClearRenderTargetView(CurrentBackBufferView(), DirectX::Colors::LightSteelBlue, 0, nullptr);
	g_DxApp.m_CommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.f, 0u, 0u, nullptr);

	// set render target (both color and depth)
	g_DxApp.m_CommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

	// Draw scene
	//------------
	ID3D12DescriptorHeap* descriptorHeaps[] = { g_DxApp.m_CbvHeap.Get() };
	g_DxApp.m_CommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	g_DxApp.m_CommandList->SetGraphicsRootSignature(g_DxApp.m_RootSignature.Get());

	g_DxApp.m_CommandList->IASetVertexBuffers(0, 1, &g_DxApp.m_BoxGeo->VertexBufferView());
	g_DxApp.m_CommandList->IASetIndexBuffer(&g_DxApp.m_BoxGeo->IndexBufferView());
	g_DxApp.m_CommandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	g_DxApp.m_CommandList->SetGraphicsRootDescriptorTable(0, g_DxApp.m_CbvHeap->GetGPUDescriptorHandleForHeapStart());

	g_DxApp.m_CommandList->DrawIndexedInstanced(g_DxApp.m_BoxGeo->m_DrawArgs["box"].m_IndexCount, 1, 0, 0, 0);

	// Buffer flip, finish
	//---------------------
	// convert the back buffer to a front buffer for display
	g_DxApp.m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT));

	// close command list so we can execute it, then queue it
	if (FAILED(g_DxApp.m_CommandList->Close())) // now we reset the command list itself
	{
		MessageBox(0, L"Failed to close command list!", L"ERROR", MB_OK);
		return;
	}

	ID3D12CommandList* cmdLists[] = { g_DxApp.m_CommandList.Get() };
	g_DxApp.m_CommandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);

	// swap buffers
	if (FAILED(g_DxApp.m_SwapChain->Present(0u, 0u)))
	{
		MessageBox(0, L"Failed to present swap chain!", L"ERROR", MB_OK);
		return;
	}

	g_DxApp.m_CurrentBackBuffer = (g_DxApp.m_CurrentBackBuffer + 1) % Settings::s_SwapChainBufferCount;

	// wait for GPU to catch up
	FlushCommandQueue();
}

//----------------------
// BuildDescriptorHeaps
//
// for constant buffer view
//
void BuildDescriptorHeaps()
{
	// create the cbv heap
	D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
	cbvHeapDesc.NumDescriptors = 1;
	cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; // shaders can see this resource
	cbvHeapDesc.NodeMask = 0;

	if (FAILED(g_DxApp.m_D3dDevice->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&g_DxApp.m_CbvHeap))))
	{
		MessageBox(0, L"Failed to create CB descriptor heap!", L"ERROR", MB_OK);
		return;
	}
}

//----------------------
// BuildConstantBuffers
//
void BuildConstantBuffers()
{
	g_DxApp.m_ObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(g_DxApp.m_D3dDevice.Get(), 1, true);
	UINT const objCBByteSize = CalcConstantBufferByteSize(sizeof(ObjectConstants));

	// offset to buffer start
	D3D12_GPU_VIRTUAL_ADDRESS cbAddress = g_DxApp.m_ObjectCB->Resource()->GetGPUVirtualAddress();

	// offset to ith object
	int boxCBufIndex = 0;
	cbAddress += boxCBufIndex * objCBByteSize;

	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
	cbvDesc.BufferLocation = cbAddress;
	cbvDesc.SizeInBytes = objCBByteSize;

	g_DxApp.m_D3dDevice->CreateConstantBufferView(&cbvDesc, g_DxApp.m_CbvHeap->GetCPUDescriptorHandleForHeapStart());
}

//----------------------
// BuildRootSignature
//
// Like a signature for a function except in this case the function is a shader and the parameters are the shader inputs
//
void BuildRootSignature()
{
	// table, root desc, or root constants
	CD3DX12_ROOT_PARAMETER slotRootParameter[1];

	// all cbvs for the shader in one table
	CD3DX12_DESCRIPTOR_RANGE cbvTable;
	cbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0); // type, desc count, base register
	slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable);

	// root sig from array of parameters
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, slotRootParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create root signature with single slot pointing to the descriptor range
	T_ComPtr<ID3DBlob> serializedRootSig = nullptr;
	T_ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		if (FAILED(hr))
		{
			MessageBox(0, L"Failed to serialize root signature!", L"ERROR", MB_OK);
			return;
		}
	}

	if (FAILED(g_DxApp.m_D3dDevice->CreateRootSignature(0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(&g_DxApp.m_RootSignature))))
	{
		MessageBox(0, L"Failed to create root signature!", L"ERROR", MB_OK);
		return;
	}
}

//----------------------------
// BuildShadersAndInputLayout
//
void BuildShadersAndInputLayout()
{
	g_DxApp.m_VsByteCode = CompileShader(g_Shader, "color.hlsl", nullptr, "VS", "vs_5_0");
	g_DxApp.m_PsByteCode = CompileShader(g_Shader, "color.hlsl", nullptr, "PS", "ps_5_0");

	// input layout for the basic Vertex struct
	g_DxApp.m_InputLayout =
	{
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
	};
}

//----------------------------
// BuildBoxGeometry
//
// Create a colored cube
//
void BuildBoxGeometry()
{
	// cube vertices and indices
	std::array<Vertex, 8> vertices =
	{
		Vertex({ XMFLOAT3(-1.f, -1.f, -1.f), XMFLOAT4(Colors::White) }),
		Vertex({ XMFLOAT3(-1.f, +1.f, -1.f), XMFLOAT4(Colors::Black) }),
		Vertex({ XMFLOAT3(+1.f, +1.f, -1.f), XMFLOAT4(Colors::Red) }),
		Vertex({ XMFLOAT3(+1.f, -1.f, -1.f), XMFLOAT4(Colors::Green) }),
		Vertex({ XMFLOAT3(-1.f, -1.f, +1.f), XMFLOAT4(Colors::Blue) }),
		Vertex({ XMFLOAT3(-1.f, +1.f, +1.f), XMFLOAT4(Colors::Yellow) }),
		Vertex({ XMFLOAT3(+1.f, +1.f, +1.f), XMFLOAT4(Colors::Cyan) }),
		Vertex({ XMFLOAT3(+1.f, -1.f, +1.f), XMFLOAT4(Colors::Magenta) })
	};

	UINT64 const vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

	std::array<std::uint16_t, 36> indices =
	{
		0, 1, 2,   0, 2, 3, // front
		4, 6, 5,   4, 7, 6, // back
		4, 5, 1,   4, 1, 0, // left
		3, 2, 6,   3, 6, 7, // right
		1, 5, 6,   1, 6, 2, // top
		4, 0, 3,   4, 3, 7, // bottom
	};

	UINT64 const ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	g_DxApp.m_BoxGeo = std::make_unique<MeshGeometry>();
	g_DxApp.m_BoxGeo->m_Name = "boxGeo";

	// CPU side memory
	if (FAILED(D3DCreateBlob(vbByteSize, &g_DxApp.m_BoxGeo->m_VertexBufferCPU)))
	{
		MessageBox(0, L"Failed to create CPU vertex buffer!", L"ERROR", MB_OK);
		return;
	}

	CopyMemory(g_DxApp.m_BoxGeo->m_VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	if (FAILED(D3DCreateBlob(ibByteSize, &g_DxApp.m_BoxGeo->m_IndexBufferCPU)))
	{
		MessageBox(0, L"Failed to create CPU index buffer!", L"ERROR", MB_OK);
		return;
	}

	CopyMemory(g_DxApp.m_BoxGeo->m_IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	// GPU side memory
	g_DxApp.m_BoxGeo->m_VertexBufferGPU = CreateDefaultBuffer(g_DxApp.m_D3dDevice.Get(),
		g_DxApp.m_CommandList.Get(),
		vertices.data(),
		vbByteSize,
		g_DxApp.m_BoxGeo->m_VertexBufferUploader);

	g_DxApp.m_BoxGeo->m_IndexBufferGPU = CreateDefaultBuffer(g_DxApp.m_D3dDevice.Get(),
		g_DxApp.m_CommandList.Get(),
		indices.data(),
		ibByteSize,
		g_DxApp.m_BoxGeo->m_IndexBufferUploader);

	// format
	g_DxApp.m_BoxGeo->m_VertexByteStride = sizeof(Vertex);
	g_DxApp.m_BoxGeo->m_VertexBufferByteSize = vbByteSize;
	g_DxApp.m_BoxGeo->m_IndexFormat = DXGI_FORMAT_R16_UINT;
	g_DxApp.m_BoxGeo->m_IndexBufferByteSize = ibByteSize;

	SubmeshGeometry submesh;
	submesh.m_IndexCount = (UINT)indices.size();
	submesh.m_StartIndexLocation = 0;
	submesh.m_BaseVertexLocation = 0;

	g_DxApp.m_BoxGeo->m_DrawArgs["box"] = submesh;
}

//----------------------------
// BuildPSO
//
void BuildPSO()
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
	ZeroMemory(&psoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));

	psoDesc.InputLayout = { g_DxApp.m_InputLayout.data(), (UINT)g_DxApp.m_InputLayout.size() };
	psoDesc.pRootSignature = g_DxApp.m_RootSignature.Get();
	psoDesc.VS = { reinterpret_cast<BYTE*>(g_DxApp.m_VsByteCode->GetBufferPointer()), g_DxApp.m_VsByteCode->GetBufferSize() };
	psoDesc.PS = { reinterpret_cast<BYTE*>(g_DxApp.m_PsByteCode->GetBufferPointer()), g_DxApp.m_PsByteCode->GetBufferSize() };
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = g_Settings.m_BackBufferFormat;
	psoDesc.SampleDesc.Count = g_DxApp.m_MsaaEnabled ? 4 : 1;
	psoDesc.SampleDesc.Quality = g_DxApp.m_MsaaEnabled ? (g_DxApp.m_4xMsaaQuality - 1) : 0;
	psoDesc.DSVFormat = g_Settings.m_DepthStencilFormat;

	if (FAILED(g_DxApp.m_D3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_DxApp.m_PSO))))
	{
		MessageBox(0, L"Failed to create PSO!", L"ERROR", MB_OK);
		return;
	}
}

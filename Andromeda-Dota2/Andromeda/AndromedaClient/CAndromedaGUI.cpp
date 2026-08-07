#include "CAndromedaGUI.hpp"

#include <ShlObj_core.h>

#include <ImGui/imgui_impl_win32.h>
#include <ImGui/imgui_impl_dx11.h>

#include <DllLauncher.hpp>
#include <Common/Helpers/StringHelper.hpp>

#include <AndromedaClient/CAndromedaClient.hpp>

static CAndromedaGUI g_AndromedaGUI{};
static ImFont* g_HeaderFont = nullptr;

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hwnd , UINT msg , WPARAM wParam , LPARAM lParam );

auto CAndromedaGUI::OnInit( IDXGISwapChain* pSwapChain ) -> void
{
	if ( m_bInit || !pSwapChain )
		return;

	DXGI_SWAP_CHAIN_DESC SwapChainDesc{};

	if ( FAILED( pSwapChain->GetDevice( IID_PPV_ARGS( &m_pDevice ) ) ) || !m_pDevice )
	{
		DEV_LOG( "[error] CAndromedaGUI::OnInit: GetDevice failed\n" );
		return;
	}

	m_pDevice->GetImmediateContext( &m_pDeviceContext );

	if ( FAILED( pSwapChain->GetDesc( &SwapChainDesc ) ) )
	{
		DEV_LOG( "[error] CAndromedaGUI::OnInit: GetDesc failed\n" );
		return;
	}

	m_hCS2Window = SwapChainDesc.OutputWindow;

	if ( !m_hCS2Window )
	{
		DEV_LOG( "[error] CAndromedaGUI::OnInit: OutputWindow is null\n" );
		return;
	}

	m_pImGuiContext = ImGui::CreateContext();

	m_GuiFile = GetDllDir() + XorStr( GUI_FILE );

	if ( !m_pFreeType_Font )
		m_pFreeType_Font = new FreeTypeBuild();

	ImGui::SetCurrentContext( m_pImGuiContext );

	ImGui::GetIO().IniFilename = m_GuiFile.c_str();
	ImGui::GetIO().LogFilename = "";

	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasSetMousePos;

	ImGui_ImplWin32_Init( m_hCS2Window );
	ImGui_ImplDX11_Init( m_pDevice , m_pDeviceContext );

	InitFont();
	UpdateStyle();

	m_WndProc_o = (WNDPROC)SetWindowLongPtrA( m_hCS2Window , GWLP_WNDPROC , (LONG_PTR)GUI_WndProc );

	m_bInit = true;
	DEV_LOG( "[+] CAndromedaGUI initialized (hwnd=%p device=%p)\n" , m_hCS2Window , m_pDevice );
}

auto CAndromedaGUI::OnDestroy() -> void
{
	SetWindowLongPtrA( m_hCS2Window , GWLP_WNDPROC , (LONG_PTR)GetAndromedaGUI()->m_WndProc_o );

	m_bVisible = false;

	int guard = 0;
	while ( ShowCursor( TRUE ) < 0 && ++guard < 8 )
	{
	}

	if ( m_pFreeType_Font )
	{
		delete m_pFreeType_Font;
		m_pFreeType_Font = nullptr;
	}

	ClearRenderTargetView();

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();

	ImGui::DestroyContext();

	m_bInit = false;
}

auto CAndromedaGUI::InitFont() -> void
{
	ImGuiIO& io = ImGui::GetIO();

	static const ImWchar TahomaRanges[] =
	{
		0x0020, 0xFFFC,
		0,
	};

	wchar_t* szWindowsFontPath = nullptr;

	if ( SHGetKnownFolderPath( FOLDERID_Fonts , 0 , 0 , &szWindowsFontPath ) == S_OK )
	{
		std::wstring TahomaFont = std::wstring( szWindowsFontPath ) + L"\\tahoma.ttf";
		ImFontConfig regularCfg{};
		regularCfg.OversampleH = 3;
		regularCfg.OversampleV = 2;
		regularCfg.RasterizerMultiply = 1.0f;

		io.Fonts->Clear();
		io.Fonts->AddFontFromFileTTF( unicode_to_utf8( TahomaFont ).c_str() , 16.f , &regularCfg , TahomaRanges );

		ImFontConfig headerCfg = regularCfg;
		headerCfg.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_Bold;
		g_HeaderFont = io.Fonts->AddFontFromFileTTF( unicode_to_utf8( TahomaFont ).c_str() , 16.f , &headerCfg , TahomaRanges );
	}

	CoTaskMemFree( szWindowsFontPath );
}

void CAndromedaGUI::OnPresent( IDXGISwapChain* pSwapChain )
{
	static int s_PresentLog = 0;

	if ( s_PresentLog < 3 )
	{
		DEV_LOG( "[+] Present callback #%d (swapchain=%p init=%d)\n" , s_PresentLog + 1 , pSwapChain , m_bInit ? 1 : 0 );
		s_PresentLog++;
	}

	ProcessHotkeys();

	if ( !m_bInit )
		OnInit( pSwapChain );

	if ( m_bInit )
		OnRender( pSwapChain );
}

void CAndromedaGUI::OnResizeBuffers( IDXGISwapChain* /*pSwapChain*/ )
{
	ClearRenderTargetView();
}
 
void CAndromedaGUI::OnRender( IDXGISwapChain* pSwapChain )
{
	if ( !pSwapChain || !m_pDevice || !m_pDeviceContext )
		return;

	if ( m_pFreeType_Font && m_pFreeType_Font->PreNewFrame() )
	{
		ImGui_ImplDX11_InvalidateDeviceObjects();
		ImGui_ImplDX11_CreateDeviceObjects();
	}

	if ( !m_pRenderTargetView )
	{
		ID3D11Texture2D* pBackBuffer = nullptr;

		if ( SUCCEEDED( pSwapChain->GetBuffer( 0 , IID_PPV_ARGS( &pBackBuffer ) ) ) && pBackBuffer )
		{
			// Let D3D pick the correct format from the backbuffer (Dota may not be R8G8B8A8).
			m_pDevice->CreateRenderTargetView( pBackBuffer , nullptr , &m_pRenderTargetView );
			pBackBuffer->Release();
		}

		if ( !m_pRenderTargetView )
		{
			static bool logged = false;
			if ( !logged )
			{
				DEV_LOG( "[error] CreateRenderTargetView failed\n" );
				logged = true;
			}
			return;
		}
	}

	ImGui::SetCurrentContext( m_pImGuiContext );

	ID3D11RenderTargetView* pOldRTV = nullptr;
	m_pDeviceContext->OMGetRenderTargets( 1 , &pOldRTV , nullptr );
	m_pDeviceContext->OMSetRenderTargets( 1 , &m_pRenderTargetView , nullptr );

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();

	ImGui::NewFrame();

	GetAndromedaClient()->OnRender();

	ImGui::EndFrame();
	ImGui::Render();

	ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );

	m_pDeviceContext->OMSetRenderTargets( 1 , &pOldRTV , nullptr );

	if ( pOldRTV )
		pOldRTV->Release();
}

auto CAndromedaGUI::ProcessHotkeys() -> void
{
	static bool f6WasDown = false;

	const bool f6Down = ( GetAsyncKeyState( VK_F6 ) & 0x8000 ) != 0;

	if ( f6Down && !f6WasDown )
		OnReopenGUI();

	f6WasDown = f6Down;
}

auto CAndromedaGUI::OnReopenGUI() -> void
{
	m_bVisible = !m_bVisible;

	DEV_LOG( "[gui] F6 toggle -> visible=%d init=%d\n" , m_bVisible ? 1 : 0 , m_bInit ? 1 : 0 );

	if ( !m_bInit )
		return;

	ImGui::SetCurrentContext( m_pImGuiContext );

	ImGui::GetIO().MouseDrawCursor = m_bVisible;

	// ShowCursor uses a display counter — drive it to an absolute visible/hidden state.
	if ( m_bVisible )
	{
		int guard = 0;
		while ( ShowCursor( FALSE ) >= 0 && ++guard < 8 )
		{
		}
	}
	else
	{
		int guard = 0;
		while ( ShowCursor( TRUE ) < 0 && ++guard < 8 )
		{
		}
	}

	if ( m_bVisible )
	{
		if ( m_vecMousePosSave.x == 0.f && m_vecMousePosSave.y == 0.f )
			m_vecMousePosSave = ImGui::GetIO().DisplaySize / 2.f;

		ImGui::GetIO().MousePos = m_vecMousePosSave;
	}
	else
	{
		m_vecMousePosSave = ImGui::GetIO().MousePos;
	}
}

LRESULT WINAPI CAndromedaGUI::GUI_WndProc( HWND hwnd , UINT uMsg , WPARAM wParam , LPARAM lParam )
{
	if ( uMsg == WM_QUIT || uMsg == WM_CLOSE || uMsg == WM_DESTROY )
	{
		GetDllLauncher()->OnDestroy();
		return true;
	}

	if ( GetAndromedaGUI()->m_bInit && GetAndromedaGUI()->IsVisible() )
	{
		// ImGui returns non-zero when it consumed the message — don't forward those to the game.
		if ( ImGui_ImplWin32_WndProcHandler( hwnd , uMsg , wParam , lParam ) )
			return true;
	}

	return CallWindowProcA( GetAndromedaGUI()->m_WndProc_o , hwnd , uMsg , wParam , lParam );
}

auto CAndromedaGUI::SetDotaDarkStyle() -> void
{
	auto& style = ImGui::GetStyle();
	auto& colors = style.Colors;

	style.WindowPadding = ImVec2( 0.f , 0.f );
	style.FramePadding = ImVec2( 9.f , 6.f );
	style.ItemSpacing = ImVec2( 8.f , 7.f );
	style.ItemInnerSpacing = ImVec2( 8.f , 6.f );
	style.WindowRounding = 8.f;
	style.FrameRounding = 8.f;
	style.ChildRounding = 8.f;
	style.PopupRounding = 8.f;
	style.ScrollbarRounding = 12.f;
	style.GrabRounding = 10.f;
	style.TabRounding = 8.f;
	style.WindowBorderSize = 1.f;
	style.FrameBorderSize = 1.f;
	style.Alpha = 1.0f;
	style.IndentSpacing = 14.f;
	style.GrabMinSize = 14.f;
	style.ScrollbarSize = 14.f;
	style.WindowTitleAlign = ImVec2( 0.05f , 0.5f );

	const ImVec4 bg0 = ImVec4( 0.035f , 0.038f , 0.043f , 0.98f );
	const ImVec4 bg1 = ImVec4( 0.050f , 0.054f , 0.060f , 0.98f );
	const ImVec4 bg2 = ImVec4( 0.075f , 0.080f , 0.090f , 1.00f );
	const ImVec4 accent = ImVec4( 0.96f , 0.20f , 0.24f , 1.00f );
	const ImVec4 accentSoft = ImVec4( accent.x , accent.y , accent.z , 0.35f );
	const ImVec4 text = ImVec4( 0.82f , 0.83f , 0.86f , 1.0f );
	const ImVec4 textMuted = ImVec4( 0.48f , 0.49f , 0.54f , 1.0f );

	colors[ImGuiCol_Text] = text;
	colors[ImGuiCol_TextDisabled] = textMuted;
	colors[ImGuiCol_WindowBg] = bg0;
	colors[ImGuiCol_ChildBg] = ImVec4( bg1.x , bg1.y , bg1.z , 0.92f );
	colors[ImGuiCol_PopupBg] = bg1;
	colors[ImGuiCol_Border] = ImVec4( 0.18f , 0.20f , 0.24f , 1.0f );
	colors[ImGuiCol_BorderShadow] = ImVec4( 0 , 0 , 0 , 0 );

	colors[ImGuiCol_FrameBg] = bg2;
	colors[ImGuiCol_FrameBgHovered] = ImVec4( bg2.x + 0.05f , bg2.y + 0.05f , bg2.z + 0.05f , 1.0f );
	colors[ImGuiCol_FrameBgActive] = ImVec4( accent.x , accent.y , accent.z , 0.20f );

	colors[ImGuiCol_TitleBg] = bg1;
	colors[ImGuiCol_TitleBgActive] = bg2;
	colors[ImGuiCol_TitleBgCollapsed] = bg1;

	colors[ImGuiCol_MenuBarBg] = bg1;

	colors[ImGuiCol_ScrollbarBg] = bg0;
	colors[ImGuiCol_ScrollbarGrab] = bg2;
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4( accent.x , accent.y , accent.z , 0.45f );
	colors[ImGuiCol_ScrollbarGrabActive] = accent;

	colors[ImGuiCol_CheckMark] = accent;

	colors[ImGuiCol_SliderGrab] = ImVec4( 0.95f , 0.95f , 0.96f , 0.95f ); // lighter slider grab
	colors[ImGuiCol_SliderGrabActive] = accent;

	colors[ImGuiCol_Button] = ImVec4( bg2.x , bg2.y , bg2.z , 0.95f );
	colors[ImGuiCol_ButtonHovered] = ImVec4( accent.x , accent.y , accent.z , 0.28f );
	colors[ImGuiCol_ButtonActive] = ImVec4( accent.x , accent.y , accent.z , 0.45f );

	colors[ImGuiCol_Header] = ImVec4( accent.x , accent.y , accent.z , 0.22f );
	colors[ImGuiCol_HeaderHovered] = ImVec4( accent.x , accent.y , accent.z , 0.32f );
	colors[ImGuiCol_HeaderActive] = ImVec4( accent.x , accent.y , accent.z , 0.48f );

	colors[ImGuiCol_Separator] = ImVec4( 0.20f , 0.22f , 0.26f , 1.0f );
	colors[ImGuiCol_SeparatorHovered] = ImVec4( accent.x , accent.y , accent.z , 0.40f );
	colors[ImGuiCol_SeparatorActive] = accent;

	colors[ImGuiCol_ResizeGrip] = accentSoft;
	colors[ImGuiCol_ResizeGripHovered] = ImVec4( accent.x , accent.y , accent.z , 0.50f );
	colors[ImGuiCol_ResizeGripActive] = accent;

	colors[ImGuiCol_Tab] = bg2;
	colors[ImGuiCol_TabHovered] = ImVec4( accent.x , accent.y , accent.z , 0.55f );
	colors[ImGuiCol_TabActive] = ImVec4( accent.x , accent.y , accent.z , 0.70f );
	colors[ImGuiCol_TabUnfocused] = ImVec4( bg1.x , bg1.y , bg1.z , 0.80f );
	colors[ImGuiCol_TabUnfocusedActive] = ImVec4( accent.x , accent.y , accent.z , 0.60f );

	colors[ImGuiCol_PlotLines] = ImVec4( 0.9f , 0.9f , 0.9f , 0.6f );
	colors[ImGuiCol_PlotLinesHovered] = accent;
	colors[ImGuiCol_PlotHistogram] = ImVec4( accent.x , accent.y , accent.z , 0.5f );
	colors[ImGuiCol_PlotHistogramHovered] = accent;

	colors[ImGuiCol_TextSelectedBg] = ImVec4( accent.x , accent.y , accent.z , 0.35f );
	colors[ImGuiCol_DragDropTarget] = ImVec4( 1.0f , 1.0f , 0.0f , 0.90f );

	colors[ImGuiCol_NavHighlight] = accent;
	colors[ImGuiCol_NavWindowingHighlight] = ImVec4( 1.f , 1.f , 1.f , 0.70f );
	colors[ImGuiCol_NavWindowingDimBg] = ImVec4( 0.08f , 0.08f , 0.10f , 0.85f );

	colors[ImGuiCol_ModalWindowDimBg] = ImVec4( 0.06f , 0.06f , 0.08f , 0.85f );
	colors[ImGuiCol_WindowShadow] = ImVec4( 0.f , 0.f , 0.f , 0.80f );
}

auto CAndromedaGUI::UpdateStyle() -> void
{
	ImGui::SetCurrentContext( m_pImGuiContext );

	SetDotaDarkStyle();
}

bool CAndromedaGUI::FreeTypeBuild::PreNewFrame()
{
	if ( !WantRebuild )
		return false;

	ImFontAtlas* atlas = ImGui::GetIO().Fonts;

	for ( int n = 0; n < atlas->Sources.Size; n++ )
		( (ImFontConfig*)&atlas->Sources[n] )->RasterizerMultiply = RasterizerMultiply;

#ifdef IMGUI_ENABLE_FREETYPE
	if ( BuildMode == FontBuildMode::FreeType )
	{
		atlas->FontBuilderIO = ImGuiFreeType::GetBuilderForFreeType();
		atlas->FontBuilderFlags = FreeTypeBuilderFlags;
	}
#endif

	atlas->Build();
	WantRebuild = false;

	return true;
}

auto CAndromedaGUI::ClearRenderTargetView() -> void
{
	if ( m_pRenderTargetView )
	{
		m_pRenderTargetView->Release();
		m_pRenderTargetView = nullptr;
	}
}

auto GetAndromedaGUI() -> CAndromedaGUI*
{
	return &g_AndromedaGUI;
}

auto CAndromedaGUI::GetHeaderFont() -> ImFont*
{
	return g_HeaderFont ? g_HeaderFont : ImGui::GetFont();
}


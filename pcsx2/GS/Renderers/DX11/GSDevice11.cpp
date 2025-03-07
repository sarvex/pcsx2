/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"
#include "GS.h"
#include "GSDevice11.h"
#include "GS/Renderers/DX11/D3D.h"
#include "GS/GSExtra.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "Host.h"
#include "ShaderCacheVersion.h"

#include "common/Align.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "imgui.h"

#include <fstream>
#include <sstream>
#include <VersionHelpers.h>
#include <d3dcompiler.h>
#include <dxgidebug.h>

// #define REPORT_LEAKED_OBJECTS 1

static constexpr std::array<float, 4> s_present_clear_color = {};

static bool SupportsTextureFormat(ID3D11Device* dev, DXGI_FORMAT format)
{
	UINT support;
	if (FAILED(dev->CheckFormatSupport(format, &support)))
		return false;

	return (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
}

GSDevice11::GSDevice11()
{
	memset(&m_state, 0, sizeof(m_state));

	m_state.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	m_state.bf = -1;

	m_features.primitive_id = true;
	m_features.texture_barrier = false;
	m_features.provoking_vertex_last = false;
	m_features.point_expand = false;
	m_features.line_expand = false;
	m_features.prefer_new_textures = false;
	m_features.dxt_textures = false;
	m_features.bptc_textures = false;
	m_features.framebuffer_fetch = false;
	m_features.dual_source_blend = true;
	m_features.stencil_buffer = true;
	m_features.clip_control = true;
	m_features.test_and_sample_depth = false;
}

GSDevice11::~GSDevice11() = default;

RenderAPI GSDevice11::GetRenderAPI() const
{
	return RenderAPI::D3D11;
}

bool GSDevice11::Create()
{
	if (!GSDevice::Create())
		return false;

	UINT create_flags = 0;
	if (GSConfig.UseDebugDevice)
		create_flags |= D3D11_CREATE_DEVICE_DEBUG;

	m_dxgi_factory = D3D::CreateFactory(GSConfig.UseDebugDevice);
	if (!m_dxgi_factory)
		return false;

	wil::com_ptr_nothrow<IDXGIAdapter1> dxgi_adapter = D3D::GetAdapterByName(m_dxgi_factory.get(), GSConfig.Adapter);

	static constexpr std::array<D3D_FEATURE_LEVEL, 3> requested_feature_levels = {
		{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0}};

	wil::com_ptr_nothrow<ID3D11Device> temp_dev;
	wil::com_ptr_nothrow<ID3D11DeviceContext> temp_ctx;

	HRESULT hr =
		D3D11CreateDevice(dxgi_adapter.get(), dxgi_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
			nullptr, create_flags, requested_feature_levels.data(), static_cast<UINT>(requested_feature_levels.size()),
			D3D11_SDK_VERSION, temp_dev.put(), nullptr, temp_ctx.put());

	if (FAILED(hr))
	{
		Console.Error("Failed to create D3D device: 0x%08X", hr);
		return false;
	}

	if (!temp_dev.try_query_to(&m_dev) || !temp_ctx.try_query_to(&m_ctx))
	{
		Console.Error("Direct3D 11.1 is required and not supported.");
		return false;
	}

	// we re-grab these later, see below
	dxgi_adapter.reset();
	temp_dev.reset();
	temp_ctx.reset();

	if (GSConfig.UseDebugDevice && IsDebuggerPresent())
	{
		wil::com_ptr_nothrow<ID3D11InfoQueue> info;
		if (m_dev.try_query_to(&info))
		{
			info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
			info->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, TRUE);

			// Silence some annoying harmless warnings.
			D3D11_MESSAGE_ID hide[] = {
				D3D11_MESSAGE_ID_DEVICE_OMSETRENDERTARGETS_HAZARD,
				D3D11_MESSAGE_ID_DEVICE_PSSETSHADERRESOURCES_HAZARD,
			};

			D3D11_INFO_QUEUE_FILTER filter = {};
			filter.DenyList.NumIDs = std::size(hide);
			filter.DenyList.pIDList = hide;
			info->AddStorageFilterEntries(&filter);
		}
	}

	wil::com_ptr_nothrow<IDXGIDevice> dxgi_device;
	if (m_dev.try_query_to(&dxgi_device) && SUCCEEDED(dxgi_device->GetParent(IID_PPV_ARGS(dxgi_adapter.put()))))
		Console.WriteLn(fmt::format("D3D Adapter: {}", D3D::GetAdapterName(dxgi_adapter.get())));
	else
		Console.Error("Failed to obtain D3D adapter name.");

	BOOL allow_tearing_supported = false;
	hr = m_dxgi_factory->CheckFeatureSupport(
		DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing_supported, sizeof(allow_tearing_supported));
	m_allow_tearing_supported = (SUCCEEDED(hr) && allow_tearing_supported == TRUE);

	if (!AcquireWindow(true) || (m_window_info.type != WindowInfo::Type::Surfaceless && !CreateSwapChain()))
		return false;

	D3D11_BUFFER_DESC bd;
	D3D11_SAMPLER_DESC sd;
	D3D11_DEPTH_STENCIL_DESC dsd;
	D3D11_RASTERIZER_DESC rd;
	D3D11_BLEND_DESC bsd;

	D3D_FEATURE_LEVEL level;

	if (GSConfig.UseDebugDevice)
		m_annotation = m_ctx.try_query<ID3DUserDefinedAnnotation>();
	level = m_dev->GetFeatureLevel();
	const bool support_feature_level_11_0 = (level >= D3D_FEATURE_LEVEL_11_0);

	if (!GSConfig.DisableShaderCache)
	{
		if (!m_shader_cache.Open(EmuFolders::Cache, m_dev->GetFeatureLevel(), SHADER_CACHE_VERSION, GSConfig.UseDebugDevice))
		{
			Console.Warning("Shader cache failed to open.");
		}
	}
	else
	{
		m_shader_cache.Open({}, m_dev->GetFeatureLevel(), SHADER_CACHE_VERSION, GSConfig.UseDebugDevice);
		Console.WriteLn("Not using shader cache.");
	}

	// Set maximum texture size limit based on supported feature level.
	if (support_feature_level_11_0)
		m_d3d_texsize = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
	else
		m_d3d_texsize = D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;

	{
		// HACK: check AMD
		// Broken point sampler should be enabled only on AMD.
		wil::com_ptr_nothrow<IDXGIDevice> dxgi_device;
		wil::com_ptr_nothrow<IDXGIAdapter1> dxgi_adapter;
		if (SUCCEEDED(m_dev->QueryInterface(dxgi_device.put())) &&
			SUCCEEDED(dxgi_device->GetParent(IID_PPV_ARGS(dxgi_adapter.put()))))
		{
			m_features.broken_point_sampler = (D3D::GetVendorID(dxgi_adapter.get()) == D3D::VendorID::AMD);
		}
	}

	SetFeatures();

	std::optional<std::string> shader = Host::ReadResourceFileToString("shaders/dx11/tfx.fx");
	if (!shader.has_value())
		return false;
	m_tfx_source = std::move(*shader);

	// convert

	D3D11_INPUT_ELEMENT_DESC il_convert[] =
	{
		{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};

	ShaderMacro sm_model(m_shader_cache.GetFeatureLevel());

	std::optional<std::string> convert_hlsl = Host::ReadResourceFileToString("shaders/dx11/convert.fx");
	if (!convert_hlsl.has_value())
		return false;
	if (!m_shader_cache.GetVertexShaderAndInputLayout(m_dev.get(), m_convert.vs.put(), m_convert.il.put(),
			il_convert, std::size(il_convert), *convert_hlsl, sm_model.GetPtr(), "vs_main"))
	{
		return false;
	}

	for (size_t i = 0; i < std::size(m_convert.ps); i++)
	{
		m_convert.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), *convert_hlsl, sm_model.GetPtr(), shaderName(static_cast<ShaderConvert>(i)));
		if (!m_convert.ps[i])
			return false;
	}

	shader = Host::ReadResourceFileToString("shaders/dx11/present.fx");
	if (!shader.has_value())
		return false;
	if (!m_shader_cache.GetVertexShaderAndInputLayout(m_dev.get(), m_present.vs.put(), m_present.il.put(),
			il_convert, std::size(il_convert), *shader, sm_model.GetPtr(), "vs_main"))
	{
		return false;
	}

	for (size_t i = 0; i < std::size(m_present.ps); i++)
	{
		m_present.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), *shader, sm_model.GetPtr(), shaderName(static_cast<PresentShader>(i)));
		if (!m_present.ps[i])
			return false;
	}

	memset(&bd, 0, sizeof(bd));

	bd.ByteWidth = sizeof(DisplayConstantBuffer);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	m_dev->CreateBuffer(&bd, nullptr, m_present.ps_cb.put());

	memset(&dsd, 0, sizeof(dsd));

	m_dev->CreateDepthStencilState(&dsd, m_convert.dss.put());

	dsd.DepthEnable = true;
	dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;

	m_dev->CreateDepthStencilState(&dsd, m_convert.dss_write.put());

	memset(&bsd, 0, sizeof(bsd));

	for (u32 i = 0; i < static_cast<u32>(m_convert.bs.size()); i++)
	{
		bsd.RenderTarget[0].RenderTargetWriteMask = static_cast<u8>(i);
		m_dev->CreateBlendState(&bsd, m_convert.bs[i].put());
	}

	// merge

	memset(&bd, 0, sizeof(bd));

	bd.ByteWidth = sizeof(MergeConstantBuffer);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	m_dev->CreateBuffer(&bd, nullptr, m_merge.cb.put());

	shader = Host::ReadResourceFileToString("shaders/dx11/merge.fx");
	if (!shader.has_value())
		return false;

	for (size_t i = 0; i < std::size(m_merge.ps); i++)
	{
		const std::string entry_point(StringUtil::StdStringFromFormat("ps_main%d", i));
		m_merge.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), *shader, sm_model.GetPtr(), entry_point.c_str());
		if (!m_merge.ps[i])
			return false;
	}

	memset(&bsd, 0, sizeof(bsd));

	bsd.RenderTarget[0].BlendEnable = true;
	bsd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bsd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	bsd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	bsd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bsd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bsd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bsd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

	m_dev->CreateBlendState(&bsd, m_merge.bs.put());

	// interlace

	memset(&bd, 0, sizeof(bd));

	bd.ByteWidth = sizeof(InterlaceConstantBuffer);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	m_dev->CreateBuffer(&bd, nullptr, m_interlace.cb.put());

	shader = Host::ReadResourceFileToString("shaders/dx11/interlace.fx");
	if (!shader.has_value())
		return false;
	for (size_t i = 0; i < std::size(m_interlace.ps); i++)
	{
		const std::string entry_point(StringUtil::StdStringFromFormat("ps_main%d", i));
		m_interlace.ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), *shader, sm_model.GetPtr(), entry_point.c_str());
		if (!m_interlace.ps[i])
			return false;
	}

	// Shade Boost

	memset(&bd, 0, sizeof(bd));
	bd.ByteWidth = sizeof(float) * 4;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

	m_dev->CreateBuffer(&bd, nullptr, m_shadeboost.cb.put());

	shader = Host::ReadResourceFileToString("shaders/dx11/shadeboost.fx");
	if (!shader.has_value())
		return false;
	m_shadeboost.ps = m_shader_cache.GetPixelShader(m_dev.get(), *shader, sm_model.GetPtr(), "ps_main");
	if (!m_shadeboost.ps)
		return false;

	// Vertex/Index Buffer
	bd = {};
	bd.ByteWidth = VERTEX_BUFFER_SIZE;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&bd, nullptr, m_vb.put())))
	{
		Console.Error("Failed to create vertex buffer.");
		return false;
	}

	bd.ByteWidth = INDEX_BUFFER_SIZE;
	bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
	if (FAILED(m_dev->CreateBuffer(&bd, nullptr, m_ib.put())))
	{
		Console.Error("Failed to create index buffer.");
		return false;
	}
	IASetIndexBuffer(m_ib.get());

	if (m_features.vs_expand)
	{
		bd.ByteWidth = VERTEX_BUFFER_SIZE;
		bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bd.StructureByteStride = sizeof(GSVertex);
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

		if (FAILED(m_dev->CreateBuffer(&bd, nullptr, m_expand_vb.put())))
		{
			Console.Error("Failed to create expand vertex buffer.");
			return false;
		}

		const CD3D11_SHADER_RESOURCE_VIEW_DESC vb_srv_desc(
			D3D11_SRV_DIMENSION_BUFFER, DXGI_FORMAT_UNKNOWN, 0, VERTEX_BUFFER_SIZE / sizeof(GSVertex));
		if (FAILED(m_dev->CreateShaderResourceView(m_expand_vb.get(), &vb_srv_desc, m_expand_vb_srv.put())))
		{
			Console.Error("Failed to create expand vertex buffer SRV.");
			return false;
		}

		m_ctx->VSSetShaderResources(0, 1, m_expand_vb_srv.addressof());

		bd.ByteWidth = EXPAND_BUFFER_SIZE;
		bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
		bd.StructureByteStride = 0;
		bd.MiscFlags = 0;

		std::unique_ptr<u8[]> expand_data = std::make_unique<u8[]>(EXPAND_BUFFER_SIZE);
		GenerateExpansionIndexBuffer(expand_data.get());

		const D3D11_SUBRESOURCE_DATA srd = {expand_data.get()};
		if (FAILED(m_dev->CreateBuffer(&bd, &srd, m_expand_ib.put())))
		{
			Console.Error("Failed to create expand index buffer.");
			return false;
		}
	}

	//

	memset(&rd, 0, sizeof(rd));

	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.FrontCounterClockwise = false;
	rd.DepthBias = false;
	rd.DepthBiasClamp = 0;
	rd.SlopeScaledDepthBias = 0;
	rd.DepthClipEnable = false; // ???
	rd.ScissorEnable = true;
	rd.MultisampleEnable = false;
	rd.AntialiasedLineEnable = false;

	m_dev->CreateRasterizerState(&rd, m_rs.put());
	m_ctx->RSSetState(m_rs.get());

	//

	memset(&sd, 0, sizeof(sd));

	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MinLOD = -FLT_MAX;
	sd.MaxLOD = FLT_MAX;
	sd.MaxAnisotropy = 1;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;

	m_dev->CreateSamplerState(&sd, m_convert.ln.put());

	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;

	m_dev->CreateSamplerState(&sd, m_convert.pt.put());

	//

	CreateTextureFX();

	//

	memset(&dsd, 0, sizeof(dsd));

	dsd.DepthEnable = false;
	dsd.StencilEnable = true;
	dsd.StencilReadMask = 1;
	dsd.StencilWriteMask = 1;
	dsd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	dsd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
	dsd.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	dsd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
	dsd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	dsd.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
	dsd.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	dsd.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;

	m_dev->CreateDepthStencilState(&dsd, m_date.dss.put());

	D3D11_BLEND_DESC blend;

	memset(&blend, 0, sizeof(blend));

	m_dev->CreateBlendState(&blend, m_date.bs.put());

	for (size_t i = 0; i < std::size(m_date.primid_init_ps); i++)
	{
		const std::string entry_point(StringUtil::StdStringFromFormat("ps_stencil_image_init_%d", i));
		m_date.primid_init_ps[i] = m_shader_cache.GetPixelShader(m_dev.get(), *convert_hlsl, sm_model.GetPtr(), entry_point.c_str());
		if (!m_date.primid_init_ps[i])
			return false;
	}

	m_features.cas_sharpening = support_feature_level_11_0 && CreateCASShaders();

	if (!CreateImGuiResources())
		return false;

	return true;
}

void GSDevice11::Destroy()
{
	GSDevice::Destroy();
	DestroySwapChain();
	ReleaseWindow();
	DestroyTimestampQueries();

	m_convert = {};
	m_present = {};
	m_merge = {};
	m_interlace = {};
	m_shadeboost = {};
	m_date = {};
	m_cas = {};
	m_imgui = {};

	m_vb.reset();
	m_ib.reset();
	m_expand_vb_srv.reset();
	m_expand_vb.reset();
	m_expand_ib.reset();

	m_vs.clear();
	m_vs_cb.reset();
	m_gs.clear();
	m_ps.clear();
	m_ps_cb.reset();
	m_ps_ss.clear();
	m_om_dss.clear();
	m_om_bs.clear();
	m_rs.reset();

	if (m_state.rt_view)
		m_state.rt_view->Release();
	if (m_state.dsv)
		m_state.dsv->Release();

	m_shader_cache.Close();

#ifdef REPORT_LEAKED_OBJECTS
	wil::com_ptr_nothrow<ID3D11Debug> debug;
	m_dev.try_query_to(&debug);
#endif

	m_annotation.reset();
	m_ctx.reset();
	m_dev.reset();
	m_dxgi_factory.reset();

#ifdef REPORT_LEAKED_OBJECTS
	if (debug)
		debug->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL);
#endif
}

void GSDevice11::SetFeatures()
{
	// Check all three formats, since the feature means any can be used.
	m_features.dxt_textures = SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC1_UNORM) &&
							  SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC2_UNORM) &&
							  SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC3_UNORM);

	m_features.bptc_textures = SupportsTextureFormat(m_dev.get(), DXGI_FORMAT_BC7_UNORM);

	const D3D_FEATURE_LEVEL feature_level = m_dev->GetFeatureLevel();
	m_features.vs_expand = (feature_level >= D3D_FEATURE_LEVEL_11_0);
}

bool GSDevice11::HasSurface() const
{
	return static_cast<bool>(m_swap_chain);
}

bool GSDevice11::GetHostRefreshRate(float* refresh_rate)
{
	if (m_swap_chain && m_is_exclusive_fullscreen)
	{
		DXGI_SWAP_CHAIN_DESC desc;
		if (SUCCEEDED(m_swap_chain->GetDesc(&desc)) && desc.BufferDesc.RefreshRate.Numerator > 0 &&
			desc.BufferDesc.RefreshRate.Denominator > 0)
		{
			DevCon.WriteLn(
				"using fs rr: %u %u", desc.BufferDesc.RefreshRate.Numerator, desc.BufferDesc.RefreshRate.Denominator);
			*refresh_rate = static_cast<float>(desc.BufferDesc.RefreshRate.Numerator) /
							static_cast<float>(desc.BufferDesc.RefreshRate.Denominator);
			return true;
		}
	}

	return GSDevice::GetHostRefreshRate(refresh_rate);
}

void GSDevice11::SetVSync(VsyncMode mode)
{
	m_vsync_mode = mode;
}

bool GSDevice11::CreateSwapChain()
{
	constexpr DXGI_FORMAT swap_chain_format = DXGI_FORMAT_R8G8B8A8_UNORM;

	if (m_window_info.type != WindowInfo::Type::Win32)
		return false;

	const HWND window_hwnd = reinterpret_cast<HWND>(m_window_info.window_handle);
	RECT client_rc{};
	GetClientRect(window_hwnd, &client_rc);

	DXGI_MODE_DESC fullscreen_mode;
	wil::com_ptr_nothrow<IDXGIOutput> fullscreen_output;
	if (Host::IsFullscreen())
	{
		u32 fullscreen_width, fullscreen_height;
		float fullscreen_refresh_rate;
		m_is_exclusive_fullscreen =
			GetRequestedExclusiveFullscreenMode(&fullscreen_width, &fullscreen_height, &fullscreen_refresh_rate) &&
			D3D::GetRequestedExclusiveFullscreenModeDesc(m_dxgi_factory.get(), client_rc, fullscreen_width,
				fullscreen_height, fullscreen_refresh_rate, swap_chain_format, &fullscreen_mode,
				fullscreen_output.put());
	}
	else
	{
		m_is_exclusive_fullscreen = false;
	}

	m_using_flip_model_swap_chain = !EmuConfig.GS.UseBlitSwapChain || m_is_exclusive_fullscreen;

	DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
	swap_chain_desc.Width = static_cast<u32>(client_rc.right - client_rc.left);
	swap_chain_desc.Height = static_cast<u32>(client_rc.bottom - client_rc.top);
	swap_chain_desc.Format = swap_chain_format;
	swap_chain_desc.SampleDesc.Count = 1;
	swap_chain_desc.BufferCount = 3;
	swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap_chain_desc.SwapEffect =
		m_using_flip_model_swap_chain ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;

	m_using_allow_tearing = (m_allow_tearing_supported && m_using_flip_model_swap_chain && !m_is_exclusive_fullscreen);
	if (m_using_allow_tearing)
		swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

	HRESULT hr = S_OK;

	if (m_is_exclusive_fullscreen)
	{
		DXGI_SWAP_CHAIN_DESC1 fs_sd_desc = swap_chain_desc;
		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc = {};

		fs_sd_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		fs_sd_desc.Width = fullscreen_mode.Width;
		fs_sd_desc.Height = fullscreen_mode.Height;
		fs_desc.RefreshRate = fullscreen_mode.RefreshRate;
		fs_desc.ScanlineOrdering = fullscreen_mode.ScanlineOrdering;
		fs_desc.Scaling = fullscreen_mode.Scaling;
		fs_desc.Windowed = FALSE;

		Console.WriteLn("Creating a %dx%d exclusive fullscreen swap chain", fs_sd_desc.Width, fs_sd_desc.Height);
		hr = m_dxgi_factory->CreateSwapChainForHwnd(
			m_dev.get(), window_hwnd, &fs_sd_desc, &fs_desc, fullscreen_output.get(), m_swap_chain.put());
		if (FAILED(hr))
		{
			Console.Warning("Failed to create fullscreen swap chain, trying windowed.");
			m_is_exclusive_fullscreen = false;
			m_using_allow_tearing = m_allow_tearing_supported && m_using_flip_model_swap_chain;
		}
	}

	if (!m_is_exclusive_fullscreen)
	{
		Console.WriteLn("Creating a %dx%d %s windowed swap chain", swap_chain_desc.Width, swap_chain_desc.Height,
			m_using_flip_model_swap_chain ? "flip-discard" : "discard");
		hr = m_dxgi_factory->CreateSwapChainForHwnd(
			m_dev.get(), window_hwnd, &swap_chain_desc, nullptr, nullptr, m_swap_chain.put());
	}

	if (FAILED(hr) && m_using_flip_model_swap_chain)
	{
		Console.Warning("Failed to create a flip-discard swap chain, trying discard.");
		swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		swap_chain_desc.Flags = 0;
		m_using_flip_model_swap_chain = false;
		m_using_allow_tearing = false;

		hr = m_dxgi_factory->CreateSwapChainForHwnd(
			m_dev.get(), window_hwnd, &swap_chain_desc, nullptr, nullptr, m_swap_chain.put());
		if (FAILED(hr))
		{
			Console.Error("CreateSwapChainForHwnd failed: 0x%08X", hr);
			return false;
		}
	}

	hr = m_dxgi_factory->MakeWindowAssociation(window_hwnd, DXGI_MWA_NO_WINDOW_CHANGES);
	if (FAILED(hr))
		Console.Warning("MakeWindowAssociation() to disable ALT+ENTER failed");

	if (!CreateSwapChainRTV())
	{
		DestroySwapChain();
		return false;
	}

	// Render a frame as soon as possible to clear out whatever was previously being displayed.
	m_ctx->ClearRenderTargetView(m_swap_chain_rtv.get(), s_present_clear_color.data());
	m_swap_chain->Present(0, m_using_allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
	return true;
}

bool GSDevice11::CreateSwapChainRTV()
{
	wil::com_ptr_nothrow<ID3D11Texture2D> backbuffer;
	HRESULT hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(backbuffer.put()));
	if (FAILED(hr))
	{
		Console.Error("GetBuffer for RTV failed: 0x%08X", hr);
		return false;
	}

	D3D11_TEXTURE2D_DESC backbuffer_desc;
	backbuffer->GetDesc(&backbuffer_desc);

	CD3D11_RENDER_TARGET_VIEW_DESC rtv_desc(
		D3D11_RTV_DIMENSION_TEXTURE2D, backbuffer_desc.Format, 0, 0, backbuffer_desc.ArraySize);
	hr = m_dev->CreateRenderTargetView(backbuffer.get(), &rtv_desc, m_swap_chain_rtv.put());
	if (FAILED(hr))
	{
		Console.Error("CreateRenderTargetView for swap chain failed: 0x%08X", hr);
		m_swap_chain_rtv.reset();
		return false;
	}

	m_window_info.surface_width = backbuffer_desc.Width;
	m_window_info.surface_height = backbuffer_desc.Height;
	DevCon.WriteLn("Swap chain buffer size: %ux%u", m_window_info.surface_width, m_window_info.surface_height);

	if (m_window_info.type == WindowInfo::Type::Win32)
	{
		BOOL fullscreen = FALSE;
		DXGI_SWAP_CHAIN_DESC desc;
		if (SUCCEEDED(m_swap_chain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen &&
			SUCCEEDED(m_swap_chain->GetDesc(&desc)))
		{
			m_window_info.surface_refresh_rate = static_cast<float>(desc.BufferDesc.RefreshRate.Numerator) /
												 static_cast<float>(desc.BufferDesc.RefreshRate.Denominator);
		}
		else
		{
			m_window_info.surface_refresh_rate = 0.0f;
		}
	}

	return true;
}

void GSDevice11::DestroySwapChain()
{
	if (!m_swap_chain)
		return;

	m_swap_chain_rtv.reset();

	// switch out of fullscreen before destroying
	BOOL is_fullscreen;
	if (SUCCEEDED(m_swap_chain->GetFullscreenState(&is_fullscreen, nullptr)) && is_fullscreen)
		m_swap_chain->SetFullscreenState(FALSE, nullptr);

	m_swap_chain.reset();
	m_is_exclusive_fullscreen = false;
}

bool GSDevice11::UpdateWindow()
{
	DestroySwapChain();

	if (!AcquireWindow(false))
		return false;

	if (m_window_info.type != WindowInfo::Type::Surfaceless && !CreateSwapChain())
	{
		Console.WriteLn("Failed to create swap chain on updated window");
		ReleaseWindow();
		return false;
	}

	return true;
}

void GSDevice11::DestroySurface()
{
	DestroySwapChain();
}

std::string GSDevice11::GetDriverInfo() const
{
	std::string ret = "Unknown Feature Level";

	static constexpr std::array<std::tuple<D3D_FEATURE_LEVEL, const char*>, 4> feature_level_names = {{
		{D3D_FEATURE_LEVEL_10_0, "D3D_FEATURE_LEVEL_10_0"},
		{D3D_FEATURE_LEVEL_10_0, "D3D_FEATURE_LEVEL_10_1"},
		{D3D_FEATURE_LEVEL_11_0, "D3D_FEATURE_LEVEL_11_0"},
		{D3D_FEATURE_LEVEL_11_1, "D3D_FEATURE_LEVEL_11_1"},
	}};

	const D3D_FEATURE_LEVEL fl = m_dev->GetFeatureLevel();
	for (size_t i = 0; i < std::size(feature_level_names); i++)
	{
		if (fl == std::get<0>(feature_level_names[i]))
		{
			ret = std::get<1>(feature_level_names[i]);
			break;
		}
	}

	ret += "\n";

	wil::com_ptr_nothrow<IDXGIDevice> dxgi_dev;
	if (m_dev.try_query_to(&dxgi_dev))
	{
		wil::com_ptr_nothrow<IDXGIAdapter> dxgi_adapter;
		if (SUCCEEDED(dxgi_dev->GetAdapter(dxgi_adapter.put())))
		{
			DXGI_ADAPTER_DESC desc;
			if (SUCCEEDED(dxgi_adapter->GetDesc(&desc)))
			{
				ret += StringUtil::StdStringFromFormat("VID: 0x%04X PID: 0x%04X\n", desc.VendorId, desc.DeviceId);
				ret += StringUtil::WideStringToUTF8String(desc.Description);
				ret += "\n";

				const std::string driver_version(D3D::GetDriverVersionFromLUID(desc.AdapterLuid));
				if (!driver_version.empty())
				{
					ret += "Driver Version: ";
					ret += driver_version;
				}
			}
		}
	}

	return ret;
}

void GSDevice11::ResizeWindow(s32 new_window_width, s32 new_window_height, float new_window_scale)
{
	if (!m_swap_chain || m_is_exclusive_fullscreen)
		return;

	m_window_info.surface_scale = new_window_scale;

	if (m_window_info.surface_width == new_window_width && m_window_info.surface_height == new_window_height)
		return;

	m_swap_chain_rtv.reset();

	HRESULT hr = m_swap_chain->ResizeBuffers(
		0, 0, 0, DXGI_FORMAT_UNKNOWN, m_using_allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
	if (FAILED(hr))
		Console.Error("ResizeBuffers() failed: 0x%08X", hr);

	if (!CreateSwapChainRTV())
		pxFailRel("Failed to recreate swap chain RTV after resize");
}

bool GSDevice11::SupportsExclusiveFullscreen() const
{
	return true;
}

GSDevice::PresentResult GSDevice11::BeginPresent(bool frame_skip)
{
	if (frame_skip || !m_swap_chain)
		return PresentResult::FrameSkipped;

	// Check if we lost exclusive fullscreen. If so, notify the host, so it can switch to windowed mode.
	// This might get called repeatedly if it takes a while to switch back, that's the host's problem.
	BOOL is_fullscreen;
	if (m_is_exclusive_fullscreen &&
		(FAILED(m_swap_chain->GetFullscreenState(&is_fullscreen, nullptr)) || !is_fullscreen))
	{
		Host::RunOnCPUThread([]() { Host::SetFullscreen(false); });
		return PresentResult::FrameSkipped;
	}

	// When using vsync, the time here seems to include the time for the buffer to become available.
	// This blows our our GPU usage number considerably, so read the timestamp before the final blit
	// in this configuration. It does reduce accuracy a little, but better than seeing 100% all of
	// the time, when it's more like a couple of percent.
	if (m_vsync_mode != VsyncMode::Off && m_gpu_timing_enabled)
		PopTimestampQuery();

	m_ctx->ClearRenderTargetView(m_swap_chain_rtv.get(), s_present_clear_color.data());
	m_ctx->OMSetRenderTargets(1, m_swap_chain_rtv.addressof(), nullptr);
	if (m_state.rt_view)
		m_state.rt_view->Release();
	m_state.rt_view = m_swap_chain_rtv.get();
	m_state.rt_view->AddRef();
	if (m_state.dsv)
	{
		m_state.dsv->Release();
		m_state.dsv = nullptr;
	}

	g_perfmon.Put(GSPerfMon::RenderPasses, 1);

	const GSVector2i size = GetWindowSize();
	SetViewport(size);
	SetScissor(GSVector4i::loadh(size));

	return PresentResult::OK;
}

void GSDevice11::EndPresent()
{
	RenderImGui();

	// See note in BeginPresent() for why it's conditional on vsync-off.
	const bool vsync_on = m_vsync_mode != VsyncMode::Off;
	if (!vsync_on && m_gpu_timing_enabled)
		PopTimestampQuery();

	if (!vsync_on && m_using_allow_tearing)
		m_swap_chain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
	else
		m_swap_chain->Present(static_cast<UINT>(vsync_on), 0);

	if (m_gpu_timing_enabled)
		KickTimestampQuery();

	// clear out the swap chain view, it might get resized..
	OMSetRenderTargets(nullptr, nullptr, nullptr);
}

bool GSDevice11::CreateTimestampQueries()
{
	for (u32 i = 0; i < NUM_TIMESTAMP_QUERIES; i++)
	{
		for (u32 j = 0; j < 3; j++)
		{
			const CD3D11_QUERY_DESC qdesc((j == 0) ? D3D11_QUERY_TIMESTAMP_DISJOINT : D3D11_QUERY_TIMESTAMP);
			const HRESULT hr = m_dev->CreateQuery(&qdesc, m_timestamp_queries[i][j].put());
			if (FAILED(hr))
			{
				m_timestamp_queries = {};
				return false;
			}
		}
	}

	KickTimestampQuery();
	return true;
}

void GSDevice11::DestroyTimestampQueries()
{
	if (!m_timestamp_queries[0][0])
		return;

	if (m_timestamp_query_started)
		m_ctx->End(m_timestamp_queries[m_write_timestamp_query][1].get());

	m_timestamp_queries = {};
	m_read_timestamp_query = 0;
	m_write_timestamp_query = 0;
	m_waiting_timestamp_queries = 0;
	m_timestamp_query_started = 0;
}

void GSDevice11::PopTimestampQuery()
{
	while (m_waiting_timestamp_queries > 0)
	{
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
		const HRESULT disjoint_hr = m_ctx->GetData(m_timestamp_queries[m_read_timestamp_query][0].get(), &disjoint,
			sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (disjoint_hr != S_OK)
			break;

		if (disjoint.Disjoint)
		{
			DevCon.WriteLn("GPU timing disjoint, resetting.");
			m_read_timestamp_query = 0;
			m_write_timestamp_query = 0;
			m_waiting_timestamp_queries = 0;
			m_timestamp_query_started = 0;
		}
		else
		{
			u64 start = 0, end = 0;
			const HRESULT start_hr = m_ctx->GetData(m_timestamp_queries[m_read_timestamp_query][1].get(), &start,
				sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			const HRESULT end_hr = m_ctx->GetData(m_timestamp_queries[m_read_timestamp_query][2].get(), &end,
				sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (start_hr == S_OK && end_hr == S_OK)
			{
				m_accumulated_gpu_time += static_cast<float>(
					static_cast<double>(end - start) / (static_cast<double>(disjoint.Frequency) / 1000.0));
				m_read_timestamp_query = (m_read_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
				m_waiting_timestamp_queries--;
			}
		}
	}

	if (m_timestamp_query_started)
	{
		m_ctx->End(m_timestamp_queries[m_write_timestamp_query][2].get());
		m_ctx->End(m_timestamp_queries[m_write_timestamp_query][0].get());
		m_write_timestamp_query = (m_write_timestamp_query + 1) % NUM_TIMESTAMP_QUERIES;
		m_timestamp_query_started = false;
		m_waiting_timestamp_queries++;
	}
}

void GSDevice11::KickTimestampQuery()
{
	if (m_timestamp_query_started || !m_timestamp_queries[0][0] || m_waiting_timestamp_queries == NUM_TIMESTAMP_QUERIES)
		return;

	m_ctx->Begin(m_timestamp_queries[m_write_timestamp_query][0].get());
	m_ctx->End(m_timestamp_queries[m_write_timestamp_query][1].get());
	m_timestamp_query_started = true;
}

bool GSDevice11::SetGPUTimingEnabled(bool enabled)
{
	if (m_gpu_timing_enabled == enabled)
		return true;

	m_gpu_timing_enabled = enabled;
	if (m_gpu_timing_enabled)
	{
		return CreateTimestampQueries();
	}
	else
	{
		DestroyTimestampQueries();
		return true;
	}
}

float GSDevice11::GetAndResetAccumulatedGPUTime()
{
	const float value = m_accumulated_gpu_time;
	m_accumulated_gpu_time = 0.0f;
	return value;
}

void GSDevice11::DrawPrimitive()
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	PSUpdateShaderState();
	m_ctx->Draw(m_vertex.count, m_vertex.start);
}

void GSDevice11::DrawIndexedPrimitive()
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	PSUpdateShaderState();
	m_ctx->DrawIndexed(m_index.count, m_index.start, m_vertex.start);
}

void GSDevice11::DrawIndexedPrimitive(int offset, int count)
{
	ASSERT(offset + count <= (int)m_index.count);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	PSUpdateShaderState();
	m_ctx->DrawIndexed(count, m_index.start + offset, m_vertex.start);
}

void GSDevice11::ClearRenderTarget(GSTexture* t, const GSVector4& c)
{
	if (!t)
		return;
	m_ctx->ClearRenderTargetView(*(GSTexture11*)t, c.v);
}

void GSDevice11::ClearRenderTarget(GSTexture* t, u32 c)
{
	if (!t)
		return;
	const GSVector4 color = GSVector4::rgba32(c) * (1.0f / 255);

	m_ctx->ClearRenderTargetView(*(GSTexture11*)t, color.v);
}

void GSDevice11::InvalidateRenderTarget(GSTexture* t)
{
	if (t->IsDepthStencil())
		m_ctx->DiscardView(static_cast<ID3D11DepthStencilView*>(*static_cast<GSTexture11*>(t)));
	else
		m_ctx->DiscardView(static_cast<ID3D11RenderTargetView*>(*static_cast<GSTexture11*>(t)));
}

void GSDevice11::ClearDepth(GSTexture* t)
{
	m_ctx->ClearDepthStencilView(*(GSTexture11*)t, D3D11_CLEAR_DEPTH, 0.0f, 0);
}

void GSDevice11::ClearStencil(GSTexture* t, u8 c)
{
	m_ctx->ClearDepthStencilView(*(GSTexture11*)t, D3D11_CLEAR_STENCIL, 0, c);
}

void GSDevice11::PushDebugGroup(const char* fmt, ...)
{
	if (!m_annotation)
		return;

	std::va_list ap;
	va_start(ap, fmt);
	std::string str(StringUtil::StdStringFromFormatV(fmt, ap));
	va_end(ap);

	m_annotation->BeginEvent(StringUtil::UTF8StringToWideString(str).c_str());
}

void GSDevice11::PopDebugGroup()
{
	if (!m_annotation)
		return;

	m_annotation->EndEvent();
}

void GSDevice11::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
	if (!m_annotation)
		return;

	std::va_list ap;
	va_start(ap, fmt);
	std::string str(StringUtil::StdStringFromFormatV(fmt, ap));
	va_end(ap);

	m_annotation->SetMarker(StringUtil::UTF8StringToWideString(str).c_str());
}

GSTexture* GSDevice11::CreateSurface(GSTexture::Type type, int width, int height, int levels, GSTexture::Format format)
{
	D3D11_TEXTURE2D_DESC desc = {};

	// Texture limit for D3D10/11 min 1, max 8192 D3D10, max 16384 D3D11.
	desc.Width = std::clamp(width, 1, m_d3d_texsize);
	desc.Height = std::clamp(height, 1, m_d3d_texsize);
	desc.Format = GSTexture11::GetDXGIFormat(format);
	desc.MipLevels = levels;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;

	switch (type)
	{
		case GSTexture::Type::RenderTarget:
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			break;
		case GSTexture::Type::DepthStencil:
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			break;
		case GSTexture::Type::Texture:
			desc.BindFlags = (levels > 1 && !GSTexture::IsCompressedFormat(format)) ? (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) : D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = (levels > 1 && !GSTexture::IsCompressedFormat(format)) ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
			break;
		case GSTexture::Type::RWTexture:
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			break;
		default:
			break;
	}

	wil::com_ptr_nothrow<ID3D11Texture2D> texture;
	HRESULT hr = m_dev->CreateTexture2D(&desc, nullptr, texture.put());
	if (FAILED(hr))
	{
		Console.Error("DX11: Failed to allocate %dx%d surface", width, height);
		return nullptr;
	}

	return new GSTexture11(std::move(texture), desc, type, format);
}

std::unique_ptr<GSDownloadTexture> GSDevice11::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTexture11::Create(width, height, format);
}

void GSDevice11::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);

	D3D11_BOX box = {(UINT)r.left, (UINT)r.top, 0U, (UINT)r.right, (UINT)r.bottom, 1U};

	// DX api isn't happy if we pass a box for depth copy
	// It complains that depth/multisample must be a full copy
	// and asks us to use a NULL for the box
	const bool depth = (sTex->GetType() == GSTexture::Type::DepthStencil);
	auto pBox = depth ? nullptr : &box;

	m_ctx->CopySubresourceRegion(*(GSTexture11*)dTex, 0, destX, destY, 0, *(GSTexture11*)sTex, 0, pBox);
}

void GSDevice11::CloneTexture(GSTexture* src, GSTexture** dest, const GSVector4i& rect)
{
	pxAssertMsg(src->GetType() == GSTexture::Type::DepthStencil || src->GetType() == GSTexture::Type::RenderTarget, "Source is RT or DS.");

	const int w = src->GetWidth();
	const int h = src->GetHeight();

	if (src->GetType() == GSTexture::Type::DepthStencil)
	{
		// DX11 requires that you copy the entire depth buffer.
		*dest = CreateDepthStencil(w, h, src->GetFormat(), false);
		CopyRect(src, *dest, GSVector4i(0, 0, w, h), 0, 0);
	}
	else
	{
		*dest = CreateRenderTarget(w, h, src->GetFormat(), false);
		CopyRect(src, *dest, rect, rect.left, rect.top);
	}
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader, bool linear)
{
	pxAssert(dTex->IsDepthStencil() == HasDepthOutput(shader));
	pxAssert(linear ? SupportsBilinear(shader) : SupportsNearest(shader));
	StretchRect(sTex, sRect, dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), nullptr, linear);
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ID3D11PixelShader* ps, ID3D11Buffer* ps_cb, bool linear)
{
	StretchRect(sTex, sRect, dTex, dRect, ps, ps_cb, m_convert.bs[D3D11_COLOR_WRITE_ENABLE_ALL].get(), linear);
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha)
{
	const u8 index = static_cast<u8>(red) | (static_cast<u8>(green) << 1) | (static_cast<u8>(blue) << 2) |
					 (static_cast<u8>(alpha) << 3);
	StretchRect(sTex, sRect, dTex, dRect, m_convert.ps[static_cast<int>(ShaderConvert::COPY)].get(), nullptr,
		m_convert.bs[index].get(), false);
}

void GSDevice11::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ID3D11PixelShader* ps, ID3D11Buffer* ps_cb, ID3D11BlendState* bs, bool linear)
{
	ASSERT(sTex);

	const bool draw_in_depth = dTex && dTex->IsDepthStencil();

	GSVector2i ds;
	if (dTex)
	{
		ds = dTex->GetSize();
		if (draw_in_depth)
			OMSetRenderTargets(nullptr, dTex);
		else
			OMSetRenderTargets(dTex, nullptr);
	}
	else
	{
		ds = GSVector2i(m_window_info.surface_width, m_window_info.surface_height);

	}

	// om
	if (draw_in_depth)
		OMSetDepthStencilState(m_convert.dss_write.get(), 0);
	else
		OMSetDepthStencilState(m_convert.dss.get(), 0);

	OMSetBlendState(bs, 0);



	// ia

	const float left = dRect.x * 2 / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2 / ds.y;
	const float right = dRect.z * 2 / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2 / ds.y;

	GSVertexPT1 vertices[] =
	{
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
	};



    IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	IASetInputLayout(m_convert.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// vs

	VSSetShader(m_convert.vs.get(), nullptr);


	// ps

	PSSetShaderResources(sTex, nullptr);
	PSSetSamplerState(linear ? m_convert.ln.get() : m_convert.pt.get());
	PSSetShader(ps, ps_cb);

	//

	DrawPrimitive();

	//

	PSSetShaderResources(nullptr, nullptr);
}

void GSDevice11::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, PresentShader shader, float shaderTime, bool linear)
{
	ASSERT(sTex);

	GSVector2i ds;
	if (dTex)
	{
		ds = dTex->GetSize();
		OMSetRenderTargets(dTex, nullptr);
	}
	else
	{
		ds = GSVector2i(m_window_info.surface_width, m_window_info.surface_height);
	}

	DisplayConstantBuffer cb;
	cb.SetSource(sRect, sTex->GetSize());
	cb.SetTarget(dRect, ds);
	cb.SetTime(shaderTime);
	m_ctx->UpdateSubresource(m_present.ps_cb.get(), 0, nullptr, &cb, 0, 0);

	// om
	OMSetDepthStencilState(m_convert.dss.get(), 0);
	OMSetBlendState(m_convert.bs[D3D11_COLOR_WRITE_ENABLE_ALL].get(), 0);



	// ia

	const float left = dRect.x * 2 / ds.x - 1.0f;
	const float top = 1.0f - dRect.y * 2 / ds.y;
	const float right = dRect.z * 2 / ds.x - 1.0f;
	const float bottom = 1.0f - dRect.w * 2 / ds.y;

	GSVertexPT1 vertices[] =
	{
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)},
	};



	IASetVertexBuffer(vertices, sizeof(vertices[0]), std::size(vertices));
	IASetInputLayout(m_present.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// vs

	VSSetShader(m_present.vs.get(), nullptr);


	// ps

	PSSetShaderResources(sTex, nullptr);
	PSSetSamplerState(linear ? m_convert.ln.get() : m_convert.pt.get());
	PSSetShader(m_present.ps[static_cast<u32>(shader)].get(), m_present.ps_cb.get());

	//

	DrawPrimitive();

	//

	PSSetShaderResources(nullptr, nullptr);
}

void GSDevice11::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize)
{
	// match merge cb
	struct Uniforms
	{
		float scale;
		float pad1[3];
		u32 offsetX, offsetY, dOffset;
	};
	const Uniforms cb = {sScale, {}, offsetX, offsetY, dOffset};
	m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &cb, 0, 0);

	const GSVector4 dRect(0, 0, dSize, 1);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	StretchRect(sTex, GSVector4::zero(), dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), m_merge.cb.get(), nullptr, false);
}

void GSDevice11::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	// match merge cb
	struct Uniforms
	{
		float scale;
		float pad1[3];
		u32 SBW, DBW, pad3;
	};

	const Uniforms cb = {sScale, {}, SBW, DBW};
	m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &cb, 0, 0);

	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	const ShaderConvert shader = ShaderConvert::RGBA_TO_8I;
	StretchRect(sTex, GSVector4::zero(), dTex, dRect, m_convert.ps[static_cast<int>(shader)].get(), m_merge.cb.get(), nullptr, false);
}

void GSDevice11::DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex, ShaderConvert shader)
{
	IASetInputLayout(m_convert.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	VSSetShader(m_convert.vs.get(), nullptr);
	PSSetShader(m_convert.ps[static_cast<int>(shader)].get(), nullptr);

	OMSetDepthStencilState(dTex->IsRenderTarget() ? m_convert.dss.get() : m_convert.dss_write.get(), 0);
	OMSetRenderTargets(dTex->IsRenderTarget() ? dTex : nullptr, dTex->IsDepthStencil() ? dTex : nullptr);

	const GSVector2 ds(static_cast<float>(dTex->GetWidth()), static_cast<float>(dTex->GetHeight()));
	GSTexture* last_tex = rects[0].src;
	bool last_linear = rects[0].linear;
	u8 last_wmask = rects[0].wmask.wrgba;

	u32 first = 0;
	u32 count = 1;

	for (u32 i = 1; i < num_rects; i++)
	{
		if (rects[i].src == last_tex && rects[i].linear == last_linear && rects[i].wmask.wrgba == last_wmask)
		{
			count++;
			continue;
		}

		DoMultiStretchRects(rects + first, count, ds);
		last_tex = rects[i].src;
		last_linear = rects[i].linear;
		last_wmask = rects[i].wmask.wrgba;
		first += count;
		count = 1;
	}

	DoMultiStretchRects(rects + first, count, ds);
}

void GSDevice11::DoMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, const GSVector2& ds)
{
	// Don't use primitive restart here, it ends up slower on some drivers.
	const u32 vertex_reserve_size = num_rects * 4;
	const u32 index_reserve_size = num_rects * 6;
	GSVertexPT1* verts = static_cast<GSVertexPT1*>(IAMapVertexBuffer(sizeof(GSVertexPT1), vertex_reserve_size));
	u16* idx = IAMapIndexBuffer(index_reserve_size);
	u32 icount = 0;
	u32 vcount = 0;
	for (u32 i = 0; i < num_rects; i++)
	{
		const GSVector4& sRect = rects[i].src_rect;
		const GSVector4& dRect = rects[i].dst_rect;
		const float left = dRect.x * 2 / ds.x - 1.0f;
		const float top = 1.0f - dRect.y * 2 / ds.y;
		const float right = dRect.z * 2 / ds.x - 1.0f;
		const float bottom = 1.0f - dRect.w * 2 / ds.y;

		const u32 vstart = vcount;
		verts[vcount++] = {GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y)};
		verts[vcount++] = {GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y)};
		verts[vcount++] = {GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w)};
		verts[vcount++] = {GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w)};

		if (i > 0)
			idx[icount++] = vstart;

		idx[icount++] = vstart;
		idx[icount++] = vstart + 1;
		idx[icount++] = vstart + 2;
		idx[icount++] = vstart + 3;
		idx[icount++] = vstart + 3;
	};

	IAUnmapVertexBuffer(sizeof(GSVertexPT1), vcount);
	IAUnmapIndexBuffer(icount);
	IASetIndexBuffer(m_ib.get());

	PSSetShaderResource(0, rects[0].src);
	PSSetSamplerState(rects[0].linear ? m_convert.ln.get() : m_convert.pt.get());

	OMSetBlendState(m_convert.bs[rects[0].wmask.wrgba].get(), 0.0f);

	DrawIndexedPrimitive();
}


void GSDevice11::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, const GSVector4& c, const bool linear)
{
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;

	// Merge the 2 source textures (sTex[0],sTex[1]). Final results go to dTex. Feedback write will go to sTex[2].
	// If either 2nd output is disabled or SLBG is 1, a background color will be used.
	// Note: background color is also used when outside of the unit rectangle area
	ClearRenderTarget(dTex, c);

	// Upload constant to select YUV algo, but skip constant buffer update if we don't need it
	if (feedback_write_2 || feedback_write_1 || sTex[0])
	{
		const MergeConstantBuffer cb = {c, EXTBUF.EMODA, EXTBUF.EMODC};
		m_ctx->UpdateSubresource(m_merge.cb.get(), 0, nullptr, &cb, 0, 0);
	}

	if (sTex[1] && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output
		// Note: value outside of dRect must contains the background color (c)
		StretchRect(sTex[1], sRect[1], dTex, PMODE.SLBG ? dRect[2] : dRect[1], ShaderConvert::COPY, linear);
	}

	// Save 2nd output
	if (feedback_write_2)
	{
		StretchRect(dTex, full_r, sTex[2], dRect[2], m_convert.ps[static_cast<int>(ShaderConvert::YUV)].get(),
			m_merge.cb.get(), nullptr, linear);
	}

	// Restore background color to process the normal merge
	if (feedback_write_2_but_blend_bg)
		ClearRenderTarget(dTex, c);

	if (sTex[0])
	{
		// 1st output is enabled. It must be blended
		StretchRect(sTex[0], sRect[0], dTex, dRect[0], m_merge.ps[PMODE.MMOD].get(), m_merge.cb.get(), m_merge.bs.get(), linear);
	}

	if (feedback_write_1)
	{
		StretchRect(sTex[0], full_r, sTex[2], dRect[2], m_convert.ps[static_cast<int>(ShaderConvert::YUV)].get(),
			m_merge.cb.get(), nullptr, linear);
	}
}

void GSDevice11::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer& cb)
{
	m_ctx->UpdateSubresource(m_interlace.cb.get(), 0, nullptr, &cb, 0, 0);

	StretchRect(sTex, sRect, dTex, dRect, m_interlace.ps[static_cast<int>(shader)].get(), m_interlace.cb.get(), linear);
}

void GSDevice11::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
	const GSVector2i s = dTex->GetSize();

	const GSVector4 sRect(0, 0, 1, 1);
	const GSVector4 dRect(0, 0, s.x, s.y);

	if (!m_fxaa_ps)
	{
		std::optional<std::string> shader = Host::ReadResourceFileToString("shaders/common/fxaa.fx");
		if (!shader.has_value())
		{
			Console.Error("FXAA shader is missing");
			return;
		}

		ShaderMacro sm(m_shader_cache.GetFeatureLevel());
		m_fxaa_ps = m_shader_cache.GetPixelShader(m_dev.get(), *shader, sm.GetPtr(), "ps_main");
		if (!m_fxaa_ps)
			return;
	}

	StretchRect(sTex, sRect, dTex, dRect, m_fxaa_ps.get(), nullptr, true);

	//sTex->Save("c:\\temp1\\1.bmp");
	//dTex->Save("c:\\temp1\\2.bmp");
}

void GSDevice11::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
	const GSVector2i s = dTex->GetSize();

	const GSVector4 sRect(0, 0, 1, 1);
	const GSVector4 dRect(0, 0, s.x, s.y);

	m_ctx->UpdateSubresource(m_shadeboost.cb.get(), 0, nullptr, params, 0, 0);

	StretchRect(sTex, sRect, dTex, dRect, m_shadeboost.ps.get(), m_shadeboost.cb.get(), false);
}

bool GSDevice11::CreateCASShaders()
{
	CD3D11_BUFFER_DESC desc(NUM_CAS_CONSTANTS * sizeof(u32), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT);
	HRESULT hr = m_dev->CreateBuffer(&desc, nullptr, m_cas.cb.put());
	if (FAILED(hr))
		return false;

	std::optional<std::string> cas_source(Host::ReadResourceFileToString("shaders/dx11/cas.hlsl"));
	if (!cas_source.has_value() || !GetCASShaderSource(&cas_source.value()))
		return false;

	static constexpr D3D_SHADER_MACRO sharpen_only_macros[] = {
		{"CAS_SHARPEN_ONLY", "1"},
		{nullptr, nullptr}};

	m_cas.cs_sharpen = m_shader_cache.GetComputeShader(m_dev.get(), cas_source.value(), sharpen_only_macros, "main");
	m_cas.cs_upscale = m_shader_cache.GetComputeShader(m_dev.get(), cas_source.value(), nullptr, "main");
	if (!m_cas.cs_sharpen || !m_cas.cs_upscale)
		return false;

	m_features.cas_sharpening = true;
	return true;
}

bool GSDevice11::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only, const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	static const int threadGroupWorkRegionDim = 16;
	const int dispatchX = (dTex->GetWidth() + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
	const int dispatchY = (dTex->GetHeight() + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;

	ID3D11ShaderResourceView* srvs[1] = {*static_cast<GSTexture11*>(sTex)};
	ID3D11UnorderedAccessView* uavs[1] = {*static_cast<GSTexture11*>(dTex)};
	m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
	m_ctx->UpdateSubresource(m_cas.cb.get(), 0, nullptr, constants.data(), 0, 0);
	m_ctx->CSSetConstantBuffers(0, 1, m_cas.cb.addressof());
	m_ctx->CSSetShader(sharpen_only ? m_cas.cs_sharpen.get() : m_cas.cs_upscale.get(), nullptr, 0);
	m_ctx->CSSetShaderResources(0, std::size(srvs), srvs);
	m_ctx->CSSetUnorderedAccessViews(0, std::size(uavs), uavs, nullptr);
	m_ctx->Dispatch(dispatchX, dispatchY, 1);

	// clear bindings out to prevent hazards
	uavs[0] = nullptr;
	srvs[0] = nullptr;
	m_ctx->CSSetShaderResources(0, std::size(srvs), srvs);
	m_ctx->CSSetUnorderedAccessViews(0, std::size(uavs), uavs, nullptr);

	return true;
}

bool GSDevice11::CreateImGuiResources()
{
	HRESULT hr;

	const std::optional<std::string> hlsl = Host::ReadResourceFileToString("shaders/dx11/imgui.fx");
	if (!hlsl.has_value())
	{
		Console.Error("Failed to read imgui.fx");
		return false;
	}

	// clang-format off
	static constexpr D3D11_INPUT_ELEMENT_DESC layout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (UINT)IM_OFFSETOF(ImDrawVert, pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (UINT)IM_OFFSETOF(ImDrawVert, uv),  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)IM_OFFSETOF(ImDrawVert, col), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	// clang-format on

	if (!m_shader_cache.GetVertexShaderAndInputLayout(m_dev.get(), m_imgui.vs.put(), m_imgui.il.put(), layout,
			std::size(layout), hlsl.value(), nullptr, "vs_main") ||
		!(m_imgui.ps = m_shader_cache.GetPixelShader(m_dev.get(), hlsl.value(), nullptr, "ps_main")))
	{
		Console.Error("Failed to compile ImGui shaders");
		return false;
	}

	D3D11_BLEND_DESC blend_desc = {};
	blend_desc.RenderTarget[0].BlendEnable = true;
	blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = m_dev->CreateBlendState(&blend_desc, m_imgui.bs.put());
	if (FAILED(hr))
	{
		Console.Error("CreateImGuiResources(): CreateBlendState() failed: %08X", hr);
		return false;
	}

	D3D11_BUFFER_DESC buffer_desc = {};
	buffer_desc.Usage = D3D11_USAGE_DEFAULT;
	buffer_desc.ByteWidth = sizeof(float) * 4 * 4;
	buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = m_dev->CreateBuffer(&buffer_desc, nullptr, m_imgui.vs_cb.put());
	if (FAILED(hr))
	{
		Console.Error("CreateImGuiResources(): CreateBlendState() failed: %08X", hr);
		return false;
	}

	return true;
}

void GSDevice11::RenderImGui()
{
	ImGui::Render();
	const ImDrawData* draw_data = ImGui::GetDrawData();
	if (draw_data->CmdListsCount == 0)
		return;

	const float L = 0.0f;
	const float R = static_cast<float>(m_window_info.surface_width);
	const float T = 0.0f;
	const float B = static_cast<float>(m_window_info.surface_height);

	// clang-format off
  const float ortho_projection[4][4] =
	{
		{ 2.0f/(R-L),   0.0f,           0.0f,       0.0f },
		{ 0.0f,         2.0f/(T-B),     0.0f,       0.0f },
		{ 0.0f,         0.0f,           0.5f,       0.0f },
		{ (R+L)/(L-R),  (T+B)/(B-T),    0.5f,       1.0f },
	};
	// clang-format on

	m_ctx->UpdateSubresource(m_imgui.vs_cb.get(), 0, nullptr, ortho_projection, 0, 0);

	const UINT vb_stride = sizeof(ImDrawVert);
	const UINT vb_offset = 0;
	m_ctx->IASetVertexBuffers(0, 1, m_vb.addressof(), &vb_stride, &vb_offset);
	IASetInputLayout(m_imgui.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	VSSetShader(m_imgui.vs.get(), m_imgui.vs_cb.get());
	PSSetShader(m_imgui.ps.get(), nullptr);
	OMSetBlendState(m_imgui.bs.get(), 0.0f);
	OMSetDepthStencilState(m_convert.dss.get(), 0);
	PSSetSamplerState(m_convert.ln.get());

	// Render command lists
	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];

		// This mess is because the vertex size isn't the same...
		u32 vertex_offset;
		{
			static_assert(Common::IsPow2(sizeof(GSVertexPT1)));

			D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;

			const u32 unaligned_size = cmd_list->VtxBuffer.Size * sizeof(ImDrawVert);
			u32 start_pos = Common::AlignUp(m_vb_pos, sizeof(ImDrawVert));
			u32 end_pos = Common::AlignUpPow2(start_pos + unaligned_size, sizeof(GSVertexPT1));
			if (end_pos > VERTEX_BUFFER_SIZE)
			{
				type = D3D11_MAP_WRITE_DISCARD;
				m_vb_pos = 0;
				start_pos = 0;
				end_pos = Common::AlignUpPow2(unaligned_size, sizeof(GSVertexPT1));
			}

			m_vb_pos = end_pos;
			vertex_offset = start_pos / sizeof(ImDrawVert);

			D3D11_MAPPED_SUBRESOURCE sr;
			const HRESULT hr = m_ctx->Map(m_vb.get(), 0, type, 0, &sr);
			if (FAILED(hr))
				continue;

			std::memcpy(static_cast<u8*>(sr.pData) + start_pos, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
			m_ctx->Unmap(m_vb.get(), 0);
		}

		static_assert(sizeof(ImDrawIdx) == sizeof(u16));
		IASetIndexBuffer(cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size);

		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
			pxAssert(!pcmd->UserCallback);

			const GSVector4 clip = GSVector4::load<false>(&pcmd->ClipRect);
			if ((clip.zwzw() <= clip.xyxy()).mask() != 0)
				continue;

			const GSVector4i iclip = GSVector4i(clip);
			if (!m_state.scissor.eq(iclip))
			{
				m_state.scissor = iclip;
				m_ctx->RSSetScissorRects(1, reinterpret_cast<const D3D11_RECT*>(&iclip));
			}

			// Since we don't have the GSTexture...
			m_state.ps_sr_views[0] = static_cast<ID3D11ShaderResourceView*>(pcmd->GetTexID());
			PSUpdateShaderState();

			m_ctx->DrawIndexed(pcmd->ElemCount, m_index.start + pcmd->IdxOffset, vertex_offset + pcmd->VtxOffset);
		}

		g_perfmon.Put(GSPerfMon::DrawCalls, cmd_list->CmdBuffer.Size);
	}

	m_ctx->IASetVertexBuffers(0, 1, m_vb.addressof(), &m_state.vb_stride, &vb_offset);
}

void GSDevice11::SetupDATE(GSTexture* rt, GSTexture* ds, const GSVertexPT1* vertices, bool datm)
{
	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows

	ClearStencil(ds, 0);

	// om

	OMSetDepthStencilState(m_date.dss.get(), 1);
	OMSetBlendState(m_date.bs.get(), 0);
	OMSetRenderTargets(nullptr, ds);

	// ia

	IASetVertexBuffer(vertices, sizeof(vertices[0]), 4);
	IASetInputLayout(m_convert.il.get());
	IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// vs

	VSSetShader(m_convert.vs.get(), nullptr);

	// ps
	PSSetShaderResources(rt, nullptr);
	PSSetSamplerState(m_convert.pt.get());
	PSSetShader(m_convert.ps[static_cast<int>(datm ? ShaderConvert::DATM_1 : ShaderConvert::DATM_0)].get(), nullptr);

	//

	DrawPrimitive();

	//
}

void* GSDevice11::IAMapVertexBuffer(u32 stride, u32 count)
{
	const u32 size = stride * count;
	if (size > VERTEX_BUFFER_SIZE)
		return nullptr;

	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;

	m_vertex.start = (m_vb_pos + (stride - 1)) / stride;
	m_vb_pos = (m_vertex.start * stride) + size;
	if (m_vb_pos > VERTEX_BUFFER_SIZE)
	{
		m_vertex.start = 0;
		m_vb_pos = size;
		type = D3D11_MAP_WRITE_DISCARD;
	}

	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(m_ctx->Map(m_vb.get(), 0, type, 0, &m)))
		return nullptr;

	return static_cast<u8*>(m.pData) + (m_vertex.start * stride);
}

void GSDevice11::IAUnmapVertexBuffer(u32 stride, u32 count)
{
	m_ctx->Unmap(m_vb.get(), 0);

	if (m_state.vb_stride != stride)
	{
		m_state.vb_stride = stride;
		const UINT vb_offset = 0;
		m_ctx->IASetVertexBuffers(0, 1, m_vb.addressof(), &stride, &vb_offset);
	}

	m_vertex.count = count;
}

bool GSDevice11::IASetVertexBuffer(const void* vertex, u32 stride, u32 count)
{
	void* map = IAMapVertexBuffer(stride, count);
	if (!map)
		return false;

	GSVector4i::storent(map, vertex, count * stride);

	IAUnmapVertexBuffer(stride, count);
	return true;
}

bool GSDevice11::IASetExpandVertexBuffer(const void* vertex, u32 stride, u32 count)
{
	const u32 size = stride * count;
	if (size > VERTEX_BUFFER_SIZE)
		return false;

	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;

	m_vertex.start = (m_structured_vb_pos + (stride - 1)) / stride;
	m_structured_vb_pos = (m_vertex.start * stride) + size;
	if (m_structured_vb_pos > VERTEX_BUFFER_SIZE)
	{
		m_vertex.start = 0;
		m_structured_vb_pos = size;
		type = D3D11_MAP_WRITE_DISCARD;
	}

	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(m_ctx->Map(m_expand_vb.get(), 0, type, 0, &m)))
		return false;

	void* map = static_cast<u8*>(m.pData) + (m_vertex.start * stride);

	GSVector4i::storent(map, vertex, count * stride);

	m_ctx->Unmap(m_expand_vb.get(), 0);

	m_vertex.count = count;
	return true;
}

u16* GSDevice11::IAMapIndexBuffer(u32 count)
{
	if (count > (INDEX_BUFFER_SIZE / sizeof(u16)))
		return nullptr;

	D3D11_MAP type = D3D11_MAP_WRITE_NO_OVERWRITE;

	m_index.start = m_ib_pos;
	m_ib_pos += count;

	if (m_ib_pos > (INDEX_BUFFER_SIZE / sizeof(u16)))
	{
		m_index.start = 0;
		m_ib_pos = count;
		type = D3D11_MAP_WRITE_DISCARD;
	}

	D3D11_MAPPED_SUBRESOURCE m;
	if (FAILED(m_ctx->Map(m_ib.get(), 0, type, 0, &m)))
		return nullptr;

	return static_cast<u16*>(m.pData) + m_index.start;
}

void GSDevice11::IAUnmapIndexBuffer(u32 count)
{
	m_ctx->Unmap(m_ib.get(), 0);
	m_index.count = count;
}

bool GSDevice11::IASetIndexBuffer(const void* index, u32 count)
{
	u16* map = IAMapIndexBuffer(count);
	if (!map)
		return false;

	std::memcpy(map, index, count * sizeof(u16));
	IAUnmapIndexBuffer(count);
	IASetIndexBuffer(m_ib.get());
	return true;
}

void GSDevice11::IASetIndexBuffer(ID3D11Buffer* buffer)
{
	if (m_state.index_buffer != buffer)
	{
		m_ctx->IASetIndexBuffer(buffer, DXGI_FORMAT_R16_UINT, 0);
		m_state.index_buffer = buffer;
	}
}

void GSDevice11::IASetInputLayout(ID3D11InputLayout* layout)
{
	if (m_state.layout != layout)
	{
		m_state.layout = layout;

		m_ctx->IASetInputLayout(layout);
	}
}

void GSDevice11::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology)
{
	if (m_state.topology != topology)
	{
		m_state.topology = topology;

		m_ctx->IASetPrimitiveTopology(topology);
	}
}

void GSDevice11::VSSetShader(ID3D11VertexShader* vs, ID3D11Buffer* vs_cb)
{
	if (m_state.vs != vs)
	{
		m_state.vs = vs;

		m_ctx->VSSetShader(vs, nullptr, 0);
	}

	if (m_state.vs_cb != vs_cb)
	{
		m_state.vs_cb = vs_cb;

		m_ctx->VSSetConstantBuffers(0, 1, &vs_cb);
	}
}

void GSDevice11::PSSetShaderResources(GSTexture* sr0, GSTexture* sr1)
{
	PSSetShaderResource(0, sr0);
	PSSetShaderResource(1, sr1);
	PSSetShaderResource(2, nullptr);
}

void GSDevice11::PSSetShaderResource(int i, GSTexture* sr)
{
	m_state.ps_sr_views[i] = sr ? static_cast<ID3D11ShaderResourceView*>(*static_cast<GSTexture11*>(sr)) : nullptr;
}

void GSDevice11::PSSetSamplerState(ID3D11SamplerState* ss0)
{
	m_state.ps_ss[0] = ss0;
}

void GSDevice11::PSSetShader(ID3D11PixelShader* ps, ID3D11Buffer* ps_cb)
{
	if (m_state.ps != ps)
	{
		m_state.ps = ps;

		m_ctx->PSSetShader(ps, nullptr, 0);
	}

	if (m_state.ps_cb != ps_cb)
	{
		m_state.ps_cb = ps_cb;

		m_ctx->PSSetConstantBuffers(0, 1, &ps_cb);
	}
}

void GSDevice11::PSUpdateShaderState()
{
	m_ctx->PSSetShaderResources(0, m_state.ps_sr_views.size(), m_state.ps_sr_views.data());
	m_ctx->PSSetSamplers(0, m_state.ps_ss.size(), m_state.ps_ss.data());
}

void GSDevice11::OMSetDepthStencilState(ID3D11DepthStencilState* dss, u8 sref)
{
	if (m_state.dss != dss || m_state.sref != sref)
	{
		m_state.dss = dss;
		m_state.sref = sref;

		m_ctx->OMSetDepthStencilState(dss, sref);
	}
}

void GSDevice11::OMSetBlendState(ID3D11BlendState* bs, float bf)
{
	if (m_state.bs != bs || m_state.bf != bf)
	{
		m_state.bs = bs;
		m_state.bf = bf;

		const float BlendFactor[] = {bf, bf, bf, 0};

		m_ctx->OMSetBlendState(bs, BlendFactor, 0xffffffff);
	}
}

void GSDevice11::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i* scissor)
{
	ID3D11RenderTargetView* rtv = nullptr;
	ID3D11DepthStencilView* dsv = nullptr;

	if (rt) rtv = *(GSTexture11*)rt;
	if (ds) dsv = *(GSTexture11*)ds;

	const bool changed = (m_state.rt_view != rtv || m_state.dsv != dsv);
	g_perfmon.Put(GSPerfMon::RenderPasses, static_cast<double>(changed));

	if (m_state.rt_view != rtv)
	{
		if (m_state.rt_view)
			m_state.rt_view->Release();
		if (rtv)
			rtv->AddRef();
		m_state.rt_view = rtv;
	}
	if (m_state.dsv != dsv)
	{
		if (m_state.dsv)
			m_state.dsv->Release();
		if (dsv)
			dsv->AddRef();
		m_state.dsv = dsv;
	}
	if (changed)
		m_ctx->OMSetRenderTargets(1, &rtv, dsv);

	if (rt || ds)
	{
		const GSVector2i size = rt ? rt->GetSize() : ds->GetSize();
		SetViewport(size);
		SetScissor(scissor ? *scissor : GSVector4i::loadh(size));
	}
}

void GSDevice11::SetViewport(const GSVector2i& viewport)
{
	if (m_state.viewport != viewport)
	{
		m_state.viewport = viewport;

		const D3D11_VIEWPORT vp = {
			0.0f, 0.0f, static_cast<float>(viewport.x), static_cast<float>(viewport.y), 0.0f, 1.0f};
		m_ctx->RSSetViewports(1, &vp);
	}
}

void GSDevice11::SetScissor(const GSVector4i& scissor)
{
	static_assert(sizeof(D3D11_RECT) == sizeof(GSVector4i));

	if (!m_state.scissor.eq(scissor))
	{
		m_state.scissor = scissor;
		m_ctx->RSSetScissorRects(1, reinterpret_cast<const D3D11_RECT*>(&scissor));
	}
}

GSDevice11::ShaderMacro::ShaderMacro(D3D_FEATURE_LEVEL fl)
{
	switch (fl)
	{
	case D3D_FEATURE_LEVEL_10_0:
		mlist.emplace_back("SHADER_MODEL", "0x400");
		break;
	case D3D_FEATURE_LEVEL_10_1:
		mlist.emplace_back("SHADER_MODEL", "0x401");
		break;
	case D3D_FEATURE_LEVEL_11_0:
	default:
		mlist.emplace_back("SHADER_MODEL", "0x500");
		break;
	}
}

void GSDevice11::ShaderMacro::AddMacro(const char* n, int d)
{
	AddMacro(n, std::to_string(d));
}

void GSDevice11::ShaderMacro::AddMacro(const char* n, std::string d)
{
	mlist.emplace_back(n, std::move(d));
}

D3D_SHADER_MACRO* GSDevice11::ShaderMacro::GetPtr(void)
{
	mout.clear();

	for (auto& i : mlist)
		mout.emplace_back(i.name.c_str(), i.def.c_str());

	mout.emplace_back(nullptr, nullptr);
	return (D3D_SHADER_MACRO*)mout.data();
}

static GSDevice11::OMBlendSelector convertSel(GSHWDrawConfig::ColorMaskSelector cm, GSHWDrawConfig::BlendState blend)
{
	GSDevice11::OMBlendSelector out;
	out.wrgba = cm.wrgba;
	if (blend.enable)
	{
		out.blend_enable = true;
		out.blend_src_factor = blend.src_factor;
		out.blend_dst_factor = blend.dst_factor;
		out.blend_op = blend.op;
	}

	return out;
}

/// Checks that we weren't sent things we declared we don't support
/// Clears things we don't support that can be quietly disabled
static void preprocessSel(GSDevice11::PSSelector& sel)
{
	ASSERT(sel.write_rg  == 0); // Not supported, shouldn't be sent
}

void GSDevice11::RenderHW(GSHWDrawConfig& config)
{
	ASSERT(!config.require_full_barrier); // We always specify no support so it shouldn't request this
	preprocessSel(config.ps);

	GSVector2i rtsize = (config.rt ? config.rt : config.ds)->GetSize();

	GSTexture* primid_tex = nullptr;
	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		primid_tex = CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::PrimID, false);
		StretchRect(config.rt, GSVector4(config.drawarea) / GSVector4(rtsize).xyxy(),
			primid_tex, GSVector4(config.drawarea), m_date.primid_init_ps[config.datm].get(), nullptr, false);
	}
	else if (config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off)
	{
		const GSVector4 src = GSVector4(config.drawarea) / GSVector4(config.ds->GetSize()).xyxy();
		const GSVector4 dst = src * 2.0f - 1.0f;

		GSVertexPT1 vertices[] =
		{
			{GSVector4(dst.x, -dst.y, 0.5f, 1.0f), GSVector2(src.x, src.y)},
			{GSVector4(dst.z, -dst.y, 0.5f, 1.0f), GSVector2(src.z, src.y)},
			{GSVector4(dst.x, -dst.w, 0.5f, 1.0f), GSVector2(src.x, src.w)},
			{GSVector4(dst.z, -dst.w, 0.5f, 1.0f), GSVector2(src.z, src.w)},
		};

		SetupDATE(config.rt, config.ds, vertices, config.datm);
	}

	GSTexture* hdr_rt = nullptr;
	if (config.ps.hdr)
	{
		const GSVector4 dRect(config.drawarea);
		const GSVector4 sRect = dRect / GSVector4(rtsize.x, rtsize.y).xyxy();
		hdr_rt = CreateRenderTarget(rtsize.x, rtsize.y, GSTexture::Format::HDRColor);
		// Warning: StretchRect must be called before BeginScene otherwise
		// vertices will be overwritten. Trust me you don't want to do that.
		StretchRect(config.rt, sRect, hdr_rt, dRect, ShaderConvert::HDR_INIT, false);
		g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	}

	if (config.vs.expand != GSHWDrawConfig::VSExpand::None)
	{
		if (!IASetExpandVertexBuffer(config.verts, sizeof(*config.verts), config.nverts))
		{
			Console.Error("Failed to upload structured vertices (%u)", config.nverts);
			return;
		}

		config.cb_vs.max_depth.y = m_vertex.start;
	}
	else
	{
		if (!IASetVertexBuffer(config.verts, sizeof(*config.verts), config.nverts))
		{
			Console.Error("Failed to upload vertices (%u)", config.nverts);
			return;
		}
	}

	if (config.vs.UseExpandIndexBuffer())
	{
		IASetIndexBuffer(m_expand_ib.get());
		m_index.start = 0;
		m_index.count = config.nindices;
	}
	else
	{
		if (!IASetIndexBuffer(config.indices, config.nindices))
		{
			Console.Error("Failed to upload indices (%u)", config.nindices);
			return;
		}
	}

	D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	switch (config.topology)
	{
		case GSHWDrawConfig::Topology::Point:    topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;    break;
		case GSHWDrawConfig::Topology::Line:     topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;     break;
		case GSHWDrawConfig::Topology::Triangle: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
	}
	IASetPrimitiveTopology(topology);

	PSSetShaderResources(config.tex, config.pal);

	GSTexture* rt_copy = nullptr;
	if (config.require_one_barrier || (config.tex && config.tex == config.rt)) // Used as "bind rt" flag when texture barrier is unsupported
	{
		// Bind the RT.This way special effect can use it.
		// Do not always bind the rt when it's not needed,
		// only bind it when effects use it such as fbmask emulation currently
		// because we copy the frame buffer and it is quite slow.
		CloneTexture(config.rt, &rt_copy, config.drawarea);
		if (rt_copy)
		{
			if (config.require_one_barrier)
				PSSetShaderResource(2, rt_copy);
			if (config.tex && config.tex == config.rt)
				PSSetShaderResource(0, rt_copy);
		}
	}

	SetupVS(config.vs, &config.cb_vs);
	SetupPS(config.ps, &config.cb_ps, config.sampler);

	if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking)
	{
		OMDepthStencilSelector dss = config.depth;
		dss.zwe = 0;
		OMBlendSelector blend;
		blend.wrgba = 0;
		blend.wr = 1;
		blend.blend_enable = 1;
		blend.blend_src_factor = CONST_ONE;
		blend.blend_dst_factor = CONST_ONE;
		blend.blend_op = 3; // MIN
		SetupOM(dss, blend, 0);
		OMSetRenderTargets(primid_tex, config.ds, &config.scissor);
		DrawIndexedPrimitive();

		config.ps.date = 3;
		config.alpha_second_pass.ps.date = 3;
		SetupPS(config.ps, nullptr, config.sampler);
		PSSetShaderResource(3, primid_tex);
	}

	SetupOM(config.depth, convertSel(config.colormask, config.blend), config.blend.constant);
	OMSetRenderTargets(hdr_rt ? hdr_rt : config.rt, config.ds, &config.scissor);
	DrawIndexedPrimitive();

	if (config.separate_alpha_pass)
	{
		GSHWDrawConfig::BlendState sap_blend = {};
		SetHWDrawConfigForAlphaPass(&config.ps, &config.colormask, &sap_blend, &config.depth);
		SetupOM(config.depth, convertSel(config.colormask, sap_blend), config.blend.constant);
		SetupPS(config.ps, &config.cb_ps, config.sampler);
		DrawIndexedPrimitive();
	}

	if (config.alpha_second_pass.enable)
	{
		preprocessSel(config.alpha_second_pass.ps);
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			SetupPS(config.alpha_second_pass.ps, &config.cb_ps, config.sampler);
		}
		else
		{
			// ps cbuffer hasn't changed, so don't bother checking
			SetupPS(config.alpha_second_pass.ps, nullptr, config.sampler);
		}

		SetupOM(config.alpha_second_pass.depth, convertSel(config.alpha_second_pass.colormask, config.blend), config.blend.constant);
		DrawIndexedPrimitive();

		if (config.second_separate_alpha_pass)
		{
			GSHWDrawConfig::BlendState sap_blend = {};
			SetHWDrawConfigForAlphaPass(&config.alpha_second_pass.ps, &config.alpha_second_pass.colormask, &sap_blend, &config.alpha_second_pass.depth);
			SetupOM(config.alpha_second_pass.depth, convertSel(config.alpha_second_pass.colormask, sap_blend), config.blend.constant);
			SetupPS(config.alpha_second_pass.ps, &config.cb_ps, config.sampler);
			DrawIndexedPrimitive();
		}
	}

	if (rt_copy)
		Recycle(rt_copy);
	if (primid_tex)
		Recycle(primid_tex);

	if (hdr_rt)
	{
		const GSVector2i size = config.rt->GetSize();
		const GSVector4 dRect(config.drawarea);
		const GSVector4 sRect = dRect / GSVector4(size.x, size.y).xyxy();
		StretchRect(hdr_rt, sRect, config.rt, dRect, ShaderConvert::HDR_RESOLVE, false);
		g_perfmon.Put(GSPerfMon::TextureCopies, 1);
		Recycle(hdr_rt);
	}
}

#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoGlyphAnalyticRenderer.hpp"

#include <d3d11_1.h>
#include "aviso/shaders/AvisoGlyphVertexShader.hpp"
#include "aviso/shaders/AvisoGlyphPixelShader.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <tuple>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr std::uint64_t MeshBudget = 64ULL * 1024ULL * 1024ULL;
	constexpr std::uint64_t VertexBudget = 64ULL * 1024ULL * 1024ULL;
	constexpr std::uint64_t ImageBudget = 320ULL * 1024ULL * 1024ULL;
	constexpr float CurveTolerance = 1.0f / 64.0f;
	constexpr float BinSize = 4.0f;

	void Check(HRESULT status)
	{
		if (FAILED(status)) throw status;
	}

	class TriangleSink final : public ID2D1TessellationSink
	{
	public:
		explicit TriangleSink(std::size_t budget) : budget_(budget) {}
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
		{
			if (object == nullptr) return E_POINTER;
			*object = nullptr;
			if (iid != __uuidof(IUnknown) && iid != __uuidof(ID2D1TessellationSink)) return E_NOINTERFACE;
			*object = static_cast<ID2D1TessellationSink*>(this);
			AddRef();
			return S_OK;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG remaining = --references_;
			if (remaining == 0) delete this;
			return remaining;
		}
		void STDMETHODCALLTYPE AddTriangles(const D2D1_TRIANGLE* triangles, UINT count) override
		{
			if (FAILED(error_) || count == 0) return;
			if (count > budget_ - triangles_.size()) { error_ = E_OUTOFMEMORY; return; }
			try { triangles_.insert(triangles_.end(), triangles, triangles + count); }
			catch (const std::bad_alloc&) { error_ = E_OUTOFMEMORY; }
		}
		HRESULT STDMETHODCALLTYPE Close() override { return error_; }
		std::vector<D2D1_TRIANGLE> Take() { Check(error_); return std::move(triangles_); }
	private:
		std::atomic<ULONG> references_{ 1 };
		std::size_t budget_;
		HRESULT error_ = S_OK;
		std::vector<D2D1_TRIANGLE> triangles_;
	};

	std::vector<D2D1_TRIANGLE> Tessellate(ID2D1Geometry* geometry, std::uint64_t byteBudget)
	{
		ComPtr<TriangleSink> sink;
		sink.Attach(new TriangleSink(static_cast<std::size_t>(byteBudget / sizeof(D2D1_TRIANGLE))));
		Check(geometry->Tessellate(nullptr, CurveTolerance, sink.Get()));
		Check(sink->Close());
		return sink->Take();
	}

	struct Vertex
	{
		float x, y, localX, localY;
		std::uint32_t style;
	};
	struct PackedTriangle
	{
		float minX, minY, maxX, maxY;
		float x1, y1, x2, y2;
		float x3, y3, padding1, padding2;
	};
	struct Style
	{
		float originX, originY, columns, rows;
		float binStart, padding1, padding2, padding3;
		D2D1_COLOR_F text, halo;
	};
	struct BinRange
	{
		std::uint32_t fillStart, fillCount, haloStart, haloCount;
	};

	class ContextStateGuard final
	{
	public:
		ContextStateGuard(ID3D11DeviceContext1* context, ID3DDeviceContextState* state) : context_(context)
		{
			context_->SwapDeviceContextState(state, previous_.GetAddressOf());
		}
		~ContextStateGuard() { context_->SwapDeviceContextState(previous_.Get(), nullptr); }
	private:
		ID3D11DeviceContext1* context_;
		ComPtr<ID3DDeviceContextState> previous_;
	};

	bool Finite(D2D1_COLOR_F color)
	{
		return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) && std::isfinite(color.a);
	}
}

struct VsmrAviso::AvisoGlyphAnalyticRenderer::Impl
{
	struct Mesh
	{
		ComPtr<ID2D1PathGeometry> outline;
		ComPtr<ID2D1StrokeStyle> style;
		std::vector<D2D1_TRIANGLE> fill;
		std::vector<D2D1_TRIANGLE> halo;
		std::vector<BinRange> bins;
		std::vector<std::uint32_t> triangleIds;
		float binOriginX = 0.0f, binOriginY = 0.0f;
		UINT binColumns = 0, binRows = 0, binStart = 0;
		UINT maximumBinTriangles = 0;
		std::uint64_t lastUsed = 0;
		D2D1_RECT_F bounds{};
		UINT fillStart = 0, haloStart = 0;
		std::uint64_t Bytes() const noexcept
		{
			return static_cast<std::uint64_t>(fill.capacity() + halo.capacity()) * sizeof(D2D1_TRIANGLE) +
				static_cast<std::uint64_t>(bins.capacity()) * sizeof(BinRange) +
				static_cast<std::uint64_t>(triangleIds.capacity()) * sizeof(std::uint32_t);
		}

		void BuildBins(float haloWidth)
		{
			const float padding = haloWidth*0.5f + 1.0f;
			binOriginX = std::floor((bounds.left-padding)/BinSize)*BinSize;
			binOriginY = std::floor((bounds.top-padding)/BinSize)*BinSize;
			const double columns = std::ceil((bounds.right+padding-binOriginX)/BinSize);
			const double rows = std::ceil((bounds.bottom+padding-binOriginY)/BinSize);
			if (!std::isfinite(columns) || !std::isfinite(rows) || columns <= 0 || rows <= 0 ||
				columns*rows > MeshBudget/sizeof(BinRange)) throw E_OUTOFMEMORY;
			binColumns = static_cast<UINT>(columns); binRows = static_cast<UINT>(rows);
			const std::size_t count = static_cast<std::size_t>(binColumns)*binRows;
			const std::uint64_t triangleBytes = Bytes();
			if (triangleBytes > MeshBudget || count > (MeshBudget-triangleBytes)/sizeof(BinRange) ||
				count > MeshBudget/(2U*sizeof(std::vector<std::uint32_t>))) throw E_OUTOFMEMORY;
			const auto referenceBudget = (MeshBudget-triangleBytes-count*sizeof(BinRange))/sizeof(std::uint32_t);
			std::vector<std::vector<std::uint32_t>> fillIds(count), haloIds(count);
			std::uint64_t references = 0;
			for (const auto* triangles : {&fill,&halo})
			{
				auto& ids = triangles == &fill ? fillIds : haloIds;
				const UINT first = triangles == &fill ? 0U : static_cast<UINT>(fill.size());
				for (std::size_t index = 0; index < triangles->size(); ++index)
				{
					const auto& triangle = (*triangles)[index];
					// Pixel centers in a bin can cover triangles half a pixel beyond
					// its boundary. This guard keeps every fractional anchor exact.
					const float left = (std::min)({triangle.point1.x,triangle.point2.x,triangle.point3.x})-0.5f;
					const float right = (std::max)({triangle.point1.x,triangle.point2.x,triangle.point3.x})+0.5f;
					const float top = (std::min)({triangle.point1.y,triangle.point2.y,triangle.point3.y})-0.5f;
					const float bottom = (std::max)({triangle.point1.y,triangle.point2.y,triangle.point3.y})+0.5f;
					const UINT x0 = static_cast<UINT>(std::clamp(std::floor((left-binOriginX)/BinSize),0.0f,float(binColumns-1U)));
					const UINT x1 = static_cast<UINT>(std::clamp(std::floor((right-binOriginX)/BinSize),0.0f,float(binColumns-1U)));
					const UINT y0 = static_cast<UINT>(std::clamp(std::floor((top-binOriginY)/BinSize),0.0f,float(binRows-1U)));
					const UINT y1 = static_cast<UINT>(std::clamp(std::floor((bottom-binOriginY)/BinSize),0.0f,float(binRows-1U)));
					const std::uint64_t added = static_cast<std::uint64_t>(x1-x0+1U)*(y1-y0+1U);
					if (added > referenceBudget-references) throw E_OUTOFMEMORY;
					references += added;
					for (UINT y = y0; y <= y1; ++y) for (UINT x = x0; x <= x1; ++x)
						ids[static_cast<std::size_t>(y)*binColumns+x].push_back(first+static_cast<UINT>(index));
				}
			}
			bins.reserve(count); triangleIds.reserve(static_cast<std::size_t>(references));
			for (std::size_t index = 0; index < count; ++index)
			{
				const auto start = static_cast<UINT>(triangleIds.size());
				const auto fills = static_cast<UINT>(fillIds[index].size()), halos = static_cast<UINT>(haloIds[index].size());
				bins.push_back({start,fills,start+fills,halos});
				triangleIds.insert(triangleIds.end(),fillIds[index].begin(),fillIds[index].end());
				triangleIds.insert(triangleIds.end(),haloIds[index].begin(),haloIds[index].end());
				maximumBinTriangles = (std::max)(maximumBinTriangles,fills+halos);
			}
		}
	};
	using Key = std::tuple<std::uintptr_t, std::uint32_t, std::uintptr_t>;
	std::map<Key, std::unique_ptr<Mesh>> meshes;
	std::uint64_t meshBytes = 0;
	std::uint64_t age = 0;
	std::vector<Vertex> vertices;
	std::vector<Style> styles;
	std::vector<PackedTriangle> packed;
	std::vector<BinRange> packedBins;
	std::vector<std::uint32_t> packedIds;
	bool packedDirty = true;
	double frameWork = 0.0;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext1> context;
	ComPtr<ID3DDeviceContextState> state;
	ComPtr<ID3D11VertexShader> vertexShader;
	ComPtr<ID3D11PixelShader> pixelShader;
	ComPtr<ID3D11InputLayout> inputLayout;
	ComPtr<ID3D11BlendState> blend;
	ComPtr<ID3D11RasterizerState> rasterizer;
	ComPtr<ID3D11DepthStencilState> depth;
	ComPtr<ID3D11Buffer> vertexBuffer;
	ComPtr<ID3D11Buffer> constants;
	ComPtr<ID3D11Buffer> trianglesBuffer, stylesBuffer;
	ComPtr<ID3D11ShaderResourceView> trianglesView, stylesView;
	ComPtr<ID3D11Buffer> binsBuffer, idsBuffer;
	ComPtr<ID3D11ShaderResourceView> binsView, idsView;
	std::uint64_t trianglesBufferBytes = 0, stylesBufferBytes = 0;
	std::uint64_t binsBufferBytes = 0, idsBufferBytes = 0;
	ComPtr<ID3D11Texture2D> multisampled;
	ComPtr<ID3D11Texture2D> resolved;
	ComPtr<ID3D11RenderTargetView> renderTarget;
	UINT width = 0, height = 0, samples = 0;
	std::uint64_t vertexBufferBytes = 0;
	HRESULT error = S_OK;
	std::string errorMessage;
	bool ready = false;

	Mesh& GetMesh(ID2D1Factory* factory, ID2D1StrokeStyle* style, const Instance& instance)
	{
		const float haloWidth = instance.haloColor.a > 0.0f ? instance.haloWidth : 0.0f;
		std::uint32_t widthBits = 0;
		std::memcpy(&widthBits, &haloWidth, sizeof(widthBits));
		const Key key{ reinterpret_cast<std::uintptr_t>(instance.outline), widthBits,
			haloWidth > 0.0f ? reinterpret_cast<std::uintptr_t>(style) : 0U };
		const auto found = meshes.find(key);
		if (found != meshes.end()) { found->second->lastUsed = ++age; return *found->second; }
		auto replacement = std::make_unique<Mesh>();
		replacement->outline = instance.outline;
		replacement->style = style;
		Check(instance.outline->GetBounds(nullptr, &replacement->bounds));
		replacement->fill = Tessellate(instance.outline, MeshBudget);
		const std::uint64_t fillBytes = replacement->Bytes();
		if (fillBytes > MeshBudget) throw E_OUTOFMEMORY;
		if (haloWidth > 0.0f)
		{
			ComPtr<ID2D1PathGeometry> halo;
			Check(factory->CreatePathGeometry(halo.GetAddressOf()));
			ComPtr<ID2D1GeometrySink> sink;
			Check(halo->Open(sink.GetAddressOf()));
			Check(instance.outline->Widen(haloWidth, style, nullptr, CurveTolerance, sink.Get()));
			Check(sink->Close());
			replacement->halo = Tessellate(halo.Get(), MeshBudget - fillBytes);
		}
		replacement->BuildBins(haloWidth);
		const std::uint64_t bytes = replacement->Bytes();
		if (bytes > MeshBudget) throw E_OUTOFMEMORY;
		while (meshBytes > MeshBudget - bytes && !meshes.empty())
		{
			auto oldest = (std::min_element)(meshes.begin(), meshes.end(), [](const auto& left, const auto& right) {
				return left.second->lastUsed < right.second->lastUsed;
			});
			meshBytes -= oldest->second->Bytes();
			meshes.erase(oldest);
		}
		replacement->lastUsed = ++age;
		const auto inserted = meshes.emplace(key, std::move(replacement));
		meshBytes += bytes; packedDirty = true;
		return *inserted.first->second;
	}

	void PackTriangles()
	{
		if (!packedDirty && trianglesBuffer) return;
		packed.clear(); packedBins.clear(); packedIds.clear();
		for (auto& entry : meshes)
		{
			auto& mesh = *entry.second;
			mesh.fillStart = static_cast<UINT>(packed.size());
			for (const auto* source : { &mesh.fill, &mesh.halo })
			{
				if (source == &mesh.halo) mesh.haloStart = static_cast<UINT>(packed.size());
				if (source->size() > MeshBudget / sizeof(PackedTriangle) - packed.size()) throw E_OUTOFMEMORY;
				for (const auto& triangle : *source)
				{
					const auto a = triangle.point1, b = triangle.point2, c = triangle.point3;
					packed.push_back({ (std::min)({a.x,b.x,c.x}), (std::min)({a.y,b.y,c.y}),
						(std::max)({a.x,b.x,c.x}), (std::max)({a.y,b.y,c.y}),
						a.x,a.y,b.x,b.y,c.x,c.y,0,0 });
				}
			}
			const auto binStart = static_cast<UINT>(packedBins.size()), idStart = static_cast<UINT>(packedIds.size());
			const std::uint64_t bytes = static_cast<std::uint64_t>(packed.size())*sizeof(PackedTriangle) +
				static_cast<std::uint64_t>(packedBins.size()+mesh.bins.size())*sizeof(BinRange) +
				static_cast<std::uint64_t>(packedIds.size()+mesh.triangleIds.size())*sizeof(std::uint32_t);
			if (bytes > MeshBudget) throw E_OUTOFMEMORY;
			mesh.binStart = binStart;
			for (const auto& bin : mesh.bins)
				packedBins.push_back({idStart+bin.fillStart,bin.fillCount,idStart+bin.haloStart,bin.haloCount});
			for (const auto index : mesh.triangleIds) packedIds.push_back(mesh.fillStart+index);
		}
		trianglesView.Reset(); trianglesBuffer.Reset(); binsView.Reset(); binsBuffer.Reset(); idsView.Reset(); idsBuffer.Reset();
		trianglesBufferBytes = binsBufferBytes = idsBufferBytes = 0;
		if (!packed.empty())
		{
			D3D11_BUFFER_DESC description{};
			description.ByteWidth = static_cast<UINT>(packed.size() * sizeof(PackedTriangle));
			description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA data{}; data.pSysMem = packed.data();
			Check(device->CreateBuffer(&description, &data, trianglesBuffer.GetAddressOf()));
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			view.Buffer.NumElements = static_cast<UINT>(packed.size() * 3U);
			Check(device->CreateShaderResourceView(trianglesBuffer.Get(), &view, trianglesView.GetAddressOf()));
			trianglesBufferBytes = description.ByteWidth;
		}
		if (!packedBins.empty())
		{
			D3D11_BUFFER_DESC description{};
			description.ByteWidth = static_cast<UINT>(packedBins.size()*sizeof(BinRange));
			description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA data{}; data.pSysMem = packedBins.data();
			Check(device->CreateBuffer(&description,&data,binsBuffer.GetAddressOf()));
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R32G32B32A32_UINT; view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			view.Buffer.NumElements = static_cast<UINT>(packedBins.size());
			Check(device->CreateShaderResourceView(binsBuffer.Get(),&view,binsView.GetAddressOf()));
			binsBufferBytes = description.ByteWidth;
		}
		if (!packedIds.empty())
		{
			D3D11_BUFFER_DESC description{};
			description.ByteWidth = static_cast<UINT>(packedIds.size()*sizeof(std::uint32_t));
			description.Usage = D3D11_USAGE_DEFAULT; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA data{}; data.pSysMem = packedIds.data();
			Check(device->CreateBuffer(&description,&data,idsBuffer.GetAddressOf()));
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R32_UINT; view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			view.Buffer.NumElements = static_cast<UINT>(packedIds.size());
			Check(device->CreateShaderResourceView(idsBuffer.Get(),&view,idsView.GetAddressOf()));
			idsBufferBytes = description.ByteWidth;
		}
		std::vector<PackedTriangle>().swap(packed);
		std::vector<BinRange>().swap(packedBins); std::vector<std::uint32_t>().swap(packedIds);
		packedDirty = false;
	}

	void Append(const Mesh& mesh, const Instance& instance)
	{
		const float padding = (instance.haloColor.a > 0.0f ? instance.haloWidth * 0.5f : 0.0f) + 1.0f;
		const float left = mesh.bounds.left - padding, right = mesh.bounds.right + padding;
		const float top = mesh.bounds.top - padding, bottom = mesh.bounds.bottom + padding;
		const float visibleWidth = (std::max)(0.0f, (std::min)(float(width),right+instance.anchor.x) - (std::max)(0.0f,left+instance.anchor.x));
		const float visibleHeight = (std::max)(0.0f, (std::min)(float(height),bottom+instance.anchor.y) - (std::max)(0.0f,top+instance.anchor.y));
		if (visibleWidth == 0.0f || visibleHeight == 0.0f) return;
		const double work = double(visibleWidth)*visibleHeight*mesh.maximumBinTriangles;
		frameWork += work;
		if (work > 16000000.0 || frameWork > 300000000.0 || styles.size() > 100000U) throw E_OUTOFMEMORY;
		const auto premultiply = [](D2D1_COLOR_F color) {
			const float alpha = std::clamp(color.a,0.0f,1.0f);
			return D2D1_COLOR_F{std::clamp(color.r,0.0f,1.0f)*alpha,
				std::clamp(color.g,0.0f,1.0f)*alpha,std::clamp(color.b,0.0f,1.0f)*alpha,alpha};
		};
		const auto index = static_cast<std::uint32_t>(styles.size());
		styles.push_back({mesh.binOriginX,mesh.binOriginY,float(mesh.binColumns),float(mesh.binRows),float(mesh.binStart),0,0,0,
			premultiply(instance.textColor),premultiply(instance.haloColor) });
		for (const auto point : { D2D1_POINT_2F{left,top},D2D1_POINT_2F{right,top},D2D1_POINT_2F{left,bottom},
			D2D1_POINT_2F{left,bottom},D2D1_POINT_2F{right,top},D2D1_POINT_2F{right,bottom} })
			vertices.push_back({ point.x+instance.anchor.x,point.y+instance.anchor.y,point.x,point.y,index });
	}

	void ReleaseGpu() noexcept
	{
		ready = false; packedDirty = true;
		trianglesView.Reset(); stylesView.Reset(); trianglesBuffer.Reset(); stylesBuffer.Reset();
		binsView.Reset(); idsView.Reset(); binsBuffer.Reset(); idsBuffer.Reset();
		trianglesBufferBytes = stylesBufferBytes = binsBufferBytes = idsBufferBytes = 0;
		renderTarget.Reset(); resolved.Reset(); multisampled.Reset();
		vertexBuffer.Reset(); constants.Reset(); rasterizer.Reset(); blend.Reset(); depth.Reset();
		inputLayout.Reset(); vertexShader.Reset(); pixelShader.Reset(); state.Reset(); context.Reset(); device.Reset();
		width = height = samples = 0; vertexBufferBytes = 0;
	}

	void EnsureDevice(ID3D11Device* requestedDevice, ID3D11DeviceContext* immediate)
	{
		if (device.Get() == requestedDevice && vertexShader && context.Get() == immediate) return;
		ReleaseGpu();
		if (immediate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) throw E_INVALIDARG;
		ComPtr<ID3D11Device> contextDevice;
		immediate->GetDevice(contextDevice.GetAddressOf());
		if (contextDevice.Get() != requestedDevice) throw E_INVALIDARG;
		device = requestedDevice;
		Check(immediate->QueryInterface(IID_PPV_ARGS(context.GetAddressOf())));
		ComPtr<ID3D11Device1> device1;
		Check(device.As(&device1));
		const auto level = device->GetFeatureLevel();
		if (level < D3D_FEATURE_LEVEL_10_0) throw E_NOINTERFACE;
		const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
			? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0U;
		Check(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
			__uuidof(ID3D11Device), nullptr, state.GetAddressOf()));
		Check(device->CreateVertexShader(AvisoGlyphVertexBytecode, sizeof(AvisoGlyphVertexBytecode), nullptr, vertexShader.GetAddressOf()));
		Check(device->CreatePixelShader(AvisoGlyphPixelBytecode, sizeof(AvisoGlyphPixelBytecode), nullptr, pixelShader.GetAddressOf()));
		const D3D11_INPUT_ELEMENT_DESC elements[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
		Check(device->CreateInputLayout(elements, 3, AvisoGlyphVertexBytecode, sizeof(AvisoGlyphVertexBytecode), inputLayout.GetAddressOf()));
		D3D11_BLEND_DESC blending{};
		blending.RenderTarget[0].BlendEnable = TRUE;
		blending.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blending.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blending.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blending.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blending.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blending.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blending.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		Check(device->CreateBlendState(&blending, blend.GetAddressOf()));
		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = TRUE; raster.MultisampleEnable = FALSE;
		Check(device->CreateRasterizerState(&raster, rasterizer.GetAddressOf()));
		D3D11_DEPTH_STENCIL_DESC depthDescription{};
		Check(device->CreateDepthStencilState(&depthDescription, depth.GetAddressOf()));
		D3D11_BUFFER_DESC buffer{};
		buffer.ByteWidth = 16; buffer.Usage = D3D11_USAGE_DEFAULT; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		Check(device->CreateBuffer(&buffer, nullptr, constants.GetAddressOf()));
	}

	void EnsureTarget(UINT requestedWidth, UINT requestedHeight)
	{
		if (width == requestedWidth && height == requestedHeight && resolved) return;
		renderTarget.Reset(); multisampled.Reset(); resolved.Reset(); width = height = samples = 0;
		const std::uint64_t bytes=static_cast<std::uint64_t>(requestedWidth)*requestedHeight*4ULL;
		if(bytes>ImageBudget) throw E_OUTOFMEMORY;
		D3D11_TEXTURE2D_DESC texture{};
		texture.Width=requestedWidth;texture.Height=requestedHeight;texture.MipLevels=texture.ArraySize=1;
		texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;texture.SampleDesc.Count=1;samples=1;
		texture.Usage=D3D11_USAGE_DEFAULT;texture.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
		Check(device->CreateTexture2D(&texture,nullptr,resolved.GetAddressOf()));
		Check(device->CreateRenderTargetView(resolved.Get(),nullptr,renderTarget.GetAddressOf()));
		width = requestedWidth; height = requestedHeight;
	}

	void Draw()
	{
		const std::uint64_t required = static_cast<std::uint64_t>(vertices.size()) * sizeof(Vertex);
		if (required > vertexBufferBytes)
		{
			vertexBuffer.Reset(); vertexBufferBytes = 0;
			D3D11_BUFFER_DESC buffer{};
			buffer.ByteWidth = static_cast<UINT>(required);
			buffer.Usage = D3D11_USAGE_DYNAMIC; buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			Check(device->CreateBuffer(&buffer, nullptr, vertexBuffer.GetAddressOf()));
			vertexBufferBytes = required;
		}
		const std::uint64_t styleBytes=static_cast<std::uint64_t>(styles.size())*sizeof(Style);
		if(styleBytes>stylesBufferBytes)
		{
			stylesView.Reset();stylesBuffer.Reset();stylesBufferBytes=0;
			D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=static_cast<UINT>(styleBytes);
			buffer.Usage=D3D11_USAGE_DYNAMIC;buffer.BindFlags=D3D11_BIND_SHADER_RESOURCE;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
			Check(device->CreateBuffer(&buffer,nullptr,stylesBuffer.GetAddressOf()));
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
			view.Buffer.NumElements=static_cast<UINT>(styles.size()*4U);
			Check(device->CreateShaderResourceView(stylesBuffer.Get(),&view,stylesView.GetAddressOf()));stylesBufferBytes=styleBytes;
		}
		ContextStateGuard restore(context.Get(), state.Get());
		const float transparent[4]{};
		context->ClearRenderTargetView(renderTarget.Get(), transparent);
		if (!vertices.empty())
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			Check(context->Map(vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			std::memcpy(mapped.pData, vertices.data(), static_cast<std::size_t>(required));
			context->Unmap(vertexBuffer.Get(), 0);
			Check(context->Map(stylesBuffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
			std::memcpy(mapped.pData,styles.data(),static_cast<std::size_t>(styleBytes));context->Unmap(stylesBuffer.Get(),0);
			ID3D11ShaderResourceView* inputs[]{trianglesView.Get(),stylesView.Get(),binsView.Get(),idsView.Get()};
			context->PSSetShaderResources(0,4,inputs);
			const float dimensions[4]{ 2.0f / width, 2.0f / height, 0.0f, 0.0f };
			context->UpdateSubresource(constants.Get(), 0, nullptr, dimensions, 0, 0);
			ID3D11Buffer* buffer = vertexBuffer.Get();
			const UINT stride = sizeof(Vertex), offset = 0;
			context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
			context->IASetInputLayout(inputLayout.Get());
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(vertexShader.Get(), nullptr, 0);
			buffer = constants.Get(); context->VSSetConstantBuffers(0, 1, &buffer);
			context->PSSetShader(pixelShader.Get(), nullptr, 0);
			context->GSSetShader(nullptr, nullptr, 0); context->HSSetShader(nullptr, nullptr, 0); context->DSSetShader(nullptr, nullptr, 0);
			context->RSSetState(rasterizer.Get());
			const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
			context->RSSetViewports(1, &viewport);
			ID3D11RenderTargetView* output = renderTarget.Get();
			context->OMSetRenderTargets(1, &output, nullptr);
			context->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFFU);
			context->OMSetDepthStencilState(depth.Get(), 0);
			context->Draw(static_cast<UINT>(vertices.size()), 0);
			context->OMSetRenderTargets(0, nullptr, nullptr);
		}
		Check(device->GetDeviceRemovedReason());
	}
};

VsmrAviso::AvisoGlyphAnalyticRenderer::AvisoGlyphAnalyticRenderer() : impl_(std::make_unique<Impl>()) {}
VsmrAviso::AvisoGlyphAnalyticRenderer::~AvisoGlyphAnalyticRenderer() = default;

bool VsmrAviso::AvisoGlyphAnalyticRenderer::Render(ID3D11Device* device, ID3D11DeviceContext* immediate,
	ID2D1Factory* factory, ID2D1StrokeStyle* haloStyle, UINT width, UINT height,
	const std::vector<Instance>& instances) noexcept
{
	impl_->ready = false; impl_->error = S_OK; impl_->errorMessage.clear(); impl_->vertices.clear(); impl_->styles.clear(); impl_->frameWork=0.0;
	if (device == nullptr || immediate == nullptr || factory == nullptr || width == 0 || height == 0 ||
		width > 6400 || height > 6400 || static_cast<std::uint64_t>(width) * height > 32000000ULL)
	{ impl_->error = E_INVALIDARG; return false; }
	try
	{
		impl_->EnsureDevice(device, immediate);
		impl_->EnsureTarget(width, height);
		for (const auto& instance : instances)
		{
			if (instance.outline == nullptr || !std::isfinite(instance.anchor.x) || !std::isfinite(instance.anchor.y) ||
				!std::isfinite(instance.haloWidth) || instance.haloWidth < 0.0f ||
				!Finite(instance.haloColor) || !Finite(instance.textColor)) throw E_INVALIDARG;
			if (instance.textColor.a <= 0.0f && (instance.haloColor.a <= 0.0f || instance.haloWidth <= 0.0f)) continue;
			impl_->GetMesh(factory,haloStyle,instance);
		}
		impl_->PackTriangles();
		for(const auto& instance:instances)
		{
			if(instance.textColor.a<=0.0f && (instance.haloColor.a<=0.0f || instance.haloWidth<=0.0f))continue;
			const auto& mesh=impl_->GetMesh(factory,haloStyle,instance);
			// More active shapes than the cache budget must fall back rather than
			// using offsets into a previously packed buffer after LRU eviction.
			if (impl_->packedDirty) throw E_OUTOFMEMORY;
			impl_->Append(mesh,instance);
		}
		impl_->Draw(); impl_->ready = true;
		return true;
	}
	catch (HRESULT error) { impl_->error = error; }
	catch (const std::bad_alloc&) { impl_->error = E_OUTOFMEMORY; }
	catch (...) { impl_->error = E_FAIL; }
	impl_->ReleaseGpu();
	return false;
}

ID3D11Texture2D* VsmrAviso::AvisoGlyphAnalyticRenderer::Texture() const noexcept { return impl_->ready ? impl_->resolved.Get() : nullptr; }
UINT VsmrAviso::AvisoGlyphAnalyticRenderer::SampleCount() const noexcept { return impl_->samples; }
HRESULT VsmrAviso::AvisoGlyphAnalyticRenderer::LastError() const noexcept { return impl_->error; }
const std::string& VsmrAviso::AvisoGlyphAnalyticRenderer::LastErrorMessage() const noexcept { return impl_->errorMessage; }
std::size_t VsmrAviso::AvisoGlyphAnalyticRenderer::LastVertexCount() const noexcept { return impl_->vertices.size(); }
std::uint64_t VsmrAviso::AvisoGlyphAnalyticRenderer::CachedMeshBytes() const noexcept { return impl_->meshBytes; }
std::uint64_t VsmrAviso::AvisoGlyphAnalyticRenderer::GpuImageBytes() const noexcept
{
	return static_cast<std::uint64_t>(impl_->width) * impl_->height * 4ULL;
}

std::uint64_t VsmrAviso::AvisoGlyphAnalyticRenderer::GpuBufferBytes() const noexcept
{
	return impl_->vertexBufferBytes+impl_->trianglesBufferBytes+impl_->stylesBufferBytes+impl_->binsBufferBytes+impl_->idsBufferBytes;
}

bool VsmrAviso::AvisoGlyphAnalyticRenderer::GlyphAreaCentroid(ID2D1Factory* factory, ID2D1PathGeometry* outline,
	D2D1_POINT_2F& centroid, double& area) noexcept
{
	if (factory == nullptr || outline == nullptr) return false;
	try
	{
		Instance instance;
		instance.outline = outline;
		auto& mesh = impl_->GetMesh(factory, nullptr, instance);
		double total = 0.0, x = 0.0, y = 0.0;
		for (const auto& triangle : mesh.fill)
		{
			const double a = std::abs((double(triangle.point2.x) - triangle.point1.x) * (double(triangle.point3.y) - triangle.point1.y) -
				(double(triangle.point2.y) - triangle.point1.y) * (double(triangle.point3.x) - triangle.point1.x)) * 0.5;
			total += a;
			x += a * (double(triangle.point1.x) + triangle.point2.x + triangle.point3.x) / 3.0;
			y += a * (double(triangle.point1.y) + triangle.point2.y + triangle.point3.y) / 3.0;
		}
		if (total <= 0.0) return false;
		centroid = { static_cast<float>(x / total), static_cast<float>(y / total) }; area = total;
		return true;
	}
	catch (...) { return false; }
}
void VsmrAviso::AvisoGlyphAnalyticRenderer::Clear() noexcept
{
	impl_->meshes.clear(); impl_->meshBytes = 0; impl_->age = 0; impl_->vertices.clear(); impl_->styles.clear();
	impl_->packed.clear(); impl_->packedBins.clear(); impl_->packedIds.clear(); impl_->packedDirty=true; impl_->ready = false;
}
void VsmrAviso::AvisoGlyphAnalyticRenderer::ReleaseResources() noexcept
{
	Clear(); impl_->ReleaseGpu(); std::vector<Vertex>().swap(impl_->vertices); std::vector<Style>().swap(impl_->styles); std::vector<PackedTriangle>().swap(impl_->packed);
	std::vector<BinRange>().swap(impl_->packedBins); std::vector<std::uint32_t>().swap(impl_->packedIds);
}
void VsmrAviso::AvisoGlyphAnalyticRenderer::RetainOutlines(const std::vector<ID2D1PathGeometry*>& outlines) noexcept
{
	for (auto entry = impl_->meshes.begin(); entry != impl_->meshes.end(); )
	{
		if (std::find(outlines.begin(),outlines.end(),entry->second->outline.Get()) == outlines.end())
		{
			impl_->meshBytes -= entry->second->Bytes();
			entry = impl_->meshes.erase(entry);
			impl_->packedDirty = true;
		}
		else ++entry;
	}
	if (impl_->packedDirty)
	{
		impl_->trianglesView.Reset(); impl_->trianglesBuffer.Reset();
		impl_->binsView.Reset(); impl_->binsBuffer.Reset(); impl_->idsView.Reset(); impl_->idsBuffer.Reset();
		impl_->trianglesBufferBytes = impl_->binsBufferBytes = impl_->idsBufferBytes = 0;
	}
}

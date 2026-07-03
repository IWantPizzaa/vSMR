#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace vsmr::mapipc
{
inline constexpr std::uint32_t ProtocolMagic = 0x524D5356u; // VSMR, little-endian.
inline constexpr std::uint32_t ProtocolVersion = 1u;
inline constexpr std::uint32_t FrameSlotCount = 3u;
inline constexpr std::uint32_t MaxAirportIdBytes = 16u;
inline constexpr std::uint32_t MaxPathBytes = 520u;
inline constexpr std::uint32_t FramePayloadAlignment = 64u;

inline constexpr char FrameMappingPrefix[] = "Local\\vSMR.MapRenderer.Frame.";
inline constexpr char CommandMappingPrefix[] = "Local\\vSMR.MapRenderer.Command.";
inline constexpr char CameraEventPrefix[] = "Local\\vSMR.MapRenderer.Camera.";
inline constexpr char ShutdownEventPrefix[] = "Local\\vSMR.MapRenderer.Shutdown.";

enum class PixelFormat : std::uint32_t
{
	Bgra8Premultiplied = 1u
};

enum class FrameStatus : std::uint32_t
{
	Empty = 0u,
	Rendering = 1u,
	Complete = 2u,
	Failed = 3u
};

enum class RendererState : std::uint32_t
{
	Starting = 0u,
	Ready = 1u,
	LoadingAirport = 2u,
	Rendering = 3u,
	Idle = 4u,
	Stopping = 5u,
	Failed = 6u
};

enum CommandFlags : std::uint32_t
{
	CommandFlagNone = 0u,
	CommandFlagCameraDirty = 1u << 0,
	CommandFlagAirportDirty = 1u << 1,
	CommandFlagStyleDirty = 1u << 2,
	CommandFlagShutdownRequested = 1u << 3
};

struct CameraState
{
	std::uint64_t requestSequence = 0;
	double minLatitude = 0.0;
	double maxLatitude = 0.0;
	double minLongitude = 0.0;
	double maxLongitude = 0.0;
	std::int32_t viewportWidth = 0;
	std::int32_t viewportHeight = 0;
	double pixelRatio = 1.0;
	std::int32_t airportRevision = 0;
	std::int32_t styleRevision = 0;
	std::uint32_t interactive = 0;
	std::uint32_t reserved0 = 0;
};

struct GroundMapStyle
{
	std::uint32_t backgroundColor = 0xFF000000u;
	std::uint32_t grassColor = 0xFF1B2A1Bu;
	std::uint32_t runwayColor = 0xFF4A4A4Au;
	std::uint32_t taxiwayColor = 0xFF343434u;
	std::uint32_t apronColor = 0xFF2D2D2Du;
	std::uint32_t buildingColor = 0xFF202020u;
	std::uint32_t coastlineColor = 0xFF406078u;
	std::uint32_t markingColor = 0xFFE6E6E6u;
	std::uint32_t labelColor = 0xFFDADADAu;
	float brightness = 1.0f;
	std::uint32_t nightMode = 0;
	std::uint32_t reserved[3] = {};
};

struct AirportState
{
	std::uint64_t airportSequence = 0;
	std::int32_t airportRevision = 0;
	std::uint32_t geoJsonPathLength = 0;
	char airportId[MaxAirportIdBytes] = {};
	char geoJsonPath[MaxPathBytes] = {};
};

struct FrameSlotMetadata
{
	std::uint64_t frameSequence = 0;
	std::uint64_t cameraRequestSequence = 0;
	std::uint64_t renderStartedQpc = 0;
	std::uint64_t renderFinishedQpc = 0;
	std::uint32_t producerProcessId = 0;
	std::int32_t airportRevision = 0;
	std::int32_t styleRevision = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t stride = 0;
	std::uint32_t pixelFormat = static_cast<std::uint32_t>(PixelFormat::Bgra8Premultiplied);
	std::uint32_t status = static_cast<std::uint32_t>(FrameStatus::Empty);
	std::uint32_t payloadOffset = 0;
	std::uint32_t payloadBytes = 0;
	std::uint32_t reserved0 = 0;
	CameraState camera = {};
	std::uint64_t reserved[8] = {};
};

struct alignas(8) SharedFrameHeader
{
	std::uint32_t magic = ProtocolMagic;
	std::uint32_t version = ProtocolVersion;
	std::uint32_t headerBytes = 0; // Producer sets sizeof(SharedFrameHeader).
	std::uint32_t slotCount = FrameSlotCount;
	std::uint32_t maxWidth = 0;
	std::uint32_t maxHeight = 0;
	std::uint32_t maxStride = 0;
	std::uint32_t pixelFormat = static_cast<std::uint32_t>(PixelFormat::Bgra8Premultiplied);
	std::uint64_t mappingBytes = 0;
	std::uint32_t producerProcessId = 0;
	std::uint32_t consumerProcessId = 0;
	std::uint64_t producerHeartbeatQpc = 0;
	std::uint64_t consumerHeartbeatQpc = 0;
	alignas(8) volatile std::uint64_t publishedSequence = 0;
	alignas(4) volatile std::uint32_t publishedSlot = 0;
	std::uint32_t rendererState = static_cast<std::uint32_t>(RendererState::Starting);
	std::uint32_t lastErrorCode = 0;
	std::uint32_t reserved0 = 0;
	FrameSlotMetadata slots[FrameSlotCount] = {};
	std::uint64_t reserved[16] = {};
};

struct alignas(8) SharedCommandBlock
{
	std::uint32_t magic = ProtocolMagic;
	std::uint32_t version = ProtocolVersion;
	std::uint32_t headerBytes = 0; // Producer sets sizeof(SharedCommandBlock).
	alignas(4) volatile std::uint32_t commandFlags = CommandFlagNone;
	std::uint32_t pluginProcessId = 0;
	std::uint32_t rendererProcessId = 0;
	std::uint64_t pluginHeartbeatQpc = 0;
	std::uint64_t rendererHeartbeatQpc = 0;
	alignas(8) volatile std::uint64_t cameraSequence = 0;
	alignas(8) volatile std::uint64_t airportSequence = 0;
	alignas(8) volatile std::uint64_t styleSequence = 0;
	alignas(8) volatile std::uint64_t shutdownSequence = 0;
	CameraState latestCamera = {};
	AirportState airport = {};
	GroundMapStyle style = {};
	std::uint64_t reserved[16] = {};
};

inline constexpr std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment) noexcept
{
	return alignment == 0 ? value : ((value + alignment - 1u) / alignment) * alignment;
}

inline constexpr std::uint32_t BgraStride(std::uint32_t width) noexcept
{
	return AlignUp(width * 4u, 4u);
}

inline constexpr std::uint64_t FramePayloadBytes(std::uint32_t width, std::uint32_t height) noexcept
{
	return static_cast<std::uint64_t>(BgraStride(width)) * static_cast<std::uint64_t>(height);
}

inline constexpr std::uint64_t FrameMappingBytes(std::uint32_t maxWidth, std::uint32_t maxHeight) noexcept
{
	const std::uint64_t slotBytes = FramePayloadBytes(maxWidth, maxHeight);
	const std::uint64_t alignedHeader = AlignUp(static_cast<std::uint32_t>(sizeof(SharedFrameHeader)), FramePayloadAlignment);
	return alignedHeader + slotBytes * FrameSlotCount;
}

inline constexpr bool IsValidFrameSlot(std::uint32_t slot) noexcept
{
	return slot < FrameSlotCount;
}

inline constexpr std::uint32_t SharedFrameHeaderBytes = static_cast<std::uint32_t>(sizeof(SharedFrameHeader));
inline constexpr std::uint32_t SharedCommandBlockBytes = static_cast<std::uint32_t>(sizeof(SharedCommandBlock));

inline constexpr std::uint32_t FramePayloadBaseOffset() noexcept
{
	return AlignUp(SharedFrameHeaderBytes, FramePayloadAlignment);
}

inline constexpr std::uint32_t FramePayloadOffset(std::uint32_t slot, std::uint32_t maxWidth, std::uint32_t maxHeight) noexcept
{
	return FramePayloadBaseOffset() +
		static_cast<std::uint32_t>(FramePayloadBytes(maxWidth, maxHeight) * static_cast<std::uint64_t>(slot));
}

inline void InitializeSharedFrameHeader(SharedFrameHeader& header, std::uint32_t maxWidth, std::uint32_t maxHeight) noexcept
{
	header = SharedFrameHeader{};
	header.headerBytes = SharedFrameHeaderBytes;
	header.maxWidth = maxWidth;
	header.maxHeight = maxHeight;
	header.maxStride = BgraStride(maxWidth);
	header.mappingBytes = FrameMappingBytes(maxWidth, maxHeight);

	for (std::uint32_t slotIndex = 0; slotIndex < FrameSlotCount; ++slotIndex)
	{
		header.slots[slotIndex].payloadOffset = FramePayloadOffset(slotIndex, maxWidth, maxHeight);
	}
}

inline void InitializeSharedCommandBlock(SharedCommandBlock& commandBlock) noexcept
{
	commandBlock = SharedCommandBlock{};
	commandBlock.headerBytes = SharedCommandBlockBytes;
}

inline constexpr bool IsCompleteFrame(const FrameSlotMetadata& slot) noexcept
{
	return slot.status == static_cast<std::uint32_t>(FrameStatus::Complete) &&
		slot.pixelFormat == static_cast<std::uint32_t>(PixelFormat::Bgra8Premultiplied) &&
		slot.width > 0 &&
		slot.height > 0 &&
		slot.stride >= slot.width * 4u &&
		static_cast<std::uint64_t>(slot.payloadBytes) >=
			static_cast<std::uint64_t>(slot.stride) * static_cast<std::uint64_t>(slot.height);
}

static_assert(std::is_standard_layout<CameraState>::value, "CameraState must be ABI-stable");
static_assert(std::is_standard_layout<GroundMapStyle>::value, "GroundMapStyle must be ABI-stable");
static_assert(std::is_standard_layout<AirportState>::value, "AirportState must be ABI-stable");
static_assert(std::is_standard_layout<FrameSlotMetadata>::value, "FrameSlotMetadata must be ABI-stable");
static_assert(std::is_standard_layout<SharedFrameHeader>::value, "SharedFrameHeader must be ABI-stable");
static_assert(std::is_standard_layout<SharedCommandBlock>::value, "SharedCommandBlock must be ABI-stable");
static_assert(alignof(SharedFrameHeader) >= 8, "SharedFrameHeader must keep 64-bit fields aligned");
static_assert(alignof(SharedCommandBlock) >= 8, "SharedCommandBlock must keep 64-bit fields aligned");
static_assert(sizeof(CameraState) == 72, "Unexpected CameraState layout");
} // namespace vsmr::mapipc

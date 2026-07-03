#include <mbgl/gfx/headless_frontend.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/map_options.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/run_loop.hpp>

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct Options
{
	std::filesystem::path geoJsonPath;
	std::filesystem::path stylePath;
	std::filesystem::path outputPngPath;
	std::filesystem::path outputBgraPath;
	std::filesystem::path cachePath = "vsmr-maplibre-cache.sqlite";
	std::filesystem::path assetRoot = ".";
	std::uint32_t width = 1280;
	std::uint32_t height = 720;
	double pixelRatio = 1.0;
	double north = 0.0;
	double west = 0.0;
	double south = 0.0;
	double east = 0.0;
	std::uint32_t frames = 1;
	bool boundsSet = false;
	bool debug = false;
};

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point start, Clock::time_point end)
{
	return std::chrono::duration<double, std::milli>(end - start).count();
}

std::string JsonEscape(const std::string& input)
{
	std::ostringstream out;
	for (const char ch : input)
	{
		switch (ch)
		{
		case '\\':
			out << "\\\\";
			break;
		case '"':
			out << "\\\"";
			break;
		case '\n':
			out << "\\n";
			break;
		case '\r':
			out << "\\r";
			break;
		case '\t':
			out << "\\t";
			break;
		default:
			out << ch;
			break;
		}
	}
	return out.str();
}

std::string UrlEncodePath(const std::string& input)
{
	std::ostringstream out;
	for (const unsigned char ch : input)
	{
		if ((ch >= 'a' && ch <= 'z') ||
			(ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') ||
			ch == '/' || ch == ':' || ch == '-' || ch == '_' || ch == '.' || ch == '~')
		{
			out << static_cast<char>(ch);
		}
		else
		{
			out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ch)
				<< std::nouppercase << std::dec;
		}
	}
	return out.str();
}

std::string FileUrl(const std::filesystem::path& path)
{
	const std::filesystem::path absolutePath = std::filesystem::absolute(path);
	std::string genericPath = absolutePath.generic_string();
#ifdef _WIN32
	if (genericPath.size() >= 2 && genericPath[1] == ':')
		genericPath.insert(genericPath.begin(), '/');
#endif
	return "file://" + UrlEncodePath(genericPath);
}

std::string GenerateAirportStyleJson(const std::filesystem::path& geoJsonPath)
{
	const std::string geoJsonUrl = JsonEscape(FileUrl(geoJsonPath));
	std::ostringstream style;
	style
		<< "{"
		<< "\"version\":8,"
		<< "\"name\":\"vSMR Airport POC\","
		<< "\"sources\":{\"airport\":{\"type\":\"geojson\",\"data\":\"" << geoJsonUrl << "\"}},"
		<< "\"layers\":["
		<< "{\"id\":\"background\",\"type\":\"background\",\"paint\":{\"background-color\":\"#050505\"}},"
		<< "{\"id\":\"airport-polygons\",\"type\":\"fill\",\"source\":\"airport\","
		<< "\"filter\":[\"==\",\"$type\",\"Polygon\"],"
		<< "\"paint\":{\"fill-color\":\"#303235\",\"fill-opacity\":0.88}},"
		<< "{\"id\":\"airport-polygon-outlines\",\"type\":\"line\",\"source\":\"airport\","
		<< "\"filter\":[\"==\",\"$type\",\"Polygon\"],"
		<< "\"layout\":{\"line-cap\":\"round\",\"line-join\":\"round\"},"
		<< "\"paint\":{\"line-color\":\"#747b82\",\"line-width\":1.0,\"line-opacity\":0.75}},"
		<< "{\"id\":\"airport-lines\",\"type\":\"line\",\"source\":\"airport\","
		<< "\"filter\":[\"==\",\"$type\",\"LineString\"],"
		<< "\"layout\":{\"line-cap\":\"round\",\"line-join\":\"round\"},"
		<< "\"paint\":{\"line-color\":\"#d8d3b0\",\"line-width\":[\"interpolate\",[\"linear\"],[\"zoom\"],10,0.4,16,2.0,20,5.0],\"line-opacity\":0.95}},"
		<< "{\"id\":\"airport-points\",\"type\":\"circle\",\"source\":\"airport\","
		<< "\"filter\":[\"==\",\"$type\",\"Point\"],"
		<< "\"paint\":{\"circle-radius\":[\"interpolate\",[\"linear\"],[\"zoom\"],10,1.5,18,5.0],\"circle-color\":\"#f0e9c8\",\"circle-opacity\":0.9}}"
		<< "]"
		<< "}";
	return style.str();
}

void WriteTextFile(const std::filesystem::path& path, const std::string& content)
{
	std::ofstream out(path, std::ios::binary);
	if (!out)
		throw std::runtime_error("failed to open " + path.string());
	out << content;
}

void WritePremultipliedBgraFile(const std::filesystem::path& path, const mbgl::PremultipliedImage& image)
{
	if (!image.valid())
		throw std::runtime_error("cannot write an invalid image");

	std::vector<std::uint8_t> bgra(image.bytes());
	const std::uint8_t* source = image.data.get();
	for (std::size_t offset = 0; offset < image.bytes(); offset += 4)
	{
		bgra[offset + 0] = source[offset + 2];
		bgra[offset + 1] = source[offset + 1];
		bgra[offset + 2] = source[offset + 0];
		bgra[offset + 3] = source[offset + 3];
	}

	std::ofstream out(path, std::ios::binary);
	if (!out)
		throw std::runtime_error("failed to open " + path.string());
	out.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
}

std::uint64_t WorkingSetBytes()
{
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX counters = {};
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
		return static_cast<std::uint64_t>(counters.WorkingSetSize);
#endif
	return 0;
}

double Percentile(std::vector<double> values, double percentile)
{
	if (values.empty())
		return 0.0;
	std::sort(values.begin(), values.end());
	const double scaledIndex = (percentile / 100.0) * static_cast<double>(values.size() - 1);
	const auto index = static_cast<std::size_t>(std::round(scaledIndex));
	return values[std::min(index, values.size() - 1)];
}

std::string RequireValue(int& index, int argc, char* argv[], const std::string& option)
{
	if (index + 1 >= argc)
		throw std::runtime_error("missing value for " + option);
	return argv[++index];
}

double ParseDouble(const std::string& text, const std::string& option)
{
	char* end = nullptr;
	const double value = std::strtod(text.c_str(), &end);
	if (end == text.c_str() || (end != nullptr && *end != '\0'))
		throw std::runtime_error("invalid numeric value for " + option + ": " + text);
	return value;
}

std::uint32_t ParseUInt32(const std::string& text, const std::string& option)
{
	char* end = nullptr;
	const unsigned long value = std::strtoul(text.c_str(), &end, 10);
	if (end == text.c_str() || (end != nullptr && *end != '\0') || value > 0xFFFFFFFFul)
		throw std::runtime_error("invalid integer value for " + option + ": " + text);
	return static_cast<std::uint32_t>(value);
}

void PrintUsage()
{
	std::cout
		<< "vSMRMapRendererPoc --geojson LFPG.geojson --bounds north west south east [options]\n\n"
		<< "Options:\n"
		<< "  --style path        Use an existing MapLibre style JSON instead of generated airport style.\n"
		<< "  --width px         Output width, default 1280.\n"
		<< "  --height px        Output height, default 720.\n"
		<< "  --ratio value      Pixel ratio, default 1.0.\n"
		<< "  --frames count     Render count for timing, default 1.\n"
		<< "  --output path      Write final frame as PNG.\n"
		<< "  --bgra path        Write final frame as raw premultiplied BGRA.\n"
		<< "  --cache path       MapLibre cache path.\n"
		<< "  --assets path      asset:// root.\n"
		<< "  --debug            Enable MapLibre tile/debug overlays.\n";
}

Options ParseOptions(int argc, char* argv[])
{
	Options options;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--help" || arg == "-h")
		{
			PrintUsage();
			std::exit(0);
		}
		if (arg == "--geojson")
			options.geoJsonPath = RequireValue(i, argc, argv, arg);
		else if (arg == "--style")
			options.stylePath = RequireValue(i, argc, argv, arg);
		else if (arg == "--output")
			options.outputPngPath = RequireValue(i, argc, argv, arg);
		else if (arg == "--bgra")
			options.outputBgraPath = RequireValue(i, argc, argv, arg);
		else if (arg == "--cache")
			options.cachePath = RequireValue(i, argc, argv, arg);
		else if (arg == "--assets")
			options.assetRoot = RequireValue(i, argc, argv, arg);
		else if (arg == "--width")
			options.width = ParseUInt32(RequireValue(i, argc, argv, arg), arg);
		else if (arg == "--height")
			options.height = ParseUInt32(RequireValue(i, argc, argv, arg), arg);
		else if (arg == "--ratio")
			options.pixelRatio = ParseDouble(RequireValue(i, argc, argv, arg), arg);
		else if (arg == "--frames")
			options.frames = std::max<std::uint32_t>(1u, ParseUInt32(RequireValue(i, argc, argv, arg), arg));
		else if (arg == "--bounds")
		{
			options.north = ParseDouble(RequireValue(i, argc, argv, arg), arg);
			options.west = ParseDouble(RequireValue(i, argc, argv, arg), arg);
			options.south = ParseDouble(RequireValue(i, argc, argv, arg), arg);
			options.east = ParseDouble(RequireValue(i, argc, argv, arg), arg);
			options.boundsSet = true;
		}
		else if (arg == "--debug")
			options.debug = true;
		else
			throw std::runtime_error("unknown option: " + arg);
	}

	if (options.geoJsonPath.empty() && options.stylePath.empty())
		throw std::runtime_error("--geojson is required unless --style is provided");
	if (!options.boundsSet)
		throw std::runtime_error("--bounds north west south east is required");
	if (options.width == 0 || options.height == 0)
		throw std::runtime_error("--width and --height must be non-zero");
	if (options.pixelRatio <= 0.0)
		throw std::runtime_error("--ratio must be positive");
	return options;
}
} // namespace

int main(int argc, char* argv[])
{
	try
	{
		const Options options = ParseOptions(argc, argv);
		const auto processStart = Clock::now();

		std::filesystem::path stylePath = options.stylePath;
		if (stylePath.empty())
		{
			stylePath = std::filesystem::temp_directory_path() / "vsmr-maplibre-airport-style.json";
			WriteTextFile(stylePath, GenerateAirportStyleJson(options.geoJsonPath));
		}

		const std::string styleUrl = FileUrl(stylePath);
		const auto styleReady = Clock::now();

		mbgl::util::RunLoop runLoop;
		mbgl::HeadlessFrontend frontend({ options.width, options.height }, static_cast<float>(options.pixelRatio));
		auto tileServerOptions = mbgl::TileServerOptions::MapTilerConfiguration();

		mbgl::Map map(
			frontend,
			mbgl::MapObserver::nullObserver(),
			mbgl::MapOptions()
				.withMapMode(mbgl::MapMode::Continuous)
				.withSize(frontend.getSize())
				.withPixelRatio(static_cast<float>(options.pixelRatio)),
			mbgl::ResourceOptions()
				.withCachePath(options.cachePath.string())
				.withAssetPath(options.assetRoot.string())
				.withTileServerOptions(tileServerOptions));

		map.getStyle().loadURL(styleUrl);

		const mbgl::LatLngBounds bounds = mbgl::LatLngBounds::hull(
			mbgl::LatLng(options.north, options.west),
			mbgl::LatLng(options.south, options.east));
		map.jumpTo(map.cameraForLatLngBounds(bounds, mbgl::EdgeInsets(), 0.0, 0.0));

		if (options.debug)
			map.setDebug(mbgl::MapDebugOptions::TileBorders | mbgl::MapDebugOptions::ParseStatus);

		std::vector<double> renderTimes;
		renderTimes.reserve(options.frames);
		mbgl::PremultipliedImage finalImage;

		for (std::uint32_t frameIndex = 0; frameIndex < options.frames; ++frameIndex)
		{
			const auto renderStart = Clock::now();
			auto rendered = frontend.render(map);
			const auto renderEnd = Clock::now();
			renderTimes.push_back(ElapsedMs(renderStart, renderEnd));
			finalImage = std::move(rendered.image);
		}

		const auto renderDone = Clock::now();
		double pngExportMs = 0.0;
		double bgraExportMs = 0.0;

		if (!options.outputPngPath.empty())
		{
			const auto exportStart = Clock::now();
			std::ofstream out(options.outputPngPath, std::ios::binary);
			if (!out)
				throw std::runtime_error("failed to open " + options.outputPngPath.string());
			out << mbgl::encodePNG(finalImage);
			pngExportMs = ElapsedMs(exportStart, Clock::now());
		}

		if (!options.outputBgraPath.empty())
		{
			const auto exportStart = Clock::now();
			WritePremultipliedBgraFile(options.outputBgraPath, finalImage);
			bgraExportMs = ElapsedMs(exportStart, Clock::now());
		}

		const auto processDone = Clock::now();
		const double firstFrameMs = renderTimes.empty() ? 0.0 : renderTimes.front();
		const double totalRenderMs = ElapsedMs(styleReady, renderDone);
		const double averageRenderMs = renderTimes.empty()
			? 0.0
			: std::accumulate(renderTimes.begin(), renderTimes.end(), 0.0) / static_cast<double>(renderTimes.size());

		std::cout
			<< "vSMR MapLibre POC metrics\n"
			<< "style_setup_ms=" << ElapsedMs(processStart, styleReady) << "\n"
			<< "first_frame_ms=" << firstFrameMs << "\n"
			<< "average_render_ms=" << averageRenderMs << "\n"
			<< "p95_render_ms=" << Percentile(renderTimes, 95.0) << "\n"
			<< "total_render_loop_ms=" << totalRenderMs << "\n"
			<< "png_export_ms=" << pngExportMs << "\n"
			<< "bgra_export_ms=" << bgraExportMs << "\n"
			<< "image_copy_bytes=" << finalImage.bytes() << "\n"
			<< "working_set_bytes=" << WorkingSetBytes() << "\n"
			<< "total_process_ms=" << ElapsedMs(processStart, processDone) << "\n";
	}
	catch (const std::exception& ex)
	{
		std::cerr << "vSMRMapRendererPoc error: " << ex.what() << "\n\n";
		PrintUsage();
		return 1;
	}

	return 0;
}

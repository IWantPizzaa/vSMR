#include <Windows.h>
#include <objidl.h>
#include <GdiPlus.h>

#include "AuditRegressionTests.hpp"
#include "shared/JsonDocument.hpp"
#include "tags/CompiledTagDefinition.hpp"
#include "rendering/TagRenderer.hpp"
#include "platform/windows/network/HttpHelper.hpp"
#include "updater/UpdaterVerification.hpp"

#include <filesystem>
#include <fstream>

std::vector<std::string> RunAuditRegressionTests()
{
	std::vector<std::string> failures;
	auto check = [&](bool condition, const char* message)
	{ if (!condition) failures.emplace_back(message); };

	rapidjson::Document document;
	const std::string valid = R"({"release":"2.0.0","files":[1,true,null,"Ã©"]})";
	check(!VsmrJson::ParseDocument(document, valid).HasParseError(), "bounded JSON accepts a valid UTF-8 document");
	for (const auto& invalid : {
		valid + std::string("\0{}", 3),
		std::string("{\"x\":\"\xc0\x80\"}"),
		std::string(20000, '[') + "0" + std::string(20000, ']'),
		std::string("{\"x\":\"") + std::string(65537, 'x') + "\"}",
		std::string("{\"x\":1e9999}"), std::string("{} trailing"), std::string("{\"x\":\"\\uD800\"}") })
	{
		check(VsmrJson::ParseDocument(document, invalid).HasParseError() && document.IsNull(),
			"bounded JSON rejects malformed, truncated or excessive input and clears stale DOM data");
	}
	check(!VsmrJson::ParseDocument(document, std::string(64, '[') + "0" + std::string(64, ']')).HasParseError(),
		"JSON accepts the exact configured depth boundary");
	check(VsmrJson::ParseDocument(document, std::string(65, '[') + "0" + std::string(65, ']')).HasParseError(),
		"JSON rejects one level beyond the depth boundary");
	VsmrJsonInputLimits::Limits limits;
	limits.maximumValues = 3;
	std::string error;
	check(!VsmrJsonInputLimits::Validate("[0,1,2]", limits, error), "SAX validation terminates when the value budget is exceeded");

	rapidjson::Document labels;
	VsmrJson::ParseDocument(labels, R"json({"departure":{"definition":[["b:callsign(1,2,3)","callsign/gs","scratchpad","holdingpoint","clearance(NO,YES)"]],"definition_detailed_inherits_normal":true,"status_definitions":{"taxi":{"definition":["deprwy"]}}}})json");
	VsmrTags::DefinitionCache definitions;
	VsmrScene::Target target;
	target.hasFlightPlan = target.correlated = true;
	target.tag.tokens = { {"callsign", "gs123"}, {"gs", "25"}, {"scratchpad", "..."}, {"holdingpoint", ""}, {"deprwy", "08L"} };
	const auto& compiled = definitions.Get(labels, "departure", "default", false);
	auto normal = VsmrTags::BuildTagVariant(compiled, target, false);
	check(normal.lines.size() == 1 && normal.lines[0].elements.size() == 5, "compiled tags retain their line and element structure");
	if (normal.lines.size() == 1 && normal.lines[0].elements.size() == 5)
	{
		const auto& elements = normal.lines[0].elements;
		check(elements[0].text == "gs123" && elements[0].bold && elements[0].customColor.red == 1,
			"compiled tags preserve styling and exact replacement values");
		check(elements[1].text == "gs123/25", "tag substitutions do not recursively replace text supplied by a pilot");
		check(elements[2].text.empty() && elements[3].text.empty() && elements[4].text == "NO",
			"normal tags preserve scratchpad, holding point and clearance behavior");
	}
	target.tag.clearanceReceived = true;
	const auto detailed = VsmrTags::BuildTagVariant(definitions.Get(labels, "departure", "default", true), target, true);
	check(detailed.lines[0].elements[2].text == "..." && detailed.lines[0].elements[3].text == "HP" &&
		detailed.lines[0].elements[4].text == "YES", "detailed inherited tags retain live field behavior");
	const auto taxi = VsmrTags::BuildTagVariant(definitions.Get(labels, "departure", "taxi", true), target, true);
	check(taxi.lines[0].elements[0].text == "08L", "compiled tags resolve inherited status-specific definitions");
	labels["departure"]["definition"][0][0].SetString("gs", labels.GetAllocator());
	definitions.Clear();
	check(VsmrTags::BuildTagVariant(definitions.Get(labels, "departure", "default", false), target, false).lines[0].elements[0].text == "25",
		"invalidating a tag cache makes profile edits visible");
	const auto capacity = target.tag.tokens.capacity();
	target.tag.tokens.ResetValues();
	check(target.tag.tokens.at("callsign").empty() && target.tag.tokens.capacity() == capacity,
		"reusing tag token storage clears old aircraft values without reallocating");

	// Graphics objects are deliberately scoped before GDI+ shuts down in the harness.
	struct GdiRuntime
	{
		ULONG_PTR token = 0;
		GdiRuntime() { Gdiplus::GdiplusStartupInput input; Gdiplus::GdiplusStartup(&token, &input, nullptr); }
		~GdiRuntime() { if (token != 0) Gdiplus::GdiplusShutdown(token); }
	} runtime;
	if (runtime.token == 0) { failures.emplace_back("GDI+ failed to start for audit regressions"); return failures; }
	Gdiplus::Bitmap bitmap(64, 64, PixelFormat32bppARGB);
	Gdiplus::Graphics graphics(&bitmap);
	Gdiplus::Font font(L"Arial", 12, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
	Gdiplus::Font largerFont(L"Arial", 20, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
	VsmrTagRendering::TextCache cache;
	{
		VsmrTagRendering::FontContext fonts(graphics, &font, 1, &cache);
		fonts.Measure("AFR1234");
		fonts.Measure("AFR1234", true);
	}
	const auto measurements = cache.MeasurementCount();
	{
		VsmrTagRendering::FontContext fonts(graphics, &font, 1, &cache);
		fonts.Measure("AFR1234");
		fonts.Measure("AFR1234", true);
	}
	check(cache.MeasurementCount() == measurements, "font measurements survive across render frames");
	{
		VsmrTagRendering::FontContext fonts(graphics, &largerFont, 1, &cache);
		fonts.Measure("AFR1234");
		check(cache.MeasurementCount() > measurements, "a font-size change invalidates cached measurements");
		for (std::size_t i = 0; i < VsmrTagRendering::TextCache::MaximumEntries + 10; ++i)
			fonts.Utf16Text(std::to_string(i));
		check(cache.CachedTextCount() <= VsmrTagRendering::TextCache::MaximumEntries, "changing aircraft text cannot grow the cache without a bound");
	}
	const auto beforeTransform = cache.MeasurementCount();
	graphics.ScaleTransform(2.0f, 2.0f);
	{
		VsmrTagRendering::FontContext fonts(graphics, &largerFont, 1, &cache);
		fonts.Measure("AFR1234");
	}
	check(cache.MeasurementCount() > beforeTransform, "graphics transform changes invalidate text measurements");

	check(HttpHelper::IsValidHttpsUrl("https://www.hoppie.nl:443/acars/system/connect.html") &&
		!HttpHelper::IsValidHttpsUrl("https://www.hoppie.nl:444/acars/system/connect.html"),
		"integration HTTP enforces the default HTTPS port");
	check(!HttpHelper::IsValidHttpsUrl("http://www.hoppie.nl/") &&
		!HttpHelper::IsValidHttpsUrl("https://user@www.hoppie.nl/"), "integration HTTP rejects plaintext and userinfo");

	const auto hashPath = std::filesystem::temp_directory_path() /
		("vsmr-audit-sha256-" + std::to_string(GetCurrentProcessId()) + ".txt");
	{ std::ofstream output(hashPath, std::ios::binary); output << "abc"; }
	std::string digest;
	check(vsmr::updater::verification::Sha256File(hashPath, digest) &&
		digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		"RAII SHA-256 computes the standard test vector");
	std::filesystem::remove(hashPath);
	check(!vsmr::updater::verification::Sha256File(hashPath, digest) && digest.empty(),
		"a failed file hash cannot return a stale digest");
	return failures;
}

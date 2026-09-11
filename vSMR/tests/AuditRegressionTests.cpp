#include <Windows.h>
#include <EuroScopePlugIn.h>
#include <objidl.h>
#include <GdiPlus.h>

#include "AuditRegressionTests.hpp"
#include "shared/JsonDocument.hpp"
#include "tags/CompiledTagDefinition.hpp"
#include "rendering/TagRenderer.hpp"
#include "safety/RunwayTraffic.hpp"
#include "config/ProfileNormalization.hpp"
#include "tags/TagDataFormatting.hpp"
#include "rendering/TargetProjection.hpp"
#include "platform/windows/network/HttpHelper.hpp"
#include "updater/UpdaterVerification.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <random>

namespace
{
void TestProfileMigration(std::vector<std::string>& failures)
{
	auto check = [&](bool condition, const char* message)
	{ if (!condition) failures.emplace_back(message); };
	for (const char* profileJson : {
		"{}",
		R"({"filters":{"max_altitude_ft":6500,"pro_mode":{"enable":true}},"labels":{"departure":{"definition_detailed":false,"definition":["callsign"]}},"targets":{"ground_icons":{"taxi":{"r":1,"g":2,"b":3,"a":255}}}})",
		R"({"font":null,"targets":{"symbol_scale":99},"labels":{"departure":{"status_background_colors":{"push":{"r":7,"g":8,"b":9}}},"airborne":{"definition":["flightlevel"],"departure_text_color":{"r":42,"g":43,"b":44}}}})" })
	{
		rapidjson::Document profile;
		VsmrJson::ParseDocument(profile, profileJson);
		check(VsmrProfile::Normalize(profile, profile.GetAllocator()), "legacy and incomplete profiles are migrated");
		check(profile["labels"]["departure"]["definition"].IsArray() && profile["targets"]["departure"].IsObject(),
			"profile migration creates valid independent label and target sections");
		rapidjson::Document snapshot;
		snapshot.CopyFrom(profile, snapshot.GetAllocator());
		check(!VsmrProfile::Normalize(profile, profile.GetAllocator()) && profile == snapshot,
			"normalizing an already migrated profile does not mutate it or request another save");
		if (profile["filters"]["display_modes"]["active"] == "Pro")
		{
			check(profile["filters"]["display_modes"]["items"][0]["max_airborne_altitude_ft"].GetInt() == 6500,
				"legacy altitude limits survive display-mode migration");
			check(profile["targets"]["departure"]["taxi"]["r"].GetInt() == 1,
				"legacy target colors survive sibling object creation");
		}
	}
	rapidjson::Document legacyStatus;
	VsmrJson::ParseDocument(legacyStatus, R"({"labels":{"departure":{"definition":["callsign"],"definition_detailed":["deprwy"],"status_definitions":{"nsts":{"definition":["gs"],"definitionDetailled":["actype"],"definition_detailed_same_as_definition":true}}}}})");
	VsmrProfile::Normalize(legacyStatus, legacyStatus.GetAllocator());
	const auto& migratedDeparture = legacyStatus["labels"]["departure"];
	check(migratedDeparture["definition"][0][0] == "gs" && migratedDeparture["definition_detailed"][0][0] == "actype" &&
		migratedDeparture["definition_detailed_inherits_normal"].GetBool() && !migratedDeparture["status_definitions"].HasMember("nsts"),
		"legacy no-status tag definitions survive replacement of their parent label fields");

}

void TestTagDataFormatting(std::vector<std::string>& failures)
{
	auto check = [&](bool condition, const char* message)
	{ if (!condition) failures.emplace_back(message); };
	VsmrTags::TagDataInput tagInput;
	VsmrTags::TokenValues formatted;
	VsmrTags::FormatTagData(tagInput, formatted);
	check(formatted.at("actype") == "NoFPL" && formatted.at("scratchpad") == "..." && formatted.at("clearance").empty(),
		"missing radar/flight-plan data formats safe tag defaults");
	tagInput.hasFlightPlan = tagInput.receivedFlightPlan = tagInput.hasRadarTarget = tagInput.correlated = true;
	tagInput.callsign = "AFR123"; tagInput.aircraftType = "A320X";
	tagInput.assignedCommunication = 'r'; tagInput.flightPlanState = EuroScopePlugIn::FLIGHT_PLAN_STATE_ASSUMED;
	tagInput.assignedSquawk = "1000"; tagInput.squawk = "2000";
	tagInput.departureRunway = "09L"; tagInput.arrivalRunway = "27R";
	tagInput.sid = "BUBLI1A"; tagInput.scratchpad = "STAND=B12";
	tagInput.groundSpeed = 25; tagInput.flightLevel = 4000; tagInput.pressureAltitude = 4100; tagInput.transitionAltitude = 5000;
	tagInput.altitudeDelta = 50; tagInput.clearance = tagInput.lineup = true;
	VsmrTags::FormatTagData(tagInput, formatted);
	check(formatted.at("callsign") == "[AFR123/r]" && formatted.at("actype") == "A320" && formatted.at("sctype") == "A1000",
		"callsign ownership, communication and squawk-error formatting survive extraction");
	check(formatted.at("seprwy") == "09L" && formatted.at("srvrwy") == "25" && formatted.at("sate") == "B12",
		"speed/runway/gate switching preserves the exact 25-knot boundary");
	check(formatted.at("flightlevel") == "A41" && formatted.at("tendency") == "^" && formatted.at("ssid") == "BUB1A" &&
		formatted.at("groundstatus") == "LNUP" && formatted.at("clearance") == "[x]",
		"altitude, tendency, short SID, lineup and clearance tokens retain their behavior");
	tagInput.groundSpeed = 51; tagInput.proMode = true; tagInput.correlated = false;
	VsmrTags::FormatTagData(tagInput, formatted);
	check(formatted.at("flightlevel") == "NoALT" && formatted.at("tendency") == "?" && formatted.at("callsign") == formatted.at("systemid"),
		"uncorrelated primary targets use the Pro-mode fallback");
	tagInput.primary = false;
	VsmrTags::FormatTagData(tagInput, formatted);
	check(formatted.at("callsign") == "2000" && formatted.at("clearance").empty(),
		"an uncorrelated secondary target uses its squawk and loses the clearance indicator");

}

void TestRunwayRefreshes(std::vector<std::string>& failures)
{
	std::mt19937 random(0x38decf6);
	VsmrRimcasLogic::RunwayTraffic traffic;
	VsmrRimcasLogic::RunwayCountdowns countdowns;
	for (int frame = 0; frame < 300; ++frame)
	{
		traffic.Clear();
		countdowns.Clear();
		std::multimap<std::string, std::string> expectedTraffic;
		std::map<std::pair<std::string, int>, std::string> expectedCountdowns;
		const unsigned count = random() % 100;
		for (unsigned i = 0; i < count; ++i)
		{
			const auto runway = std::to_string(random() % 6);
			const auto callsign = "TEST" + std::to_string(random() % 20);
			const int seconds = (random() % 4) * 15;
			traffic.Add(runway, callsign);
			countdowns.Set(runway, seconds, callsign);
			expectedTraffic.emplace(runway, callsign);
			expectedCountdowns[{runway, seconds}] = callsign;
		}
		traffic.Sort();
		for (int r = 0; r < 7; ++r)
		{
			const auto runway = std::to_string(r);
			const auto actual = traffic.EqualRange(runway);
			const auto expected = expectedTraffic.equal_range(runway);
			if (!std::equal(actual.first, actual.second, expected.first, expected.second,
				[](const auto& a, const auto& b) { return a.first == b.first && a.second == b.second; }))
			{ failures.emplace_back("runway refreshes preserve multimap ordering, duplicates and removals"); return; }
			if (countdowns.HasRunway(runway) != (expected.first != expected.second))
			{ failures.emplace_back("countdown runway lookup excludes stale refresh records"); return; }
			for (int seconds = 0; seconds <= 60; seconds += 15)
			{
				const auto* value = countdowns.Find(runway, seconds);
				const auto found = expectedCountdowns.find({runway, seconds});
				if ((value != nullptr) != (found != expectedCountdowns.end()) ||
					(value != nullptr && *value != found->second))
				{ failures.emplace_back("countdown refreshes preserve the last aircraft in each runway/time slot"); return; }
			}
		}
	}
}

void TestTargetProjection(std::vector<std::string>& failures)
{
	auto check = [&](bool condition, const char* message)
	{ if (!condition) failures.emplace_back(message); };
	VsmrScene::Target projectedSource;
	projectedSource.position = { 2, 3, true };
	projectedSource.style.icon = VsmrScene::IconStyle::Nova;
	projectedSource.style.showPrimaryReturn = true;
	projectedSource.primaryReturnPolygon = { {0, 0, true}, {0, 6, true}, {6, 0, true}, {} };
	projectedSource.primaryReturnAfterglow[0] = projectedSource.primaryReturnPolygon;
	projectedSource.trailPositions = { {1, 1, true}, {}, {2, 20, true}, {3, 3, true} };
	VsmrScene::TargetPresentation presentation;
	presentation.symbolScale = 2;
	VsmrTargetRendering::ProjectedTarget projected;
	int projections = 0;
	const auto project = [&](const VsmrScene::GeoPoint& point) -> POINT
	{ ++projections; return { static_cast<LONG>(point.longitude), static_cast<LONG>(point.latitude) }; };
	const auto visible = [](const POINT& point, int) { return point.x < 10; };
	VsmrTargetRendering::DrawOptions drawOptions;
	projected.Update(projectedSource, presentation, drawOptions, project, visible);
	check(projections == 10 && projected.primary.size() == 3 && projected.primary[0].X == -2 &&
		projected.trail.size() == 2 && projected.trail[1].index == 3,
		"concrete projection skips invalid points, scales polygons, clips trails and retains original trail age");
	const auto polygonCapacity = projected.primary.capacity();
	drawOptions.drawTrail = drawOptions.drawPrimaryReturn = false;
	projected.Update(projectedSource, presentation, drawOptions, project, visible);
	check(projected.primary.empty() && projected.trail.empty() && projected.afterglow[0].empty() && projected.primary.capacity() == polygonCapacity,
		"disabled projection layers clear old coordinates while retaining storage");
	projectedSource.style.icon = VsmrScene::IconStyle::Triangle;
	projectedSource.headingProbe = { 4, 3, true };
	drawOptions.drawTrail = drawOptions.drawPrimaryReturn = true;
	presentation.trailEnabled = false;
	projections = 0;
	projected.Update(projectedSource, presentation, drawOptions, project, visible);
	check(projections == 2 && projected.heading.x == 3 && projected.heading.y == 4 &&
		projected.primary.empty() && projected.afterglow[0].empty() && projected.trail.empty(),
		"switching away from Nova clears primary returns and uses the viewport's projected heading");

}
}

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

	TestProfileMigration(failures);
	TestTagDataFormatting(failures);
	TestTargetProjection(failures);
	TestRunwayRefreshes(failures);

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
	const auto* retainedLine = normal.lines.data();
	const auto* retainedElements = normal.lines[0].elements.data();
	VsmrTags::UpdateTagVariant(compiled, target, false, normal);
	check(!VsmrTags::UpdateTagVariant(compiled, target, false, normal),
		"unchanged live inputs skip rebuilding the tag model");
	target.tag.tokens["unused"] = "change";
	check(!VsmrTags::UpdateTagVariant(compiled, target, false, normal),
		"unreferenced token changes do not rebuild tag models");
	target.tag.tokens["gs"] = "30";
	check(VsmrTags::UpdateTagVariant(compiled, target, false, normal) && normal.lines[0].elements[1].text == "gs123/30",
		"changed referenced tokens update cached text");
	check(normal.lines.data() == retainedLine && normal.lines[0].elements.data() == retainedElements,
		"live updates retain tag line and element storage");
	target.correlated = false;
	VsmrTags::UpdateTagVariant(compiled, target, false, normal);
	check(normal.lines[0].elements[4].text.empty(), "loss of correlation removes a cached clearance indicator");
	target.correlated = true;
	target.tag.tokens["gs"] = "25";
	labels["departure"]["definition"][0][0].SetString("gs", labels.GetAllocator());
	definitions.Clear();
	check(VsmrTags::BuildTagVariant(definitions.Get(labels, "departure", "default", false), target, false).lines[0].elements[0].text == "25",
		"invalidating a tag cache makes profile edits visible");
	VsmrTags::UpdateTagVariant(definitions.Get(labels, "departure", "default", false), target, false, normal);
	check(normal.lines[0].elements[0].text == "25", "profile reload invalidates an existing scene tag model");

	VsmrRimcasLogic::RunwayTraffic traffic;
	traffic.Add("09", "AFR3"); traffic.Add("27", "AFR2"); traffic.Add("09", "AFR1");
	traffic.Sort();
	const auto occupants = traffic.EqualRange("09");
	check(std::distance(occupants.first, occupants.second) == 2 && occupants.first->second == "AFR3",
		"runway grouping preserves insertion order within a runway");
	check(traffic.EqualRange("18").first == traffic.EqualRange("18").second,
		"a missing runway has no occupants");
	const auto trafficCapacity = traffic.Capacity();
	traffic.Clear(); traffic.Sort();
	check(traffic.EqualRange("09").first == traffic.EqualRange("09").second && traffic.Capacity() == trafficCapacity,
		"runway records clear stale aircraft while retaining frame storage");
	VsmrRimcasLogic::RunwayCountdowns countdowns;
	countdowns.Set("09", 30, "AFR1"); countdowns.Set("27", 30, "AFR2"); countdowns.Set("09", 30, "AFR3");
	check(countdowns.Find("09", 30) && *countdowns.Find("09", 30) == "AFR3" &&
		*countdowns.Find("27", 30) == "AFR2" && !countdowns.Find("09", 45),
		"countdown slots preserve replacement behavior and isolate runway/time keys");
	const auto countdownCapacity = countdowns.Capacity();
	countdowns.Clear();
	check(!countdowns.HasRunway("09") && !countdowns.Find("27", 30) && countdowns.Capacity() == countdownCapacity,
		"countdowns do not retain stale entries across refreshes");
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
	// Empty definition lines retain their storage but must consume no display row.
	rapidjson::Document emptyLabels;
	VsmrJson::ParseDocument(emptyLabels, R"({"departure":{"definition":["scratchpad","callsign"]}})");
	VsmrTags::DefinitionCache emptyDefinitions;
	target.tag.tokens["scratchpad"] = "..."; target.tag.tokens["callsign"] = "AFR1";
	auto hiddenLineTag = VsmrTags::BuildTagVariant(emptyDefinitions.Get(emptyLabels, "departure", "default", false), target, false);
	{
		VsmrTagRendering::FontContext fonts(graphics, &font, 1, &cache);
		VsmrTagRendering::Layout layout;
		VsmrTagRendering::MeasureLayout(fonts, hiddenLineTag, layout);
		check(layout.lines.size() == 1 && layout.lines[0].elements[0].text == "AFR1",
			"cached empty lines do not create blank tag rows");
		target.tag.tokens["scratchpad"] = "TAXI";
		VsmrTags::UpdateTagVariant(emptyDefinitions.Get(emptyLabels, "departure", "default", false), target, false, hiddenLineTag);
		VsmrTagRendering::MeasureLayout(fonts, hiddenLineTag, layout);
		check(layout.lines.size() == 2 && layout.lines[0].elements[0].text == "TAXI",
			"a newly populated cached line reappears in its original position");
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

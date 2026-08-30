#pragma once

#include "rapidjson/document.h"

#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct AvisoFeatureSummary
{
	int featureIndex = -1;
	std::string featureId;
	std::string nameText;
	std::string layer;
	std::string category;
	std::string objectType;
	std::string geometryType;
	std::string styleId;
	bool visible = true;
	bool editableText = false;
	std::string searchText;
};

struct AvisoFeatureFilter
{
	std::string search;
	std::string layer;
	std::string category;
	std::string objectType;
	std::string geometryType;
	std::string visibility;
	std::string styleId;
};

struct AvisoValidationResult
{
	bool ok = true;
	std::string errorText;
};

class AvisoDocumentModel
{
public:
	inline static constexpr size_t MaximumSerializedInputBytes =
		32U * 1024U * 1024U;

	rapidjson::Document& MutableDocument();
	const rapidjson::Document& GetDocument() const;

	void ResetToEmpty();
	bool LoadFromFile(const std::string& path, std::string& errorText);
	static bool ReadBoundedSourceFile(
		const std::filesystem::path& path,
		std::string& sourceJson,
		std::string& errorText) noexcept;
	static bool ValidateSerializedInputLimits(
		const std::string& sourceJson,
		std::string& errorText);
	void CreateEmptyFeatureCollection();
	bool ValidateLoadedFeatureCollection(std::string& errorText) const;
	bool SaveAtomically(
		const std::string& path,
		std::string& errorText);

	void MarkIndexesDirty();
	void BuildIndexes();
	void EnsureIndexes();

	int FeatureCount() const;
	int StyleCount() const;
	bool HasStyleCatalog() const;

	const std::vector<AvisoFeatureSummary>& GetSummaries();
	const AvisoFeatureSummary* GetSummaryByFeatureIndex(int featureIndex);
	const AvisoFeatureSummary* GetSummaryByFeatureId(const std::string& featureId);
	std::vector<int> FilterFeatures(const AvisoFeatureFilter& filter);

	const std::set<std::string>& GetLayers();
	const std::set<std::string>& GetCategories();
	const std::set<std::string>& GetObjectTypes();
	const std::set<std::string>& GetGeometryTypes();
	const std::set<std::string>& GetStyleIds();

	std::string MakeUniqueFeatureId(const std::string& preferredPrefix);
	void EnsureFeatureId(rapidjson::Value& feature, const std::string& preferredPrefix);
	void NoteFeatureInserted(int featureIndex);
	void NoteFeatureDeleted(int featureIndex);
	void MarkFeatureGeometryDirty(int featureIndex);
	AvisoValidationResult ValidateAndRecalculate();

	static std::string ReadStringProperty(const rapidjson::Value* properties, const char* key, const std::string& fallback = "");
	static bool ReadBoolProperty(const rapidjson::Value* properties, const char* key, bool fallback);
	static bool IsFeatureVisible(const rapidjson::Value* properties);
	static std::string GeometryTypeFromFeature(const rapidjson::Value& feature);
	static bool IsEditableTextFeature(const rapidjson::Value& feature);

private:
	rapidjson::Document Document;
	bool IndexesDirty = true;
	unsigned long long RuntimeIdCounter = 0;
	std::vector<std::string> RuntimeFeatureIds;
	std::vector<AvisoFeatureSummary> Summaries;
	std::vector<int> SummaryPositionByFeatureIndex;
	std::unordered_map<std::string, size_t> FeatureIdToIndex;
	std::unordered_map<std::string, std::string> OriginalCoordinatesJsonByFeatureId;
	std::set<std::string> GeometryDirtyFeatureIds;
	std::set<std::string> Layers;
	std::set<std::string> Categories;
	std::set<std::string> ObjectTypes;
	std::set<std::string> GeometryTypes;
	std::set<std::string> StyleIds;

	const rapidjson::Value* GetFeatureArray() const;
	rapidjson::Value* GetFeatureArray();
	static std::string TrimAsciiWhitespaceCopy(const std::string& text);
	static std::string ToLowerAscii(std::string value);
	static std::string ToUpperAscii(std::string value);
	static bool EqualsNoCase(const std::string& left, const std::string& right);
	static bool SearchMatches(const std::string& searchText, const std::string& query);
	static std::string BuildDisplayText(const rapidjson::Value& feature, int featureIndex);
	static std::string BuildSearchText(const AvisoFeatureSummary& summary, const rapidjson::Value* properties);
	static bool IsGeometryCoordinatesValid(const rapidjson::Value& geometry);
	static bool HasDuplicatePersistedFeatureIds(const rapidjson::Value& features, std::string& duplicateId);
	static bool FindMatchingJsonBracket(const std::string& json, size_t openOffset, char openChar, char closeChar, size_t& closeOffset);
	static bool FindJsonStringKey(const std::string& json, size_t searchStart, size_t searchEnd, const char* key, size_t& keyOffset);
	static bool FindCoordinatesJsonRangeFromFeatureText(const std::string& json, size_t featureStart, size_t featureEnd, size_t& coordinatesStart, size_t& coordinatesEnd);
	static bool ExtractCoordinatesJsonFromFeatureText(const std::string& json, size_t featureStart, size_t featureEnd, std::string& coordinatesJson);
	std::string FeatureIdentityForPreservation(const rapidjson::Value& feature, int featureIndex) const;
	std::string GenerateRuntimeFeatureId();
	void EnsureRuntimeFeatureIds();
	void AssignRuntimeFeatureIdsForCurrentDocument();
	void CaptureOriginalCoordinatesJson(const std::string& sourceJson);
	void PatchSerializedCoordinates(std::string& serializedJson) const;
	void SetNumberMember(rapidjson::Value& object, const char* key, int value);
	void SetObjectMember(rapidjson::Value& object, const char* key, rapidjson::Value& value);
};

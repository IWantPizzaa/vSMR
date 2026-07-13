#include "stdafx.h"
#include "AvisoDocumentModel.hpp"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

rapidjson::Document& AvisoDocumentModel::MutableDocument()
{
	return Document;
}

const rapidjson::Document& AvisoDocumentModel::GetDocument() const
{
	return Document;
}

void AvisoDocumentModel::ResetToEmpty()
{
	Document.SetObject();
	OriginalCoordinatesJsonByFeatureId.clear();
	GeometryDirtyFeatureIds.clear();
	EnsureFeatureCollection();
	MarkIndexesDirty();
}

bool AvisoDocumentModel::LoadFromFile(const std::string& path, std::string& errorText)
{
	errorText.clear();
	std::string sourceJson;
	if (path.empty())
	{
		errorText = "No AVISO path is available.";
		return false;
	}

	try
	{
		if (!std::filesystem::exists(path))
		{
			ResetToEmpty();
			return true;
		}

		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			errorText = "Unable to open AVISO file.";
			return false;
		}

		std::stringstream buffer;
		buffer << input.rdbuf();
		sourceJson = buffer.str();
		Document.Parse<0>(sourceJson.c_str());
		if (Document.HasParseError())
		{
			errorText = "AVISO GeoJSON parse failed at offset " + std::to_string(Document.GetErrorOffset()) + ".";
			Document.SetObject();
			MarkIndexesDirty();
			return false;
		}
	}
	catch (const std::exception& ex)
	{
		errorText = "AVISO load failed: " + std::string(ex.what());
		return false;
	}
	catch (...)
	{
		errorText = "AVISO load failed.";
		return false;
	}

	EnsureFeatureCollection();
	BuildIndexes();
	CaptureOriginalCoordinatesJson(sourceJson);
	return true;
}

bool AvisoDocumentModel::EnsureFeatureCollection()
{
	if (!Document.IsObject())
		Document.SetObject();

	rapidjson::Document::AllocatorType& allocator = Document.GetAllocator();
	if (!Document.HasMember("type") || !Document["type"].IsString())
	{
		if (Document.HasMember("type"))
			Document.RemoveMember("type");
		rapidjson::Value typeValue;
		typeValue.SetString("FeatureCollection", allocator);
		Document.AddMember("type", typeValue, allocator);
	}

	if (!Document.HasMember("features") || !Document["features"].IsArray())
	{
		if (Document.HasMember("features"))
			Document.RemoveMember("features");
		rapidjson::Value features(rapidjson::kArrayType);
		Document.AddMember("features", features, allocator);
	}

	return true;
}

bool AvisoDocumentModel::SaveAtomically(const std::string& path, std::string& errorText)
{
	errorText.clear();
	if (path.empty())
	{
		errorText = "No AVISO path is available.";
		return false;
	}

	EnsureFeatureCollection();
	RecalculateMetadata();

	rapidjson::StringBuffer buffer;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
	writer.SetIndent('\t', 1);
	Document.Accept(writer);
	std::string serializedJson(buffer.GetString(), buffer.Size());
	PatchSerializedCoordinates(serializedJson);

	rapidjson::Document validation;
	if (validation.Parse<0>(serializedJson.c_str()).HasParseError() ||
		!validation.IsObject() ||
		!validation.HasMember("features") ||
		!validation["features"].IsArray())
	{
		errorText = "AVISO save validation failed before writing.";
		return false;
	}

	try
	{
		const std::filesystem::path outputPath(path);
		if (outputPath.has_parent_path())
			std::filesystem::create_directories(outputPath.parent_path());

		const std::filesystem::path tempPath = outputPath.string() + ".tmp";
		{
			std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
			if (!output.is_open())
			{
				errorText = "Unable to write AVISO temp file.";
				return false;
			}
			output.write(serializedJson.c_str(), static_cast<std::streamsize>(serializedJson.size()));
			output.close();
			if (!output)
			{
				errorText = "Unable to flush AVISO temp file.";
				std::error_code ignored;
				std::filesystem::remove(tempPath, ignored);
				return false;
			}
		}

		if (!::MoveFileExA(tempPath.string().c_str(), outputPath.string().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			const DWORD error = ::GetLastError();
			errorText = "Unable to replace AVISO file atomically. Windows error " + std::to_string(error) + ".";
			std::error_code ignored;
			std::filesystem::remove(tempPath, ignored);
			return false;
		}
	}
	catch (const std::exception& ex)
	{
		errorText = "AVISO save failed: " + std::string(ex.what());
		return false;
	}
	catch (...)
	{
		errorText = "AVISO save failed.";
		return false;
	}

	BuildIndexes();
	return true;
}

void AvisoDocumentModel::MarkIndexesDirty()
{
	IndexesDirty = true;
}

void AvisoDocumentModel::BuildIndexes()
{
	Summaries.clear();
	FeatureIdToIndex.clear();
	Layers.clear();
	Categories.clear();
	ObjectTypes.clear();
	GeometryTypes.clear();
	StyleIds.clear();

	if (Document.IsObject() && Document.HasMember("styles") && Document["styles"].IsObject())
	{
		const rapidjson::Value& styles = Document["styles"];
		for (rapidjson::Value::ConstMemberIterator it = styles.MemberBegin(); it != styles.MemberEnd(); ++it)
		{
			if (it->name.IsString() && it->name.GetString()[0] != '\0')
				StyleIds.insert(it->name.GetString());
		}
	}

	const rapidjson::Value* features = GetFeatureArray();
	if (features == nullptr)
	{
		IndexesDirty = false;
		return;
	}

	Summaries.reserve(features->Size());
	for (rapidjson::SizeType i = 0; i < features->Size(); ++i)
	{
		const rapidjson::Value& feature = (*features)[i];
		if (!feature.IsObject())
			continue;

		const rapidjson::Value* properties = nullptr;
		if (feature.HasMember("properties") && feature["properties"].IsObject())
			properties = &feature["properties"];

		AvisoFeatureSummary summary;
		summary.featureIndex = static_cast<int>(i);
		if (feature.HasMember("id") && feature["id"].IsString())
			summary.featureId = feature["id"].GetString();
		if (summary.featureId.empty())
			summary.featureId = "__editor_feature_" + std::to_string(i + 1);
		summary.nameText = BuildDisplayText(feature, summary.featureIndex);
		summary.layer = ReadStringProperty(properties, "layer");
		summary.category = ReadStringProperty(properties, "category");
		summary.objectType = ReadStringProperty(properties, "object_type", ReadStringProperty(properties, "type"));
		summary.geometryType = GeometryTypeFromFeature(feature);
		summary.styleId = ReadStringProperty(properties, "style_id");
		summary.visible = IsFeatureVisible(properties);
		summary.editableText = IsEditableTextFeature(feature);
		summary.searchText = BuildSearchText(summary, properties);

		if (!summary.layer.empty())
			Layers.insert(summary.layer);
		if (!summary.category.empty())
			Categories.insert(summary.category);
		if (!summary.objectType.empty())
			ObjectTypes.insert(summary.objectType);
		if (!summary.geometryType.empty())
			GeometryTypes.insert(summary.geometryType);
		if (!summary.styleId.empty())
			StyleIds.insert(summary.styleId);

		FeatureIdToIndex[summary.featureId] = static_cast<size_t>(summary.featureIndex);
		Summaries.push_back(std::move(summary));
	}

	IndexesDirty = false;
}

void AvisoDocumentModel::EnsureIndexes()
{
	if (IndexesDirty)
		BuildIndexes();
}

int AvisoDocumentModel::FeatureCount() const
{
	const rapidjson::Value* features = GetFeatureArray();
	return features != nullptr ? static_cast<int>(features->Size()) : 0;
}

int AvisoDocumentModel::StyleCount() const
{
	if (!Document.IsObject() || !Document.HasMember("styles") || !Document["styles"].IsObject())
		return 0;
	int count = 0;
	for (rapidjson::Value::ConstMemberIterator it = Document["styles"].MemberBegin(); it != Document["styles"].MemberEnd(); ++it)
		++count;
	return count;
}

bool AvisoDocumentModel::HasStyleCatalog() const
{
	return StyleCount() > 0;
}

const std::vector<AvisoFeatureSummary>& AvisoDocumentModel::GetSummaries()
{
	EnsureIndexes();
	return Summaries;
}

const AvisoFeatureSummary* AvisoDocumentModel::GetSummaryByFeatureIndex(int featureIndex)
{
	EnsureIndexes();
	for (const AvisoFeatureSummary& summary : Summaries)
	{
		if (summary.featureIndex == featureIndex)
			return &summary;
	}
	return nullptr;
}

const AvisoFeatureSummary* AvisoDocumentModel::GetSummaryByFeatureId(const std::string& featureId)
{
	EnsureIndexes();
	const auto found = FeatureIdToIndex.find(featureId);
	if (found == FeatureIdToIndex.end())
		return nullptr;
	return GetSummaryByFeatureIndex(static_cast<int>(found->second));
}

std::vector<int> AvisoDocumentModel::FilterFeatures(const AvisoFeatureFilter& filter)
{
	EnsureIndexes();
	std::vector<int> indices;
	indices.reserve(Summaries.size());
	for (const AvisoFeatureSummary& summary : Summaries)
	{
		if (!SearchMatches(summary.searchText, filter.search))
			continue;
		if (!filter.layer.empty() && !EqualsNoCase(filter.layer, "All") && !EqualsNoCase(summary.layer, filter.layer))
			continue;
		if (!filter.category.empty() && !EqualsNoCase(filter.category, "All") && !EqualsNoCase(summary.category, filter.category))
			continue;
		if (!filter.objectType.empty() && !EqualsNoCase(filter.objectType, "All") && !EqualsNoCase(summary.objectType, filter.objectType))
			continue;
		if (!filter.geometryType.empty() && !EqualsNoCase(filter.geometryType, "All") && !EqualsNoCase(summary.geometryType, filter.geometryType))
			continue;
		if (!filter.styleId.empty() && !EqualsNoCase(filter.styleId, "All") && !EqualsNoCase(summary.styleId, filter.styleId))
			continue;
		if (EqualsNoCase(filter.visibility, "Visible") && !summary.visible)
			continue;
		if (EqualsNoCase(filter.visibility, "Hidden") && summary.visible)
			continue;
		indices.push_back(summary.featureIndex);
	}
	return indices;
}

const std::set<std::string>& AvisoDocumentModel::GetLayers()
{
	EnsureIndexes();
	return Layers;
}

const std::set<std::string>& AvisoDocumentModel::GetCategories()
{
	EnsureIndexes();
	return Categories;
}

const std::set<std::string>& AvisoDocumentModel::GetObjectTypes()
{
	EnsureIndexes();
	return ObjectTypes;
}

const std::set<std::string>& AvisoDocumentModel::GetGeometryTypes()
{
	EnsureIndexes();
	return GeometryTypes;
}

const std::set<std::string>& AvisoDocumentModel::GetStyleIds()
{
	EnsureIndexes();
	return StyleIds;
}

std::string AvisoDocumentModel::MakeUniqueFeatureId(const std::string& preferredPrefix)
{
	EnsureIndexes();
	std::string prefix = TrimAsciiWhitespaceCopy(preferredPrefix);
	if (prefix.empty())
		prefix = "editor.feature";
	std::replace(prefix.begin(), prefix.end(), ' ', '_');

	for (int suffix = 1; suffix < 1000000; ++suffix)
	{
		std::ostringstream stream;
		stream << prefix << "." << suffix;
		const std::string candidate = stream.str();
		if (FeatureIdToIndex.find(candidate) == FeatureIdToIndex.end())
			return candidate;
	}
	return prefix + "." + std::to_string(::GetTickCount());
}

void AvisoDocumentModel::EnsureFeatureId(rapidjson::Value& feature, const std::string& preferredPrefix)
{
	if (!feature.IsObject())
		return;
	if (feature.HasMember("id") && feature["id"].IsString() && feature["id"].GetString()[0] != '\0')
		return;

	if (feature.HasMember("id"))
		feature.RemoveMember("id");
	const std::string id = MakeUniqueFeatureId(preferredPrefix);
	rapidjson::Value idValue;
	idValue.SetString(id.c_str(), static_cast<rapidjson::SizeType>(id.size()), Document.GetAllocator());
	feature.AddMember("id", idValue, Document.GetAllocator());
	MarkIndexesDirty();
}

void AvisoDocumentModel::MarkFeatureGeometryDirty(int featureIndex)
{
	const rapidjson::Value* features = GetFeatureArray();
	if (features == nullptr || featureIndex < 0 || static_cast<rapidjson::SizeType>(featureIndex) >= features->Size())
		return;
	const std::string identity = FeatureIdentityForPreservation((*features)[static_cast<rapidjson::SizeType>(featureIndex)], featureIndex);
	if (!identity.empty())
		GeometryDirtyFeatureIds.insert(identity);
}

void AvisoDocumentModel::RecalculateMetadata()
{
	if (!Document.IsObject())
		return;

	const int featureCount = FeatureCount();
	std::map<std::string, int> styleFeatureCounts;
	const rapidjson::Value* features = GetFeatureArray();
	if (features != nullptr)
	{
		for (rapidjson::SizeType i = 0; i < features->Size(); ++i)
		{
			const rapidjson::Value& feature = (*features)[i];
			if (!feature.IsObject() || !feature.HasMember("properties") || !feature["properties"].IsObject())
				continue;
			const std::string styleId = ReadStringProperty(&feature["properties"], "style_id");
			if (!styleId.empty())
				++styleFeatureCounts[styleId];
		}
	}

	if (Document.HasMember("styles") && Document["styles"].IsObject())
	{
		rapidjson::Value& styles = Document["styles"];
		for (rapidjson::Value::MemberIterator it = styles.MemberBegin(); it != styles.MemberEnd(); ++it)
		{
			if (!it->value.IsObject() || !it->name.IsString())
				continue;
			SetNumberMember(it->value, "feature_count", styleFeatureCounts[it->name.GetString()]);
		}
	}

	if (Document.HasMember("metadata") && Document["metadata"].IsObject())
	{
		SetNumberMember(Document["metadata"], "feature_count", featureCount);
		SetNumberMember(Document["metadata"], "style_count", StyleCount());
	}

	MarkIndexesDirty();
}

std::string AvisoDocumentModel::ReadStringProperty(const rapidjson::Value* properties, const char* key, const std::string& fallback)
{
	if (properties == nullptr || !properties->IsObject() || key == nullptr || !properties->HasMember(key) || !(*properties)[key].IsString())
		return fallback;
	return (*properties)[key].GetString();
}

bool AvisoDocumentModel::ReadBoolProperty(const rapidjson::Value* properties, const char* key, bool fallback)
{
	if (properties == nullptr || !properties->IsObject() || key == nullptr || !properties->HasMember(key))
		return fallback;
	if ((*properties)[key].IsBool())
		return (*properties)[key].GetBool();
	if ((*properties)[key].IsString())
	{
		const std::string value = ToUpperAscii(TrimAsciiWhitespaceCopy((*properties)[key].GetString()));
		if (value == "FALSE" || value == "0" || value == "NO" || value == "HIDDEN" || value == "NONE" || value == "OFF")
			return false;
		if (value == "TRUE" || value == "1" || value == "YES" || value == "VISIBLE" || value == "ON")
			return true;
	}
	return fallback;
}

bool AvisoDocumentModel::IsFeatureVisible(const rapidjson::Value* properties)
{
	if (!ReadBoolProperty(properties, "visible", true))
		return false;

	const std::string visibility = ToUpperAscii(TrimAsciiWhitespaceCopy(ReadStringProperty(properties, "visibility")));
	if (visibility == "NONE" || visibility == "HIDDEN" || visibility == "FALSE" || visibility == "OFF" || visibility == "0")
		return false;
	return true;
}

std::string AvisoDocumentModel::GeometryTypeFromFeature(const rapidjson::Value& feature)
{
	if (!feature.IsObject() ||
		!feature.HasMember("geometry") ||
		!feature["geometry"].IsObject() ||
		!feature["geometry"].HasMember("type") ||
		!feature["geometry"]["type"].IsString())
	{
		return "";
	}
	return feature["geometry"]["type"].GetString();
}

bool AvisoDocumentModel::IsEditableTextFeature(const rapidjson::Value& feature)
{
	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];
	if (properties == nullptr)
		return false;

	if (EqualsNoCase(ReadStringProperty(properties, "object_type"), "Label"))
		return true;
	const std::string role = ToUpperAscii(ReadStringProperty(properties, "geometry_role"));
	if (role == "TEXT_LABEL")
		return true;

	const char* textKeys[] = { "text-field", "text", "label", "name", "title", "description" };
	for (const char* key : textKeys)
	{
		if (properties->HasMember(key) && (*properties)[key].IsString())
			return true;
	}
	return false;
}

const rapidjson::Value* AvisoDocumentModel::GetFeatureArray() const
{
	if (!Document.IsObject() || !Document.HasMember("features") || !Document["features"].IsArray())
		return nullptr;
	return &Document["features"];
}

rapidjson::Value* AvisoDocumentModel::GetFeatureArray()
{
	if (!Document.IsObject() || !Document.HasMember("features") || !Document["features"].IsArray())
		return nullptr;
	return &Document["features"];
}

std::string AvisoDocumentModel::TrimAsciiWhitespaceCopy(const std::string& text)
{
	size_t start = 0;
	while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0)
		++start;
	size_t end = text.size();
	while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
		--end;
	return text.substr(start, end - start);
}

std::string AvisoDocumentModel::ToLowerAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

std::string AvisoDocumentModel::ToUpperAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::toupper(c));
	});
	return value;
}

bool AvisoDocumentModel::EqualsNoCase(const std::string& left, const std::string& right)
{
	if (left.size() != right.size())
		return false;
	for (size_t i = 0; i < left.size(); ++i)
	{
		if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i])))
			return false;
	}
	return true;
}

bool AvisoDocumentModel::SearchMatches(const std::string& searchText, const std::string& query)
{
	const std::string normalizedQuery = ToLowerAscii(TrimAsciiWhitespaceCopy(query));
	if (normalizedQuery.empty())
		return true;

	std::istringstream stream(normalizedQuery);
	std::string token;
	while (stream >> token)
	{
		if (searchText.find(token) == std::string::npos)
			return false;
	}
	return true;
}

std::string AvisoDocumentModel::BuildDisplayText(const rapidjson::Value& feature, int featureIndex)
{
	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];

	const std::string name = ReadStringProperty(properties, "name");
	const std::string text = ReadStringProperty(properties, "text-field",
		ReadStringProperty(properties, "text",
			ReadStringProperty(properties, "label",
				ReadStringProperty(properties, "title",
					ReadStringProperty(properties, "description")))));
	if (!name.empty() && !text.empty() && !EqualsNoCase(name, text))
		return name + " | " + text;
	if (!name.empty())
		return name;
	if (!text.empty())
		return text;

	const std::string layer = ReadStringProperty(properties, "layer");
	if (!layer.empty())
		return layer;
	return "Object " + std::to_string(featureIndex + 1);
}

std::string AvisoDocumentModel::BuildSearchText(const AvisoFeatureSummary& summary, const rapidjson::Value* properties)
{
	std::string text = summary.featureId + " " + summary.nameText + " " + summary.layer + " " + summary.category + " " +
		summary.objectType + " " + summary.geometryType + " " + summary.styleId;

	const char* keys[] = {
		"name", "text-field", "text", "label", "title", "description", "layer", "category",
		"object_type", "type", "geometry_role", "label_class", "section", "style_id",
		"group_id", "layout", "variant", "source_section"
	};
	for (const char* key : keys)
	{
		const std::string value = ReadStringProperty(properties, key);
		if (!value.empty())
		{
			text += " ";
			text += value;
		}
	}
	return ToLowerAscii(text);
}

bool AvisoDocumentModel::FindMatchingJsonBracket(const std::string& json, size_t openOffset, char openChar, char closeChar, size_t& closeOffset)
{
	if (openOffset >= json.size() || json[openOffset] != openChar)
		return false;

	bool inString = false;
	bool escaped = false;
	int depth = 0;
	for (size_t i = openOffset; i < json.size(); ++i)
	{
		const char c = json[i];
		if (inString)
		{
			if (escaped)
			{
				escaped = false;
				continue;
			}
			if (c == '\\')
			{
				escaped = true;
				continue;
			}
			if (c == '"')
				inString = false;
			continue;
		}

		if (c == '"')
		{
			inString = true;
			continue;
		}
		if (c == openChar)
			++depth;
		else if (c == closeChar)
		{
			--depth;
			if (depth == 0)
			{
				closeOffset = i;
				return true;
			}
		}
	}
	return false;
}

bool AvisoDocumentModel::FindJsonStringKey(const std::string& json, size_t searchStart, size_t searchEnd, const char* key, size_t& keyOffset)
{
	if (key == nullptr || searchStart >= json.size())
		return false;
	searchEnd = (std::min)(searchEnd, json.size());
	const size_t keyLength = std::strlen(key);
	for (size_t i = searchStart; i < searchEnd; ++i)
	{
		if (json[i] != '"')
			continue;

		size_t j = i + 1;
		bool escaped = false;
		bool hasEscape = false;
		while (j < searchEnd)
		{
			const char c = json[j];
			if (escaped)
			{
				escaped = false;
				hasEscape = true;
				++j;
				continue;
			}
			if (c == '\\')
			{
				escaped = true;
				hasEscape = true;
				++j;
				continue;
			}
			if (c == '"')
				break;
			++j;
		}
		if (j >= searchEnd)
			return false;

		const size_t textLength = j - i - 1;
		size_t colon = j + 1;
		while (colon < searchEnd && std::isspace(static_cast<unsigned char>(json[colon])) != 0)
			++colon;
		if (!hasEscape && textLength == keyLength && colon < searchEnd && json[colon] == ':' &&
			json.compare(i + 1, keyLength, key) == 0)
		{
			keyOffset = i;
			return true;
		}
		i = j;
	}
	return false;
}

bool AvisoDocumentModel::FindCoordinatesJsonRangeFromFeatureText(const std::string& json, size_t featureStart, size_t featureEnd, size_t& coordinatesStart, size_t& coordinatesEnd)
{
	size_t geometryKey = std::string::npos;
	size_t searchStart = featureStart;
	size_t searchEnd = featureEnd;
	if (FindJsonStringKey(json, featureStart, featureEnd, "geometry", geometryKey))
	{
		size_t geometryValue = json.find(':', geometryKey);
		if (geometryValue != std::string::npos)
		{
			++geometryValue;
			while (geometryValue < featureEnd && std::isspace(static_cast<unsigned char>(json[geometryValue])) != 0)
				++geometryValue;
			if (geometryValue < featureEnd && json[geometryValue] == '{')
			{
				size_t geometryEnd = 0;
				if (FindMatchingJsonBracket(json, geometryValue, '{', '}', geometryEnd))
				{
					searchStart = geometryValue;
					searchEnd = geometryEnd;
				}
			}
		}
	}

	size_t coordinatesKey = std::string::npos;
	if (!FindJsonStringKey(json, searchStart, searchEnd, "coordinates", coordinatesKey))
		return false;

	size_t valueStart = json.find(':', coordinatesKey);
	if (valueStart == std::string::npos)
		return false;
	++valueStart;
	while (valueStart < searchEnd && std::isspace(static_cast<unsigned char>(json[valueStart])) != 0)
		++valueStart;
	if (valueStart >= searchEnd || json[valueStart] != '[')
		return false;

	size_t valueEnd = 0;
	if (!FindMatchingJsonBracket(json, valueStart, '[', ']', valueEnd))
		return false;
	coordinatesStart = valueStart;
	coordinatesEnd = valueEnd;
	return true;
}

bool AvisoDocumentModel::ExtractCoordinatesJsonFromFeatureText(const std::string& json, size_t featureStart, size_t featureEnd, std::string& coordinatesJson)
{
	size_t coordinatesStart = 0;
	size_t coordinatesEnd = 0;
	if (!FindCoordinatesJsonRangeFromFeatureText(json, featureStart, featureEnd, coordinatesStart, coordinatesEnd))
		return false;
	coordinatesJson = json.substr(coordinatesStart, coordinatesEnd - coordinatesStart + 1);
	return true;
}

std::string AvisoDocumentModel::FeatureIdentityForPreservation(const rapidjson::Value& feature, int featureIndex)
{
	if (feature.IsObject() && feature.HasMember("id") && feature["id"].IsString() && feature["id"].GetString()[0] != '\0')
		return feature["id"].GetString();
	return "__editor_feature_" + std::to_string(featureIndex + 1);
}

void AvisoDocumentModel::CaptureOriginalCoordinatesJson(const std::string& sourceJson)
{
	OriginalCoordinatesJsonByFeatureId.clear();
	GeometryDirtyFeatureIds.clear();
	if (sourceJson.empty())
		return;

	size_t featuresKey = std::string::npos;
	if (!FindJsonStringKey(sourceJson, 0, sourceJson.size(), "features", featuresKey))
		return;
	size_t arrayStart = sourceJson.find(':', featuresKey);
	if (arrayStart == std::string::npos)
		return;
	++arrayStart;
	while (arrayStart < sourceJson.size() && std::isspace(static_cast<unsigned char>(sourceJson[arrayStart])) != 0)
		++arrayStart;
	if (arrayStart >= sourceJson.size() || sourceJson[arrayStart] != '[')
		return;

	size_t arrayEnd = 0;
	if (!FindMatchingJsonBracket(sourceJson, arrayStart, '[', ']', arrayEnd))
		return;

	const rapidjson::Value* features = GetFeatureArray();
	size_t featureIndex = 0;
	for (size_t offset = arrayStart + 1; offset < arrayEnd && features != nullptr && featureIndex < features->Size();)
	{
		while (offset < arrayEnd &&
			(std::isspace(static_cast<unsigned char>(sourceJson[offset])) != 0 || sourceJson[offset] == ','))
		{
			++offset;
		}
		if (offset >= arrayEnd)
			break;
		if (sourceJson[offset] != '{')
		{
			++offset;
			continue;
		}

		size_t featureEnd = 0;
		if (!FindMatchingJsonBracket(sourceJson, offset, '{', '}', featureEnd))
			break;

		std::string coordinatesJson;
		if (ExtractCoordinatesJsonFromFeatureText(sourceJson, offset, featureEnd, coordinatesJson))
		{
			const std::string identity = FeatureIdentityForPreservation((*features)[static_cast<rapidjson::SizeType>(featureIndex)], static_cast<int>(featureIndex));
			OriginalCoordinatesJsonByFeatureId[identity] = coordinatesJson;
		}
		++featureIndex;
		offset = featureEnd + 1;
	}
}

void AvisoDocumentModel::PatchSerializedCoordinates(std::string& serializedJson) const
{
	if (OriginalCoordinatesJsonByFeatureId.empty() || !Document.IsObject())
		return;

	size_t featuresKey = std::string::npos;
	if (!FindJsonStringKey(serializedJson, 0, serializedJson.size(), "features", featuresKey))
		return;
	size_t arrayStart = serializedJson.find(':', featuresKey);
	if (arrayStart == std::string::npos)
		return;
	++arrayStart;
	while (arrayStart < serializedJson.size() && std::isspace(static_cast<unsigned char>(serializedJson[arrayStart])) != 0)
		++arrayStart;
	if (arrayStart >= serializedJson.size() || serializedJson[arrayStart] != '[')
		return;

	size_t arrayEnd = 0;
	if (!FindMatchingJsonBracket(serializedJson, arrayStart, '[', ']', arrayEnd))
		return;

	const rapidjson::Value* features = GetFeatureArray();
	size_t featureIndex = 0;
	for (size_t offset = arrayStart + 1; offset < arrayEnd && features != nullptr && featureIndex < features->Size();)
	{
		while (offset < arrayEnd &&
			(std::isspace(static_cast<unsigned char>(serializedJson[offset])) != 0 || serializedJson[offset] == ','))
		{
			++offset;
		}
		if (offset >= arrayEnd)
			break;
		if (serializedJson[offset] != '{')
		{
			++offset;
			continue;
		}

		size_t featureEnd = 0;
		if (!FindMatchingJsonBracket(serializedJson, offset, '{', '}', featureEnd))
			break;

		const std::string identity = FeatureIdentityForPreservation((*features)[static_cast<rapidjson::SizeType>(featureIndex)], static_cast<int>(featureIndex));
		const auto original = OriginalCoordinatesJsonByFeatureId.find(identity);
		if (original != OriginalCoordinatesJsonByFeatureId.end() && GeometryDirtyFeatureIds.find(identity) == GeometryDirtyFeatureIds.end())
		{
			size_t coordinatesStart = 0;
			size_t coordinatesEnd = 0;
			if (FindCoordinatesJsonRangeFromFeatureText(serializedJson, offset, featureEnd, coordinatesStart, coordinatesEnd))
			{
				serializedJson.replace(coordinatesStart, coordinatesEnd - coordinatesStart + 1, original->second);
				const ptrdiff_t delta = static_cast<ptrdiff_t>(original->second.size()) - static_cast<ptrdiff_t>(coordinatesEnd - coordinatesStart + 1);
				featureEnd = static_cast<size_t>(static_cast<ptrdiff_t>(featureEnd) + delta);
				arrayEnd = static_cast<size_t>(static_cast<ptrdiff_t>(arrayEnd) + delta);
			}
		}
		++featureIndex;
		offset = featureEnd + 1;
	}
}

void AvisoDocumentModel::SetNumberMember(rapidjson::Value& object, const char* key, int value)
{
	if (!object.IsObject() || key == nullptr)
		return;
	if (object.HasMember(key))
		object.RemoveMember(key);
	rapidjson::Value keyValue;
	keyValue.SetString(key, Document.GetAllocator());
	rapidjson::Value numberValue;
	numberValue.SetInt(value);
	object.AddMember(keyValue, numberValue, Document.GetAllocator());
}

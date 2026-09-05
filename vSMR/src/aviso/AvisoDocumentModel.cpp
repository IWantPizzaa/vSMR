#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoDocumentModel.hpp"
#include "shared/JsonInputLimits.hpp"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>

namespace
{
	constexpr size_t kMaximumAvisoFileBytes =
		AvisoDocumentModel::MaximumSerializedInputBytes;
	constexpr size_t kMaximumAvisoFeatures = 50000U;
	constexpr size_t kMaximumAvisoCoordinates = 500000U;
	constexpr size_t kMaximumAvisoStringBytes = 64U * 1024U;
	constexpr size_t kMaximumAvisoJsonDepth = 64U;
	constexpr size_t kMaximumAvisoJsonValues = 2000000U;
	constexpr size_t kMaximumAvisoContainerEntries = 1000000U;

	bool ValidateSourceStructureAndCounts(
		const std::string& sourceJson,
		std::string& errorText)
	{
		VsmrJsonInputLimits::Limits limits;
		limits.maximumDepth = kMaximumAvisoJsonDepth;
		limits.maximumValues = kMaximumAvisoJsonValues;
		limits.maximumContainerEntries = kMaximumAvisoContainerEntries;
		limits.maximumStringBytes = kMaximumAvisoStringBytes;
		limits.maximumFeatures = kMaximumAvisoFeatures;
		limits.maximumCoordinatePairs = kMaximumAvisoCoordinates;
		return VsmrJsonInputLimits::Validate(sourceJson, limits, errorText);
	}

	bool ValidateJsonValueLimits(
		const rapidjson::Value& value,
		size_t depth,
		size_t& valueCount,
		std::string& errorText)
	{
		if (depth > kMaximumAvisoJsonDepth)
		{
			errorText = "AVISO GeoJSON exceeds the maximum nesting depth of 64.";
			return false;
		}
		if (++valueCount > kMaximumAvisoJsonValues)
		{
			errorText = "AVISO GeoJSON contains too many JSON values.";
			return false;
		}
		if (value.IsString() && value.GetStringLength() > kMaximumAvisoStringBytes)
		{
			errorText = "AVISO GeoJSON contains a string longer than 64 KB.";
			return false;
		}
		if (value.IsArray())
		{
			if (value.Size() > kMaximumAvisoContainerEntries)
			{
				errorText = "AVISO GeoJSON contains an oversized array.";
				return false;
			}
			for (rapidjson::SizeType index = 0; index < value.Size(); ++index)
			{
				if (!ValidateJsonValueLimits(value[index], depth + 1, valueCount, errorText))
					return false;
			}
		}
		else if (value.IsObject())
		{
			size_t memberCount = 0;
			for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
			{
				if (++memberCount > kMaximumAvisoContainerEntries)
				{
					errorText = "AVISO GeoJSON contains an oversized object.";
					return false;
				}
				if (member->name.GetStringLength() > kMaximumAvisoStringBytes)
				{
					errorText = "AVISO GeoJSON contains a property name longer than 64 KB.";
					return false;
				}
				if (!ValidateJsonValueLimits(member->value, depth + 1, valueCount, errorText))
					return false;
			}
		}
		return true;
	}

	bool CountCoordinatePoints(
		const rapidjson::Value& value,
		size_t depth,
		size_t& coordinateCount,
		std::string& errorText)
	{
		if (!value.IsArray())
			return true;
		if (depth > 8U)
		{
			errorText = "AVISO geometry exceeds the supported coordinate nesting depth.";
			return false;
		}
		if (value.Size() >= 2U &&
			value[static_cast<rapidjson::SizeType>(0)].IsNumber() &&
			value[static_cast<rapidjson::SizeType>(1)].IsNumber())
		{
			if (++coordinateCount > kMaximumAvisoCoordinates)
			{
				errorText = "AVISO GeoJSON exceeds the 500,000-coordinate limit.";
				return false;
			}
			return true;
		}
		for (rapidjson::SizeType index = 0; index < value.Size(); ++index)
		{
			if (!CountCoordinatePoints(value[index], depth + 1, coordinateCount, errorText))
				return false;
		}
		return true;
	}

	bool ValidateDocumentResourceLimits(
		const rapidjson::Document& document,
		std::string& errorText)
	{
		size_t valueCount = 0;
		if (!ValidateJsonValueLimits(document, 0, valueCount, errorText))
			return false;
		if (!document.IsObject() || !document.HasMember("features") ||
			!document["features"].IsArray())
		{
			return true;
		}

		const rapidjson::Value& features = document["features"];
		if (features.Size() > kMaximumAvisoFeatures)
		{
			errorText = "AVISO GeoJSON exceeds the 50,000-feature limit.";
			return false;
		}
		size_t coordinateCount = 0;
		for (rapidjson::SizeType index = 0; index < features.Size(); ++index)
		{
			const rapidjson::Value& feature = features[index];
			if (!feature.IsObject() || !feature.HasMember("geometry") ||
				!feature["geometry"].IsObject() ||
				!feature["geometry"].HasMember("coordinates"))
			{
				continue;
			}
			if (!CountCoordinatePoints(
				feature["geometry"]["coordinates"],
				0,
				coordinateCount,
				errorText))
			{
				return false;
			}
		}
		return true;
	}
}

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
	RuntimeFeatureIds.clear();
	RuntimeIdCounter = 0;
	OriginalCoordinatesJsonByFeatureId.clear();
	GeometryDirtyFeatureIds.clear();
	CreateEmptyFeatureCollection();
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
		const std::filesystem::path sourcePath = std::filesystem::u8path(path);
		if (!std::filesystem::exists(sourcePath))
		{
			ResetToEmpty();
			return true;
		}

		if (!ReadBoundedSourceFile(sourcePath, sourceJson, errorText))
			return false;
		Document.Parse<0>(sourceJson.c_str());
		if (Document.HasParseError())
		{
			errorText = "AVISO GeoJSON parse failed at offset " + std::to_string(Document.GetErrorOffset()) + ".";
			Document.SetObject();
			MarkIndexesDirty();
			return false;
		}
		if (!ValidateFeatureCollectionSchema(Document, errorText))
		{
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

	AssignRuntimeFeatureIdsForCurrentDocument();
	BuildIndexes();
	CaptureOriginalCoordinatesJson(sourceJson);
	return true;
}

bool AvisoDocumentModel::ValidateSerializedInputLimits(
	const std::string& sourceJson,
	std::string& errorText)
{
	errorText.clear();
	if (sourceJson.size() > kMaximumAvisoFileBytes)
	{
		errorText = "AVISO GeoJSON exceeds the 32 MB file limit.";
		return false;
	}
	return ValidateSourceStructureAndCounts(sourceJson, errorText);
}

bool AvisoDocumentModel::ReadBoundedSourceFile(
	const std::filesystem::path& path,
	std::string& sourceJson,
	std::string& errorText) noexcept
{
	sourceJson.clear();
	errorText.clear();
	try
	{
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			errorText = "Unable to open AVISO file.";
			return false;
		}

		input.seekg(0, std::ios::end);
		const std::streamoff length = input.tellg();
		if (length < 0)
		{
			errorText = "Unable to determine the AVISO file size.";
			return false;
		}
		if (static_cast<unsigned long long>(length) > kMaximumAvisoFileBytes)
		{
			errorText = "AVISO GeoJSON exceeds the 32 MB file limit.";
			return false;
		}
		input.seekg(0, std::ios::beg);
		if (!input.good())
		{
			errorText = "Unable to read AVISO file.";
			return false;
		}

		sourceJson.resize(static_cast<size_t>(length));
		if (length > 0)
		{
			input.read(sourceJson.data(), static_cast<std::streamsize>(length));
			if (input.gcount() != length)
			{
				sourceJson.clear();
				errorText = "AVISO file changed or became unreadable while loading.";
				return false;
			}
		}
		if (input.peek() != std::char_traits<char>::eof())
		{
			sourceJson.clear();
			errorText = "AVISO file changed while loading.";
			return false;
		}
		// The stream length was already bounded before allocation. Run the shared
		// structural and resource-count validation once on the loaded bytes.
		if (!ValidateSourceStructureAndCounts(sourceJson, errorText))
		{
			sourceJson.clear();
			return false;
		}
		return true;
	}
	catch (const std::exception& exception)
	{
		sourceJson.clear();
		try
		{
			errorText = "AVISO input validation failed: " +
				std::string(exception.what());
		}
		catch (...)
		{
			errorText.clear();
		}
		return false;
	}
	catch (...)
	{
		sourceJson.clear();
		try
		{
			errorText = "AVISO input validation failed.";
		}
		catch (...)
		{
			errorText.clear();
		}
		return false;
	}
}

void AvisoDocumentModel::CreateEmptyFeatureCollection()
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
	EnsureRuntimeFeatureIds();
}

bool AvisoDocumentModel::ValidateFeatureCollectionSchema(
	const rapidjson::Value& document,
	std::string& errorText)
{
	if (!document.IsObject())
	{
		errorText = "AVISO GeoJSON root must be an object.";
		return false;
	}
	if (!document.HasMember("type") ||
		!document["type"].IsString() ||
		std::strcmp(document["type"].GetString(), "FeatureCollection") != 0)
	{
		errorText = "AVISO GeoJSON type must be FeatureCollection.";
		return false;
	}
	if (document.HasMember("metadata"))
	{
		const rapidjson::Value& metadata = document["metadata"];
		if (!metadata.IsObject())
		{
			errorText = "AVISO metadata must be an object.";
			return false;
		}
		if (metadata.HasMember("schema_version"))
		{
			const rapidjson::Value& schemaVersion = metadata["schema_version"];
			if (!schemaVersion.IsInt() ||
				schemaVersion.GetInt() < 1 ||
				schemaVersion.GetInt() > 2)
			{
				errorText = "AVISO metadata.schema_version must be an integer from 1 to 2.";
				return false;
			}
		}
		if (metadata.HasMember("background_colors"))
		{
			const rapidjson::Value& colors = metadata["background_colors"];
			if (!colors.IsObject())
			{
				errorText = "AVISO metadata.background_colors must be an object.";
				return false;
			}
			auto isHexColor = [](const rapidjson::Value& value)
			{
				if (!value.IsString() || value.GetStringLength() != 7 || value.GetString()[0] != '#')
					return false;
				for (rapidjson::SizeType i = 1; i < value.GetStringLength(); ++i)
				{
					const unsigned char character = static_cast<unsigned char>(value.GetString()[i]);
					if (!std::isxdigit(character))
						return false;
				}
				return true;
			};
			for (const char* palette : { "dark", "light", "real", "night", "day" })
			{
				if (colors.HasMember(palette) && !isHexColor(colors[palette]))
				{
					errorText = std::string("AVISO metadata.background_colors.") + palette +
						" must be a #RRGGBB color.";
					return false;
				}
			}
		}
	}
	if (!document.HasMember("features") || !document["features"].IsArray())
	{
		errorText = "AVISO GeoJSON must contain a features array.";
		return false;
	}
	const rapidjson::Value& features = document["features"];
	std::string duplicateId;
	if (HasDuplicatePersistedFeatureIds(features, duplicateId))
	{
		errorText = "AVISO GeoJSON contains duplicate feature id '" + duplicateId + "'.";
		return false;
	}
	for (rapidjson::SizeType i = 0; i < features.Size(); ++i)
	{
		const rapidjson::Value& feature = features[i];
		if (!feature.IsObject())
		{
			errorText = "AVISO feature " + std::to_string(i + 1) + " must be an object.";
			return false;
		}
		if (feature.HasMember("type") && feature["type"].IsString() && std::string(feature["type"].GetString()) != "Feature")
		{
			errorText = "AVISO feature " + std::to_string(i + 1) + " has unsupported type.";
			return false;
		}
		if (!feature.HasMember("properties") || !feature["properties"].IsObject())
		{
			errorText = "AVISO feature " + std::to_string(i + 1) + " must contain a properties object.";
			return false;
		}
		if (!feature.HasMember("geometry") || !feature["geometry"].IsObject() || !IsGeometryCoordinatesValid(feature["geometry"]))
		{
			errorText = "AVISO feature " + std::to_string(i + 1) + " has invalid geometry.";
			return false;
		}
	}
	return true;
}

bool AvisoDocumentModel::ValidateLoadedFeatureCollection(std::string& errorText) const
{
	// Editor mutations do not pass through the serialized streaming validator.
	if (!ValidateDocumentResourceLimits(Document, errorText))
		return false;
	return ValidateFeatureCollectionSchema(Document, errorText);
}

bool AvisoDocumentModel::SaveAtomically(
	const std::string& path,
	std::string& errorText)
{
	errorText.clear();
	if (path.empty())
	{
		errorText = "No AVISO path is available.";
		return false;
	}

	AvisoValidationResult validationResult = ValidateAndRecalculate();
	if (!validationResult.ok)
	{
		errorText = validationResult.errorText;
		return false;
	}

	// Keeping untouched coordinate arrays byte-for-byte stable avoids rewriting
	// large airport datasets when only styles or metadata changed
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	Document.Accept(writer);
	std::string serializedJson(buffer.GetString(), buffer.Size());
	PatchSerializedCoordinates(serializedJson);
	if (serializedJson.size() > kMaximumAvisoFileBytes)
	{
		errorText = "AVISO GeoJSON exceeds the 32 MB file limit.";
		return false;
	}
	if (!ValidateSourceStructureAndCounts(serializedJson, errorText))
		return false;

	rapidjson::Document validation;
	if (validation.Parse<0>(serializedJson.c_str()).HasParseError() ||
		!ValidateFeatureCollectionSchema(validation, errorText))
	{
		if (errorText.empty())
			errorText = "AVISO save validation failed before writing.";
		return false;
	}

	try
	{
		const std::filesystem::path outputPath = std::filesystem::u8path(path);
		if (outputPath.has_parent_path())
			std::filesystem::create_directories(outputPath.parent_path());

		// Writing and verifying a temporary copy before replacing the active dataset
		std::filesystem::path tempPath;
		HANDLE output = INVALID_HANDLE_VALUE;
		for (int attempt = 0; attempt < 128; ++attempt)
		{
			tempPath = outputPath;
			tempPath +=
				L".tmp." +
				std::to_wstring(::GetCurrentProcessId()) +
				L"." +
				std::to_wstring(::GetTickCount()) +
				L"." +
				std::to_wstring(attempt);
			output = ::CreateFileW(
				tempPath.c_str(),
				GENERIC_WRITE,
				0,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
				nullptr);
			if (output != INVALID_HANDLE_VALUE)
				break;
			const DWORD createError = ::GetLastError();
			if (createError != ERROR_FILE_EXISTS &&
				createError != ERROR_ALREADY_EXISTS)
			{
				errorText = "Unable to write AVISO temp file.";
				return false;
			}
		}
		if (output == INVALID_HANDLE_VALUE)
		{
			errorText = "Unable to allocate a unique AVISO temp file.";
			return false;
		}

		bool writeOk = true;
		size_t offset = 0;
		while (offset < serializedJson.size())
		{
			const size_t remaining = serializedJson.size() - offset;
			const DWORD chunk = static_cast<DWORD>(
				(std::min)(remaining, static_cast<size_t>(0x7fffffff)));
			DWORD written = 0;
			if (!::WriteFile(
				output,
				serializedJson.data() + offset,
				chunk,
				&written,
				nullptr) ||
				written == 0)
			{
				writeOk = false;
				break;
			}
			offset += written;
		}
		if (writeOk && !::FlushFileBuffers(output))
			writeOk = false;
		if (!::CloseHandle(output))
			writeOk = false;
		if (!writeOk)
		{
			errorText = "Unable to flush AVISO temp file.";
			std::error_code ignored;
			std::filesystem::remove(tempPath, ignored);
			return false;
		}

		std::ifstream persistedInput(tempPath, std::ios::binary);
		std::ostringstream persistedBuffer;
		persistedBuffer << persistedInput.rdbuf();
		const std::string persistedJson = persistedBuffer.str();
		if (persistedInput.bad() ||
			persistedJson != serializedJson)
		{
			errorText = "AVISO temp-file validation failed.";
			persistedInput.close();
			std::error_code ignored;
			std::filesystem::remove(tempPath, ignored);
			return false;
		}
		persistedInput.close();

		if (!::MoveFileExW(tempPath.c_str(), outputPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
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
	EnsureRuntimeFeatureIds();
	Summaries.clear();
	SummaryPositionByFeatureIndex.clear();
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
	SummaryPositionByFeatureIndex.assign(features->Size(), -1);
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
			summary.featureId = FeatureIdentityForPreservation(feature, summary.featureIndex);
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
		SummaryPositionByFeatureIndex[static_cast<size_t>(summary.featureIndex)] = static_cast<int>(Summaries.size());
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
	if (featureIndex < 0 || static_cast<size_t>(featureIndex) >= SummaryPositionByFeatureIndex.size())
		return nullptr;
	const int summaryPosition = SummaryPositionByFeatureIndex[static_cast<size_t>(featureIndex)];
	if (summaryPosition < 0 || static_cast<size_t>(summaryPosition) >= Summaries.size())
		return nullptr;
	return &Summaries[static_cast<size_t>(summaryPosition)];
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

void AvisoDocumentModel::NoteFeatureInserted(int featureIndex)
{
	EnsureRuntimeFeatureIds();
	const int clampedIndex = (std::max)(0, (std::min)(featureIndex, static_cast<int>(RuntimeFeatureIds.size())));
	RuntimeFeatureIds.insert(RuntimeFeatureIds.begin() + clampedIndex, GenerateRuntimeFeatureId());
	MarkIndexesDirty();
}

void AvisoDocumentModel::NoteFeatureDeleted(int featureIndex)
{
	EnsureRuntimeFeatureIds();
	if (featureIndex >= 0 && static_cast<size_t>(featureIndex) < RuntimeFeatureIds.size())
		RuntimeFeatureIds.erase(RuntimeFeatureIds.begin() + featureIndex);
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

AvisoValidationResult AvisoDocumentModel::ValidateAndRecalculate()
{
	AvisoValidationResult result;
	if (!ValidateLoadedFeatureCollection(result.errorText))
	{
		result.ok = false;
		return result;
	}
	const int featureCount = FeatureCount();
	const rapidjson::Value* features = GetFeatureArray();

	std::map<std::string, int> styleFeatureCounts;
	std::map<std::string, int> layerCounts;
	std::map<std::string, int> categoryCounts;
	bool hasBounds = false;
	double minLongitude = 0.0;
	double minLatitude = 0.0;
	double maxLongitude = 0.0;
	double maxLatitude = 0.0;

	// Collecting nested GeoJSON bounds while validating style references
	std::function<void(const rapidjson::Value&)> collectBounds = [&](const rapidjson::Value& value)
	{
		if (!value.IsArray())
			return;
		if (value.Size() >= 2 &&
			value[static_cast<rapidjson::SizeType>(0)].IsNumber() &&
			value[static_cast<rapidjson::SizeType>(1)].IsNumber())
		{
			const double longitude = value[static_cast<rapidjson::SizeType>(0)].GetDouble();
			const double latitude = value[static_cast<rapidjson::SizeType>(1)].GetDouble();
			if (!std::isfinite(longitude) || !std::isfinite(latitude))
				return;
			if (!hasBounds)
			{
				minLongitude = maxLongitude = longitude;
				minLatitude = maxLatitude = latitude;
				hasBounds = true;
			}
			else
			{
				minLongitude = (std::min)(minLongitude, longitude);
				maxLongitude = (std::max)(maxLongitude, longitude);
				minLatitude = (std::min)(minLatitude, latitude);
				maxLatitude = (std::max)(maxLatitude, latitude);
			}
			return;
		}
		for (rapidjson::SizeType i = 0; i < value.Size(); ++i)
			collectBounds(value[i]);
	};

	for (rapidjson::SizeType i = 0; i < features->Size(); ++i)
	{
		const rapidjson::Value& feature = (*features)[i];
		if (!feature.IsObject())
		{
			result.ok = false;
			result.errorText = "AVISO feature " + std::to_string(i + 1) + " must be an object.";
			return result;
		}
		if (!feature.HasMember("properties") || !feature["properties"].IsObject())
		{
			result.ok = false;
			result.errorText = "AVISO feature " + std::to_string(i + 1) + " must contain a properties object.";
			return result;
		}
		if (!feature.HasMember("geometry") || !feature["geometry"].IsObject() || !IsGeometryCoordinatesValid(feature["geometry"]))
		{
			result.ok = false;
			result.errorText = "AVISO feature " + std::to_string(i + 1) + " has invalid geometry.";
			return result;
		}

		const rapidjson::Value& properties = feature["properties"];
		const std::string layer = ReadStringProperty(&properties, "layer");
		const std::string category = ReadStringProperty(&properties, "category");
		const std::string styleId = ReadStringProperty(&properties, "style_id");
		if (!layer.empty())
			++layerCounts[layer];
		if (!category.empty())
			++categoryCounts[category];
		if (!styleId.empty())
		{
			if (Document.HasMember("styles") && Document["styles"].IsObject() && !Document["styles"].HasMember(styleId.c_str()))
			{
				result.ok = false;
				result.errorText = "AVISO feature references missing style_id '" + styleId + "'.";
				return result;
			}
			++styleFeatureCounts[styleId];
		}
		collectBounds(feature["geometry"]["coordinates"]);
	}

	// Updating derived metadata only after every feature has passed validation
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
		auto writeCountsObject = [&](const char* key, const std::map<std::string, int>& counts)
		{
			rapidjson::Value countObject(rapidjson::kObjectType);
			for (const auto& entry : counts)
			{
				rapidjson::Value name;
				name.SetString(entry.first.c_str(), static_cast<rapidjson::SizeType>(entry.first.size()), Document.GetAllocator());
				rapidjson::Value countValue;
				countValue.SetInt(entry.second);
				countObject.AddMember(name, countValue, Document.GetAllocator());
			}
			SetObjectMember(Document["metadata"], key, countObject);
		};
		writeCountsObject("layer_counts", layerCounts);
		writeCountsObject("category_counts", categoryCounts);
	}

	if (hasBounds)
	{
		rapidjson::Value bbox(rapidjson::kArrayType);
		bbox.PushBack(minLongitude, Document.GetAllocator());
		bbox.PushBack(minLatitude, Document.GetAllocator());
		bbox.PushBack(maxLongitude, Document.GetAllocator());
		bbox.PushBack(maxLatitude, Document.GetAllocator());
		SetObjectMember(Document, "bbox", bbox);
	}

	MarkIndexesDirty();
	result.ok = true;
	return result;
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
	if (GeometryTypeFromFeature(feature) != "Point")
		return false;

	const rapidjson::Value* properties = nullptr;
	if (feature.IsObject() && feature.HasMember("properties") && feature["properties"].IsObject())
		properties = &feature["properties"];
	if (properties == nullptr)
		return false;

	if (EqualsNoCase(ReadStringProperty(properties, "object_type"), "Label"))
		return true;
	const std::string role = ToUpperAscii(ReadStringProperty(properties, "geometry_role"));
	return role == "TEXT_LABEL";
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

bool AvisoDocumentModel::IsGeometryCoordinatesValid(const rapidjson::Value& geometry)
{
	if (!geometry.IsObject() ||
		!geometry.HasMember("type") ||
		!geometry["type"].IsString() ||
		!geometry.HasMember("coordinates") ||
		!geometry["coordinates"].IsArray())
	{
		return false;
	}

	const std::string geometryType = geometry["type"].GetString();
	const rapidjson::Value& coordinates = geometry["coordinates"];
	auto isPoint = [](const rapidjson::Value& point) {
		return point.IsArray() &&
			point.Size() >= 2 &&
			point[static_cast<rapidjson::SizeType>(0)].IsNumber() &&
			point[static_cast<rapidjson::SizeType>(1)].IsNumber() &&
			std::isfinite(point[static_cast<rapidjson::SizeType>(0)].GetDouble()) &&
			std::isfinite(point[static_cast<rapidjson::SizeType>(1)].GetDouble());
	};
	auto isLine = [&](const rapidjson::Value& line, size_t minPoints) {
		if (!line.IsArray() || line.Size() < minPoints)
			return false;
		for (rapidjson::SizeType i = 0; i < line.Size(); ++i)
		{
			if (!isPoint(line[i]))
				return false;
		}
		return true;
	};

	if (geometryType == "Point")
		return isPoint(coordinates);
	if (geometryType == "LineString")
		return isLine(coordinates, 2);
	if (geometryType == "Polygon")
	{
		if (coordinates.Size() == 0)
			return false;
		for (rapidjson::SizeType i = 0; i < coordinates.Size(); ++i)
		{
			if (!isLine(coordinates[i], 4))
				return false;
		}
		return true;
	}
	if (geometryType == "MultiLineString")
	{
		if (coordinates.Size() == 0)
			return false;
		for (rapidjson::SizeType i = 0; i < coordinates.Size(); ++i)
		{
			if (!isLine(coordinates[i], 2))
				return false;
		}
		return true;
	}
	if (geometryType == "MultiPolygon")
	{
		if (coordinates.Size() == 0)
			return false;
		for (rapidjson::SizeType polygonIndex = 0; polygonIndex < coordinates.Size(); ++polygonIndex)
		{
			const rapidjson::Value& polygon = coordinates[polygonIndex];
			if (!polygon.IsArray() || polygon.Size() == 0)
				return false;
			for (rapidjson::SizeType ringIndex = 0; ringIndex < polygon.Size(); ++ringIndex)
			{
				if (!isLine(polygon[ringIndex], 4))
					return false;
			}
		}
		return true;
	}
	return false;
}

bool AvisoDocumentModel::HasDuplicatePersistedFeatureIds(const rapidjson::Value& features, std::string& duplicateId)
{
	if (!features.IsArray())
		return false;

	std::set<std::string> seenIds;
	for (rapidjson::SizeType i = 0; i < features.Size(); ++i)
	{
		const rapidjson::Value& feature = features[i];
		if (!feature.IsObject() || !feature.HasMember("id") || !feature["id"].IsString())
			continue;
		const std::string id = feature["id"].GetString();
		if (id.empty())
			continue;
		if (!seenIds.insert(id).second)
		{
			duplicateId = id;
			return true;
		}
	}
	return false;
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

std::string AvisoDocumentModel::FeatureIdentityForPreservation(const rapidjson::Value& feature, int featureIndex) const
{
	if (feature.IsObject() && feature.HasMember("id") && feature["id"].IsString() && feature["id"].GetString()[0] != '\0')
		return feature["id"].GetString();
	if (featureIndex >= 0 && static_cast<size_t>(featureIndex) < RuntimeFeatureIds.size())
		return RuntimeFeatureIds[static_cast<size_t>(featureIndex)];
	return "legacy.runtime.missing";
}

std::string AvisoDocumentModel::GenerateRuntimeFeatureId()
{
	++RuntimeIdCounter;
	std::ostringstream stream;
	stream << "legacy.runtime." << ::GetTickCount() << "." << RuntimeIdCounter;
	return stream.str();
}

void AvisoDocumentModel::EnsureRuntimeFeatureIds()
{
	const rapidjson::Value* features = GetFeatureArray();
	const size_t featureCount = features != nullptr ? static_cast<size_t>(features->Size()) : 0;
	while (RuntimeFeatureIds.size() < featureCount)
		RuntimeFeatureIds.push_back(GenerateRuntimeFeatureId());
	if (RuntimeFeatureIds.size() > featureCount)
		RuntimeFeatureIds.resize(featureCount);
}

void AvisoDocumentModel::AssignRuntimeFeatureIdsForCurrentDocument()
{
	RuntimeFeatureIds.clear();
	const rapidjson::Value* features = GetFeatureArray();
	const size_t featureCount = features != nullptr ? static_cast<size_t>(features->Size()) : 0;
	RuntimeFeatureIds.reserve(featureCount);
	for (size_t i = 0; i < featureCount; ++i)
		RuntimeFeatureIds.push_back(GenerateRuntimeFeatureId());
}

void AvisoDocumentModel::CaptureOriginalCoordinatesJson(const std::string& sourceJson)
{
	EnsureRuntimeFeatureIds();
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

	// Runtime identities let legacy features preserve coordinates without adding IDs
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

	// Restoring only coordinates whose geometry was not edited
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

void AvisoDocumentModel::SetObjectMember(rapidjson::Value& object, const char* key, rapidjson::Value& value)
{
	if (!object.IsObject() || key == nullptr)
		return;
	if (object.HasMember(key))
		object.RemoveMember(key);
	rapidjson::Value keyValue;
	keyValue.SetString(key, Document.GetAllocator());
	object.AddMember(keyValue, value, Document.GetAllocator());
}

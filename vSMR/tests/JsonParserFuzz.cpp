#include "shared/JsonDocument.hpp"
#include "control_center/WebMessageValidation.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <vector>

// Also usable with libFuzzer when compiled without VSMR_FUZZ_STANDALONE.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	const std::string_view json(size == 0 ? "" : reinterpret_cast<const char*>(data), size);
	rapidjson::Document document;
	VsmrJson::ParseDocument(document, json);
	std::string selector;
	(void)VsmrWebMessageValidation::TryGetInboundWebMessageSelector(json, selector);
	return 0;
}

#if defined(VSMR_FUZZ_STANDALONE)
int main(int argc, char** argv)
{
	std::vector<std::string> corpus = {
		"{}", "[]", "null", "[1e309,-0,1.7976931348623157e308]",
		R"({"version":1,"type":"settings.update","payload":{"enabled":true}})",
		R"({"schema_version":1,"product":"vSMR","version":"2.0.0","files":[]})",
		std::string(4096, '[') + "0" + std::string(4096, ']'),
		std::string("{}\0{}", 5) };
	if (argc > 1)
	{
		for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1]))
		{
			if (entry.is_regular_file() && entry.path().extension() == ".json" && entry.file_size() < 1024 * 1024)
			{
				std::ifstream input(entry.path(), std::ios::binary);
				corpus.emplace_back(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
			}
		}
	}
	std::mt19937 random(0x56534d52);
	std::size_t executed = 0;
	for (const auto& seed : corpus)
	{
		LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size());
		++executed;
	}
	for (unsigned i = 0; i < 10000; ++i)
	{
		std::string input = corpus[random() % corpus.size()];
		for (unsigned mutation = 0; mutation < 1 + random() % 8; ++mutation)
		{
			const std::size_t offset = random() % (input.size() + 1);
			switch (random() % 4)
			{
			case 0: input.insert(offset, 1, static_cast<char>(random() & 0xff)); break;
			case 1: if (offset < input.size()) input.erase(offset, 1); break;
			case 2: if (offset < input.size()) input[offset] = static_cast<char>(random() & 0xff); break;
			default: input.resize(offset); break;
			}
		}
		LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
		++executed;
	}
	std::cout << "JSON parser sanitizer smoke test passed: " << executed << " inputs\n";
}
#endif

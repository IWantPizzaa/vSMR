#pragma once

#include <cstddef>
#include <string_view>

namespace VsmrJson
{
	class StringViewStream
	{
	public:
		using Ch = char;

		explicit StringViewStream(std::string_view input) noexcept
			: Begin(input.empty() ? &Empty : input.data()),
			Current(Begin),
			End(Begin + input.size())
		{
		}

		Ch Peek() const noexcept { return Current != End ? *Current : '\0'; }
		Ch Take() noexcept { return Current != End ? *Current++ : '\0'; }
		std::size_t Tell() const noexcept
		{
			return static_cast<std::size_t>(Current - Begin);
		}
		Ch* PutBegin() noexcept { return nullptr; }
		void Put(Ch) noexcept {}
		std::size_t PutEnd(Ch*) noexcept { return 0U; }

	private:
		inline static constexpr Ch Empty = '\0';
		const Ch* Begin = nullptr;
		const Ch* Current = nullptr;
		const Ch* End = nullptr;
	};
}

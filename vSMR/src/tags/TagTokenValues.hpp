#pragma once

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VsmrTags
{
	// A tag has a small, stable set of keys. Keep values contiguous and retain
	// their string capacity when a scene buffer is reused.
	class TokenValues
	{
	public:
		using value_type = std::pair<std::string, std::string>;
		using Storage = std::vector<value_type>;
		TokenValues() = default;
		TokenValues(std::initializer_list<value_type> values)
		{
			for (const auto& value : values) (*this)[value.first] = value.second;
		}
		auto begin() noexcept { return values_.begin(); }
		auto end() noexcept { return values_.end(); }
		auto begin() const noexcept { return values_.begin(); }
		auto end() const noexcept { return values_.end(); }
		std::size_t size() const noexcept { return values_.size(); }
		std::size_t capacity() const noexcept { return values_.capacity(); }
		bool empty() const noexcept { return values_.empty(); }
		void clear() noexcept { values_.clear(); }
		void ResetValues() noexcept { for (auto& value : values_) value.second.clear(); }
		auto find(std::string_view key) const
		{
			const auto it = LowerBound(values_, key);
			return it != end() && it->first == key ? it : end();
		}
		auto find(std::string_view key)
		{
			const auto it = LowerBound(values_, key);
			return it != end() && it->first == key ? it : end();
		}
		std::string& operator[](std::string_view key)
		{
			if (values_.capacity() == 0) values_.reserve(64);
			auto it = LowerBound(values_, key);
			if (it == end() || it->first != key)
				it = values_.emplace(it, std::string(key), std::string());
			return it->second;
		}
		const std::string& at(std::string_view key) const
		{
			const auto it = find(key);
			if (it == end()) throw std::out_of_range("Unknown tag token");
			return it->second;
		}
	private:
		template<class Values>
		static auto LowerBound(Values& values, std::string_view key)
		{
			return std::lower_bound(values.begin(), values.end(), key,
				[](const value_type& value, std::string_view candidate) { return value.first < candidate; });
		}
		Storage values_;
	};
}

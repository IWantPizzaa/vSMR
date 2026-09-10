#pragma once

#include <GdiPlus.h>
#include <memory>
#include <unordered_map>

namespace VsmrRendering
{
	class BrushCache
	{
	public:
		Gdiplus::SolidBrush& Get(const Gdiplus::Color& color)
		{
			const auto key = color.GetValue();
			const auto found = brushes_.find(key);
			if (found != brushes_.end()) return *found->second;
			// The caller consumes each brush immediately. A bounded overflow brush
			// handles unusual user palettes without retaining unlimited GDI+ objects.
			if (brushes_.size() >= 256)
			{
				overflow_.SetColor(color);
				return overflow_;
			}
			return *brushes_.emplace(key, std::make_unique<Gdiplus::SolidBrush>(color)).first->second;
		}
	private:
		std::unordered_map<Gdiplus::ARGB, std::unique_ptr<Gdiplus::SolidBrush>> brushes_;
		Gdiplus::SolidBrush overflow_{Gdiplus::Color(0, 0, 0, 0)};
	};
}

#pragma once

#include <GdiPlus.h>

#include <cctype>
#include <cmath>
#include <string>
#include <vector>

inline static double ClampDouble(double value, double minValue, double maxValue)
{
	if (value < minValue) return minValue;
	if (value > maxValue) return maxValue;
	return value;
}

inline static int ClampInt(int value, int minValue, int maxValue)
{
	if (value < minValue) return minValue;
	if (value > maxValue) return maxValue;
	return value;
}

inline static void RgbToHsv(int r, int g, int b, double& h, double& s, double& v)
{
	double rf = ClampDouble(r / 255.0, 0.0, 1.0);
	double gf = ClampDouble(g / 255.0, 0.0, 1.0);
	double bf = ClampDouble(b / 255.0, 0.0, 1.0);

	double cmax = (std::max)(rf, (std::max)(gf, bf));
	double cmin = (std::min)(rf, (std::min)(gf, bf));
	double delta = cmax - cmin;

	h = 0.0;
	if (delta > 1e-9)
	{
		if (cmax == rf)
			h = 60.0 * std::fmod(((gf - bf) / delta), 6.0);
		else if (cmax == gf)
			h = 60.0 * (((bf - rf) / delta) + 2.0);
		else
			h = 60.0 * (((rf - gf) / delta) + 4.0);
	}
	if (h < 0.0)
		h += 360.0;

	s = (cmax <= 1e-9) ? 0.0 : (delta / cmax);
	v = cmax;
}

inline static Gdiplus::Color HsvToColor(double h, double s, double v, int a = 255)
{
	double hue = std::fmod(h, 360.0);
	if (hue < 0.0)
		hue += 360.0;
	double sat = ClampDouble(s, 0.0, 1.0);
	double val = ClampDouble(v, 0.0, 1.0);

	double c = val * sat;
	double x = c * (1.0 - std::fabs(std::fmod(hue / 60.0, 2.0) - 1.0));
	double m = val - c;

	double rf = 0.0, gf = 0.0, bf = 0.0;
	if (hue < 60.0) {
		rf = c; gf = x; bf = 0.0;
	}
	else if (hue < 120.0) {
		rf = x; gf = c; bf = 0.0;
	}
	else if (hue < 180.0) {
		rf = 0.0; gf = c; bf = x;
	}
	else if (hue < 240.0) {
		rf = 0.0; gf = x; bf = c;
	}
	else if (hue < 300.0) {
		rf = x; gf = 0.0; bf = c;
	}
	else {
		rf = c; gf = 0.0; bf = x;
	}

	int r = ClampInt(static_cast<int>((rf + m) * 255.0 + 0.5), 0, 255);
	int g = ClampInt(static_cast<int>((gf + m) * 255.0 + 0.5), 0, 255);
	int b = ClampInt(static_cast<int>((bf + m) * 255.0 + 0.5), 0, 255);
	int alpha = ClampInt(a, 0, 255);
	return Gdiplus::Color(
		static_cast<BYTE>(alpha),
		static_cast<BYTE>(r),
		static_cast<BYTE>(g),
		static_cast<BYTE>(b));
}

inline static std::vector<int> ExtractIntegers(const std::string& text)
{
	std::vector<int> values;
	int sign = 1;
	int number = 0;
	bool inNumber = false;

	for (size_t i = 0; i < text.size(); ++i)
	{
		const char c = text[i];
		if (c == '-' && !inNumber)
		{
			sign = -1;
			continue;
		}
		if (std::isdigit(static_cast<unsigned char>(c)))
		{
			inNumber = true;
			number = number * 10 + (c - '0');
			continue;
		}

		if (inNumber)
		{
			values.push_back(sign * number);
			number = 0;
			sign = 1;
			inNumber = false;
		}
		else
		{
			sign = 1;
		}
	}

	if (inNumber)
		values.push_back(sign * number);

	return values;
}

inline static bool TryParseHexByte(const std::string& text, size_t offset, int& outValue)
{
	if (offset + 1 >= text.size())
		return false;

	auto hexValue = [](char ch) -> int
	{
		unsigned char c = static_cast<unsigned char>(ch);
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'A' && c <= 'F')
			return 10 + (c - 'A');
		if (c >= 'a' && c <= 'f')
			return 10 + (c - 'a');
		return -1;
	};

	int hi = hexValue(text[offset]);
	int lo = hexValue(text[offset + 1]);
	if (hi < 0 || lo < 0)
		return false;

	outValue = (hi << 4) | lo;
	return true;
}

inline static bool TryParseHexColor(const std::string& text, int& r, int& g, int& b, int& a, bool& hasAlpha)
{
	std::string normalized;
	normalized.reserve(text.size());
	for (char ch : text)
	{
		if (!std::isspace(static_cast<unsigned char>(ch)))
			normalized.push_back(ch);
	}

	if (normalized.empty())
		return false;

	if (normalized[0] == '#')
		normalized.erase(0, 1);

	if (normalized.size() != 6 && normalized.size() != 8)
		return false;

	int rr = 0, gg = 0, bb = 0, aa = 255;
	if (!TryParseHexByte(normalized, 0, rr) ||
		!TryParseHexByte(normalized, 2, gg) ||
		!TryParseHexByte(normalized, 4, bb))
	{
		return false;
	}

	hasAlpha = false;
	if (normalized.size() == 8)
	{
		if (!TryParseHexByte(normalized, 6, aa))
			return false;
		hasAlpha = true;
	}

	r = rr;
	g = gg;
	b = bb;
	a = aa;
	return true;
}

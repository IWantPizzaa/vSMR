#pragma once

namespace VsmrInsetSnap
{
	struct Frame
	{
		int left = 0;
		int top = 0;
		int right = 0;
		int bottom = 0;
	};

	enum class Edge
	{
		None,
		Left,
		Right,
		Top,
		Bottom,
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight
	};

	inline int NearestSide(int firstDistance, int secondDistance, int threshold) noexcept
	{
		const bool firstNear = firstDistance <= threshold;
		const bool secondNear = secondDistance <= threshold;
		if (firstNear && (!secondNear || firstDistance <= secondDistance)) return -1;
		if (secondNear) return 1;
		return 0;
	}

	inline Edge EdgeForFrame(Frame frame, Frame bounds, int edgeThreshold, int cornerThreshold) noexcept
	{
		if (frame.right <= frame.left || frame.bottom <= frame.top ||
			bounds.right <= bounds.left || bounds.bottom <= bounds.top)
			return Edge::None;
		const int left = frame.left - bounds.left;
		const int right = bounds.right - frame.right;
		const int top = frame.top - bounds.top;
		const int bottom = bounds.bottom - frame.bottom;
		const int cornerX = NearestSide(left, right, cornerThreshold);
		const int cornerY = NearestSide(top, bottom, cornerThreshold);
		if (cornerX != 0 && cornerY != 0)
		{
			if (cornerX < 0) return cornerY < 0 ? Edge::TopLeft : Edge::BottomLeft;
			return cornerY < 0 ? Edge::TopRight : Edge::BottomRight;
		}
		const int edgeX = NearestSide(left, right, edgeThreshold);
		if (edgeX != 0) return edgeX < 0 ? Edge::Left : Edge::Right;
		const int edgeY = NearestSide(top, bottom, edgeThreshold);
		if (edgeY != 0) return edgeY < 0 ? Edge::Top : Edge::Bottom;
		return Edge::None;
	}
}

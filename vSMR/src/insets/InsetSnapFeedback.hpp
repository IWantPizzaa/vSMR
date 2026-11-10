#pragma once

#include <algorithm>
#include <cstdint>

namespace VsmrInsetSnap
{
	// The cue is armed only during the first short visit to an edge. Holding a
	// dragged inset there dismisses the cue and prevents a surprise snap.
	inline constexpr std::uint64_t FadeMilliseconds = 140;
	inline constexpr std::uint64_t HoldMilliseconds = 750;

	class Feedback
	{
	public:
		void Reset() noexcept { *this = Feedback{}; }

		void SetTarget(bool present, int edge, std::uint64_t now) noexcept
		{
			if (present)
			{
				if (!present_ || edge != edge_)
				{
					present_ = true;
					edge_ = edge;
					entered_ = now;
					exiting_ = false;
				}
				return;
			}
			if (present_)
			{
				exitOpacity_ = Opacity(now);
				exited_ = now;
				exiting_ = exitOpacity_ > 0.0f;
				present_ = false;
			}
		}

		bool CanSnap(std::uint64_t now) const noexcept
		{
			return present_ && now - entered_ < HoldMilliseconds;
		}

		float Opacity(std::uint64_t now) const noexcept
		{
			if (present_)
			{
				const auto elapsed = now - entered_;
				if (elapsed < FadeMilliseconds)
					return Ease(static_cast<float>(elapsed) / FadeMilliseconds);
				if (elapsed < HoldMilliseconds)
					return 1.0f;
				if (elapsed < HoldMilliseconds + FadeMilliseconds)
					return 1.0f - Ease(static_cast<float>(elapsed - HoldMilliseconds) / FadeMilliseconds);
				return 0.0f;
			}
			if (exiting_ && now - exited_ < FadeMilliseconds)
				return exitOpacity_ * (1.0f - Ease(static_cast<float>(now - exited_) / FadeMilliseconds));
			return 0.0f;
		}

		bool NeedsTimer(std::uint64_t now) const noexcept
		{
			return (present_ && now - entered_ < HoldMilliseconds + FadeMilliseconds) ||
				(exiting_ && now - exited_ < FadeMilliseconds);
		}

		bool IsAnimating(std::uint64_t now) const noexcept
		{
			if (present_)
			{
				const auto elapsed = now - entered_;
				return elapsed < FadeMilliseconds ||
					(elapsed >= HoldMilliseconds && elapsed < HoldMilliseconds + FadeMilliseconds);
			}
			return exiting_ && now - exited_ < FadeMilliseconds;
		}

	private:
		static float Ease(float progress) noexcept
		{
			progress = std::clamp(progress, 0.0f, 1.0f);
			return progress * progress * (3.0f - 2.0f * progress);
		}

		bool present_ = false;
		bool exiting_ = false;
		int edge_ = 0;
		std::uint64_t entered_ = 0;
		std::uint64_t exited_ = 0;
		float exitOpacity_ = 0.0f;
	};
}

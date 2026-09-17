#pragma once

#include <random>

namespace VsmrRandom
{
	// Private state avoids reseeding the process-wide C generator shared by the host.
	inline int UniformInt(int minimum, int maximum)
	{
		thread_local std::mt19937 engine(std::random_device{}());
		return std::uniform_int_distribution<int>(minimum, maximum)(engine);
	}
}

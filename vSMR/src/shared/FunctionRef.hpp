#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace Vsmr
{
	template<class Signature> class FunctionRef;

	// Non-owning callback: bind a named callable that outlives the render pass.
	// Rvalues cannot bind, which prevents dangling captured viewport state.
	template<class Result, class... Args>
	class FunctionRef<Result(Args...)>
	{
	public:
		FunctionRef() = default;
		template<class Function, std::enable_if_t<!std::is_same_v<std::decay_t<Function>, FunctionRef>, int> = 0>
		FunctionRef(Function& function) noexcept : context_(std::addressof(function)),
			callback_([](const void* context, Args... args) -> Result
			{ return (*static_cast<const Function*>(context))(std::forward<Args>(args)...); }) {}
		explicit operator bool() const noexcept { return callback_ != nullptr; }
		Result operator()(Args... args) const { return callback_(context_, std::forward<Args>(args)...); }
	private:
		const void* context_ = nullptr;
		Result (*callback_)(const void*, Args...) = nullptr;
	};
}

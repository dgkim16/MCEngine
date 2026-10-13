#pragma once
#include <format>
#include <string_view>
#include <utility>

namespace MC {

	// @brief	Engine-wide logging. Callers format a message; the installed sink decides where it
	// @brief	goes. Core never includes the platform layer: the platform layer installs its
	// @brief	OS-specific sink at startup (dependency inversion), so it can log too.
	class LoggingSystem {
	public:
		enum class Level { Debug, Info, Warning, Error };

		LoggingSystem() = delete;	// static only


		// @brief	Format-and-write helpers, std::format syntax: Info("[switch] FAIL: {}", e.what())
		template <typename... Args>
		static void Debug(std::format_string<Args...> format, Args&&... args)
		{
			Write(Level::Debug, std::format(format, std::forward<Args>(args)...));
		}
		template <typename... Args>
		static void Info(std::format_string<Args...> format, Args&&... args)
		{
			Write(Level::Info, std::format(format, std::forward<Args>(args)...));
		}
		template <typename... Args>
		static void Warning(std::format_string<Args...> format, Args&&... args)
		{
			Write(Level::Warning, std::format(format, std::forward<Args>(args)...));
		}
		template <typename... Args>
		static void Error(std::format_string<Args...> format, Args&&... args)
		{
			Write(Level::Error, std::format(format, std::forward<Args>(args)...));
		}
	private:

		// @brief	Sends one finished message to the sink.
		static void Write(Level level, std::string_view message);
	};

}  // namespace MC
#pragma once

/// Macros for convenient usage of Poco logger.

#include <fmt/format.h>
#include <Poco/Logger.h>
#include <Poco/Message.h>


namespace
{
    template <typename... Ts> constexpr size_t numArgs(Ts &&...) { return sizeof...(Ts); }
    template <typename T, typename... Ts> constexpr auto firstArg(T && x, Ts &&...) { return std::forward<T>(x); }
}

template<typename... Args>
std::string logFormat(const std::string& fmt, Args&&... args) {
    std::string formatted_string = fmt::format(fmt::runtime(std::string_view(fmt.data(), fmt.size())), std::forward<Args>(args)...);
    return formatted_string;
}

/// Logs a message to a specified logger with that level.
/// If more than one argument is provided,
///  the first argument is interpreted as template with {}-substitutions
///  and the latter arguments treat as values to substitute.
/// If only one argument is provided, it is threat as message without substitutions.

#define LOG_IMPL(logger, PRIORITY, ...) do                                        \
{                                                                                 \
    if ((logger)->is((PRIORITY)))                                                 \
    {                                                                             \
        std::string formatted_message = numArgs(__VA_ARGS__) > 1 ? logFormat(__VA_ARGS__) : firstArg(__VA_ARGS__); \
        if (auto channel = (logger)->getChannel())                                \
        {                                                                         \
            std::string file_function;                                            \
            file_function += __FILE__;                                            \
            file_function += "; ";                                                \
            file_function += __PRETTY_FUNCTION__;                                 \
            Poco::Message poco_message((logger)->name(), formatted_message,       \
                                 (PRIORITY), file_function.c_str(), __LINE__);    \
            channel->log(poco_message);                                           \
        }                                                                         \
    }                                                                             \
} while (false)


#define LOG_TRACE(logger, ...)   LOG_IMPL(logger, Poco::Message::PRIO_TRACE, __VA_ARGS__)
#define LOG_DEBUG(logger, ...)   LOG_IMPL(logger, Poco::Message::PRIO_DEBUG, __VA_ARGS__)
#define LOG_INFO(logger, ...)    LOG_IMPL(logger, Poco::Message::PRIO_INFORMATION, __VA_ARGS__)
#define LOG_WARNING(logger, ...) LOG_IMPL(logger, Poco::Message::PRIO_WARNING, __VA_ARGS__)
#define LOG_ERROR(logger, ...)   LOG_IMPL(logger, Poco::Message::PRIO_ERROR, __VA_ARGS__)
#define LOG_FATAL(logger, ...)   LOG_IMPL(logger, Poco::Message::PRIO_FATAL, __VA_ARGS__)


/// Compatibility for external projects.
#if defined(ARCADIA_BUILD)
    using Poco::Logger;
    using Poco::Message;
#endif

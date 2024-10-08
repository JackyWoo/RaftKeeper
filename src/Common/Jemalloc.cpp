#include <Common/Jemalloc.h>

#if USE_JEMALLOC

#include <unistd.h>

#include <Common/Exception.h>
#include <common/logger_useful.h>
#include <jemalloc/jemalloc.h>

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)


namespace RK
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}


Poco::Logger * log = &Poco::Logger::get("Jemalloc");

void purgeJemallocArenas()
{
    LOG_INFO(log, "Purging unused memory");
    mallctl("arena." STRINGIFY(MALLCTL_ARENAS_ALL) ".purge", nullptr, nullptr, nullptr, 0);
}

void checkJemallocProfilingEnabled()
{
    bool active = true;
    size_t active_size = sizeof(active);
    mallctl("opt.prof", &active, &active_size, nullptr, 0);

    if (!active)
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "RaftKeeper was started without enabling profiling for jemalloc. To use jemalloc's profiler, following env variable should be "
            "set: MALLOC_CONF=background_thread:true,prof:true");
}

template <typename T>
void setJemallocValue(const char * name, T value)
{
    T old_value;
    size_t old_value_size = sizeof(T);
    if (mallctl(name, &old_value, &old_value_size, reinterpret_cast<void*>(&value), sizeof(T)))
    {
        LOG_WARNING(log, "mallctl for {} failed", name);
        return;
    }

    LOG_INFO(log, "Value for {} set to {} (from {})", name, value, old_value);
}

void setJemallocProfileActive(bool value)
{
    checkJemallocProfilingEnabled();
    bool active = true;
    size_t active_size = sizeof(active);
    mallctl("prof.active", &active, &active_size, nullptr, 0);
    if (active == value)
    {
        LOG_INFO(log, "Profiling is already {}", active ? "enabled" : "disabled");
        return;
    }

    setJemallocValue("prof.active", value);
    LOG_INFO(log, "Profiling is {}", value ? "enabled" : "disabled");
}

std::string flushJemallocProfile(const std::string & file_prefix)
{
    checkJemallocProfilingEnabled();
    char * prefix_buffer;
    size_t prefix_size = sizeof(prefix_buffer);
    int n = mallctl("opt.prof_prefix", &prefix_buffer, &prefix_size, nullptr, 0); // NOLINT
    if (!n && std::string_view(prefix_buffer) != "jeprof")
    {
        LOG_INFO(log, "Flushing memory profile with prefix {}", prefix_buffer);
        mallctl("prof.dump", nullptr, nullptr, nullptr, 0);
        return prefix_buffer;
    }

    static std::atomic<size_t> profile_counter{0};
    std::string profile_dump_path = fmt::format("{}.{}.{}.heap", file_prefix, getpid(), profile_counter.fetch_add(1));
    const auto * profile_dump_path_str = profile_dump_path.c_str();

    LOG_INFO(log, "Flushing memory profile to {}", profile_dump_path_str);
    mallctl("prof.dump", nullptr, nullptr, &profile_dump_path_str, sizeof(profile_dump_path_str)); // NOLINT
    return profile_dump_path;
}

void setJemallocBackgroundThreads(bool enabled)
{
    setJemallocValue("background_thread", enabled);
}

void setJemallocMaxBackgroundThreads(size_t max_threads)
{
    setJemallocValue("max_background_threads", max_threads);
}

}

#endif

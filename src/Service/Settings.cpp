#include <filesystem>
#include <Service/Settings.h>
#include <Common/IO/WriteHelpers.h>
#include <Common/getNumberOfPhysicalCPUCores.h>
#include <ZooKeeper/ZooKeeperConstants.h>


namespace RK
{
namespace ErrorCodes
{
    extern const int UNKNOWN_SETTING;
    extern const int ILLEGAL_SETTING_VALUE;
}

namespace FsyncModeNS
{
    FsyncMode parseFsyncMode(const String & in)
    {
        if (in == "fsync_parallel")
            return FsyncMode::FSYNC_PARALLEL;
        else if (in == "fsync")
            return FsyncMode::FSYNC;
        else if (in == "fsync_batch")
            return FsyncMode::FSYNC_BATCH;
        else
            throw Exception("Unknown config 'log_fsync_mode'.", ErrorCodes::UNKNOWN_SETTING);
    }

    String toString(FsyncMode mode)
    {
        if (mode == FsyncMode::FSYNC_PARALLEL)
            return "fsync_parallel";
        else if (mode == FsyncMode::FSYNC)
            return "fsync";
        else if (mode == FsyncMode::FSYNC_BATCH)
            return "fsync_batch";
        else
            throw Exception("Unknown config 'log_fsync_mode'.", ErrorCodes::UNKNOWN_SETTING);
    }

}

void RaftSettings::loadFromConfig(const String & config_elem, const Poco::Util::AbstractConfiguration & config)
{
    if (!config.has(config_elem))
        return;

    Poco::Util::AbstractConfiguration::Keys config_keys;
    config.keys(config_elem, config_keys);

    try
    {
        auto get_key = [&config_elem](String key) -> String { return config_elem + "." + key; };

        max_session_timeout_ms = config.getUInt(get_key("max_session_timeout_ms"), Coordination::DEFAULT_MAX_SESSION_TIMEOUT_MS);
        min_session_timeout_ms = config.getUInt(get_key("min_session_timeout_ms"), Coordination::DEFAULT_MIN_SESSION_TIMEOUT_MS);
        operation_timeout_ms = config.getUInt(get_key("operation_timeout_ms"), Coordination::DEFAULT_OPERATION_TIMEOUT_MS);
        if (min_session_timeout_ms > max_session_timeout_ms)
        {
            LOG_WARNING(
                log,
                "Invalid sesison timeout setting, need min_session_timeout_ms <= max_session_timeout_ms, got {}, {}. "
                "Reset session timeout to default values: {}, {}",
                min_session_timeout_ms,
                max_session_timeout_ms,
                Coordination::DEFAULT_MIN_SESSION_TIMEOUT_MS,
                Coordination::DEFAULT_MAX_SESSION_TIMEOUT_MS);
            max_session_timeout_ms = Coordination::DEFAULT_MAX_SESSION_TIMEOUT_MS;
            min_session_timeout_ms = Coordination::DEFAULT_MIN_SESSION_TIMEOUT_MS;
        }
        dead_session_check_period_ms = config.getUInt(get_key("dead_session_check_period_ms"), 500);
        heart_beat_interval_ms = config.getUInt(get_key("heart_beat_interval_ms"), 500);
        client_req_timeout_ms = config.getUInt(get_key("client_req_timeout_ms"), operation_timeout_ms);
        election_timeout_lower_bound_ms = config.getUInt(get_key("election_timeout_lower_bound_ms"), Coordination::ELECTION_TIMEOUT_LOWER_BOUND_MS);
        election_timeout_upper_bound_ms = config.getUInt(get_key("election_timeout_upper_bound_ms"), Coordination::ELECTION_TIMEOUT_UPPER_BOUND_MS);
        reserved_log_items = config.getUInt(get_key("reserved_log_items"), 1000000);
        snapshot_distance = config.getUInt(get_key("snapshot_distance"), 3000000);
        max_stored_snapshots = config.getUInt(get_key("max_stored_snapshots"), 5);
        startup_timeout = config.getUInt(get_key("startup_timeout"), 6000000);
        shutdown_timeout = config.getUInt(get_key("shutdown_timeout"), 5000);

        String log_level = config.getString(get_key("raft_logs_level"), "information");
        raft_logs_level = parseNuRaftLogLevel(log_level);
        nuraft_thread_size = config.getUInt(get_key("nuraft_thread_size"), getNumberOfPhysicalCPUCores());
        fresh_log_gap = config.getUInt(get_key("fresh_log_gap"), 200);
        configuration_change_tries_count = config.getUInt(get_key("configuration_change_tries_count"), 30);
        max_batch_size = config.getUInt(get_key("max_batch_size"), 1000);
        log_fsync_mode = FsyncModeNS::parseFsyncMode(config.getString(get_key("log_fsync_mode"), "fsync_parallel"));
        log_fsync_interval = config.getUInt(get_key("log_fsync_interval"), 1000);
        max_log_segment_file_size = config.getUInt(get_key("max_log_segment_file_size"), 1073741824);
        async_snapshot = config.getBool(get_key("async_snapshot"), true);
    }
    catch (Exception & e)
    {
        if (e.code() == ErrorCodes::UNKNOWN_SETTING)
            e.addMessage("in configuration.");
        throw;
    }
}

RaftSettingsPtr RaftSettings::getDefault()
{
    RaftSettingsPtr settings = std::make_shared<RaftSettings>();
    settings->max_session_timeout_ms = Coordination::DEFAULT_MAX_SESSION_TIMEOUT_MS;
    settings->min_session_timeout_ms = Coordination::DEFAULT_MIN_SESSION_TIMEOUT_MS;
    settings->operation_timeout_ms = Coordination::DEFAULT_OPERATION_TIMEOUT_MS;
    settings->dead_session_check_period_ms = 500;
    settings->heart_beat_interval_ms = 500;
    settings->client_req_timeout_ms = settings->operation_timeout_ms;
    settings->election_timeout_lower_bound_ms = Coordination::ELECTION_TIMEOUT_LOWER_BOUND_MS;
    settings->election_timeout_upper_bound_ms = Coordination::ELECTION_TIMEOUT_UPPER_BOUND_MS;
    settings->reserved_log_items = 10000000;
    settings->snapshot_distance = 3000000;
    settings->max_stored_snapshots = 5;
    settings->shutdown_timeout = 5000;
    settings->startup_timeout = 6000000;

    settings->raft_logs_level = NuRaftLogLevel::RAFT_LOG_INFORMATION;
    settings->nuraft_thread_size = getNumberOfPhysicalCPUCores();
    settings->fresh_log_gap = 200;
    settings->configuration_change_tries_count = 30;
    settings->max_batch_size = 1000;
    settings->log_fsync_interval = 1000;
    settings->max_log_segment_file_size = 1073741824;
    settings->log_fsync_mode = FsyncMode::FSYNC_PARALLEL;
    settings->async_snapshot = true;

    return settings;
}

const String Settings::DEFAULT_FOUR_LETTER_WORD_CMD =
#if USE_JEMALLOC
"jmst,jmpg,jmep,jmfp,jmdp,"
#endif
"conf,cons,crst,envi,ruok,srst,srvr,stat,wchs,dirs,mntr,isro,lgif,rqld,uptm,csnp";

Settings::Settings() : my_id(NOT_EXIST), port(NOT_EXIST), standalone_keeper(false), raft_settings(RaftSettings::getDefault())
{
}

void Settings::dump(WriteBufferFromOwnString & buf) const
{
    auto write_int = [&buf](int64_t value)
    {
        writeIntText(value, buf);
        buf.write('\n');
    };

    writeText("my_id=", buf);
    write_int(my_id);

    if (port != NOT_EXIST)
    {
        writeText("port=", buf);
        write_int(port);
    }

    writeText("host=", buf);
    writeText(host, buf);
    buf.write('\n');

    writeText("internal_port=", buf);
    write_int(internal_port);

    writeText("parallel=", buf);
    write_int(parallel);

    writeText("snapshot_create_interval=", buf);
    write_int(snapshot_create_interval);

    writeText("four_letter_word_white_list=", buf);
    writeText(four_letter_word_white_list, buf);
    buf.write('\n');

    writeText("log_dir=", buf);
    writeText(log_dir, buf);
    buf.write('\n');

    writeText("snapshot_dir=", buf);
    writeText(snapshot_dir, buf);
    buf.write('\n');

    /// raft_settings

    writeText("max_session_timeout_ms=", buf);
    write_int(raft_settings->max_session_timeout_ms);
    writeText("min_session_timeout_ms=", buf);
    write_int(raft_settings->min_session_timeout_ms);
    writeText("operation_timeout_ms=", buf);
    write_int(raft_settings->operation_timeout_ms);
    writeText("dead_session_check_period_ms=", buf);
    write_int(raft_settings->dead_session_check_period_ms);

    writeText("heart_beat_interval_ms=", buf);
    write_int(raft_settings->heart_beat_interval_ms);
    writeText("client_req_timeout_ms=", buf);
    write_int(raft_settings->client_req_timeout_ms);
    writeText("election_timeout_lower_bound_ms=", buf);
    write_int(raft_settings->election_timeout_lower_bound_ms);
    writeText("election_timeout_upper_bound_ms=", buf);
    write_int(raft_settings->election_timeout_upper_bound_ms);

    writeText("reserved_log_items=", buf);
    write_int(raft_settings->reserved_log_items);
    writeText("snapshot_distance=", buf);
    write_int(raft_settings->snapshot_distance);
    writeText("async_snapshot=", buf);
    write_int(raft_settings->async_snapshot);
    writeText("max_stored_snapshots=", buf);
    write_int(raft_settings->max_stored_snapshots);

    writeText("shutdown_timeout=", buf);
    write_int(raft_settings->shutdown_timeout);
    writeText("startup_timeout=", buf);
    write_int(raft_settings->startup_timeout);

    writeText("raft_logs_level=", buf);
    writeText(nuRaftLogLevelToString(raft_settings->raft_logs_level), buf);
    buf.write('\n');

    writeText("log_fsync_mode=", buf);
    writeText(FsyncModeNS::toString(raft_settings->log_fsync_mode), buf);
    buf.write('\n');
    writeText("log_fsync_interval=", buf);
    write_int(raft_settings->log_fsync_interval);
    buf.write('\n');
    writeText("max_log_segment_file_size=", buf);
    write_int(raft_settings->max_log_segment_file_size);

    writeText("nuraft_thread_size=", buf);
    write_int(raft_settings->nuraft_thread_size);
    writeText("fresh_log_gap=", buf);
    write_int(raft_settings->fresh_log_gap);
}

SettingsPtr Settings::loadFromConfig(const Poco::Util::AbstractConfiguration & config, bool standalone_keeper_)
{
    std::shared_ptr<Settings> ret = std::make_shared<Settings>();

    ret->my_id = config.getInt("keeper.my_id");
    if (ret->my_id < 0)
        throw Exception(ErrorCodes::ILLEGAL_SETTING_VALUE, "my_id can not be a negative value.");

    ret->standalone_keeper = standalone_keeper_;

    ret->port = config.getInt("keeper.port", 8101);
    ret->host = config.getString("keeper.host", "0.0.0.0");

    ret->internal_port = config.getInt("keeper.internal_port", 8103);
    ret->parallel = config.getInt("keeper.parallel", std::max(4U, getNumberOfPhysicalCPUCores()));

    ret->snapshot_create_interval = config.getUInt("keeper.snapshot_create_interval", 3600);
    ret->snapshot_create_interval = std::max(ret->snapshot_create_interval, 1U);

    ret->super_digest = config.getString("keeper.superdigest", "");

    ret->four_letter_word_white_list = config.getString("keeper.four_letter_word_white_list", DEFAULT_FOUR_LETTER_WORD_CMD);

    ret->log_dir = getLogsPathFromConfig(config, standalone_keeper_);
    ret->snapshot_dir = getSnapshotsPathFromConfig(config, standalone_keeper_);

    ret->raft_settings->loadFromConfig("keeper.raft_settings", config);

    return ret;
}

String Settings::getLogsPathFromConfig(const Poco::Util::AbstractConfiguration & config, bool)
{
    return config.getString("keeper.log_dir", "./data/log");
}

String Settings::getSnapshotsPathFromConfig(const Poco::Util::AbstractConfiguration & config, bool)
{
    return config.getString("keeper.snapshot_dir", "./data/snapshot");
}
}

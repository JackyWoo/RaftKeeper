#pragma once

#if !defined(ARCADIA_BUILD)
#    include <Common/config.h>
#endif

#include <functional>

#include <Poco/FIFOBuffer.h>
#include <Poco/Util/AbstractConfiguration.h>

#include <Common/ConcurrentBoundedQueue.h>
#include <Common/Exception.h>
#include <Common/ThreadPool.h>
#include <common/logger_useful.h>

#include <Service/ConnectionStats.h>
#include <Service/Keeper4LWInfo.h>
#include <Service/KeeperServer.h>
#include <Service/NuRaftStateMachine.h>
#include <Service/RequestAccumulator.h>
#include <Service/RequestForwarder.h>
#include <Service/RequestProcessor.h>
#include <Service/RequestsQueue.h>
#include <Service/Settings.h>

namespace RK
{
using ZooKeeperResponseCallback = std::function<void(const Coordination::ZooKeeperResponsePtr & response)>;
using ForwardResponseCallback = std::function<void(ForwardResponsePtr response)>;

class KeeperDispatcher : public std::enable_shared_from_this<KeeperDispatcher>
{
private:
    std::mutex push_request_mutex;
    ptr<RequestsQueue> requests_queue;
    ThreadSafeQueue<ResponseForSession> responses_queue;
    std::atomic<bool> shutdown_called{false};

    /// Response callback which will send response to IO handler. Key is session_id
    /// which are local session which are directly connected to the node.
    using UserResponseCallbacks = std::unordered_map<int64_t, ZooKeeperResponseCallback>;
    UserResponseCallbacks user_response_callbacks;
    std::shared_mutex response_callbacks_mutex;

    /// Just like user_response_callbacks, but only concerns new session or update session requests.
    /// For new session request the key is internal_id, for update session request the key is session id.
    using SessionResponseCallbacks = std::unordered_map<int64_t, ZooKeeperResponseCallback>;
    SessionResponseCallbacks session_response_callbacks;

    struct PairHash
    {
        template <class T1, class T2>
        std::size_t operator()(const std::pair<T1, T2> & p) const
        {
            auto h1 = std::hash<T1>{}(p.first);
            auto h2 = std::hash<T2>{}(p.second);
            return h1 ^ h2;
        }
    };

    /// <server id, client id>
    using ForwardClientId = std::pair<int32_t, int32_t>;
    using ForwardResponseCallbacks = std::unordered_map<ForwardClientId, ForwardResponseCallback, PairHash>;

    ForwardResponseCallbacks forward_response_callbacks;
    std::shared_mutex forward_response_callbacks_mutex;

    using UpdateConfigurationQueue = ConcurrentBoundedQueue<ConfigUpdateAction>;
    /// More than 1k updates is definitely misconfiguration.
    UpdateConfigurationQueue update_configuration_queue{1000};

    ThreadPoolPtr request_thread;
    ThreadPoolPtr responses_thread;

    ThreadFromGlobalPool session_cleaner_thread;

    /// Apply or wait for configuration changes
    ThreadFromGlobalPool update_configuration_thread;

    std::shared_ptr<KeeperServer> server;

    ConnectionStats keeper_stats;

    SettingsPtr configuration_and_settings;

    Poco::Logger * log;

    /// Request processing chain :
    ///     1. request_accumulator for accumulating request into batch
    ///     2. request_forwarder for forwarding requests to leader
    ///     3. request_processor for processing requests
    std::shared_ptr<RequestProcessor> request_processor;
    RequestAccumulator request_accumulator;
    RequestForwarder request_forwarder;

    Poco::Timestamp uptime;

    /// Used as new session request internal id counter
    std::atomic<int64_t> new_session_internal_id_counter;

    void requestThread(RunnerId runner_id);
    void responseThread();

    /// Clean dead sessions
    void deadSessionCleanThread();
    void invokeResponseCallBack(int64_t session_id, const Coordination::ZooKeeperResponsePtr & response);

public:
    KeeperDispatcher();

    void initialize(const Poco::Util::AbstractConfiguration & config);

    void shutdown();

    ~KeeperDispatcher() = default;

    /// Push user requests
    bool pushRequest(const Coordination::ZooKeeperRequestPtr & request, int64_t session_id);

    /// Push new session or update session request
    bool pushSessionRequest(const Coordination::ZooKeeperRequestPtr & request, int64_t internal_id);

    /// Push forward request
    bool pushForwardRequest(size_t server_id, size_t client_id, ForwardRequestPtr request);

    /// Register response callback for forwarder
    void registerForwarderResponseCallBack(ForwardClientId client_id, ForwardResponseCallback callback);
    void unRegisterForwarderResponseCallBack(ForwardClientId client_id);

    /// Register response callback for user request
    void registerUserResponseCallBackWithoutLock(int64_t session_id, ZooKeeperResponseCallback callback, bool is_reconnected = false);
    void unregisterUserResponseCallBack(int64_t session_id);
    void unregisterUserResponseCallBackWithoutLock(int64_t session_id);

    /// Register response callback for new session or update session request
    void registerSessionResponseCallback(int64_t id, ZooKeeperResponseCallback callback);
    void unRegisterSessionResponseCallback(int64_t id);
    void unRegisterSessionResponseCallbackWithoutLock(int64_t id);

    bool isLocalSession(int64_t session_id);

    void filterLocalSessions(std::unordered_map<int64_t, int64_t> & session_to_expiration_time);

    /// from follower
    void handleRemoteSession(int64_t session_id, int64_t expiration_time) { server->handleRemoteSession(session_id, expiration_time); }

    /// Thread apply or wait configuration changes from leader
    void updateConfigurationThread();
    /// Registered in ConfigReloader callback. Add new configuration changes to
    /// update_configuration_queue. Keeper Dispatcher apply them asynchronously.
    void updateConfiguration(const Poco::Util::AbstractConfiguration & config);

    /// Invoked when a request completes.
    void updateKeeperStatLatency(uint64_t process_time_ms);

    /// Send forward response
    void invokeForwardResponseCallBack(ForwardClientId client_id, ForwardResponsePtr response);

    /// Are we leader
    bool isLeader() const { return server->isLeader(); }
    bool hasLeader() const { return server->isLeaderAlive(); }
    bool isObserver() const { return server->isObserver(); }

    /// get log size in bytes
    uint64_t getLogDirSize() const;

    /// get snapshot size in bytes
    uint64_t getSnapDirSize() const;

    /// Request statistics such as qps, latency etc.
    const ConnectionStats & getKeeperConnectionStats() const
    {
        return keeper_stats;
    }

    Keeper4LWInfo getKeeper4LWInfo();

    const NuRaftStateMachine & getStateMachine() const { return *server->getKeeperStateMachine(); }

    const SettingsPtr & getKeeperConfigurationAndSettings() const { return configuration_and_settings; }

    void incrementPacketsSent()
    {
        keeper_stats.incrementPacketsSent();
    }

    void incrementPacketsReceived()
    {
        keeper_stats.incrementPacketsReceived();
    }

    void resetConnectionStats()
    {
        keeper_stats.reset();
    }

    void incrementPushRequest()
    {
        keeper_stats.incrementPacketsReceived();
    }


    uint64_t createSnapshot() { return server->createSnapshot(); }

    KeeperLogInfo getKeeperLogInfo() { return server->getKeeperLogInfo(); }

    /// Request to be leader
    bool requestLeader() { return server->requestLeader(); }

    /// Return process start time in us.
    int64_t uptimeFromStartup() { return Poco::Timestamp() - uptime; }

    /// My server id
    int32_t myId() const { return server->myId(); }

    /// When user creating new session, we use this id as fake session id.
    /// Note that the internal id for different nodes can not be same.
    int64_t getAndAddInternalId() { return new_session_internal_id_counter.fetch_add(server->getClusterNodeCount()); }
};

}

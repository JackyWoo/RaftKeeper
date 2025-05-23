#include <Service/KeeperDispatcher.h>
#include <Service/RequestForwarder.h>
#include <Service/Context.h>
#include <Common/setThreadName.h>
#include <fmt/ranges.h>

namespace RK
{

namespace ErrorCodes
{
    extern const int RAFT_FORWARD_ERROR;
    extern const int RAFT_IS_LEADER;
    extern const int RAFT_NO_LEADER;
    extern const int RAFT_FWD_NO_CONN;
}

void RequestForwarder::push(const RequestForSession & request_for_session)
{
    requests_queue->push(request_for_session);
}

void RequestForwarder::runSend(RunnerId runner_id)
{
    setThreadName(("ReqFwdSend#" + toString(runner_id)).c_str());

    LOG_DEBUG(log, "Starting forward request sending thread.");
    while (!shutdown_called)
    {
        UInt64 max_wait = session_sync_period_ms;
        if (session_sync_idx == runner_id)
        {
            auto elapsed_milliseconds = session_sync_time_watch.elapsedMilliseconds();
            max_wait = elapsed_milliseconds >= session_sync_period_ms ? 0 : session_sync_period_ms - elapsed_milliseconds;
        }

        RequestForSession request_for_session;

        if (requests_queue->tryPop(runner_id, request_for_session, max_wait))
        {
            try
            {
                if (server->isLeader())
                {
                    LOG_WARNING(log, "A leader switch may have occurred suddenly");
                    throw Exception("Can't forward request", ErrorCodes::RAFT_IS_LEADER);
                }

                if (!server->isLeaderAlive())
                    throw Exception("Raft no leader", ErrorCodes::RAFT_NO_LEADER);

                int32_t leader = server->getLeader();
                ptr<ForwardConnection> connection;
                {
                    std::lock_guard<std::mutex> lock(connections_mutex);
                    connection = connections[leader][runner_id];
                }

                if (!connection)
                    throw Exception("Not found connection for runner " + std::to_string(runner_id), ErrorCodes::RAFT_FWD_NO_CONN);

                ForwardRequestPtr forward_request = ForwardRequestFactory::instance().convertFromRequest(request_for_session);
                forward_request->send_time = clock::now();
                forward_request_queue[runner_id]->push(forward_request);
                connection->send(forward_request);
            }
            catch (...)
            {
                tryLogCurrentException(log, "Error when forwarding request with runner " + std::to_string(runner_id));
                request_processor->onError(
                    false,
                    nuraft::cmd_result_code::FAILED,
                    request_for_session.session_id,
                    request_for_session.request->xid,
                    request_for_session.request->getOpNum());
            }
        }

        if (session_sync_idx % parallel == runner_id && session_sync_time_watch.elapsedMilliseconds() >= session_sync_period_ms)
        {
            if (!server->isLeader() && server->isLeaderAlive())
            {
                /// send sessions
                try
                {
                    int32_t leader = server->getLeader();
                    ptr<ForwardConnection> connection;
                    {
                        std::lock_guard<std::mutex> lock(connections_mutex);
                        connection = connections[leader][runner_id];
                    }

                    if (connection)
                    {
                        /// TODO if keeper nodes time has large gap something will be wrong.
                        auto session_to_expiration_time = server->getKeeperStateMachine()->getStore().sessionToExpirationTime();
                        keeper_dispatcher->filterLocalSessions(session_to_expiration_time);

                        if (!session_to_expiration_time.empty())
                            LOG_DEBUG(log, "Has {} local sessions to send", session_to_expiration_time.size());

                        if (!session_to_expiration_time.empty())
                        {
                            ForwardRequestPtr forward_request = std::make_shared<ForwardSyncSessionsRequest>(std::move(session_to_expiration_time));
                            forward_request->send_time = clock::now();
                            forward_request_queue[runner_id]->push(forward_request);
                            connection->send(forward_request);
                        }
                    }
                    else
                    {
                        throw Exception(
                            ErrorCodes::RAFT_FORWARD_ERROR, "Not found connection when sending sessions for runner {}", runner_id);
                    }
                }
                catch (...)
                {
                    tryLogCurrentException(log, "error forward session to leader for runner " + std::to_string(runner_id));
                }
            }

            session_sync_time_watch.restart();
            ++session_sync_idx;
        }
    }
}

void RequestForwarder::runReceive(RunnerId runner_id)
{
    setThreadName(("ReqFwdRecv#" + toString(runner_id)).c_str());

    LOG_DEBUG(log, "Starting forward response receiving thread.");
    while (!shutdown_called)
    {
        auto sleep = [this]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(session_sync_period_ms));
        };

        auto to_microseconds = [](const clock::duration & duration) -> UInt64
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        };

        try
        {
            UInt64 max_wait = session_sync_period_ms;
            clock::time_point now = clock::now();

            /// Check if the earliest request has timed out. And handle all timed out requests.
            ForwardRequestPtr earliest_request;
            if (forward_request_queue[runner_id]->peek(earliest_request))
            {
                auto earliest_request_deadline = earliest_request->send_time + std::chrono::microseconds(operation_timeout.totalMicroseconds());
                if (now > earliest_request_deadline)
                {
                    LOG_DEBUG(
                        log,
                        "Earliest request {} in runner {} deadline {}, now {}",
                        earliest_request->toString(),
                        runner_id,
                        to_microseconds(earliest_request_deadline.time_since_epoch()),
                        to_microseconds(now.time_since_epoch()));

                    if (processTimeoutRequest(runner_id, earliest_request))
                        earliest_request_deadline = earliest_request->send_time + std::chrono::microseconds(operation_timeout.totalMicroseconds());
                }

                max_wait = std::min(max_wait, to_microseconds(earliest_request_deadline - now) / 1000);
            }

            if (!server->isLeader() && server->isLeaderAlive())
            {
                int32_t leader = server->getLeader();
                if (leader == server->myId() || leader == -1) /// In case of data condition caused by leader switch
                {
                    LOG_INFO(log, "I become leader or no leader when receiving forward response, sleep for a while and try again.");
                    sleep();
                    continue;
                }

                ptr<ForwardConnection> connection;
                {
                    std::lock_guard<std::mutex> lock(connections_mutex);
                    connection = connections[leader][runner_id];
                }

                if (connection && connection->isConnected())
                {
                    if (!connection->poll(max_wait * 1000))
                    {
                        continue;
                    }

                    ForwardResponsePtr response;
                    connection->receive(response);
                    processResponse(runner_id, response);
                }
                else
                {
                    if (!connection)
                        LOG_WARNING(log, "Not found connection for runner {}", runner_id);
                    else if (!connection->isConnected())
                        LOG_TRACE(log, "Connection not connected for runner {}, maybe no session attached to me", runner_id);
                    sleep();
                }
            }
            else
            {
                sleep();
            }
        }
        catch (...)
        {
            tryLogCurrentException(log, "Error when receiving forward response, runner " + std::to_string(runner_id));
            sleep();
        }
    }
}

bool RequestForwarder::processTimeoutRequest(RunnerId runner_id, ForwardRequestPtr newFront)
{
    LOG_INFO(log, "Process timeout request for runner {} queue size {}", runner_id, forward_request_queue[runner_id]->size());

    clock::time_point now = clock::now();

    auto func = [this, now](const ForwardRequestPtr & request) -> bool
    {
        clock::time_point earliest_send_time = request->send_time;
        auto earliest_operation_deadline
            = clock::time_point(earliest_send_time) + std::chrono::microseconds(operation_timeout.totalMicroseconds());
        if (now > earliest_operation_deadline)
        {
            LOG_WARNING(log, "Forward request {} timeout", request->toString());
            ForwardResponsePtr response = request->makeResponse();
            response->setAppendEntryResult(false, nuraft::cmd_result_code::TIMEOUT);
            response->onError(*this); /// timeout
            return true;
        }
        else
        {
            return false;
        }
    };

    return forward_request_queue[runner_id]->removeFrontIf(func, newFront);
}


bool RequestForwarder::removeFromQueue(RunnerId runner_id, ForwardResponsePtr forward_response_ptr)
{
    return forward_request_queue[runner_id]->findAndRemove([forward_response_ptr](const ForwardRequestPtr & request) -> bool
    {
        if (request->forwardType() != forward_response_ptr->forwardType())
            return false;

        return forward_response_ptr->match(request);
    });
}


void RequestForwarder::processResponse(RunnerId runner_id, ForwardResponsePtr forward_response_ptr)
{
    if (!removeFromQueue(runner_id, forward_response_ptr))
    {
        LOG_WARNING(log, "Not found request in runner {} for forward response {}", runner_id, forward_response_ptr->toString());
        return;
    }

    if (forward_response_ptr->accepted)
    {
        LOG_DEBUG(log, "Receive a forward response {} for runner {}", forward_response_ptr->toString(), runner_id);
        return;
    }

    /// common request
    LOG_ERROR(log, "Receive failed forward response {}", forward_response_ptr->toString());

    forward_response_ptr->onError(*this); /// for NewSession UpdateSession Op, maybe peer not accepted or raft not accepted
}

void RequestForwarder::shutdown()
{
    LOG_INFO(log, "Shutting down request forwarder!");
    if (shutdown_called)
        return;

    shutdown_called = true;

    request_thread->wait();
    response_thread->wait();

    for (auto & queue : forward_request_queue)
    {
        queue->forEach([this](const ForwardRequestPtr & request) -> bool
        {
            ForwardResponsePtr response = request->makeResponse();
            response->setAppendEntryResult(false, nuraft::cmd_result_code::FAILED);
            response->onError(*this); /// shutdown
            return true;
        });
    }

    RequestForSession request_for_session;
    while (requests_queue->tryPopAny(request_for_session))
    {
        request_processor->onError(
            false,
            nuraft::cmd_result_code::CANCELLED,
            request_for_session.session_id,
            request_for_session.request->xid,
            request_for_session.request->getOpNum());
    }
}

void RequestForwarder::initConnections()
{
    LOG_INFO(log, "Begin to init request forwarder connections");

    /// Find config diff
    String config_name = "keeper.cluster";
    const auto & config = Context::get().getConfigRef();
    Poco::Util::AbstractConfiguration::Keys keys;
    config.keys(config_name, keys);

    int32_t my_id = server->myId();

    std::unordered_map<UInt32, EndPoint> new_cluster_config_forward;

    for (const auto & key : keys)
    {
        if (startsWith(key, "server"))
        {
            /// only care for host forwarding_port and learner
            int32_t id = config.getInt(config_name + "." + key + ".id");
            String host = config.getString(config_name + "." + key + ".host");

            String forwarding_port = config.getString(config_name + "." + key + ".forwarding_port", "8102");
            String endpoint = host + ":" + forwarding_port;

            bool learner = config.getBool(config_name + "." + key + ".learner", false);

            if (my_id != id && !learner)
                new_cluster_config_forward[id] = endpoint;
        }
    }

    LOG_INFO(log, "Found {} servers in config and now we have {} servers", new_cluster_config_forward.size(), cluster_config_forward.size());

    std::lock_guard<std::mutex> lock(connections_mutex);

    /// Diff config, update and add connections
    for (auto & [server_id, endpoint] : new_cluster_config_forward)
    {
        auto it = cluster_config_forward.find(server_id);
        if (it != cluster_config_forward.end() && it->second == endpoint)
            continue;

        cluster_config_forward[server_id] = endpoint;
        connections.erase(server_id);

        ConnectionPool connection_pool;
        for (size_t runner_id = 0; runner_id < parallel; ++runner_id)
        {
            LOG_INFO(log, "Creating forward connection #{}#{} to {}", server_id, runner_id, endpoint);
            std::shared_ptr<ForwardConnection> connection = std::make_shared<ForwardConnection>(
                my_id, runner_id, endpoint, operation_timeout);
            connection_pool.push_back(connection);
        }
        connections.emplace(server_id, connection_pool);
    }

    /// Diff config, remove connections
    for (auto it = cluster_config_forward.begin(); it != cluster_config_forward.end();)
    {
        auto new_it = new_cluster_config_forward.find(it->first);
        if (new_it == new_cluster_config_forward.end())
        {
            it = cluster_config_forward.erase(it);
            connections.erase(it->first);
        }
        else
        {
            it++;
        }
    }
}

void RequestForwarder::initialize(
    size_t parallel_,
    std::shared_ptr<KeeperServer> server_,
    std::shared_ptr<KeeperDispatcher> keeper_dispatcher_,
    UInt64 session_sync_period_ms_,
    UInt64 operation_timeout_ms_)
{
    parallel = parallel_;
    session_sync_period_ms = session_sync_period_ms_;
    server = server_;
    keeper_dispatcher = keeper_dispatcher_;
    requests_queue = std::make_shared<RequestsQueue>(parallel, 20000);

    operation_timeout = operation_timeout_ms_ * 1000;

    for (RunnerId runner_id = 0; runner_id < parallel; runner_id++)
    {
        forward_request_queue.push_back(std::make_unique<ForwardRequestQueue>());
    }

    initConnections();
    server->registerForWardListener([this](){ initConnections(); });

    request_thread = std::make_shared<ThreadPool>(parallel);
    for (RunnerId runner_id = 0; runner_id < parallel; runner_id++)
    {
        request_thread->trySchedule([this, runner_id] { runSend(runner_id); });
    }

    response_thread = std::make_shared<ThreadPool>(parallel);
    for (RunnerId runner_id = 0; runner_id < parallel; runner_id++)
    {
        response_thread->trySchedule([this, runner_id] { runReceive(runner_id); });
    }
}

}

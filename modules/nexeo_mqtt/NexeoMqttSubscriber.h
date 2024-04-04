#pragma once

// System and Library Includes
#include "json_fwd.hpp"
#include <memory>
#include <thread>
#include <ZMS.hpp>

// Forward Declarations
class NexeoMqttConnection;

// Class Interface
class NexeoMqttSubscriber
{
public:
    NexeoMqttSubscriber(
        NexeoMqttConnection& aMqttConnection);
    ~NexeoMqttSubscriber();

    void rxMessage(
        const std::string& aTopic,
        const std::string& aMessage);

private:
    void groupJoin(
        const nlohmann::json& aData);
    void groupLeave(
        const nlohmann::json& aData);
    void groupsQuery(
        const nlohmann::json& aData);
    void groupsAssign(
        const nlohmann::json& aData);

private:
    NexeoMqttConnection& mMqttConnection;
    std::shared_ptr<zms::LinuxAgent> mZmsAgent;
};


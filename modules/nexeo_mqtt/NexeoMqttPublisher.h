#pragma once

// System and Library Includes
#include <memory>
#include <thread>
#include <ZMS.hpp>

// Forward Declarations
class NexeoMqttConnection;

// Class Interface
class NexeoMqttPublisher
{
public:
    NexeoMqttPublisher(
        NexeoMqttConnection& aMqttConnection);
    ~NexeoMqttPublisher();

private:
    void rxMessage();

private:
    NexeoMqttConnection& mMqttConnection;
    bool mRun;
    std::shared_ptr<zms::LinuxAgent> mZmsAgent;
    std::shared_ptr<std::thread> mRxThread;
};


#pragma once

// System and Library Includes
#include <cstdint>
#include "json_fwd.hpp"
#include <re.h>
#include <string>

// Forward Declarations
class NexeoMqttSubscriber;
struct mosquitto;

// Class Interface
class NexeoMqttConnection
{
public:
    NexeoMqttConnection();
    ~NexeoMqttConnection();

    void publish(
        const nlohmann::json& aPayload);
    void publish(
        const std::string& aPayload);
    void publish(
        const std::string& aTopic,
        const nlohmann::json& aPayload);
    void publish(
        const std::string& aTopic,
        const std::string& aPayload);

    void setSubscriber(
        NexeoMqttSubscriber* aSubscriber);

private:
    void loadConfig();
    template <typename T> T getConfig(
        const std::string& aPrimaryConfig,
        const std::string& aSecondaryConfig,
        const T& aDefaultValue);
    void setConnectionOptions();
    void reconnect();
    void subscribe();
    void handleMessage(
        const std::string& aTopic,
        const std::string& aMessage);

private:
    struct mosquitto* mInstance;
    std::string mHost;
    uint16_t mPort;
    std::string mCaFile;
    std::string mUsername;
    std::string mPassword;
    std::string mClientId;
    std::string mBaseTopic;
    std::string mPublishTopic;
    std::string mSubscribeTopic;
    bool mRunning;
    struct tmr mTimer;
    NexeoMqttSubscriber* mSubscriber;
};


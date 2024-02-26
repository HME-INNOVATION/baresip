#pragma once

// System and Library Includes
#include <cstdint>
#include <json_fwd.hpp>
#include <string>

// Forward Declarations
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

private:
    void loadConfig();
    void getConfig(
        const std::string& aConfig,
        std::string& aTarget);
    void getConfig(
        const std::string& aConfig,
        uint32_t& aTarget);
    void setConnectionOptions();

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
};


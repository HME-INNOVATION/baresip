// Class Interface
#include "NexeoMqttConnection.h"

// System and Library Includes
#include <json.hpp>
#include <re.h>
#include <baresip.h>
#include <mosquitto.h>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Constructor.
// ---------------------------------------------------------------------------
NexeoMqttConnection::NexeoMqttConnection()
:   mInstance(nullptr),
    mHost("127.0.0.1"),
    mPort(1883),
    mBaseTopic("nexeo_mqtt")
{
    mosquitto_lib_init();

    // Load our configuration options.
    loadConfig();

    // Create the mosquitto instance.
    mInstance = mosquitto_new(
        (mClientId.empty() ? nullptr : mClientId.c_str()),
        true,
        this);
    if (!mInstance)
    {
        throw std::runtime_error("Could not create MQTT client instance");
    }

    // Set options on the mosquitto instance.
    setConnectionOptions();

    // Connect to the broker.
    if (mosquitto_connect(
            mInstance,
            mHost.c_str(),
            mPort,
            60) != MOSQ_ERR_SUCCESS)
    {
        throw std::runtime_error("Could not connect to broker");
    }

    if (mosquitto_loop_start(mInstance) != MOSQ_ERR_SUCCESS)
    {
        throw std::runtime_error("Could not start processing loop");
    }
}

// ---------------------------------------------------------------------------
// Destructor.
// ---------------------------------------------------------------------------
NexeoMqttConnection::~NexeoMqttConnection()
{
    if (mInstance)
    {
        mosquitto_disconnect(mInstance);
        mosquitto_loop_stop(mInstance, true);
        mosquitto_destroy(mInstance);
        mInstance = nullptr;
    }

    mosquitto_lib_cleanup();
}

// ---------------------------------------------------------------------------
// Publishes a JSON payload to the default topic.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::publish(
    const nlohmann::json& aPayload)
{
    publish(aPayload.dump());
}

// ---------------------------------------------------------------------------
// Publishes a JSON payload to the specified topic.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::publish(
    const std::string& aTopic,
    const nlohmann::json& aPayload)
{
    publish(aTopic, aPayload.dump());
}

// ---------------------------------------------------------------------------
// Publishes a string payload to the default topic.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::publish(
    const std::string& aPayload)
{
    publish(mPublishTopic, aPayload);
}

// ---------------------------------------------------------------------------
// Publishes a string payload to the specified topic.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::publish(
    const std::string& aTopic,
    const std::string& aPayload)
{
    if (mosquitto_publish(
            mInstance,
            nullptr,
            aTopic.c_str(),
            aPayload.length(),
            aPayload.c_str(),
            0,
            false) != MOSQ_ERR_SUCCESS)
    {
        throw std::runtime_error("Failed to publish message");
    }
}

// ---------------------------------------------------------------------------
// Loads the MQTT configuration from the application config, and validates
// the values.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::loadConfig()
{
    // Get configuration values.
    getConfig("mqtt_broker_host", mHost);
    getConfig("mqtt_broker_cafile", mCaFile);
    getConfig("mqtt_broker_user", mUsername);
    getConfig("mqtt_broker_password", mPassword);
    getConfig("mqtt_broker_clientid", mClientId);
    getConfig("mqtt_basetopic", mBaseTopic);
    getConfig("mqtt_publishtopic", mPublishTopic);
    uint32_t temp;
    getConfig("mqtt_broker_port", temp);

    // Validate configuration values.
    if (temp < 65536)
    {
        mPort = static_cast<uint16_t>(temp);
    }
    else
    {
        throw std::runtime_error("Invalid port value");
    }

    // Set appropriate defaults.
    if (mPublishTopic.empty())
    {
        mPublishTopic =
            std::string("/") +
            mBaseTopic +
            std::string("/event");
    }

    info(
        "nexeo_mqtt: Connecting to %s:%d as '%s', publishing on: %s\n",
        mHost.c_str(),
        mPort,
        mClientId.c_str(),
        mPublishTopic.c_str());
}

// ---------------------------------------------------------------------------
// Sets connection options on the mosquitto instance.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::setConnectionOptions()
{
    if (!mUsername.empty() && mosquitto_username_pw_set(
            mInstance,
            mUsername.c_str(),
            mPassword.c_str()) != MOSQ_ERR_SUCCESS)
    {
        throw std::runtime_error("Could not set username / password");
    }

    if (!mCaFile.empty() && mosquitto_tls_set(
            mInstance,
            mCaFile.c_str(),
            NULL,
            NULL,
            NULL,
            NULL) != MOSQ_ERR_SUCCESS)
    {
        throw std::runtime_error("Could not set CA");
    }
}

// ---------------------------------------------------------------------------
// Gets a configuration item as a string.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::getConfig(
    const std::string& aConfig,
    std::string& aTarget)
{
    std::string temp(256, 0);
    if (conf_get_str(
            conf_cur(),
            aConfig.c_str(),
            const_cast<char*>(temp.data()),
            temp.length()) != 0)
    {
        return;
    }

    temp.erase(temp.find('\0'));
    aTarget = std::move(temp);
}

// ---------------------------------------------------------------------------
// Gets a configuration item as an unsigned integer.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::getConfig(
    const std::string& aConfig,
    uint32_t& aTarget)
{
    conf_get_u32(
        conf_cur(),
        aConfig.c_str(),
        &aTarget);
}


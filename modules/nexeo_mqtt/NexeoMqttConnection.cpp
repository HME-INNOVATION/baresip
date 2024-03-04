// Class Interface
#include "NexeoMqttConnection.h"

// System and Library Includes
#include <json.hpp>
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
	tmr_init(&mTimer);

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
	tmr_cancel(&mTimer);

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
// Gets a configuration item as a string.
// ---------------------------------------------------------------------------
template <>
std::string NexeoMqttConnection::getConfig(
    const std::string& aPrimaryConfig,
    const std::string& aSecondaryConfig,
    const std::string& aDefaultValue)
{
    std::string temp(256, 0);

    if (conf_get_str(
            conf_cur(),
            aPrimaryConfig.c_str(),
            const_cast<char*>(temp.data()),
            temp.length()) != 0 &&
        !aSecondaryConfig.empty() &&
        conf_get_str(
            conf_cur(),
            aSecondaryConfig.c_str(),
            const_cast<char*>(temp.data()),
            temp.length()) != 0)
    {
        return aDefaultValue;
    }

    temp.erase(temp.find('\0'));
    return temp;
}

// ---------------------------------------------------------------------------
// Gets a configuration item as an unsigned integer.
// ---------------------------------------------------------------------------
template<typename T>
T NexeoMqttConnection::getConfig(
    const std::string& aPrimaryConfig,
    const std::string& aSecondaryConfig,
    const T& aDefaultValue)
{
    uint32_t temp;

    if (conf_get_u32(
            conf_cur(),
            aPrimaryConfig.c_str(),
            &temp) != 0 &&
        !aSecondaryConfig.empty() &&
        conf_get_u32(
            conf_cur(),
            aPrimaryConfig.c_str(),
            &temp) != 0)
    {
        return aDefaultValue;
    }

    if (temp < std::numeric_limits<T>::min() ||
        temp > std::numeric_limits<T>::max())
    {
        return aDefaultValue;
    }

    return static_cast<T>(temp);
}

// ---------------------------------------------------------------------------
// Loads the MQTT configuration from the application config, and validates
// the values.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::loadConfig()
{
    // Get configuration values.
    mHost = getConfig<std::string>(
        "nexeo_mqtt_broker_host",
        "mqtt_broker_host",
        "127.0.0.1");
    mPort = getConfig<uint16_t>(
        "nexeo_mqtt_broker_port",
        "mqtt_broker_port",
        1883);
    mCaFile = getConfig<std::string>(
        "nexeo_mqtt_broker_cafile",
        "mqtt_broker_cafile",
        "");
    mUsername = getConfig<std::string>(
        "nexeo_mqtt_broker_user",
        "mqtt_broker_user",
        "");
    mPassword = getConfig<std::string>(
        "nexeo_mqtt_broker_password",
        "mqtt_broker_password",
        "");
    mClientId = getConfig<std::string>(
        "nexeo_mqtt_broker_clientid",
        "", // don't fall back to share a client id with mqtt.so
        "");
    mBaseTopic = getConfig<std::string>(
        "nexeo_mqtt_basetopic",
        "mqtt_basetopic",
        "nexeo_mqtt");
    mPublishTopic = getConfig<std::string>(
        "nexeo_mqtt_publishtopic",
        "mqtt_publishtopic",
        "");

    // Set appropriate defaults.
    if (mPublishTopic.empty())
    {
        mPublishTopic = mBaseTopic + "/event";
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

	mosquitto_disconnect_callback_set(
        mInstance,
        [](struct mosquitto* aInstance, void* aObj, int aError)
        {
            auto self = static_cast<NexeoMqttConnection*>(aObj);
            self->reconnect();
        });
}

// ---------------------------------------------------------------------------
// Reconnects to the broker if the connection is lost.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::reconnect()
{
	auto err = mosquitto_reconnect(mInstance);
	if (err == MOSQ_ERR_SUCCESS)
    {
        info(
            "nexeo_mqtt: reconnected to %s:%d as '%s'\n",
            mHost.c_str(),
            mPort,
            mClientId.c_str());
        return;
    }

    warning("nexeo_mqtt: reconnect failed, retrying\n");
    tmr_start(
        &mTimer,
        2000,
        [](void* aObj)
        {
            auto self = static_cast<NexeoMqttConnection*>(aObj);
            self->reconnect();
        },
        this);
}


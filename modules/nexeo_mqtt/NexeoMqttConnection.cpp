// Class Interface
#include "NexeoMqttConnection.h"

// System and Library Includes
#include "json.hpp"
#include <baresip.h>
#include <mosquitto.h>
#include <stdexcept>

// Local Includes
#include "NexeoMqttSubscriber.h"

// ---------------------------------------------------------------------------
// Constructor.
// ---------------------------------------------------------------------------
NexeoMqttConnection::NexeoMqttConnection()
:   mInstance(nullptr),
    mHost("127.0.0.1"),
    mPort(1883),
    mBaseTopic("nexeo_mqtt"),
    mRunning(true),
    mSubscriber(nullptr)
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
    auto err = mosquitto_connect(
        mInstance,
        mHost.c_str(),
        mPort,
        60);
    if (err == MOSQ_ERR_INVAL)
    {
        throw std::runtime_error("Could not connect to broker");
    }
    else if (err != MOSQ_ERR_SUCCESS)
    {
        // NB: All other errors should be recoverable by retrying.
        reconnect();
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
    mRunning = false;

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
// Sets the subscriber object.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::setSubscriber(
    NexeoMqttSubscriber* aSubscriber)
{
    mSubscriber = aSubscriber;
}

// ---------------------------------------------------------------------------
// Publishes a string payload to the specified topic.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::publish(
    const std::string& aTopic,
    const std::string& aPayload)
{
    if (!mRunning)
    {
        return;
    }

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
    mSubscribeTopic = getConfig<std::string>(
        "nexeo_mqtt_subscribetopic",
        "mqtt_subscribetopic",
        "");

    // Set appropriate defaults.
    if (mPublishTopic.empty())
    {
        mPublishTopic = mBaseTopic + "/event";
    }

    if (mSubscribeTopic.empty())
    {
        mSubscribeTopic = mBaseTopic + "/command/+";
    }

    info(
        "nexeo_mqtt: Connecting to %s:%d as '%s', "
        "publishing on: %s, "
        "subscribing on: %s\n",
        mHost.c_str(),
        mPort,
        mClientId.c_str(),
        mPublishTopic.c_str(),
        mSubscribeTopic.c_str());
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

	mosquitto_connect_callback_set(
        mInstance,
        [](struct mosquitto* aInstance, void* aObj, int aError)
        {
            auto self = static_cast<NexeoMqttConnection*>(aObj);
            self->subscribe();
        });
	mosquitto_disconnect_callback_set(
        mInstance,
        [](struct mosquitto* aInstance, void* aObj, int aError)
        {
            auto self = static_cast<NexeoMqttConnection*>(aObj);
            self->reconnect();
        });
	mosquitto_message_callback_set(
        mInstance,
        [](struct mosquitto* aInstance, void* aObj, const struct mosquitto_message* aMessage)
        {
            auto self = static_cast<NexeoMqttConnection*>(aObj);
            std::string message(
                static_cast<const char*>(aMessage->payload),
                aMessage->payloadlen);
            self->handleMessage(aMessage->topic, message);
        });
}

// ---------------------------------------------------------------------------
// Reconnects to the broker if the connection is lost.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::reconnect()
{
    if (!mRunning)
    {
        return;
    }

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

// ---------------------------------------------------------------------------
// Subscribes to the MQTT topic.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::subscribe()
{
    if (!mRunning)
    {
        return;
    }

    auto err = mosquitto_subscribe(
        mInstance,
        nullptr,
        mSubscribeTopic.c_str(),
        0);

	if (err != MOSQ_ERR_SUCCESS)
    {
        warning("nexeo_mqtt: failed to subscribe: %d\n", err);
        return;
    }
}

// ---------------------------------------------------------------------------
// Handles reception of an MQTT message.
// ---------------------------------------------------------------------------
void NexeoMqttConnection::handleMessage(
    const std::string& aTopic,
    const std::string& aMessage)
{
    if (!mRunning)
    {
        return;
    }

	bool match = false;
	mosquitto_topic_matches_sub(
        mSubscribeTopic.c_str(),
        aTopic.c_str(),
        &match);
    if (!match)
    {
        debug(
            "nexeo_mqtt: Ignoring message, topic mismatch: '%s' '%s'\n",
            aTopic.c_str(),
            aMessage.c_str());
        return;
    }

    // Pass the message off to the subscriber object.
    if (mSubscriber)
    {
        mSubscriber->rxMessage(aTopic, aMessage);
    }
}


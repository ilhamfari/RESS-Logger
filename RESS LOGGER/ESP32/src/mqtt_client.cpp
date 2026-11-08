#include "mqtt_client.h"
#include "config.h"
#include "ntp_time.h"
#include <WiFi.h>
#include <MQTT.h>
#include <ArduinoJson.h>

static WiFiClient s_wifi_client;
static MQTTClient s_mqtt(4096);
static SystemConfig s_cfg;

static RelayCmdReceivedCallback s_relay_cmd_cb = nullptr;
static TelemetryDeliveredCallback s_delivered_cb = nullptr;

static uint32_t s_last_reconnect_attempt = 0;
static uint32_t s_last_wait_log = 0;

static const char *returnCodeText(int rc) {
    switch (rc) {
    case 0: return "accepted";
    case 1: return "unacceptable protocol version";
    case 2: return "client id rejected";
    case 3: return "server unavailable";
    case 4: return "BAD USERNAME OR PASSWORD";
    case 5: return "NOT AUTHORIZED (user/client id/ACL)";
    default: return "unknown";
    }
}

static const char *lastErrorText(lwmqtt_err_t e) {
    switch (e) {
    case LWMQTT_SUCCESS: return "none";
    case LWMQTT_NETWORK_FAILED_CONNECT: return "TCP connect failed (host/port/firewall)";
    case LWMQTT_NETWORK_TIMEOUT: return "network timeout (no CONNACK)";
    case LWMQTT_NETWORK_FAILED_READ: return "network read failed (broker closed connection)";
    case LWMQTT_NETWORK_FAILED_WRITE: return "network write failed";
    case LWMQTT_CONNECTION_DENIED: return "connection denied by broker";
    default: return "other";
    }
}

// Plain TCP probe, to tell "network/port problem" apart from "login problem".
static void probeBroker(const char *host, uint16_t port) {
    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) {
        Serial.printf("[MQTT] DNS lookup of %s FAILED (router has no DNS/internet?)\r\n", host);
        return;
    }
    WiFiClient probe;
    bool ok = probe.connect(ip, port, 4000);
    Serial.printf("[MQTT] probe %s (%s):%u -> TCP %s\r\n", host, ip.toString().c_str(), port,
                  ok ? "OK (port reachable, so the problem is login/protocol)"
                     : "FAILED (port blocked or broker down)");
    probe.stop();
}

namespace {
struct BatchAdder {
    JsonArray &batch;
    explicit BatchAdder(JsonArray &b) : batch(b) {}

    template <typename T>
    void operator()(const char *devId, const char *key, const T &val) const {
        JsonObject item = batch.add<JsonObject>();
        item["deviceId"] = devId;
        item["key"] = key;
        item["val"] = val;
    }
};
} // namespace

void MQTTManager::setOnRelayCmd(RelayCmdReceivedCallback cb) {
    s_relay_cmd_cb = cb;
}

void MQTTManager::setOnTelemetryDelivered(TelemetryDeliveredCallback cb) {
    s_delivered_cb = cb;
}

void MQTTManager::reloadConfig(const SystemConfig &cfg) {
    s_cfg = cfg;
    if (s_mqtt.connected()) {
        s_mqtt.disconnect();
    }
    s_mqtt.begin(s_cfg.mqtt_server, s_cfg.mqtt_port, s_wifi_client);
    s_last_reconnect_attempt = 0;
}

void MQTTManager::init(const SystemConfig &cfg) {
    s_cfg = cfg;
    s_mqtt.begin(s_cfg.mqtt_server, s_cfg.mqtt_port, s_wifi_client);
    s_mqtt.onMessage(onMqttMessage);
    s_mqtt.setOptions(60, true, 5000); // 60s keepalive, cleanSession = true, 5000ms timeout
}

bool MQTTManager::isConnected() {
    return s_mqtt.connected();
}

void MQTTManager::connect() {
    if (WiFi.status() != WL_CONNECTED) return;

    // Contract: every "ts" must be a real UTC time (>= 2026-01-01). Like the
    // reference sketch, do not connect/publish before the clock is valid.
    if (!NTPTimeManager::isTimeSynchronized()) {
        uint32_t now = millis();
        if (now - s_last_wait_log >= 10000) {
            s_last_wait_log = now;
            Serial.println("[MQTT] Waiting for a valid clock (NTP) before connecting...");
        }
        return;
    }

    Serial.printf("[MQTT] Connecting to %s:%u as %s...\r\n",
                  s_cfg.mqtt_server, s_cfg.mqtt_port, s_cfg.board_id);

    // 1. Prepare LWT Payload
    char will_payload[128];
    snprintf(will_payload, sizeof(will_payload),
             "{\"v\":%d,\"boardId\":\"%s\",\"online\":false}",
             CONTRACT_VERSION, s_cfg.board_id);

    // 2. Set LWT before connect(): QoS 1, Retain = true
    s_mqtt.setWill(s_cfg.topic_status, will_payload, true, 1);

    // 3. Connect: ClientID == BoardID, User, Pass
    const char *user = strlen(s_cfg.mqtt_user) > 0 ? s_cfg.mqtt_user : s_cfg.board_id;
    const char *pass = strlen(s_cfg.mqtt_pass) > 0 ? s_cfg.mqtt_pass : s_cfg.board_id;

    bool connected = s_mqtt.connect(s_cfg.board_id, user, pass);

    if (connected) {
        Serial.println("[MQTT] Connected successfully!");

        // 4. Publish Online Status: Retained = true, QoS = 1
        uint32_t ts = NTPTimeManager::getEpochSeconds();
        char online_payload[256];
        snprintf(online_payload, sizeof(online_payload),
                 "{\"v\":%d,\"boardId\":\"%s\",\"online\":true,\"ts\":%lu,\"fw\":\"%s\"}",
                 CONTRACT_VERSION, s_cfg.board_id, (unsigned long)ts, FIRMWARE_VERSION);

        bool status_ok = s_mqtt.publish(s_cfg.topic_status, online_payload, true, 1);
        Serial.printf("[MQTT] Status publish %s to %s\r\n",
                      status_ok ? "OK" : "FAILED", s_cfg.topic_status);

        // 5. Subscribe to Command topic: QoS = 1
        bool sub_ok = s_mqtt.subscribe(s_cfg.topic_cmd, 1);
        Serial.printf("[MQTT] Subscribed to %s (%s)\r\n",
                      s_cfg.topic_cmd, sub_ok ? "OK" : "FAILED");
    } else {
        int rc = (int)s_mqtt.returnCode();
        lwmqtt_err_t le = s_mqtt.lastError();
        Serial.printf("[MQTT] Failed to connect: returnCode=%d (%s), lastError=%d (%s)\r\n",
                      rc, returnCodeText(rc), (int)le, lastErrorText(le));
        Serial.printf("[MQTT] used host=%s port=%u clientId=%s user=%s\r\n", s_cfg.mqtt_server,
                      s_cfg.mqtt_port, s_cfg.board_id, user);
        if (le == LWMQTT_NETWORK_FAILED_CONNECT || le == LWMQTT_NETWORK_TIMEOUT) {
            probeBroker(s_cfg.mqtt_server, s_cfg.mqtt_port);
        }
    }
}

void MQTTManager::onMqttMessage(String &topic, String &payload) {
    Serial.printf("[MQTT-RX] Topic: %s, Payload: %s\r\n", topic.c_str(), payload.c_str());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[MQTT-RX] JSON parse failed: %s\r\n", err.c_str());
        return;
    }

    // Flexible extraction for relay command
    uint8_t ch = 0;
    if (!doc["ch"].isNull()) {
        ch = doc["ch"].as<uint8_t>();
    } else if (!doc["channel"].isNull()) {
        ch = doc["channel"].as<uint8_t>();
    } else if (!doc["relay"].isNull()) {
        ch = doc["relay"].as<uint8_t>();
    }

    uint8_t state = 0;
    if (!doc["state"].isNull()) {
        if (doc["state"].is<bool>()) {
            state = doc["state"].as<bool>() ? 1 : 0;
        } else if (doc["state"].is<int>()) {
            state = (doc["state"].as<int>() > 0) ? 1 : 0;
        } else if (doc["state"].is<const char *>()) {
            const char *st_str = doc["state"].as<const char *>();
            if (strcasecmp(st_str, "on") == 0 || strcmp(st_str, "1") == 0 || strcasecmp(st_str, "true") == 0) {
                state = 1;
            } else {
                state = 0;
            }
        }
    }

    if (ch >= 1 && ch <= 4) {
        Serial.printf("[MQTT-RX] Relay command recognized: Ch %u -> %u\r\n", ch, state);
        if (s_relay_cmd_cb) {
            s_relay_cmd_cb(ch, state);
        }
    } else {
        Serial.printf("[MQTT-RX] Command ignored or channel %u out of range (1-4)\r\n", ch);
    }
}

void MQTTManager::process() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    // Maintain MQTT event loop (keep-alive, incoming messages, puback)
    s_mqtt.loop();

    if (!s_mqtt.connected()) {
        uint32_t now = millis();
        if (now - s_last_reconnect_attempt >= 5000) {
            s_last_reconnect_attempt = now;
            connect();
        }
    }
}

bool MQTTManager::publishTelemetry(const TelemetrySnapshot &snap, uint32_t epoch_ts) {
    if (!s_mqtt.connected()) {
        Serial.println("[MQTT] Cannot publish telemetry: MQTT disconnected");
        return false;
    }
    if ((uint64_t)epoch_ts < MIN_VALID_EPOCH) {
        Serial.printf("[MQTT] Refusing to publish: ts=%lu is not a valid UTC time\r\n", (unsigned long)epoch_ts);
        return false;
    }

    JsonDocument doc;
    doc["v"] = CONTRACT_VERSION;
    doc["boardId"] = s_cfg.board_id;
    doc["ts"] = epoch_ts;

    JsonArray batch = doc["batch"].to<JsonArray>();

    // Helper to add batch item
    BatchAdder addItem(batch);

    // GoodWe is the only Modbus device published by this gateway.
    if (snap.goodwe.online) {
        if (snap.goodwe.has_vpv)    addItem("inv_goodwe_atap", "pv_voltage", snap.goodwe.vpv);
        if (snap.goodwe.has_ipv)    addItem("inv_goodwe_atap", "pv_current", snap.goodwe.ipv);
        if (snap.goodwe.has_ppv)    addItem("inv_goodwe_atap", "pv_power", snap.goodwe.ppv);
        if (snap.goodwe.has_energy) addItem("inv_goodwe_atap", "pv_energy_total", snap.goodwe.energy_kwh);
        if (snap.goodwe.has_pout)   addItem("inv_goodwe_atap", "inv_power_out", snap.goodwe.pout);
        if (snap.goodwe.has_pload)  addItem("inv_goodwe_atap", "load_power", snap.goodwe.pload);
        if (snap.goodwe.has_status) addItem("inv_goodwe_atap", "inv_status", snap.goodwe.status);
        if (snap.goodwe.has_temp)   addItem("inv_goodwe_atap", "temp_internal", snap.goodwe.temp);
    }

    // Board health
    addItem("gw", "rssi", WiFi.RSSI());
    addItem("gw", "uptime", (uint32_t)(millis() / 1000));
    addItem("gw", "fw_version", FIRMWARE_VERSION);

    // Serialize
    char json_buffer[4096];
    size_t len = serializeJson(doc, json_buffer, sizeof(json_buffer));

    if (len == 0 || len >= sizeof(json_buffer)) {
        Serial.println("[MQTT] Telemetry JSON serialize overflow!");
        return false;
    }

    Serial.printf("[MQTT-TX] Topic: %s (%u bytes)\r\n", s_cfg.topic_telemetry, (unsigned)len);
    // Publish telemetry contract: Retain = false, QoS = 1
    bool pub_ok = s_mqtt.publish(s_cfg.topic_telemetry, json_buffer, false, 1);

    if (pub_ok) {
        Serial.printf("[MQTT] Batch telemetry successfully published (seq=%lu)!\r\n",
                      (unsigned long)snap.seq);
        if (s_delivered_cb) {
            s_delivered_cb(snap.seq, true);
        }
        return true;
    } else {
        Serial.println("[MQTT] Telemetry publish failed!");
        if (s_delivered_cb) {
            s_delivered_cb(snap.seq, false);
        }
        return false;
    }
}

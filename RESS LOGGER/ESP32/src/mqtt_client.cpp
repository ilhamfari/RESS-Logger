#include "mqtt_client.h"
#include "config.h"
#include "ntp_time.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static WiFiClient s_wifi_client;
static PubSubClient s_mqtt(s_wifi_client);
static SystemConfig s_cfg;

static RelayCmdReceivedCallback s_relay_cmd_cb = nullptr;
static TelemetryDeliveredCallback s_delivered_cb = nullptr;

static uint32_t s_last_reconnect_attempt = 0;

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
    s_mqtt.setServer(s_cfg.mqtt_server, s_cfg.mqtt_port);
    if (s_mqtt.connected()) {
        s_mqtt.disconnect();
    }
}

void MQTTManager::init(const SystemConfig &cfg) {
    s_cfg = cfg;
    s_mqtt.setBufferSize(4096);
    s_mqtt.setServer(s_cfg.mqtt_server, s_cfg.mqtt_port);
    s_mqtt.setCallback(onMqttMessage);
}

bool MQTTManager::isConnected() {
    return s_mqtt.connected();
}

void MQTTManager::connect() {
    if (WiFi.status() != WL_CONNECTED) return;

    Serial.printf("[MQTT] Connecting to %s:%d as %s...\r\n",
                  s_cfg.mqtt_server, s_cfg.mqtt_port, s_cfg.board_id);

    // LWT Payload
    char lwt_payload[128];
    snprintf(lwt_payload, sizeof(lwt_payload),
             "{\"v\":%d,\"boardId\":\"%s\",\"online\":false}",
             CONTRACT_VERSION, s_cfg.board_id);

    boolean connected = false;
    if (strlen(s_cfg.mqtt_user) > 0) {
        connected = s_mqtt.connect(s_cfg.board_id, s_cfg.mqtt_user, s_cfg.mqtt_pass,
                                   s_cfg.topic_status, 1, true, lwt_payload);
    } else {
        connected = s_mqtt.connect(s_cfg.board_id, s_cfg.topic_status, 1, true, lwt_payload);
    }

    if (connected) {
        Serial.println("[MQTT] Connected successfully!");

        // Publish Online Status (Retained, QoS 1)
        uint32_t ts = NTPTimeManager::getEpochSeconds();
        char online_payload[256];
        snprintf(online_payload, sizeof(online_payload),
                 "{\"v\":%d,\"boardId\":\"%s\",\"online\":true,\"ts\":%lu,\"fw\":\"%s\"}",
                 CONTRACT_VERSION, s_cfg.board_id, (unsigned long)ts, FIRMWARE_VERSION);

        s_mqtt.publish(s_cfg.topic_status, online_payload, true);
        Serial.printf("[MQTT] Published online status to %s\r\n", s_cfg.topic_status);

        // Subscribe to Command topic
        s_mqtt.subscribe(s_cfg.topic_cmd, 1);
        Serial.printf("[MQTT] Subscribed to %s\r\n", s_cfg.topic_cmd);
    } else {
        Serial.printf("[MQTT] Failed to connect, state: %d\r\n", s_mqtt.state());
    }
}

void MQTTManager::onMqttMessage(char *topic, byte *payload, unsigned int length) {
    char msg[512];
    if (length >= sizeof(msg)) length = sizeof(msg) - 1;
    memcpy(msg, payload, length);
    msg[length] = '\0';

    Serial.printf("[MQTT-RX] Topic: %s, Payload: %s\r\n", topic, msg);

    // Check if it's a relay command
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg);
    if (!err) {
        const char *cmd = doc["cmd"] | "";
        if (strcmp(cmd, "relay") == 0 || !doc["ch"].isNull()) {
            uint8_t ch = doc["ch"] | 0;
            uint8_t state = doc["state"] | 0;
            if (ch >= 1 && ch <= 4 && s_relay_cmd_cb) {
                Serial.printf("[MQTT] Relay command received: Ch %u -> %u\r\n", ch, state);
                s_relay_cmd_cb(ch, state);
            }
        }
    }
}

void MQTTManager::process() {
    if (!s_mqtt.connected()) {
        uint32_t now = millis();
        if (now - s_last_reconnect_attempt > 5000) {
            s_last_reconnect_attempt = now;
            connect();
        }
    } else {
        s_mqtt.loop();
    }
}

bool MQTTManager::publishTelemetry(const TelemetrySnapshot &snap, uint32_t epoch_ts) {
    if (!s_mqtt.connected()) {
        Serial.println("[MQTT] Cannot publish telemetry: MQTT disconnected");
        return false;
    }

    JsonDocument doc;
    doc["v"] = CONTRACT_VERSION;
    doc["boardId"] = s_cfg.board_id;
    doc["ts"] = epoch_ts;

    JsonArray batch = doc["batch"].to<JsonArray>();

    // Helper to add batch item
    BatchAdder addItem(batch);

    // 1. Larkin Meter (meter1)
    if (snap.larkin.online) {
        addItem("meter1", "voltage", snap.larkin.ua);
        addItem("meter1", "current", snap.larkin.ia);
        addItem("meter1", "grid_power", snap.larkin.p_total);
        addItem("meter1", "reactive_power", snap.larkin.q_total);
        addItem("meter1", "apparent_power", snap.larkin.s_total);
        addItem("meter1", "power_factor", snap.larkin.pf_avg);
        addItem("meter1", "frequency", snap.larkin.freq);
        if (snap.larkin.has_e_imp) {
            addItem("meter1", "energy_import_total", snap.larkin.e_imp);
        }
        if (snap.larkin.has_e_exp) {
            addItem("meter1", "energy_export_total", snap.larkin.e_exp);
        }
    }

    // 2. GoodWe Inverter (inv_goodwe_atap)
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

    // 3. Sungrow Inverter (inv_sungrow)
    if (snap.sungrow.online) {
        if (snap.sungrow.has_vpv)    addItem("inv_sungrow", "pv_voltage", snap.sungrow.vpv);
        if (snap.sungrow.has_ipv)    addItem("inv_sungrow", "pv_current", snap.sungrow.ipv);
        if (snap.sungrow.has_ppv)    addItem("inv_sungrow", "pv_power", snap.sungrow.ppv);
        if (snap.sungrow.has_energy) addItem("inv_sungrow", "pv_energy_total", snap.sungrow.energy_kwh);
        if (snap.sungrow.has_pout)   addItem("inv_sungrow", "inv_power_out", snap.sungrow.pout);
        if (snap.sungrow.has_pload)  addItem("inv_sungrow", "load_power", snap.sungrow.pload);
        if (snap.sungrow.has_status) addItem("inv_sungrow", "inv_status", snap.sungrow.status);
        if (snap.sungrow.has_temp)   addItem("inv_sungrow", "temp_internal", snap.sungrow.temp);
    }

    // 4. Board Health (gw)
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
    boolean pub_ok = s_mqtt.publish(s_cfg.topic_telemetry, json_buffer, false);

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

#include "nvs_config.h"
#include "config.h"
#include <Preferences.h>

static Preferences s_prefs;
static const char *PREF_NAMESPACE = "ress_cfg";

void NVSConfigManager::updateDerivedTopics(SystemConfig &cfg) {
    if (strlen(cfg.topic_telemetry) == 0) {
        snprintf(cfg.topic_telemetry, sizeof(cfg.topic_telemetry), "ress/%s/telemetry", cfg.board_id);
    }
    if (strlen(cfg.topic_status) == 0) {
        snprintf(cfg.topic_status, sizeof(cfg.topic_status), "ress/%s/status", cfg.board_id);
    }
    if (strlen(cfg.topic_cmd) == 0) {
        snprintf(cfg.topic_cmd, sizeof(cfg.topic_cmd), "ress/%s/cmd", cfg.board_id);
    }
}

void NVSConfigManager::resetDefaults(SystemConfig &cfg) {
    memset(&cfg, 0, sizeof(SystemConfig));
    strlcpy(cfg.wifi_ssid, DEFAULT_WIFI_SSID, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, DEFAULT_WIFI_PASS, sizeof(cfg.wifi_pass));

    strlcpy(cfg.mqtt_server, DEFAULT_MQTT_SERVER, sizeof(cfg.mqtt_server));
    cfg.mqtt_port = DEFAULT_MQTT_PORT;
    strlcpy(cfg.board_id, DEFAULT_BOARD_ID, sizeof(cfg.board_id));
    strlcpy(cfg.mqtt_user, DEFAULT_MQTT_USER, sizeof(cfg.mqtt_user));
    strlcpy(cfg.mqtt_pass, DEFAULT_MQTT_PASS, sizeof(cfg.mqtt_pass));

    cfg.send_interval_min = DEFAULT_INTERVAL_MIN;
    updateDerivedTopics(cfg);
}

void NVSConfigManager::init() {
    s_prefs.begin(PREF_NAMESPACE, false);
}

bool NVSConfigManager::loadConfig(SystemConfig &cfg) {
    init();
    if (!s_prefs.isKey("initialized")) {
        resetDefaults(cfg);
        saveConfig(cfg);
        return true;
    }

    s_prefs.getString("w_ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    s_prefs.getString("w_pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));

    s_prefs.getString("m_srv", cfg.mqtt_server, sizeof(cfg.mqtt_server));
    cfg.mqtt_port = s_prefs.getUShort("m_port", DEFAULT_MQTT_PORT);
    s_prefs.getString("m_bid", cfg.board_id, sizeof(cfg.board_id));
    s_prefs.getString("m_usr", cfg.mqtt_user, sizeof(cfg.mqtt_user));
    s_prefs.getString("m_pwd", cfg.mqtt_pass, sizeof(cfg.mqtt_pass));

    cfg.send_interval_min = s_prefs.getUShort("interval", DEFAULT_INTERVAL_MIN);
    if (cfg.send_interval_min == 0) {
        cfg.send_interval_min = 1;
    }

    s_prefs.getString("t_tele", cfg.topic_telemetry, sizeof(cfg.topic_telemetry));
    s_prefs.getString("t_stat", cfg.topic_status, sizeof(cfg.topic_status));
    s_prefs.getString("t_cmd", cfg.topic_cmd, sizeof(cfg.topic_cmd));

    if (strlen(cfg.wifi_ssid) == 0) strlcpy(cfg.wifi_ssid, DEFAULT_WIFI_SSID, sizeof(cfg.wifi_ssid));
    if (strlen(cfg.wifi_pass) == 0) strlcpy(cfg.wifi_pass, DEFAULT_WIFI_PASS, sizeof(cfg.wifi_pass));
    if (strlen(cfg.mqtt_server) == 0) strlcpy(cfg.mqtt_server, DEFAULT_MQTT_SERVER, sizeof(cfg.mqtt_server));
    if (cfg.mqtt_port == 0) cfg.mqtt_port = DEFAULT_MQTT_PORT;
    if (strlen(cfg.board_id) == 0) strlcpy(cfg.board_id, DEFAULT_BOARD_ID, sizeof(cfg.board_id));
    if (strlen(cfg.mqtt_user) == 0) strlcpy(cfg.mqtt_user, DEFAULT_MQTT_USER, sizeof(cfg.mqtt_user));
    if (strlen(cfg.mqtt_pass) == 0) strlcpy(cfg.mqtt_pass, DEFAULT_MQTT_PASS, sizeof(cfg.mqtt_pass));

    updateDerivedTopics(cfg);
    return true;
}

bool NVSConfigManager::saveConfig(const SystemConfig &cfg) {
    init();
    s_prefs.putBool("initialized", true);
    s_prefs.putString("w_ssid", cfg.wifi_ssid);
    s_prefs.putString("w_pass", cfg.wifi_pass);

    s_prefs.putString("m_srv", cfg.mqtt_server);
    s_prefs.putUShort("m_port", cfg.mqtt_port);
    s_prefs.putString("m_bid", cfg.board_id);
    s_prefs.putString("m_usr", cfg.mqtt_user);
    s_prefs.putString("m_pwd", cfg.mqtt_pass);

    s_prefs.putUShort("interval", cfg.send_interval_min);

    s_prefs.putString("t_tele", cfg.topic_telemetry);
    s_prefs.putString("t_stat", cfg.topic_status);
    s_prefs.putString("t_cmd", cfg.topic_cmd);

    return true;
}

void NVSConfigManager::saveRelayState(uint8_t ch, uint8_t state) {
    if (ch < 1 || ch > 4) return;
    init();
    char key[4];
    snprintf(key, sizeof(key), "r%u", ch);
    s_prefs.putUChar(key, state ? 1 : 0);
    Serial.printf("[NVS] Saved relay %u state: %u\r\n", ch, state ? 1 : 0);
}

void NVSConfigManager::loadRelayStates(uint8_t &r1, uint8_t &r2, uint8_t &r3, uint8_t &r4) {
    init();
    r1 = s_prefs.getUChar("r1", 0);
    r2 = s_prefs.getUChar("r2", 0);
    r3 = s_prefs.getUChar("r3", 0);
    r4 = s_prefs.getUChar("r4", 0);
}

uint8_t NVSConfigManager::getRelayState(uint8_t ch) {
    if (ch < 1 || ch > 4) return 0;
    init();
    char key[4];
    snprintf(key, sizeof(key), "r%u", ch);
    return s_prefs.getUChar(key, 0);
}

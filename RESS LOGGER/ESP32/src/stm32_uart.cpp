#include "stm32_uart.h"
#include "config.h"

static TelemetrySnapshot s_current_snap;
static TelemetrySnapshot s_latest_snap;
static RelayState s_latest_relay;

static SnapshotCallback s_snapshot_cb = nullptr;
static RelayAckCallback s_relay_ack_cb = nullptr;
static PendingEmptyCallback s_pending_empty_cb = nullptr;
static void (*s_time_req_cb)() = nullptr;
static bool s_time_acked = false;

static char s_rx_buf[512];
static size_t s_rx_len = 0;
static bool s_in_frame = false;

uint8_t STM32UARTBridge::calcChecksum(const char *payload) {
    uint8_t xor_sum = 0;
    while (*payload) {
        xor_sum ^= (uint8_t)(*payload++);
    }
    return xor_sum;
}

static bool sendFrame(const char *payload) {
    uint8_t chk = STM32UARTBridge::calcChecksum(payload);
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "$%s*%02X\r\n", payload, chk);
    if (n > 0 && (size_t)n < sizeof(buf)) {
        Serial1.write((const uint8_t *)buf, n);
        Serial.printf("[STM32-TX] %s", buf);
        return true;
    }
    return false;
}

void STM32UARTBridge::init() {
    memset(&s_current_snap, 0, sizeof(s_current_snap));
    memset(&s_latest_snap, 0, sizeof(s_latest_snap));
    memset(&s_latest_relay, 0, sizeof(s_latest_relay));

    Serial1.begin(STM32_BAUD_RATE, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);
    Serial.printf("[STM32] UART link initialized on RX=%d, TX=%d @ %d baud\r\n",
                  STM32_RX_PIN, STM32_TX_PIN, STM32_BAUD_RATE);
}

void STM32UARTBridge::setOnSnapshotReceived(SnapshotCallback cb) {
    s_snapshot_cb = cb;
}

void STM32UARTBridge::setOnRelayAck(RelayAckCallback cb) {
    s_relay_ack_cb = cb;
}

void STM32UARTBridge::setOnPendingEmpty(PendingEmptyCallback cb) {
    s_pending_empty_cb = cb;
}

bool STM32UARTBridge::isTimeAcked() {
    return s_time_acked;
}

void STM32UARTBridge::setOnTimeRequest(void (*cb)()) {
    s_time_req_cb = cb;
}

const TelemetrySnapshot& STM32UARTBridge::getLatestSnapshot() {
    return s_latest_snap;
}

const RelayState& STM32UARTBridge::getLatestRelay() {
    return s_latest_relay;
}

bool STM32UARTBridge::requestTelemetry(uint32_t scheduled_epoch, int16_t rssi,
                                       uint32_t uptime, const char *fw_version) {
    char payload[96];
    snprintf(payload, sizeof(payload), "REQ,TELE,%lu,%d,%lu,%s", (unsigned long)scheduled_epoch,
             (int)rssi, (unsigned long)uptime, fw_version ? fw_version : "NA");
    return sendFrame(payload);
}

bool STM32UARTBridge::requestPending() { return sendFrame("REQ,PENDING"); }

bool STM32UARTBridge::sendAckTelemetry(uint32_t seq, bool success) {
    char payload[64];
    snprintf(payload, sizeof(payload), "ACK,TELE,%lu,%s", (unsigned long)seq,
             success ? "SUCCESS" : "FAIL");
    return sendFrame(payload);
}

bool STM32UARTBridge::sendRelayCommand(uint8_t channel, uint8_t state) {
    char payload[64];
    snprintf(payload, sizeof(payload), "CMD,RELAY,%u,%u", (unsigned)channel, (unsigned)state);
    return sendFrame(payload);
}

bool STM32UARTBridge::sendGoodWeEnable(bool enabled) {
    return sendFrame(enabled ? "CMD,GOODWE,1" : "CMD,GOODWE,0");
}

bool STM32UARTBridge::sendTimeSync(uint16_t year, uint8_t month, uint8_t date,
                                   uint8_t hour, uint8_t minute, uint8_t second, uint32_t epoch) {
    char payload[96];
    snprintf(payload, sizeof(payload), "CMD,TIME,%04u,%02u,%02u,%02u,%02u,%02u,%lu",
             (unsigned)year, (unsigned)month, (unsigned)date,
             (unsigned)hour, (unsigned)minute, (unsigned)second, (unsigned long)epoch);
    return sendFrame(payload);
}

void STM32UARTBridge::parseLarkin(char *args[], size_t count) {
    // args[0]="DATA", args[1]="LARKIN", args[2]=seq
    // if offline: args[3]="NA"
    // if online: args[3..15] = Ua,Ub,Uc,Ia,Ib,Ic,Ptot,Qtot,Stot,PF,Freq,Eimp,Eexp
    if (count < 4) return;
    uint32_t seq = strtoul(args[2], nullptr, 10);
    s_current_snap.seq = seq;

    if (strcmp(args[3], "NA") == 0) {
        s_current_snap.larkin.online = false;
        return;
    }

    if (count < 16) return;
    s_current_snap.larkin.online = true;
    s_current_snap.larkin.ua = atof(args[3]);
    s_current_snap.larkin.ub = atof(args[4]);
    s_current_snap.larkin.uc = atof(args[5]);
    s_current_snap.larkin.ia = atof(args[6]);
    s_current_snap.larkin.ib = atof(args[7]);
    s_current_snap.larkin.ic = atof(args[8]);
    s_current_snap.larkin.p_total = atof(args[9]);
    s_current_snap.larkin.q_total = atof(args[10]);
    s_current_snap.larkin.s_total = atof(args[11]);
    s_current_snap.larkin.pf_avg = atof(args[12]);
    s_current_snap.larkin.freq = atof(args[13]);

    if (strcmp(args[14], "NA") != 0) {
        s_current_snap.larkin.has_e_imp = true;
        s_current_snap.larkin.e_imp = atof(args[14]);
    } else {
        s_current_snap.larkin.has_e_imp = false;
    }

    if (strcmp(args[15], "NA") != 0) {
        s_current_snap.larkin.has_e_exp = true;
        s_current_snap.larkin.e_exp = atof(args[15]);
    } else {
        s_current_snap.larkin.has_e_exp = false;
    }
}

void STM32UARTBridge::parseInverter(InverterTelemetry &inv, char *args[], size_t count) {
    // args[0]="DATA", args[1]="GOODWE"/"SUNGROW", args[2]=seq
    // if offline: args[3]="NA"
    // if online: args[3..10] = Vpv,Ipv,Ppv,Energy,Pout,Pload,Status,Temp
    if (count < 4) return;
    uint32_t seq = strtoul(args[2], nullptr, 10);
    s_current_snap.seq = seq;

    if (strcmp(args[3], "NA") == 0) {
        inv.online = false;
        return;
    }

    if (count < 11) return;
    inv.online = true;

    auto parseField = [](const char *str, bool &hasVal, float &val) {
        if (strcmp(str, "NA") != 0) {
            hasVal = true;
            val = atof(str);
        } else {
            hasVal = false;
            val = 0.0f;
        }
    };

    parseField(args[3], inv.has_vpv, inv.vpv);
    parseField(args[4], inv.has_ipv, inv.ipv);
    parseField(args[5], inv.has_ppv, inv.ppv);
    parseField(args[6], inv.has_energy, inv.energy_kwh);
    parseField(args[7], inv.has_pout, inv.pout);
    parseField(args[8], inv.has_pload, inv.pload);

    if (strcmp(args[9], "NA") != 0) {
        inv.has_status = true;
        strlcpy(inv.status, args[9], sizeof(inv.status));
    } else {
        inv.has_status = false;
        strcpy(inv.status, "unknown");
    }

    parseField(args[10], inv.has_temp, inv.temp);
}

static void parseFloat(const char *s, bool &valid, float &value) {
    valid = strcmp(s, "NA") != 0;
    value = valid ? atof(s) : 0.0f;
}

static void parseU32(const char *s, bool &valid, uint32_t &value) {
    valid = strcmp(s, "NA") != 0;
    value = valid ? strtoul(s, nullptr, 10) : 0U;
}

static void parseText(const char *s, bool &valid, char *value, size_t cap) {
    valid = strcmp(s, "NA") != 0;
    strlcpy(value, valid ? s : "", cap);
}

void STM32UARTBridge::parseGoodWeGroup(const char *group, char *a[], size_t n) {
    GoodWeTelemetry &g = s_current_snap.goodwe;
    if (n < 4) return;
    s_current_snap.seq = strtoul(a[2], nullptr, 10);
    if (strcmp(group, "GWID") == 0 && n >= 5) {
        parseText(a[3], g.has_device_serial, g.device_serial, sizeof(g.device_serial));
        parseText(a[4], g.has_device_type, g.device_type, sizeof(g.device_type));
    } else if (strcmp(group, "GWSTATUS") == 0 && n >= 8) {
        parseU32(a[3], g.has_error_code, g.error_code);
        parseU32(a[4], g.has_operating_hours, g.operating_hours_total);
        parseText(a[5], g.has_working_status, g.working_status, sizeof(g.working_status));
        parseText(a[6], g.has_grid_connection_status, g.grid_connection_status, sizeof(g.grid_connection_status));
        parseFloat(a[7], g.has_inverter_temperature, g.inverter_temperature);
    } else if (strcmp(group, "GWDC") == 0 && n >= 12) {
        parseFloat(a[3],g.has_mppt1_voltage,g.mppt1_voltage); parseFloat(a[4],g.has_mppt1_current,g.mppt1_current);
        parseFloat(a[5],g.has_mppt1_power,g.mppt1_power); parseFloat(a[6],g.has_mppt2_voltage,g.mppt2_voltage);
        parseFloat(a[7],g.has_mppt2_current,g.mppt2_current); parseFloat(a[8],g.has_mppt2_power,g.mppt2_power);
        parseFloat(a[9],g.has_pv_power_total,g.pv_power_total); parseFloat(a[10],g.has_insulation_resistance,g.insulation_resistance);
        parseFloat(a[11],g.has_leakage_current,g.leakage_current);
    } else if (strcmp(group, "GWAC1") == 0 && n >= 12) {
        parseFloat(a[3],g.has_active_power,g.active_power); parseFloat(a[4],g.has_reactive_power,g.reactive_power);
        parseFloat(a[5],g.has_power_factor,g.power_factor); parseFloat(a[6],g.has_phase_a_voltage,g.phase_a_voltage);
        parseFloat(a[7],g.has_phase_b_voltage,g.phase_b_voltage); parseFloat(a[8],g.has_phase_c_voltage,g.phase_c_voltage);
        parseFloat(a[9],g.has_phase_a_current,g.phase_a_current); parseFloat(a[10],g.has_phase_b_current,g.phase_b_current);
        parseFloat(a[11],g.has_phase_c_current,g.phase_c_current);
    } else if (strcmp(group, "GWAC2") == 0 && n >= 10) {
        parseFloat(a[3],g.has_phase_a_frequency,g.phase_a_frequency); parseFloat(a[4],g.has_phase_b_frequency,g.phase_b_frequency);
        parseFloat(a[5],g.has_phase_c_frequency,g.phase_c_frequency); parseFloat(a[6],g.has_grid_frequency,g.grid_frequency);
        parseFloat(a[7],g.has_line_ab_voltage,g.line_ab_voltage); parseFloat(a[8],g.has_line_bc_voltage,g.line_bc_voltage);
        parseFloat(a[9],g.has_line_ca_voltage,g.line_ca_voltage);
    } else if (strcmp(group, "GWENERGY") == 0 && n >= 5) {
        parseFloat(a[3],g.has_energy_total,g.energy_total); parseFloat(a[4],g.has_energy_today,g.energy_today);
    }
    g.online = g.has_device_serial || g.has_error_code || g.has_active_power;
}

void STM32UARTBridge::parseRelay(char *args[], size_t count) {
    // args[0]="DATA", args[1]="RELAY", args[2]=seq, args[3..6]=r1..r4
    if (count < 7) return;
    s_current_snap.relay.valid = true;
    s_current_snap.relay.r1 = atoi(args[3]);
    s_current_snap.relay.r2 = atoi(args[4]);
    s_current_snap.relay.r3 = atoi(args[5]);
    s_current_snap.relay.r4 = atoi(args[6]);

    s_latest_relay = s_current_snap.relay;
}

void STM32UARTBridge::handleLine(char *line) {
    char *star = strrchr(line, '*');
    if (!star || strlen(star) != 3) {
        return;
    }

    uint8_t rx_sum = (uint8_t)strtoul(star + 1, nullptr, 16);
    *star = '\0';
    uint8_t calc_sum = calcChecksum(line);

    if (rx_sum != calc_sum) {
        Serial.printf("[STM32-RX] Checksum error (calc: 0x%02X, got: 0x%02X): %s\r\n",
                      calc_sum, rx_sum, line);
        return;
    }

    Serial.printf("[STM32-RX] %s\r\n", line);

    char *tokens[24];
    size_t count = 0;
    char *p = line;
    while (count < 24) {
        tokens[count++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }

    if (count < 2) return;

    if (strcmp(tokens[0], "DATA") == 0) {
        if (strcmp(tokens[1], "BEGIN") == 0 && count >= 5) {
            memset(&s_current_snap, 0, sizeof(s_current_snap));
            s_current_snap.seq = strtoul(tokens[2], nullptr, 10);
            s_current_snap.epoch = strtoul(tokens[3], nullptr, 10);
            s_current_snap.replayed = atoi(tokens[4]) != 0;
        } else if (strcmp(tokens[1], "LARKIN") == 0) {
            parseLarkin(tokens, count);
        } else if (strncmp(tokens[1], "GW", 2) == 0 && strcmp(tokens[1], "GWHEALTH") != 0) {
            parseGoodWeGroup(tokens[1], tokens, count);
        } else if (strcmp(tokens[1], "GWHEALTH") == 0 && count >= 6) {
            s_current_snap.gateway.valid = strcmp(tokens[3], "NA") != 0;
            if (s_current_snap.gateway.valid) {
                s_current_snap.gateway.rssi = atoi(tokens[3]);
                s_current_snap.gateway.uptime = strtoul(tokens[4], nullptr, 10);
                strlcpy(s_current_snap.gateway.fw_version,
                        strcmp(tokens[5], "NA") != 0 ? tokens[5] : "",
                        sizeof(s_current_snap.gateway.fw_version));
            }
        } else if (strcmp(tokens[1], "SUNGROW") == 0) {
            parseInverter(s_current_snap.sungrow, tokens, count);
        } else if (strcmp(tokens[1], "RELAY") == 0) {
            parseRelay(tokens, count);
        } else if (strcmp(tokens[1], "END") == 0) {
            s_current_snap.complete = true;
            s_current_snap.received_tick = millis();
            s_latest_snap = s_current_snap;
            if (s_snapshot_cb) {
                s_snapshot_cb(s_latest_snap);
            }
            memset(&s_current_snap, 0, sizeof(s_current_snap));
        }
    } else if (strcmp(tokens[0], "ACK") == 0 && strcmp(tokens[1], "RELAY") == 0) {
        // ACK,RELAY,<ch>,<state>
        if (count >= 4) {
            uint8_t ch = atoi(tokens[2]);
            uint8_t state = atoi(tokens[3]);
            if (ch == 1) s_latest_relay.r1 = state;
            else if (ch == 2) s_latest_relay.r2 = state;
            else if (ch == 3) s_latest_relay.r3 = state;
            else if (ch == 4) s_latest_relay.r4 = state;
            s_latest_relay.valid = true;

            if (s_relay_ack_cb) {
                s_relay_ack_cb(ch, state);
            }
        }
    } else if (strcmp(tokens[0], "ACK") == 0 && strcmp(tokens[1], "PENDING") == 0 &&
               count >= 3 && strcmp(tokens[2], "NONE") == 0) {
        if (s_pending_empty_cb) s_pending_empty_cb();
    } else if (strcmp(tokens[0], "ACK") == 0 && strcmp(tokens[1], "TIME") == 0) {
        s_time_acked = true;
        Serial.println("[STM32-RX] Time synchronization confirmed by STM32 ($ACK,TIME)");
    } else if (strcmp(tokens[0], "REQ") == 0 && strcmp(tokens[1], "TIME") == 0) {
        // STM32 has no valid time (boot/reset): resend it immediately
        s_time_acked = false;
        Serial.println("[STM32-RX] STM32 requests time ($REQ,TIME)");
        if (s_time_req_cb) {
            s_time_req_cb();
        }
    }
}

void STM32UARTBridge::process() {
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (c == '$') {
            s_in_frame = true;
            s_rx_len = 0;
        } else if (!s_in_frame || c == '\r') {
            continue;
        } else if (c == '\n') {
            s_rx_buf[s_rx_len] = '\0';
            s_in_frame = false;
            handleLine(s_rx_buf);
        } else if (s_rx_len < sizeof(s_rx_buf) - 1) {
            s_rx_buf[s_rx_len++] = c;
        } else {
            s_in_frame = false; // Overflow
        }
    }
}

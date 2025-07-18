// DG-Lab estim box support.
// References:
// https://rezreal.github.io/coyote/web-bluetooth-example.html
// https://github.com/OpenDGLab/OpenDGLab-Connect/blob/master/src/services/DGLab.js

#include <functional>
#include <NimBLEDevice.h>

#include "coyote.hpp"
#include "coyote-modes.h"
#include <esp_log.h>
#include <map>
#include <freertos/timers.h>

// Coyote2
NimBLEUUID COYOTE_SERVICE_BLEUUID("955a180b-0fe2-f5aa-a094-84b8d4f3e8ad");
NimBLEUUID CONFIG_CHAR_BLEUUID("955a1507-0fe2-f5aa-a094-84b8d4f3e8ad");
NimBLEUUID POWER_CHAR_BLEUUID("955a1504-0fe2-f5aa-a094-84b8d4f3e8ad");
NimBLEUUID A_CHAR_BLEUUID("955a1506-0fe2-f5aa-a094-84b8d4f3e8ad");
NimBLEUUID B_CHAR_BLEUUID("955a1505-0fe2-f5aa-a094-84b8d4f3e8ad");
NimBLEUUID BATTERY_SERVICE_BLEUUID("955a180a-0fe2-f5aa-a094-84b8d4f3e8ad");
NimBLEUUID BATTERY_CHAR_BLEUUID("955a1500-0fe2-f5aa-a094-84b8d4f3e8ad");

// Coyote3
NimBLEUUID COYOTE3_SERVICE_BLEUUID("180c");
NimBLEUUID COYOTE3_TX_BLEUUID("150a");
NimBLEUUID COYOTE3_NOTIFY_BLEUUID("150b");
NimBLEUUID COYOTE3_BATSERVICE_BLEUUID("180a");
NimBLEUUID COYOTE3_BAT_BLEUUID("1500");

std::map<TimerHandle_t, Coyote*> coyote_timer_map;

CoyoteChannel::CoyoteChannel(Coyote& c, std::string name) : parent(c), channel_name(name) {}

bool Coyote::is_coyote(NimBLEAdvertisedDevice* advertisedDevice) {
  auto ble_manufacturer_specific_data = advertisedDevice->getManufacturerData();
  if (ble_manufacturer_specific_data.length() > 2 && ble_manufacturer_specific_data[1] == 0x19 && ble_manufacturer_specific_data[0] == 0x96) {
    ESP_LOGI("coyote", "Found Coyote 2");
    return true;
  }
  if (!strcmp(advertisedDevice->getName().c_str(),"47L121000")) {
    ESP_LOGI("coyote", "Found Coyote 3");
    return true;
  }
  return false;
}

bool Coyote::get_isconnected() {
  return coyote_connected;
}

// go up or down in steps of 1%, but never above 70% of max
void CoyoteChannel::put_power_diff(short change) {
    ESP_LOGD("coyote", "called put_power_diff %d", change);
  if (change > 5) return;  // max 5% in one go, sanity check
  int pc = (parent.coyote_maxPower / 100);
  int max = parent.coyote_maxPower;
  if (change < 0 && coyote_power_wanted < -change * pc)
    return;
  coyote_power_wanted += change * pc;
  if (coyote_power_wanted > max) {
    coyote_power_wanted = max;
  }
}

int CoyoteChannel::get_power_pc() const {
  return int((coyote_power * 100 / parent.coyote_maxPower));
}

// go to a specific percentage. Be careful about this...
void CoyoteChannel::put_power_pc(short power_percent) {
  ESP_LOGD("coyote", "called put_power_pc %d", power_percent);
  if (power_percent<0 || power_percent > 100)
    return;
  
  coyote_power_wanted = (parent.coyote_maxPower * power_percent)/100;

  if (coyote_power_wanted > parent.coyote_maxPower) {
    coyote_power_wanted = parent.coyote_maxPower;
  }
}

uint8_t Coyote::get_batterylevel() {
  return coyote_batterylevel;
}

void CoyoteChannel::put_setmode(coyote_mode mode) {
  ESP_LOGD("coyote", "called put_setmode %d", mode);
  wavemode = mode;
  wavemode_changed = true;

  switch ( wavemode ) {
    case M_BREATH:
      wanted_mode_function = coyote_mode_breath;
      break;
    case M_WAVES:
      wanted_mode_function = coyote_mode_waves;
      break;
    case M_NONE:
    case M_CUSTOM:
      wanted_mode_function = coyote_mode_nothing;
      break;
  }

  ESP_LOGD("coyote", "Set WaveMode %s %d", channel_name.c_str(), mode);
}

void CoyoteChannel::put_setmode(coyote_mode_function f) {
  ESP_LOGD("coyote", "called put_setmode function");
  wavemode = M_CUSTOM;
  wanted_mode_function = std::move(f);
  wavemode_changed = true;
}

void Coyote::set_callback(coyote_callback c) {
  update_callback = c;
}

void Coyote::notify (coyote_type_of_change change) {
  if ( update_callback )
    update_callback(change);
}

void Coyote::parse_power(const std::vector<uint8_t> buf) {
  // notify/write: 3 bytes: flipFirstAndThirdByte(zero(2) ~ uint(11).as("powerLevelB") ~uint(11).as("powerLevelA")
  channel_a.coyote_power = (buf[2] * 256 + buf[1]) >> 3;
  channel_b.coyote_power = (buf[1] * 256 + buf[0]) & 0b0000011111111111;
  ESP_LOGD("coyote", "coyote power A=%d (%d%%) B=%d (%d%%)", channel_a.coyote_power, int(channel_a.coyote_power * 100 / coyote_maxPower), channel_b.coyote_power, int(channel_b.coyote_power * 100 / coyote_maxPower));
}

std::vector<uint8_t> Coyote::encode_power(int xpowerA, int xpowerB) {
  std::vector<uint8_t> buf(3);
  // notify/write: 3 bytes: flipFirstAndThirdByte(zero(2) ~ uint(11).as("powerLevelB") ~uint(11).as("powerLevelA")
  buf[2] = (xpowerA & 0b11111100000) >> 5;
  buf[1] = (xpowerA & 0b11111) << 3 | (xpowerB & 0b11100000000) >> 8;
  buf[0] = xpowerB & 0xff;
  ESP_LOGD("coyote", "coyote power A=%d B=%d", xpowerA, xpowerB);
  return buf;
}

std::vector<uint8_t> Coyote::encode_pattern(coyote_pattern pattern) {
  std::vector<uint8_t> buf(3);
  // flipFirstAndThirdByte(zero(4) ~ uint(5).as("az") ~ uint(10).as("ay") ~ uint(5).as("ax"))
  buf[2] = (pattern.amplitude/4 & 0b11111) >> 1;
  buf[1] = ((pattern.amplitude/4 & 0b1) * 128) | ((pattern.pause_length & 0b1111111000) >> 3);
  buf[0] = (pattern.pulse_length & 0b11111) | ((pattern.pause_length & 0b111) << 5);
  //Serial.printf("encode pattern %02x %02x %02x ax=%d ay=%d az=%d\n", buf[2],buf[1],buf[0],ax,ay,az);
  return buf;
}

coyote_pattern Coyote::parse_pattern(const std::vector<uint8_t> buf) {
  coyote_pattern out;
  // flipFirstAndThirdByte(zero(4) ~ uint(5).as("az") ~ uint(10).as("ay") ~ uint(5).as("ax"))
  out.amplitude = 4 * (((buf[2] * 256 + buf[1]) & 0b0000111110000000) >> 7);
  out.pause_length = ((buf[2] * 256 * 256 + buf[1] * 256 + buf[0]) & 0b000000000111111111100000) >> 5;
  out.pulse_length = (buf[0]) & 0b11111;
  //Serial.printf("parse pattern %02x %02x %02x ax=%d ay=%d az=%d\n", buf[2],buf[1],buf[0],*ax,*ay,*az);
  return out;
}

void coyote_timer_callback(TimerHandle_t xTimerID) {
  try {
    auto instance = coyote_timer_map.at(xTimerID);
    instance->timer_callback(xTimerID);
  } catch ( const std::out_of_range& e ) {
    ESP_LOGE("coyote", "Could not find timer callback instance");
  }
}

void CoyoteChannel::update_pattern() {
  if ((wavemode == M_NONE || waveclock == 0) && (wavemode_changed)) {
    wavemode_changed = false;
    mode_function = wanted_mode_function;
    waveclock = 0;
    cyclecount = 0;
    parent.notify(C_WAVEMODE_A);
  }

  if (coyote_power > 0 ) {
    pattern = mode_function(waveclock, cyclecount);
  }
}

// This is called every 100mS to provide the coyote box with what to do next
//
void Coyote::timer_callback(TimerHandle_t xTimerID) {
  if (!coyote_connected)
    return;

  if (coyote_version == 3) {
    std::vector<uint8_t> buf(20);

    // 0xB0 (1-byte command HEAD) + serial number (4 bits) + intensity value interpretation method (4 bits) + A channel intensity setting value (1 byte) + B channel intensity setting value (1 byte)
    // + A channel waveform frequency 4 lines (4 bytes) + A channel waveform intensity 4 lines (4 bytes) + B channel waveform frequency 4 lines (4 bytes) + B channel waveform intensity 4 lines (4 bytes)

    buf[0] = 0xb0;

    // The 4 bits of intensity value interpretation are divided into two parts. The upper two bits represent the interpretation of channel A, and the lower two bits represent the interpretation of channel B.
    // 0b11 -> represents that the intensity of the corresponding channel changes absolutely. If the intensity setting value of channel A is 32, then the intensity of channel A is set to 32.

    buf[1] = 0xf;

    // The channel strength setting value is 1 byte long, with a valid range of (0~200). Values outside the input range are treated as 0. The absolute range of the strength of each channel of the Coyote host is also (0~200).

    buf[2] = channel_a.coyote_power_wanted;
    buf[3] = channel_b.coyote_power_wanted;
    channel_a.coyote_power = channel_a.coyote_power_wanted; // since we have to send it all the time anyway, no need to get this by the callback
    channel_b.coyote_power = channel_b.coyote_power_wanted;

    for (uint8_t p=0;p<4;p++) {
      channel_a.update_pattern();
      channel_b.update_pattern();
      buf[4+p] = channel_a.pattern.frequency;
      buf[8+p] = channel_a.pattern.amplitude;
      buf[12+p] = channel_b.pattern.frequency;
      buf[16+p] = channel_b.pattern.amplitude;
    }

    // The channel waveform frequency length is 1 byte, the value range is (10~240), and the channel waveform intensity is 1 byte, the value range is (0~100). In the B0 instruction, 4 sets of
    // waveform frequencies and waveform intensity are sent to each of the two channels every 100ms. Each set of frequency-intensity represents a 25ms waveform output, and 4 sets of data represent 100ms data.
    // In the waveform data, if the input value of a channel is not in the valid range, the pulse host will abandon all 4 sets of data of the channel.

    // Tips If you only want to output a waveform to a single channel, enter at least one non-valid data (a waveform intensity value greater than 100) into the data of another channel, as shown in the example above.

    //ESP_LOGD("coyote", "write: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
    //     buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9],
    //     buf[10], buf[11], buf[12], buf[13], buf[14], buf[15], buf[16], buf[17], buf[18], buf[19]);

    if (!coyote3txCharacteristic->writeValue(buf))
      ESP_LOGE("coyote", "Failed to write pattern");

    return;
  }

  // this sets the power to where we want it (also stop you changing it on the rocker switches)
  if (channel_a.coyote_power != channel_a.coyote_power_wanted || channel_b.coyote_power != channel_b.coyote_power_wanted) {
    auto enc = encode_power(channel_a.coyote_power_wanted, channel_b.coyote_power_wanted);
    if (!powerCharacteristic->writeValue(enc))
      ESP_LOGE("coyote", "Failed to write power %u %u %u", enc[0], enc[1], enc[2]);
  }
  // Do WaveA
  channel_a.update_pattern();
  if ( channel_a.coyote_power > 0 && channel_a.wavemode != M_NONE ) {
    auto enc = encode_pattern(channel_a.pattern);
    if (!patternACharacteristic->writeValue(enc))
      ESP_LOGE("coyote", "Failed to write pattern A");
  }
  channel_a.update_pattern(); // coyote 3 sends 3 times as much data
  channel_a.update_pattern();
  channel_a.update_pattern();

  // Do WaveB
  channel_b.update_pattern();
  if ( channel_b.coyote_power > 0 && channel_b.wavemode != M_NONE ) {
    auto enc = encode_pattern(channel_b.pattern);
    if (!patternBCharacteristic->writeValue(enc))
      ESP_LOGE("coyote", "Failed to write pattern B");
  }
  channel_b.update_pattern(); // coyote 3 sends 3 times as much data
  channel_b.update_pattern();
  channel_b.update_pattern();

}

void Coyote::connected_callback() {
    ESP_LOGI("coyote", "Client onConnect");
}

void Coyote::disconnected_callback(int reason) {
    coyote_connected = false;
    xTimerStop(coyoteTimer, 0);
    ESP_LOGI("coyote", "Client onDisconnect reason: %d", reason);
    notify(C_DISCONNECTED);
}

class CoyoteNimBLEClientCallback : public NimBLEClientCallbacks {
public:
  CoyoteNimBLEClientCallback(Coyote* instance) {
    coyote_instance = instance;
  }

  void onConnect(NimBLEClient* pclient) {
    coyote_instance->connected_callback();
  }

  // arduino
  void onDisconnect(NimBLEClient* pclient) {
    coyote_instance->disconnected_callback(0);
  }

  // esp-idf
  void onDisconnect(NimBLEClient* pclient, int reason) {
    coyote_instance->disconnected_callback(reason);
  }

private:
  Coyote* coyote_instance;
};

bool Coyote::getService(NimBLERemoteService*& service, NimBLEUUID uuid) {
  ESP_LOGD("coyote", "Getting service '%s'", uuid.toString().c_str());
  service = bleClient->getService(uuid);
  if (service == nullptr) {
    ESP_LOGE("coyote", "Failed to find service UUID: %s", uuid.toString().c_str());
    return false;
  }
  return true;
}

bool getCharacteristic(NimBLERemoteService* service, NimBLERemoteCharacteristic*& c, NimBLEUUID uuid, notify_callback notifyCallback = nullptr) {
  ESP_LOGD("coyote", "Getting characteristic %s", uuid.toString().c_str());
  c = service->getCharacteristic(uuid);
  if (c == nullptr) {
    ESP_LOGE("coyote", "Failed to find characteristic UUID: %s", uuid.toString().c_str());
    return false;
  }

  if (!notifyCallback)
    return true;

  // we want notifications
  if (c->canNotify() && c->subscribe(true, notifyCallback))
    return true;
  else {
    ESP_LOGE("coyote", "Failed to register for notifications for characteristic UUID: %s", uuid.toString().c_str());
    return false;
  }
}

void Coyote::batterylevel_callback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t length, bool isNotify) {
  coyote_batterylevel = data[0];
  ESP_LOGD("coypte", "coyote batterycb: %d", coyote_batterylevel);
}

void Coyote::power_callback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t length, bool isNotify) {
  if (length >= 3) {
    parse_power({data, data+length});
    notify(C_POWER);
  }
  else
    ESP_LOGE("coyote", "Power callback with incorrect length");
}

void Coyote::coyote3_callback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t length, bool isNotify) {
  if (length == 4 ) {
    //ESP_LOGD("coyote","coyote3 callback  %x %x %x %x",data[0], data[1],data[2],data[3]);  // b1 (serial) A power B power
  }
}


Coyote::Coyote() : channel_a(*this, "a"), channel_b(*this, "b") {
}

Coyote::~Coyote() {
  if ( coyoteTimer ) {
    coyote_timer_map.erase(coyoteTimer);
    xTimerDelete(coyoteTimer, 100);
  }
  bleClient->deleteServices(); // deletes all services, which should delete all characteristics
  NimBLEDevice::deleteClient(bleClient); // will also disconnect
}

bool Coyote::connect_to_device(NimBLEAdvertisedDevice* coyote_device) {
  if (coyote_connected)
    return false;

  if (!strcmp(coyote_device->getName().c_str(),"47L121000")) {
    coyote_version = 3;
  } else {
    coyote_version = 2;
  }

  if (!bleClient) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new CoyoteNimBLEClientCallback(this));
  }

  notify(C_CONNECTING);
  bool res = true;

  if (connection_fully_established && coyote_device->getAddress() == bleClient->getPeerAddress()) {
    // reconnect
    ESP_LOGI("coyote", "trying reconnect...");
    if (!bleClient->connect(false)) {
      ESP_LOGE("coyote", "reconnect failed");
      return false;
    }
    ESP_LOGI("coyote", "reconnect finished");
  } else {
    connection_fully_established = false;
    ESP_LOGI("coyote", "Will try to connect to Coyote %d at %s", coyote_version, coyote_device->getAddress().toString().c_str());
    if (!bleClient->connect(coyote_device)) {
      ESP_LOGE("coyote", "Connection failed");
      return false;
    }
    ESP_LOGI("coyote", "Connection established");

    if (coyote_version == 2) {
      res &= getService(coyoteService, COYOTE_SERVICE_BLEUUID);
      res &= getService(batteryService, BATTERY_SERVICE_BLEUUID);
    } else {
      res &= getService(coyoteService, COYOTE3_SERVICE_BLEUUID);
      res &= getService(batteryService, COYOTE3_BATSERVICE_BLEUUID);
    }


    if (res == false) {
      ESP_LOGE("coyote", "Missing service");
      bleClient->disconnect();
      return false;
    }
  }
  
  if (coyote_version == 2) {
  // call when reconnecting to re-attach notifications
    res &= getCharacteristic(batteryService, batteryLevelCharacteristic, BATTERY_CHAR_BLEUUID, std::bind(&Coyote::batterylevel_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
    res &= getCharacteristic(coyoteService, configCharacteristic, CONFIG_CHAR_BLEUUID);
    res &= getCharacteristic(coyoteService, powerCharacteristic, POWER_CHAR_BLEUUID, std::bind(&Coyote::power_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
    res &= getCharacteristic(coyoteService, patternACharacteristic, A_CHAR_BLEUUID);
    res &= getCharacteristic(coyoteService, patternBCharacteristic, B_CHAR_BLEUUID);
  } else {
    res &= getCharacteristic(batteryService, batteryLevelCharacteristic, COYOTE3_BAT_BLEUUID, std::bind(&Coyote::batterylevel_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
    res &= getCharacteristic(coyoteService, coyote3txCharacteristic, COYOTE3_TX_BLEUUID);
    res &= getCharacteristic(coyoteService, coyote3notifyCharacteristic, COYOTE3_NOTIFY_BLEUUID, std::bind(&Coyote::coyote3_callback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
  }

  if (res == false) {
    ESP_LOGE("coyote", "Missing characteristic");
    bleClient->disconnect();
    return false;
  }

  ESP_LOGI("coyote", "Found services and characteristics");

  coyote_connected = true;
  coyote_batterylevel = batteryLevelCharacteristic->readValue<uint8_t>();
  ESP_LOGD("coyote", "Battery level: %u", coyote_batterylevel);

  if (coyote_version ==3) {
    coyote_maxPower = 200; // always send 0-200 because below commands reduce it
    std::vector<uint8_t> buf(7);
    buf[0] = 0xbf;
    buf[1] = coyote_maxPower * (double)coyote_max_power_percent / 100;
    buf[2] = buf[1];
    buf[3] = 160; buf[4] = buf[3]; // affect freq balance
    buf[5] = 0; buf[6] = buf[5]; // affect pulse width
    if (!coyote3txCharacteristic->writeValue(buf)) {
      ESP_LOGE("coyote", "Failed to write initial levels");
    }
  // 0xBF (1-byte command HEAD) + AB channel strength soft upper limit (2 bytes) + AB channel waveform frequency balance parameter (2 bytes) + AB channel waveform strength balance parameter (2 bytes)
  }


  if (coyote_version ==2) {
    auto configData = configCharacteristic->readValue();
    coyote_maxPower = (configData[2] & 0xf) * 256 + configData[1];
    ESP_LOGD("coyote", "coyote maxPower: %d", coyote_maxPower);
    coyote_maxPower *= (double)coyote_max_power_percent/100;
    ESP_LOGD("coyote", "coyote maxPower clamped to: %d", coyote_maxPower);
    coyote_powerStep = configData[0];
    ESP_LOGD("coyote", "coyote powerStep: %d", coyote_powerStep);

    auto powerData = powerCharacteristic->readValue();
    if (powerData.size() >= 3)
      parse_power({powerData.begin(), powerData.end()});

    ESP_LOGD("coyote", "read pattern a");
    auto patternAData = patternACharacteristic->readValue();
    ESP_LOGD("coyote", "read pattern b");
    auto patternBData = patternBCharacteristic->readValue();
    if (patternAData.size() < 3 || patternBData.size() < 3) {
      ESP_LOGE("coyote", "No pattern data?");
    }
    channel_a.pattern = parse_pattern({patternAData.begin(), patternAData.end()});
    channel_b.pattern = parse_pattern({patternBData.begin(), patternBData.end()});

    auto buf = encode_power(start_power, start_power);
    if (!powerCharacteristic->writeValue(buf))
      ESP_LOGE("coyote", "Failed to write powerCharacteristic");
  }
  channel_a.wavemode = M_NONE;
  channel_b.wavemode = M_NONE;

  if (!coyoteTimer) {
    coyoteTimer = xTimerCreate(nullptr, pdMS_TO_TICKS(100), true, nullptr, coyote_timer_callback);
    coyote_timer_map[coyoteTimer] = this;
  }
  xTimerStart(coyoteTimer, 0);

  ESP_LOGI("coyote", "coyote connected");
  connection_fully_established = true;
  notify(C_CONNECTED);
  return true;
}

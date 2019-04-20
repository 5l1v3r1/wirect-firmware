#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <TimeLib.h>
#include <unordered_map>
#include <vector>

using namespace std;
extern "C" {
#include <user_interface.h>
}

#define DATA_LENGTH 112
#define DISABLE 0
#define ENABLE 1
#define TYPE_MANAGEMENT 0x00
#define TYPE_CONTROL 0x01
#define TYPE_DATA 0x02
#define SUBTYPE_PROBE_REQUEST 0x04

String deviceMAC = "";
const char *ssid = "Yey";
const char *password = "RiGhOLoG";
WiFiClient client;
HTTPClient http;

String macToStr(const uint8_t *mac) {
    String result;
    for (int i = 0; i < 6; ++i) {
        char buf[3];
        sprintf(buf, "%02X", mac[i]);
        result += buf;
        if (i < 5)
            result += ':';
    }
    return result;
}
typedef struct Packet {
    String MAC;
    time_t timestamp;
    float RSSI;
    String selfMAC;
} Packet;

vector<Packet> sniffedPackets;
unordered_map<string, Packet> sweepMap;
struct RxControl {
    signed rssi : 8;  // signal intensity of packet
    unsigned rate : 4;
    unsigned is_group : 1;
    unsigned : 1;
    unsigned sig_mode : 2;        // 0:is 11n packet; 1:is not 11n packet;
    unsigned legacy_length : 12;  // if not 11n packet, shows length of packet.
    unsigned damatch0 : 1;
    unsigned damatch1 : 1;
    unsigned bssidmatch0 : 1;
    unsigned bssidmatch1 : 1;
    unsigned MCS : 7;         // if is 11n packet, shows the modulation and code used (range from 0 to 76)
    unsigned CWB : 1;         // if is 11n packet, shows if is HT40 packet or not
    unsigned HT_length : 16;  // if is 11n packet, shows length of packet.
    unsigned Smoothing : 1;
    unsigned Not_Sounding : 1;
    unsigned : 1;
    unsigned Aggregation : 1;
    unsigned STBC : 2;
    unsigned FEC_CODING : 1;  // if is 11n packet, shows if is LDPC packet or not.
    unsigned SGI : 1;
    unsigned rxend_state : 8;
    unsigned ampdu_cnt : 8;
    unsigned channel : 4;  //which channel this packet in.
    unsigned : 12;
};

struct SnifferPacket {
    struct RxControl rx_ctrl;
    uint8_t data[DATA_LENGTH];
    uint16_t cnt;
    uint16_t len;
};

// Declare each custom function (excluding built-in, such as setup and loop) before it will be called.
// https://docs.platformio.org/en/latest/faq.html#convert-arduino-file-to-c-manually
static void showMetadata(SnifferPacket *snifferPacket);
static void ICACHE_FLASH_ATTR sniffer_callback(uint8_t *buffer, uint16_t length);
static void printDataSpan(uint16_t start, uint16_t size, uint8_t *data);
static void getMAC(char *addr, uint8_t *data, uint16_t offset);
void channelHop();

static void showMetadata(SnifferPacket *snifferPacket) {
    unsigned int frameControl = ((unsigned int)snifferPacket->data[1] << 8) + snifferPacket->data[0];

    uint8_t version = (frameControl & 0b0000000000000011) >> 0;
    uint8_t frameType = (frameControl & 0b0000000000001100) >> 2;
    uint8_t frameSubType = (frameControl & 0b0000000011110000) >> 4;
    uint8_t toDS = (frameControl & 0b0000000100000000) >> 8;
    uint8_t fromDS = (frameControl & 0b0000001000000000) >> 9;

    // Only look for probe request packets
    if (frameType != TYPE_MANAGEMENT ||
        frameSubType != SUBTYPE_PROBE_REQUEST)
        return;

    Serial.print("RSSI: ");
    Serial.print(snifferPacket->rx_ctrl.rssi, DEC);

    Serial.print(" Ch: ");
    Serial.print(wifi_get_channel());

    char addr[] = "00:00:00:00:00:00";
    getMAC(addr, snifferPacket->data, 10);
    Serial.print(" Peer MAC: ");

    String str(addr);
    Serial.print(str.c_str());
    Packet sniffedPacket;
    sniffedPacket.MAC = str;
    sniffedPacket.RSSI = snifferPacket->rx_ctrl.rssi;
    sniffedPacket.timestamp = now();
    sniffedPacket.selfMAC = deviceMAC;

    string moc = str.c_str();
    sweepMap[moc] = sniffedPacket;

    uint8_t SSID_length = snifferPacket->data[25];
    Serial.print(" SSID: ");
    printDataSpan(26, SSID_length, snifferPacket->data);
    Serial.println();
}

/**
 * Callback for promiscuous mode
 */
static void ICACHE_FLASH_ATTR sniffer_callback(uint8_t *buffer, uint16_t length) {
    struct SnifferPacket *snifferPacket = (struct SnifferPacket *)buffer;
    showMetadata(snifferPacket);
}

static void printDataSpan(uint16_t start, uint16_t size, uint8_t *data) {
    for (uint16_t i = start; i < DATA_LENGTH && i < start + size; i++) {
        Serial.write(data[i]);
    }
}

static void getMAC(char *addr, uint8_t *data, uint16_t offset) {
    sprintf(addr, "%02x:%02x:%02x:%02x:%02x:%02x", data[offset + 0], data[offset + 1], data[offset + 2], data[offset + 3], data[offset + 4], data[offset + 5]);
}

#define CHANNEL_HOP_INTERVAL_MS 1000
static os_timer_t channelHop_timer;
static os_timer_t sendInfo_timer;
/**
 * Callback for channel hoping
 */
void channelHop() {
    // hoping channels 1-13
    uint8 new_channel = wifi_get_channel() + 1;

    if (new_channel > 13) {
        Serial.print("Sweep size : ");
        Serial.println(sweepMap.size());
        for (auto it : sweepMap) {
            sniffedPackets.push_back(it.second);
        }
        sweepMap.clear();
        Serial.print("Total sniffed : ");
        Serial.println(sniffedPackets.size());
        new_channel = 1;
    }
    wifi_set_channel(new_channel);
}
int infoFlag = 0;
void sendInfo() {
    infoFlag = 1;
}
Ticker ticker;

void promiscousSetup() {
    wifi_set_opmode(STATION_MODE);
    wifi_set_channel(1);
    wifi_promiscuous_enable(DISABLE);
    delay(10);
    wifi_set_promiscuous_rx_cb(sniffer_callback);
    delay(10);
    wifi_promiscuous_enable(ENABLE);  // setup the channel hoping callback timer
    os_timer_disarm(&channelHop_timer);

    os_timer_setfn(&channelHop_timer, (os_timer_func_t *)channelHop, NULL);
    os_timer_arm(&channelHop_timer, CHANNEL_HOP_INTERVAL_MS, 1);
}
void setup() {
    // set the WiFi chip to "promiscuous" mode aka monitor mode
    Serial.begin(115200);
    delay(10);
    sniffedPackets.reserve(500);
    promiscousSetup();
    ticker.attach(20, sendInfo);

    unsigned char mac[6];
    WiFi.macAddress(mac);
    deviceMAC += macToStr(mac);
}
void loop() {
    //Serial.println(deviceMAC);
    if (infoFlag == 1) {
        ticker.detach();
        os_timer_disarm(&channelHop_timer);
        wifi_promiscuous_enable(DISABLE);

        Serial.println("Connecting to ");
        Serial.println(ssid);
        WiFi.begin(ssid, password);
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
        Serial.println("");
        Serial.println("WiFi connected");
        http.begin("http://192.168.43.161:1323/packet");
        http.addHeader("Content-Type", "application/json");

        unsigned numberOfPackets = sniffedPackets.size();

        DynamicJsonDocument doc(numberOfPackets + 1 + (numberOfPackets * 126));
        JsonArray ar = doc.to<JsonArray>();

        DynamicJsonDocument pkt(126);

        for (int i = 0; i < sniffedPackets.size(); i++) {
            JsonObject obj = pkt.to<JsonObject>();
            Packet sniffedPacket = sniffedPackets[i];

            obj["MAC"] = sniffedPacket.MAC;
            obj["RSSI"] = sniffedPacket.RSSI;
            obj["timestamp"] = sniffedPacket.timestamp;
            obj["selfMAC"] = deviceMAC;

            ar.add(obj);
        }
        String json;
        serializeJson(ar, json);

        int httpCode = http.POST(json);
        http.end();

        sniffedPackets.clear();

        Serial.println(httpCode);
        infoFlag = 0;
        ticker.attach(20, sendInfo);
        WiFi.disconnect(true);
        while (WiFi.isConnected()) {
            Serial.println("Disconnecting");
            Serial.print(".");
            delay(100);
        }
        Serial.println("Disconnected successfulyy.");

        promiscousSetup();
    }
}

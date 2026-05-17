#include "esp32-hal-gpio.h"
#include <WiFi.h>
#include <time.h>
#include <stdarg.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFiMulti.h>
#include <driver/gpio.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

WiFiMulti wifiMulti;

AsyncWebServer server(80);
using Req = AsyncWebServerRequest;

const char* ZONE_ENABLE  = "^\\/api\\/zones\\/(\\d+)\\/enable$";
const char* ZONE_DISABLE = "^\\/api\\/zones\\/(\\d+)\\/disable$";

const int DC_OK = 4;

bool pwrOk24V() {
	return digitalRead(DC_OK) == LOW;
}

enum LEDMode { ON, OFF, FLASH };

struct StatLED {
	int pin;
	int onTime;
	int cycleTime;
	LEDMode mode;
	unsigned long start;

	void on() { mode = ON; }
	void off() { mode = OFF; }
	void flash() { mode = FLASH; start = millis(); }

	void update(int newOnTime, int newCycleTime) {
		onTime = newOnTime;
		cycleTime = newCycleTime;
	}

	void tick() {
		unsigned long now = millis();
		int status = digitalRead(pin);
		switch (mode) {
			case ON:
				if (status == LOW) digitalWrite(pin, HIGH);
				break;
			case OFF:
				if (status == HIGH) digitalWrite(pin, LOW);
				break;
			case FLASH: {
				unsigned long runtime = now - start;
				if (runtime >= cycleTime && status == LOW) {
					digitalWrite(pin, HIGH);
					start = now;
				} else if (runtime >= onTime && status == HIGH) {
					digitalWrite(pin, LOW);
				}
				break;
			}
		}
	}
};

struct Schedule {
	bool days[7];
	int startHour;
	int startMin;
	int stopHour;
	int stopMin;
};

struct Zone {
	int pin;
	bool running;
	const char* name;
	time_t startTime;
	Schedule schedule;
	StatLED LED;
};

const Schedule DEFAULT_SCHEDULE = {
	.days  = { false, false, false, false, false, false, false },
	.startHour = 6, .startMin = 0, .stopHour  = 7, .stopMin  = 15
};

StatLED Ok = { 14, 1000, 4000, OFF, 0 };
StatLED Warn = { 13, 100, 400, FLASH, 0 };

Zone zones[3] = {
	{ .pin=16, .name="Garden faucet", .schedule=DEFAULT_SCHEDULE,
		.LED={ .pin=25, .onTime=200, .cycleTime=800 }, },
	{ .pin=17, .name="House faucet",  .schedule=DEFAULT_SCHEDULE,
		.LED={ .pin=26, .onTime=200, .cycleTime=800 }, },
	{ .pin=18, .name="Shed faucet",   .schedule=DEFAULT_SCHEDULE,
		.LED={ .pin=27, .onTime=200, .cycleTime=800 }, },
};


// Accessory functions --------------------------------------------------------
String getStateJSON() {
	JsonDocument doc;
	JsonArray zonesArr = doc["zones"].to<JsonArray>();

	for (int i = 0; i < 3; i++) {
		Zone& z = zones[i];
		JsonObject obj = zonesArr.add<JsonObject>();
		obj["id"] = i;
		obj["name"] = z.name;
		obj["running"] = z.running;
		if (z.running && z.startTime > 0) {
			char buf[6];
			struct tm* t = localtime(&z.startTime);
			strftime(buf, sizeof(buf), "%H:%M", t);
			obj["since"] = buf;
		} else {
			obj["since"] = nullptr;
		}
	}

	String output;
	serializeJson(doc, output);
	return output;
}

void saveSchedule() {
	JsonDocument doc;
	JsonArray arr = doc["zones"].to<JsonArray>();

	for (int i = 0; i < 3; i++) {
		JsonObject obj = arr.add<JsonObject>();
		obj["id"] = i;
		JsonArray days = obj["days"].to<JsonArray>();

		for (int d = 0; d < 7; d++) {
			days.add(zones[i].schedule.days[d]);
		}
		obj["startHour"] = zones[i].schedule.startHour;
		obj["startMin"] = zones[i].schedule.startMin;
		obj["stopHour"] = zones[i].schedule.stopHour;
		obj["stopMin"] = zones[i].schedule.stopMin;
	}

	File f = LittleFS.open("/save.json", "w");
	serializeJson(doc, f);
	f.close();
}

void loadSchedule() {
	if (!LittleFS.exists("/save.json")) {
		Serial.println("No save.json found - using empty defaults");
		return;
	}
	JsonDocument doc; File f = LittleFS.open("/save.json", "r");
	DeserializationError err = deserializeJson(doc, f); f.close();
	if (err) {
		Serial.println("Failed to parse save.json");
		return;
	}

	for (int i = 0; i < 3; i++) {
		for (int d = 0; d < 7; d++ ) {
			zones[i].schedule.days[d] = doc["zones"][i]["days"][d];
		}
		zones[i].schedule.startHour = doc["zones"][i]["startHour"];
		zones[i].schedule.startMin = doc["zones"][i]["startMin"];
		zones[i].schedule.stopHour = doc["zones"][i]["stopHour"];
		zones[i].schedule.stopMin = doc["zones"][i]["stopMin"];
	}
}

const char* timestamp() {
	static char buf[16];
	struct tm t;
	getLocalTime(&t);
	strftime(buf, sizeof(buf), "%m-%d %H:%M:%S", &t);
	return buf;
}

void logger(const char* fmt, ...) {
	char buf[128];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	Serial.printf("%s %s\n", timestamp(), buf);
}


// FreeRTOS Tasks -------------------------------------------------------------
void ledTask(void*) {
	while (true) {
		Ok.tick();
		Warn.tick();
		for (int i = 0; i < 3; i++) {
			// could do a check for problems and flash light if any exist?
			zones[i].LED.mode = zones[i].running ? ON : OFF;
			zones[i].LED.tick();
		}
		vTaskDelay(50 / portTICK_PERIOD_MS);
	}
}

void powerMonitorTask(void*) {
    bool lastState = false;
    while (true) {
        bool currentState = pwrOk24V();
        if (currentState != lastState) {
            if (currentState) {
                logger("24V OK");
                Ok.on();
                Warn.off();
            } else {
                logger("24V supply fault");
                Ok.flash();
                Warn.flash();
            }
            lastState = currentState;
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

// Serial commands ------------------------------------------------------------
void cmdRSC() {
    float total    = ESP.getHeapSize()     / 1024.0f;
    float freeHeap = ESP.getFreeHeap()     / 1024.0f;
    float minFree  = ESP.getMinFreeHeap()  / 1024.0f;
    float maxBlk   = ESP.getMaxAllocHeap() / 1024.0f;
    float used     = total - freeHeap;
    float peak     = total - minFree;

    logger("[RESOURCE MONITOR]");
    Serial.printf("  Heap:     %.1f / %.1f KiB (%.1f%%)\n",
			used, total, (used/total)*100);
    Serial.printf("  Peak:     %.1f KiB used\n", peak);
    Serial.printf("  Headroom: %.1f KiB free, largest block %.1f KiB\n",
			freeHeap, maxBlk);

	// uptime
	unsigned long upSec = millis() / 1000;
	unsigned long upMin = upSec / 60;
	unsigned long upHr  = upMin / 60;
	Serial.printf("  Uptime:   %luh %lum %lus\n", 
		upHr, upMin % 60, upSec % 60);

	// LittleFS usage
	float fsUsed  = LittleFS.usedBytes()  / 1024.0f;
	float fsTotal = LittleFS.totalBytes() / 1024.0f;
	Serial.printf("  FS used:  %.2f / %.2f KiB (%.1f%%)\n",
		fsUsed, fsTotal, (fsUsed / fsTotal) * 100);
}

void commandListener() {
	if (Serial.available()) {
		String cmd = Serial.readStringUntil('\n');
		cmd.trim();
		if (cmd == "rsc") {
			cmdRSC();
		}
	}
}


// Setup Functions ------------------------------------------------------------
void initPins() {
	Serial.println("Pin setup");

	for (int i = 0; i < 3; i++) {
		digitalWrite(zones[i].pin, LOW);
		pinMode(zones[i].pin, OUTPUT);
		pinMode(zones[i].LED.pin, OUTPUT);
		zones[i].LED.off();
	}

	pinMode(Ok.pin, OUTPUT);
	pinMode(Warn.pin, OUTPUT);
	pinMode(DC_OK, INPUT_PULLUP);
}

void initWifi() {
    wifiMulti.addAP(HOUSENET, HOUSEKEY);
    wifiMulti.addAP(CGINET, CGIKEY);
	wifiMulti.addAP(PNET, PKEY);

	Serial.printf("Connecting to network...");
	unsigned long timer = 0;
	while (wifiMulti.run() != WL_CONNECTED) {
		unsigned long now = millis();
		if (now - timer >= 500) {
			Serial.print(".");
			timer = now;
		}
	}
	Serial.printf(" success, connected to %s\n", WiFi.SSID().c_str());
	Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
}

void initNTP(tm &timeinfo) {
	Serial.printf("Syncing with timeserver...");
	unsigned long timer = 0;
	const long GMT_OFFSET = -6 * 3600;
	const int DST_OFFSET  = 3600;

	configTime(GMT_OFFSET, DST_OFFSET, "pool.ntp.org");

	while (!getLocalTime(&timeinfo)) {
		unsigned long now = millis();
		if (now - timer >= 100) {
			Serial.print(".");
			timer = now;
		}
	}

	Serial.println(&timeinfo, " synced, time now: %H:%M:%S");
}

void initMDNS() {
	if (MDNS.begin("watering")) {
		logger("mDNS started: http://watering.local");
	}
}

void initLittleFS() {
	if (!LittleFS.begin()) {
		logger("LittleFS mount failed");
		return;
	}	
	logger("LittleFS mounted");
	if (!LittleFS.exists("/save.json")) {
		saveSchedule();
		logger("/save.json initialized with defaults");
	}
    loadSchedule();
}

void initAPIRouting() {
	// Basic page ---------------------------------------------------
	server.on("/", HTTP_GET, [](Req *r) {
		r->send(LittleFS, "/index.html", "text/html");
	});

	server.on("/app.js", HTTP_GET, [](Req *r) {
		r->send(LittleFS, "/app.js", "text/javascript");
	});

	server.on("/style.css", HTTP_GET, [](Req *r) {
		r->send(LittleFS, "/style.css", "text/css");
	});

	server.on("/wtr.ico", HTTP_GET, [](Req *r) {
		r->send(LittleFS, "/wtr.ico", "image/x-icon");
	});

	// Routes -------------------------------------------------------
	server.on("/api/state", HTTP_GET, [](Req *r) {
		r->send(200, "application/json", getStateJSON());
	});

	server.on("/api/schedule", HTTP_GET, [](Req *r) {
		File f = LittleFS.open("/save.json", "r");
		if (!f) {
			r->send(404, "text/plain", "not found");
			return;
		}
		String content = f.readString();
		f.close();
		r->send(200, "application/json", content);
	});

	server.on(ZONE_ENABLE, HTTP_POST, [](Req *r) {
		if (!pwrOk24V()) {
			r->send(503, "application/json", "{\"error\":\"24V not ready\"}");
			logger("Zone enable rejected - 24VDC supply fault");
			return;	
		}

		int id = r->pathArg(0).toInt();
		if (id < 0 || id > 2) {
			r->send(400, "application/json", "{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].running = true;
		time(&zones[id].startTime);
		r->send(200, "application/json", "{\"ok\":true}");
		digitalWrite(zones[id].pin, HIGH);
		logger("%s ON", zones[id].name);
	});

	server.on(ZONE_DISABLE, HTTP_POST, [](Req *r) {
		int id = r->pathArg(0).toInt();
		if (id < 0 || id > 2) {
			r->send(400, "application/json", "{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].running = false;
		zones[id].startTime = 0;
		r->send(200, "application/json", "{\"ok\":true}");
		digitalWrite(zones[id].pin, LOW);
		logger("%s OFF", zones[id].name);
	});

	server.on("/api/schedule/save", HTTP_POST,
		[](Req *r) {
			r->send(200, "application/json", "{\"ok\":true}");
		},
		nullptr,
		[](Req *r, uint8_t *data, size_t len, size_t index, size_t total) {
			JsonDocument doc;
			deserializeJson(doc, data, len);
			JsonArray arr = doc["zones"].as<JsonArray>();
			for (JsonObject zone : arr) {
				int id = zone["id"];
				if (id < 0 || id > 2) continue;
				zones[id].schedule.startHour = zone["startHour"];
				zones[id].schedule.startMin  = zone["startMin"];
				zones[id].schedule.stopHour  = zone["stopHour"];
				zones[id].schedule.stopMin   = zone["stopMin"];
				JsonArray days = zone["days"];
				for (int d = 0; d < 7; d++) {
					zones[id].schedule.days[d] = days[d];
				}
			}
			saveSchedule();
			logger("Schedule saved");
		}
	);
}


// Main: setup & loop ---------------------------------------------------------
void setup() {
	Serial.begin(115200);
	xTaskCreate(ledTask, "LEDs", 2048, nullptr, 1, nullptr);

	initPins();
	initWifi();
	struct tm timeinfo;
	initNTP(timeinfo);
	initMDNS();
	initLittleFS();
	initAPIRouting();

	server.begin();

	logger("Webserver running on %s", WiFi.localIP().toString().c_str());
	xTaskCreate(powerMonitorTask, "PWR", 2048, nullptr, 1, nullptr);
	float startup = static_cast<float>(millis()) / 1000;
	logger("Boot sequence took %.1f seconds", startup);

	Ok.on();
	Warn.off();
}

void loop() {
	commandListener();
}

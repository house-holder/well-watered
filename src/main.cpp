#include "esp32-hal-gpio.h"
#include <WiFi.h>
#include <time.h>
#include <stdarg.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <driver/gpio.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

AsyncWebServer server(80);
using Req = AsyncWebServerRequest;

const char* ZONE_ENABLE  = "^\\/api\\/zones\\/(\\d+)\\/enable$";
const char* ZONE_DISABLE = "^\\/api\\/zones\\/(\\d+)\\/disable$";

const int DC_OK = 4;

bool syncOkNTP = false;

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

StatLED Ok = { 14, 1000, 4000, OFF, 0 };
StatLED Warn = { 13, 100, 400, FLASH, 0 };

struct Schedule {
	bool days[7];
	int startHour;
	int startMin;
	int stopHour;
	int stopMin;
};

const Schedule DEFAULT_SCHEDULE = {
	.days  = { false, false, false, false, false, false, false },
	.startHour = 6, .startMin = 0, .stopHour  = 7, .stopMin  = 15
};

enum runMode { IDLE, SCHD, OVRD };

struct Zone {
	int pin;
	runMode mode;
	const char* name;
	time_t startTime;
	time_t stopTime;
	Schedule schedule;
	StatLED LED;

	bool running() { return mode != IDLE; }
};

Zone zones[3] = {
	{
		.pin=16, .mode=IDLE, .name="Garden", .schedule=DEFAULT_SCHEDULE,
		.LED={ .pin=25, .onTime=200, .cycleTime=800 },
	},
	{
		.pin=17, .mode=IDLE, .name="House", .schedule=DEFAULT_SCHEDULE,
		.LED={ .pin=26, .onTime=200, .cycleTime=800 },
	},
	{
		.pin=18, .mode=IDLE, .name="Shed", .schedule=DEFAULT_SCHEDULE,
		.LED={ .pin=27, .onTime=200, .cycleTime=800 },
	},
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
		obj["running"] = z.running();
		obj["stopTime"] = z.stopTime;
		if (z.running() && z.startTime > 0) {
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

int minutesNow(struct tm &t) {
    return t.tm_hour * 60 + t.tm_min;
}

int minutesOf(int hour, int min) {
    return hour * 60 + min;
}

bool isDayActive(Schedule &s, struct tm &t) {
    int today     = t.tm_wday;
    int yesterday = (today + 6) % 7;

    int now   = minutesNow(t);
    int start = minutesOf(s.startHour, s.startMin);
    int stop  = minutesOf(s.stopHour,  s.stopMin);

    if (start <= stop) {
        return s.days[today];
    } else {
        if (now >= start) {
            return s.days[today];
        } else {
            return s.days[yesterday];
        }
    }
}

bool shouldBeActive(Schedule &s, struct tm &t) {
    int now   = minutesNow(t);
    int start = minutesOf(s.startHour, s.startMin);
    int stop  = minutesOf(s.stopHour,  s.stopMin);

    if (start <= stop) {
        return now >= start && now < stop;
    } else {
        return now >= start || now < stop;
    }
}

bool zoneShouldBeActive(Zone &zone, struct tm &t) {
    return isDayActive(zone.schedule, t) &&
           shouldBeActive(zone.schedule, t);
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
			zones[i].LED.mode = zones[i].running() ? ON : OFF;
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

void scheduleTask(void*) {
    while (true) {
        struct tm t;
        if (getLocalTime(&t)) {
            for (int i = 0; i < 3; i++) {
                bool dayOk    = isDayActive(zones[i].schedule, t);
                bool timeOk   = shouldBeActive(zones[i].schedule, t);
                bool shouldRun = dayOk && timeOk;

				if (shouldRun && zones[i].mode == IDLE) {
					zones[i].mode = SCHD;
					time(&zones[i].startTime);
					digitalWrite(zones[i].pin, HIGH);
					logger("Schedule: %s ON", zones[i].name);
				} else if (!shouldRun && zones[i].mode == SCHD) {
					zones[i].mode = IDLE;
					zones[i].startTime = 0;
					digitalWrite(zones[i].pin, LOW);
					logger("Schedule: %s OFF", zones[i].name);
				}
            }
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS);
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
	Serial.printf("Connecting to network...");
	WiFi.begin(WNET, WKEY);
	unsigned long timer = 0;
	while (WiFi.status() != WL_CONNECTED) {
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
	syncOkNTP = true;
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

	// Basic webapp utils -----------------------------------------------------
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

	server.on(ZONE_ENABLE, HTTP_POST,
		[](Req *r) {
			if (!pwrOk24V()) {
				r->send(503, "application/json", "{\"error\":\"24V not ready\"}");
				logger("Zone enable rejected - 24VDC supply fault");
				return;
			}
			r->send(200, "application/json", "{\"ok\":true}");
		},
		nullptr,
		[](Req *r, uint8_t *data, size_t len, size_t index, size_t total) {
			if (!pwrOk24V()) return;

			int id = r->pathArg(0).toInt();
			if (id < 0 || id > 2) return;

			JsonDocument doc;
			deserializeJson(doc, data, len);
			int duration = doc["duration"] | 120;

			zones[id].mode = OVRD;
			time(&zones[id].startTime);
			zones[id].stopTime = zones[id].startTime + (duration * 60);
			digitalWrite(zones[id].pin, HIGH);
			logger("%s ON, duration=%dmin", zones[id].name, duration);
		}
	);

	server.on(ZONE_DISABLE, HTTP_POST, [](Req *r) {
		int id = r->pathArg(0).toInt();
		if (id < 0 || id > 2) {
			r->send(400, "application/json", "{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].mode = IDLE;
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

	// Device administration --------------------------------------------------
	server.on("/api/fs", HTTP_GET, [](Req *r) {
		JsonDocument doc;
		JsonArray files = doc["files"].to<JsonArray>();
		File root = LittleFS.open("/");
		File f = root.openNextFile();
		while (f) {
			JsonObject file = files.add<JsonObject>();
			file["name"] = f.name();
			file["size"] = f.size();
			f = root.openNextFile();
		}
		String out;
		serializeJson(doc, out);
		r->send(200, "application/json", out);
	});

	server.on("^\\/api\\/fs\\/delete\\/(.+)$", HTTP_POST, [](Req *r) {
		String filename = "/" + r->pathArg(0);
		if (LittleFS.remove(filename)) {
			r->send(200, "application/json", "{\"ok\":true}");
			logger("Deleted file '%s'", filename.c_str());
		} else {
			r->send(404, "application/json", "{\"error\":\"not found\"}");
		}
	});

	server.on("^\\/api\\/fs\\/clear\\/(.+)$", HTTP_POST, [](Req *r) {
		String filename = "/" + r->pathArg(0);
		File f = LittleFS.open(filename, "w");
		f.close();
		logger("Cleared file '%s'", filename.c_str());
		r->send(200, "application/json", "{\"ok\":true}");
	});
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
	xTaskCreate(scheduleTask, "Schedule", 2048, nullptr, 1, nullptr);

	Ok.on();
	Warn.off();
}

void loop() {
	commandListener();
}

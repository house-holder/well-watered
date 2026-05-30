#include "esp32-hal-gpio.h"
#include "freertos/portmacro.h"
#include <WiFi.h>
#include <time.h>
#include <vector>
#include <stdarg.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <algorithm>
#include <LittleFS.h>
#include <driver/gpio.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

void logger(const char* fmt, ...);

AsyncWebServer server(80);

using Vector = std::vector<String>;
using Req = AsyncWebServerRequest;

const int DC_OK = 4;
const char* ZONE_ENABLE  = "^\\/api\\/zones\\/(\\d+)\\/enable$";
const char* ZONE_DISABLE = "^\\/api\\/zones\\/(\\d+)\\/disable$";
const char* ZONE_PAUSE  = "^\\/api\\/zones\\/(\\d+)\\/pause$";
const char* ZONE_RESUME = "^\\/api\\/zones\\/(\\d+)\\/resume$";

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
	.days = { false, false, false, false, false, false, false },
	.startHour = 6, .startMin = 0, .stopHour  = 7, .stopMin  = 15
};

enum RunMode { IDLE, SCHD, OVRD };

struct Zone {
    int pin;
    RunMode mode;
    const char* name;
    time_t startTime;
    time_t stopTime;
    time_t pausedUntil;
    time_t skipUntil;
    Schedule schedule;
    StatLED LED;

    bool isRunning()   { return mode != IDLE; }
    bool isOverride()  { return mode == OVRD; }
    bool isScheduled() { return mode == SCHD; }
    bool isPaused()    { return pausedUntil > 0; }

    void enable(int durationMins) {
        mode = OVRD;
        time(&startTime);
        stopTime = startTime + (durationMins * 60);
        pausedUntil = 0;
        digitalWrite(pin, HIGH);
    }

    void disable() {
        mode = IDLE;
        startTime = 0;
        stopTime = 0;
        pausedUntil = 0;
        digitalWrite(pin, LOW);
    }

    void pause() {
        struct tm t;
        getLocalTime(&t);
        t.tm_hour = schedule.stopHour;
        t.tm_min  = schedule.stopMin;
        t.tm_sec  = 0;
        pausedUntil = mktime(&t);
        mode = IDLE;
        startTime = 0;
        digitalWrite(pin, LOW);
    }

    void resume() {
        pausedUntil = 0;
    }

    void scheduleStart() {
        mode = SCHD;
        time(&startTime);
        stopTime = 0;
        pausedUntil = 0;
        digitalWrite(pin, HIGH);
    }

    void scheduleStop() {
        mode = IDLE;
        startTime = 0;
        stopTime = 0;
        digitalWrite(pin, LOW);
    }
};

Zone zones[3] = {
    {	.pin=16, .mode=IDLE, .name="Garden",
        .startTime=0, .stopTime=0, .pausedUntil=0, .skipUntil=0,
        .schedule=DEFAULT_SCHEDULE,
        .LED={ .pin=25, .onTime=200, .cycleTime=800 },
    },
    {	.pin=17, .mode=IDLE, .name="House",
        .startTime=0, .stopTime=0, .pausedUntil=0, .skipUntil=0,
        .schedule=DEFAULT_SCHEDULE,
        .LED={ .pin=26, .onTime=200, .cycleTime=800 },
    },
    {	.pin=18, .mode=IDLE, .name="Shed",
        .startTime=0, .stopTime=0, .pausedUntil=0, .skipUntil=0,
        .schedule=DEFAULT_SCHEDULE,
        .LED={ .pin=27, .onTime=200, .cycleTime=800 },
    },
};


// Accessory functions --------------------------------------------------------
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
String getStateJSON() {
	JsonDocument doc;
	JsonArray zonesArr = doc["zones"].to<JsonArray>();

	for (int i = 0; i < 3; i++) {
		JsonObject obj = zonesArr.add<JsonObject>();

		Zone& z = zones[i];
		time_t nowT;
		time(&nowT);
		struct tm* tNow = localtime(&nowT);
		bool stillInWindow = shouldBeActive(z.schedule, *tNow);
		const char* modeStr = z.mode == OVRD ? "override"  :
							  z.mode == SCHD ? "scheduled" :
							  (z.isPaused() && stillInWindow) ? "paused" :
							  "idle";
		obj["id"]		   = i;
		obj["mode"]        = modeStr;
		obj["running"]	   = z.isRunning();
		obj["stopTime"]    = z.stopTime;
		obj["pausedUntil"] = z.pausedUntil;
		obj["name"] = z.name;

		if (z.isRunning() && z.startTime > 0) {
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
	static char buf[24];
	struct tm t;
	getLocalTime(&t);
	strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
	return buf;
}

const char* LOG_FILES[2]     = { "/log0.txt", "/log1.txt" };
const size_t LOG_SOFT_CUTOFF = 50 * 1024; // 50KiB
const size_t LOG_HARD_MAX    = 150 * 1024;

Preferences logPrefs;
SemaphoreHandle_t logMutex = nullptr;
uint8_t logActive = 0;

size_t fileSize(const char* path) {
	File f = LittleFS.open(path, "r");
	size_t s = f ? f.size() : 0;
	if (f) f.close();
	return s;
}

void initLog() {
	logMutex = xSemaphoreCreateMutex();
	logPrefs.begin("log", false);
	logActive = logPrefs.getUChar("active", 0);
	if (logActive > 1) logActive = 0;

	for (int i = 0; i < 2; i++) {
		if (!LittleFS.exists(LOG_FILES[i])) {
			File f = LittleFS.open(LOG_FILES[i], "w");
			if (f) f.close();
		}
	}
	logger("Log ready: active=%s", LOG_FILES[logActive]);
}

void appendLog(const char* line) {
	if (!logMutex) return;
	xSemaphoreTake(logMutex, portMAX_DELAY);

	const char* active = LOG_FILES[logActive];
	File f = LittleFS.open(active, "a");
	if (f) {
		f.print(line);
		f.print('\n');
		f.close();
	}

	size_t activeSize = fileSize(active);
	if (activeSize >= LOG_SOFT_CUTOFF) {
		const char* inactive = LOG_FILES[1 - logActive];
		size_t inactiveSize  = fileSize(inactive);

		if (inactiveSize == 0) {
			logActive = 1 - logActive;
			logPrefs.putUChar("active", logActive);
			Serial.printf("LOG ROTATE -> active=%s\n", LOG_FILES[logActive]);
		} else if (activeSize >= LOG_HARD_MAX) {
			File z = LittleFS.open(inactive, "w");
			if (z) z.close();
			logActive = 1 - logActive;
			logPrefs.putUChar("active", logActive);
			Serial.printf("LOG DROP (Pi behind) -> dropped %s, active=%s\n",
				inactive, LOG_FILES[logActive]);
		}
	}

	xSemaphoreGive(logMutex);
}

void logger(const char* fmt, ...) {
	char buf[128];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	char line[160];
	snprintf(line, sizeof(line), "%s %s", timestamp(), buf);
	Serial.println(line);
	appendLog(line);
}


// FreeRTOS Tasks -------------------------------------------------------------
void ledTask(void*) {
	while (true) {
		Ok.tick();
		Warn.tick();
		for (int i = 0; i < 3; i++) {
			zones[i].LED.mode = zones[i].isRunning() ? ON : OFF;
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
                time_t now;
                time(&now);

                if (zones[i].isPaused() && now >= zones[i].pausedUntil) {
                    zones[i].pausedUntil = 0;
                    logger("%s pause expired", zones[i].name);
                }

                bool shouldRun = zoneShouldBeActive(zones[i], t);

				if (shouldRun && zones[i].mode == IDLE && !zones[i].isPaused()) {
					zones[i].scheduleStart();
					logger("Schedule: %s ON", zones[i].name);
				} else if (!shouldRun && zones[i].mode == SCHD) {
					zones[i].scheduleStop();
					logger("Schedule: %s OFF", zones[i].name);
				}

				if (zones[i].isOverride()
					&& zones[i].stopTime > 0
					&& now >= zones[i].stopTime
				) {
					zones[i].disable();
					logger("Override: %s auto-stopped", zones[i].name);
				}
			}
        vTaskDelay(1000 / portTICK_PERIOD_MS);
		}
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

    Serial.println("[RESOURCE MONITOR]");
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
	initLog();
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

	server.on("/api/logs/state", HTTP_GET, [](Req *r) {
		JsonDocument doc;
		doc["active"]   = logActive;
		doc["inactive"] = 1 - logActive;
		doc["files"][0]["name"] = LOG_FILES[0];
		doc["files"][0]["size"] = fileSize(LOG_FILES[0]);
		doc["files"][1]["name"] = LOG_FILES[1];
		doc["files"][1]["size"] = fileSize(LOG_FILES[1]);
		String out;
		serializeJson(doc, out);
		r->send(200, "application/json", out);
	});

	server.on("^\\/api\\/logs\\/file\\/([01])$", HTTP_GET, [](Req *r) {
		int idx = r->pathArg(0).toInt();
		r->send(LittleFS, LOG_FILES[idx], "text/plain");
	});

	server.on("^\\/api\\/logs\\/delete\\/([01])$", HTTP_POST, [](Req *r) {
		int idx = r->pathArg(0).toInt();
		if (idx == logActive) {
			r->send(409, "application/json",
				"{\"error\":\"refusing to clear active log\"}");
			logger("Log delete REJECTED: %s is active", LOG_FILES[idx]);
			return;
		}
		File f = LittleFS.open(LOG_FILES[idx], "w");
		if (f) f.close();
		r->send(200, "application/json", "{\"ok\":true}");
		logger("Log cleared: %s", LOG_FILES[idx]);
	});

	server.on("^\\/api\\/logs\\/last\\/(\\d+)$", HTTP_GET, [](Req *r) {
		int count = r->pathArg(0).toInt();
		count = max(1, min(count, 10000));
		
		File f = LittleFS.open(LOG_FILES[logActive], "r");
		if (!f) {
			r->send(200, "text/plain", "");
			return;
		}
		String content = f.readString();
		f.close();
		
		Vector lines;
		int start = 0;
		for (int i = 0; i <= content.length(); i++) {
			if (i == content.length() || content[i] == '\n') {
				if (i > start) {
					lines.push_back(content.substring(start, i));
				}
				start = i + 1;
			}
		}
		
		String result;
		int begin = max(0, (int)lines.size() - count);
		for (int i = lines.size() - 1; i >= begin; i--) {
			result += lines[i] + "\n";
		}
		
		r->send(200, "text/plain", result);
	});

	server.on("^\\/api\\/logs\\/full$", HTTP_GET, [](Req *r) {
		Vector allLines;
		
		File f = LittleFS.open(LOG_FILES[logActive], "r");
		if (f) {
			String content = f.readString();
			f.close();
			
			int start = 0;
			for (int i = 0; i <= content.length(); i++) {
				if (i == content.length() || content[i] == '\n') {
					if (i > start) {
						allLines.push_back(content.substring(start, i));
					}
					start = i + 1;
				}
			}
		}
		
		f = LittleFS.open(LOG_FILES[1 - logActive], "r");
		if (f) {
			String content = f.readString();
			f.close();
			
			int start = 0;
			for (int i = 0; i <= content.length(); i++) {
				if (i == content.length() || content[i] == '\n') {
					if (i > start) {
						allLines.push_back(content.substring(start, i));
					}
					start = i + 1;
				}
			}
		}
		std::reverse(allLines.begin(), allLines.end());
		String result;
		for (const auto& line : allLines) {
			result += line + "\n";
		}
		
		r->send(200, "text/plain", result);
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
			zones[id].enable(duration);
			logger("%s ON, duration=%dmin", zones[id].name, duration);
		}
	);

	server.on(ZONE_DISABLE, HTTP_POST, [](Req *r) {
		int id = r->pathArg(0).toInt();
		if (id < 0 || id > 2) {
			r->send(400, "application/json", "{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].disable();
		r->send(200, "application/json", "{\"ok\":true}");
		logger("%s OFF", zones[id].name);
	});

	server.on(ZONE_PAUSE, HTTP_POST, [](Req *r) {
		int id = r->pathArg(0).toInt();
		if (id < 0 || id > 2) {
			r->send(400, "application/json", "{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].pause();
		logger("%s paused until %02d:%02d",
			zones[id].name,
			zones[id].schedule.stopHour,
			zones[id].schedule.stopMin);
		r->send(200, "application/json", "{\"ok\":true}");
	});

	server.on(ZONE_RESUME, HTTP_POST, [](Req *r) {
		int id = r->pathArg(0).toInt();
		if (id < 0 || id > 2) {
			r->send(400, "application/json", "{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].resume();
		logger("%s resumed", zones[id].name);
		r->send(200, "application/json", "{\"ok\":true}");
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
			logger("schedule modified, save.json updated");
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
	xTaskCreate(powerMonitorTask, "PWR", 4096, nullptr, 1, nullptr);
	float startup = static_cast<float>(millis()) / 1000;
	logger("Boot sequence took %.1f seconds", startup);
	xTaskCreate(scheduleTask, "Schedule", 4096, nullptr, 1, nullptr);

	Ok.on();
	Warn.off();
}

void loop() {
	commandListener();
	vTaskDelay(10 / portTICK_PERIOD_MS);
}

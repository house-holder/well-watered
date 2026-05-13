#include <WiFi.h>
#include <time.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

AsyncWebServer server(80);
using Req = AsyncWebServerRequest;

const char* ZONE_ENABLE  = "^\\/api\\/zones\\/(\\d+)\\/enable$";
const char* ZONE_DISABLE = "^\\/api\\/zones\\/(\\d+)\\/disable$";

enum LEDMode { ON, OFF, FLASH };

struct StatLED {
	int pin;
	int onTime;
	int cycleTime;
	LEDMode mode;
	unsigned long start;
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
	StatLED indication;
};

void updateLED(StatLED &LED) {
	unsigned long now = millis();
	int status = digitalRead(LED.pin);
	switch (LED.mode) {
		case ON:
			if (status == LOW) digitalWrite(LED.pin, HIGH);
			break;
		case OFF:
			if (status == HIGH) digitalWrite(LED.pin, LOW);
			break;
		case FLASH: {
			unsigned long runtime = now - LED.start;
			if (runtime >= LED.cycleTime && status == LOW) {
				digitalWrite(LED.pin, HIGH);
				LED.start = now;					
			} else {
				if (runtime >= LED.onTime && status == HIGH) {
					digitalWrite(LED.pin, LOW);
				}
			}
			break;
		}
	}
}

const Schedule DEFAULT_SCHEDULE = {
    .days  = { false, false, false, false, false, false, false },
    .startHour = 6, .startMin = 0, .stopHour  = 7, .stopMin  = 15
};

StatLED Warn =	 { 13,  100,  400, OFF, 0 };
StatLED Ok =	 { 14, 1000, 4000, OFF, 0 };

Zone zones[3] = {
	{ .pin=21, .name="Garden faucet", .schedule=DEFAULT_SCHEDULE,
		.indication={ .pin=16, .onTime=200, .cycleTime=800 }, },
	{ .pin=22, .name="House faucet",  .schedule=DEFAULT_SCHEDULE,
		.indication={ .pin=17, .onTime=200, .cycleTime=800 }, },
	{ .pin=23, .name="Shed faucet",   .schedule=DEFAULT_SCHEDULE,
		.indication={ .pin=18, .onTime=200, .cycleTime=800 }, },
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
        Warn.mode = FLASH;
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
void updater() {
	updateLED(Ok);
	updateLED(Warn);

	for (int i = 0; i < 3; i++) {
		zones[i].indication.mode = zones[i].running ? ON : OFF;
		digitalWrite(zones[i].pin, zones[i].running ? HIGH : LOW);
		updateLED(zones[i].indication);
	}
}

void commandListener() {
	if (Serial.available()) {
		String cmd = Serial.readStringUntil('\n');
		cmd.trim();
		if (cmd == "rsc") {
			Serial.println("[ Resource monitor ]");
			Serial.printf("  Heap size: %d\n", ESP.getHeapSize());
			Serial.printf("  Min heap:  %d\n", ESP.getMinFreeHeap());
		}
	}
}

// Setup Functions ------------------------------------------------------------
void initPins() {
	pinMode(Ok.pin, OUTPUT);
	pinMode(Warn.pin, OUTPUT);
	for (int i = 0; i < 3; i++) {
		pinMode(zones[i].pin, OUTPUT);
		pinMode(zones[i].indication.pin, OUTPUT);
	}
	Warn.mode = FLASH; // set flash only after pin setup
}

void initWifi() {
    const char *ntw = HOUSENET;
    const char *psk = HOUSEKEY;
    // const char *ntw = CGINET;
    // const char *psk = CGIKEY;

	WiFi.begin(ntw, psk);
	Serial.print("Connecting to network");

	unsigned long timer = 0;
	while (WiFi.status() != WL_CONNECTED) {
		unsigned long now = millis();
		updateLED(Warn);
		if (now - timer >= 500) {
			Serial.print(".");
			timer = now;
		}
	}
	Serial.println(" connected.");	
}

void initMDNS() {
	if (MDNS.begin("watering")) {
		Serial.println("mDNS started: http://watering.local");
	}
}

void initNTP(tm &timeinfo) {
	Serial.print("Syncing time...");
	unsigned long timer = 0;
	const long GMT_OFFSET = -6 * 3600;
	const int DST_OFFSET  = 3600;

	configTime(GMT_OFFSET, DST_OFFSET, "pool.ntp.org");

	while (!getLocalTime(&timeinfo)) {
		updateLED(Warn);
		unsigned long now = millis();
		if (now - timer >= 100) {
			Serial.print(".");
			timer = now;
		}
	}

	Serial.println(&timeinfo, " synced, time now: %H:%M:%S");
}

void initLittleFS() {
	if (!LittleFS.begin()) {
		Serial.println("LittleFS mount failed");
		Warn.mode = FLASH;
		return;
	}	
	Serial.println("LittleFS mounted");
	if (!LittleFS.exists("/save.json")) {
		saveSchedule();
		Serial.println("save.json initialized with defaults");
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
		int id = r->pathArg(0).toInt();
		if (id < 0 || id > 2) {
			r->send(
					400,
					"application/json",
					"{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].running = true;
		time(&zones[id].startTime);
		r->send(200, "application/json", "{\"ok\":true}");
	});

	server.on(ZONE_DISABLE, HTTP_POST, [](Req *r) {
		int id = r->pathArg(0).toInt();
		if (id < 0 || id > 2) {
			r->send(
					400,
					"application/json",
					"{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].running = false;
		zones[id].startTime = 0;
		r->send(200, "application/json", "{\"ok\":true}");
	});

	server.on("/api/schedule/save", HTTP_POST, [](Req *r) {
		// we'll handle the body next
		saveSchedule();
		r->send(200, "application/json", "{\"ok\":true}");
	});
}

// Main: setup & loop ---------------------------------------------------------
void setup() {
	Serial.begin(115200);
	struct tm timeinfo;

	initPins();
	initWifi();
	initMDNS();
	initNTP(timeinfo);
	initLittleFS();
	initAPIRouting();

	server.begin();
	Serial.printf("Webserver running (%s)\n", WiFi.localIP().toString()); 

	Ok.mode = ON;
	Warn.mode = OFF;
}

void loop() {
	updater();
	commandListener();
}

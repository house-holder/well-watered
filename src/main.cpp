#include <WiFi.h>
#include <time.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

enum LEDMode { ON, OFF, FLASH };

struct StatLED {
	int pin;
	int onTime;
	int cycleTime;
	LEDMode mode;
	unsigned long start;
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

// system-wide LEDs
StatLED Ok =	 { 13, 100, 4900, OFF, 0 };
StatLED Warn =	 { 14, 250, 1750, OFF, 0 };

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
        Serial.println("No save.json found, using defaults");
        return;
    }
    File f = LittleFS.open("/save.json", "r");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.println("Failed to parse save.json");
        return;
    }

	for (i = 0; i < 3; i++) { // load each zone
		zones[i].schedule.startHour = doc["zones"][i]["startHour"];
		zones[i].schedule.startMin = doc["zones"][i]["startMin"];
		zones[i].schedule.startHour = doc["zones"][i]["startHour"];
		zones[i].schedule.startMin = doc["zones"][i]["startMin"];
		for (d = 0; d < 7; d++ ) { // load each day

		}
	}

}

struct Zone {
	int pin;
	bool running;
	const char* name;
	time_t startTime;
	Schedule schedule;
	StatLED indication;
};

Zone zones[3] = {
	{ .pin=21, .name="Garden faucet", .schedule=DEFAULT_SCHEDULE,
		.indication={ .pin=16, .onTime=200, .cycleTime=800 }, },
	{ .pin=22, .name="House faucet",  .schedule=DEFAULT_SCHEDULE,
		.indication={ .pin=17, .onTime=200, .cycleTime=800 }, },
	{ .pin=23, .name="Shed faucet",   .schedule=DEFAULT_SCHEDULE,
		.indication={ .pin=18, .onTime=200, .cycleTime=800 }, },
};

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

AsyncWebServer server(80);

// Setup ----------------------------------------------------------------------
void setup() {
	Serial.begin(115200);
	unsigned long timer = 0;

	pinMode(Ok.pin, OUTPUT);
	pinMode(Warn.pin, OUTPUT);
	for (int i = 0; i < 3; i++) {
		pinMode(zones[i].pin, OUTPUT);
		pinMode(zones[i].indication.pin, OUTPUT);
	}

	Warn.mode = FLASH;
	WiFi.begin(WIFI_SSID, WIFI_PSKY);

	Serial.print("Connecting to network");
	while (WiFi.status() != WL_CONNECTED) {
		unsigned long now = millis();
		if (now - timer >= 100) {
			Serial.print(".");
			updateLED(Warn);
			timer = now;
		}
	}
	Serial.println(" connected.");

	if (MDNS.begin("watering")) {
		Serial.println("mDNS started: http://watering.local");
	}

	const long GMT_OFFSET = -6 * 3600;
	const int DST_OFFSET  = 3600;

	configTime(GMT_OFFSET, DST_OFFSET, "pool.ntp.org");
	struct tm timeinfo;
	timer = 0;
	Serial.print("Syncing time...");
	while (!getLocalTime(&timeinfo)) {
		unsigned long now = millis();
		if (now - timer >= 100) {
			Serial.print(".");
			updateLED(Warn);
			timer = now;
		}
	}
	Serial.println(&timeinfo, " synced. Time now: %H:%M:%S");

	if (!LittleFS.begin()) {
		Serial.println("LittleFS mount failed");
		Warn.mode = FLASH;
		return;
	}
	Serial.println("LittleFS mounted");

	// garbage endpoint; quiets favicon 404 error
	server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send(204);
	});

	// static serving
	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send(LittleFS, "/index.html", "text/html");
	});
	server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send(LittleFS, "/app.js", "text/javascript");
	});
	server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send(LittleFS, "/style.css", "text/css");
	});

	// basic state
	server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send(200, "application/json", getStateJSON());
	});

	// valve toggle endpoints
	server.on("^\\/api\\/zones\\/(\\d+)\\/enable$",
	HTTP_POST,
	[](AsyncWebServerRequest *request) {
		int id = request->pathArg(0).toInt();
		if (id < 0 || id > 2) {
			request->send(400, "application/json", "{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].running = true;
		time(&zones[id].startTime);
		request->send(200, "application/json", "{\"ok\":true}");
	});

	server.on("^\\/api\\/zones\\/(\\d+)\\/disable$",
	HTTP_POST,
	[](AsyncWebServerRequest *request) {
		int id = request->pathArg(0).toInt();
		if (id < 0 || id > 2) {
			request->send(400, "application/json", "{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].running = false;
		zones[id].startTime = 0;
		request->send(200, "application/json", "{\"ok\":true}");
	});

	server.begin();
	Warn.mode = OFF;
	Ok.mode = ON;

	Serial.printf("Webserver running (%s)\n", WiFi.localIP().toString()); 
}

void updateAllLEDs() {
	updateLED(Ok);
	updateLED(Warn);

	for (int i = 0; i < 3; i++) {
		zones[i].indication.mode = zones[i].running ? ON : OFF;
		digitalWrite(zones[i].pin, zones[i].running ? HIGH : LOW);
		updateLED(zones[i].indication);
	}
}

// Loop -----------------------------------------------------------------------
void loop() {
	if (Serial.available()) {
		String cmd = Serial.readStringUntil('\n');
		cmd.trim();
		if (cmd == "rsc") {
			Serial.println("[ Resource monitor ]");
			Serial.printf("  Heap size: %d\n", ESP.getHeapSize());
			Serial.printf("  Min heap:  %d\n", ESP.getMinFreeHeap());
		}
	}
	
	updateAllLEDs();
}

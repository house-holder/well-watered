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
	{ .pin=21, .name="Garden faucet", .schedule=DEFAULT_SCHEDULE,
		.LED={ .pin=16, .onTime=200, .cycleTime=800 }, },
	{ .pin=22, .name="House faucet",  .schedule=DEFAULT_SCHEDULE,
		.LED={ .pin=17, .onTime=200, .cycleTime=800 }, },
	{ .pin=23, .name="Shed faucet",   .schedule=DEFAULT_SCHEDULE,
		.LED={ .pin=18, .onTime=200, .cycleTime=800 }, },
};

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

const char* logTime() {
	static char buf[16];
	struct tm t;
	getLocalTime(&t);
	// 13May 10:10:10
	strftime(buf, sizeof(buf), "%d%b %H:%M:%S ", &t);
	return buf;
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
		pinMode(zones[i].LED.pin, OUTPUT);
		zones[i].LED.off();
	}
}

void initWifi() {
	const char *ntw = HOUSENET;
	const char *psk = HOUSEKEY;
	// const char *ntw = CGINET;
	// const char *psk = CGIKEY;

	WiFi.begin(ntw, psk);
	Serial.printf("%s Connecting to network");

	unsigned long timer = 0;
	while (WiFi.status() != WL_CONNECTED) {
		unsigned long now = millis();
		if (now - timer >= 500) {
			Serial.print(".");
			timer = now;
		}
	}
	Serial.printf("\n%s Connection successful\n", logTime());	
}

void initMDNS() {
	if (MDNS.begin("watering")) {
		Serial.printf("%s mDNS started: http://watering.local\n", logTime());
	}
}

void initNTP(tm &timeinfo) {
	Serial.printf("%s Syncing with timeserver", logTime());
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

void initLittleFS() {
	if (!LittleFS.begin()) {
		Serial.printf("%s LittleFS mount failed\n", logTime());
		return;
	}	
	Serial.printf("%s LittleFS mounted\n", logTime());
	if (!LittleFS.exists("/save.json")) {
		saveSchedule();
		Serial.printf("%s save.json initialized with defaults\n", logTime());
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
			r->send(400, "application/json", "{\"error\":\"invalid zone\"}");
			return;
		}
		zones[id].running = true;
		time(&zones[id].startTime);
		r->send(200, "application/json", "{\"ok\":true}");
		Serial.printf("%s %s ON\n", logTime(), zones[id].name);
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
		Serial.printf("%s %s OFF\n", logTime(), zones[id].name);
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
	xTaskCreate(ledTask, "LEDs", 2048, nullptr, 1, nullptr);

	initWifi();
	initMDNS();
	initNTP(timeinfo);
	initLittleFS();
	initAPIRouting();

	server.begin();
	Serial.printf("Webserver running (%s)\n", WiFi.localIP().toString()); 

	Ok.on();
	Warn.off();
}

void loop() {
	commandListener();
}

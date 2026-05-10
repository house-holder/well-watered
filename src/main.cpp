#include <WiFi.h>
#include <time.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
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

StatLED Ok =	 { 13, 100, 4900, OFF, 0 };
StatLED Warn =	 { 14, 250, 1750, OFF, 0 };
StatLED Shed =	 { 16, 500, 4500, OFF, 0 };
StatLED House =  { 17, 500, 4500, OFF, 0 };
StatLED Garden = { 18, 500, 4500, OFF, 0 };

// struct Zone {
// 	const char* name;
// 	int pin;
// 	bool running;
// 	StatLED indication;
// };
// Zone zones[3] = {
// 	{ "Shed faucet",   33, false, Shed },
// 	{ "House faucet",  34, false, House },
// 	{ "Garden faucet", 35, false, Garden },
// };

AsyncWebServer server(80);

// Setup ----------------------------------------------------------------------
void setup() {
	Serial.begin(115200);
	unsigned long timer = 0;

	pinMode(Ok.pin, OUTPUT);
	pinMode(Warn.pin, OUTPUT);
	pinMode(Shed.pin, OUTPUT);
	pinMode(House.pin, OUTPUT);
	pinMode(Garden.pin, OUTPUT);

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
	Serial.println("connected!");

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
	Serial.println(&timeinfo, " synced: %H:%M:%S");

	if (!LittleFS.begin()) {
		Serial.println("LittleFS mount failed");
		Warn.mode = FLASH;
		return;
	}
	Serial.println("LittleFS mounted");

	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send(LittleFS, "/index.html", "text/html");
	});

	server.begin();
	Warn.mode = OFF;
	Ok.mode = ON;

	Serial.printf("Webserver running (%s)\n", WiFi.localIP().toString()); 
}

void updateAllLEDs() {
	updateLED(Ok);
	updateLED(Warn);
	updateLED(Shed);
	updateLED(House);
	updateLED(Garden);
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

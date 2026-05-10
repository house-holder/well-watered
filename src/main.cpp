#include <WiFi.h>
#include <Arduino.h>
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

	while (WiFi.status() != WL_CONNECTED) {
		unsigned long now = millis();
		if (now - timer >= 100) {
			Serial.print(".");
			updateLED(Warn);
			timer = now;
		}
	}

	Serial.println("connected!");
	Ok.mode = ON;
	Warn.mode = ON;
	Shed.mode = ON;
	House.mode = ON;
	Garden.mode = ON;

	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send(200, "text/html", "<h1>well-watered</h1><p>works</p>");
	});
	server.begin();

	Serial.printf("webserver running (%s)\n", WiFi.localIP().toString()); 
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

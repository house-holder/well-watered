#include <WiFi.h>
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

AsyncWebServer server(80);

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
			if (status == LOW && runtime >= LED.cycleTime) { // restart cycle
				digitalWrite(LED.pin, HIGH);
				LED.start = now;					
			} else { // mid-cycle, just turn off LED if onTime expires
				if (runtime >= LED.onTime) {
					digitalWrite(LED.pin, LOW);
				}
			}
			break;
		}
	}
}

StatLED Ok = { 23, 25, 475, OFF, 0 };
StatLED Warn = { 22, 250, 1750, OFF, 0 };

struct Zone {
	const char* name;
	int pin;
	bool active;
};

Zone zones[3] = {
	{ "Shed faucet",   16, false },
	{ "House faucet",  17, false },
	{ "Garden faucet", 18, false },
};

// Setup ----------------------------------------------------------------------
void setup() {
	Serial.begin(115200);

	unsigned long timer = 0;
	pinMode(Ok.pin, OUTPUT);
	pinMode(Warn.pin, OUTPUT);
	for (int i=0; i<3; i++) {
		pinMode(zones[i].pin, OUTPUT);
	}


	WiFi.begin(WIFI_SSID, WIFI_PSKY);
	while (WiFi.status() != WL_CONNECTED) {
		unsigned long now = millis();
		Warn.mode = FLASH;
		if (now - timer >= 100) {
			Serial.print(".");
			timer = now;
		}
	}
	Serial.println("connected!");
	Ok.mode = ON;

	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->send(200, "text/html", "<h1>well-watered</h1><p>works</p>");
	});
	server.begin();

	Serial.printf("webserver running (%s)\n", WiFi.localIP().toString()); 
}

// Loop -----------------------------------------------------------------------
void loop() {

	updateLED(Ok); updateLED(Warn);
}

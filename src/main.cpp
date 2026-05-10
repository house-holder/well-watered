#include <Arduino.h>
#include <WiFi.h>

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

void setup() {
	Serial.begin(115200);
	for (int i=0; i<3; i++) {
		pinMode(zones[i].pin, OUTPUT);
	}
}

void loop() {
	for (int i=0; i<3; i++) {
		digitalWrite(zones[i].pin, HIGH);
		delay(800);
		digitalWrite(zones[i].pin, LOW);
		delay(200);
	}
}

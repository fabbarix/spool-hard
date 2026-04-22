// Watches all free GPIOs and reports which one goes LOW on button press.
// Upload, open Serial Monitor at 115200, then press each button.

// Skip pins already in use
const int SKIP[] = {4, 5, 8, 15, 16, 17, 18, 48, 19, 20}; // HX711, PN532, USB
const int SKIP_N = sizeof(SKIP) / sizeof(SKIP[0]);

// ESP32-S3 usable GPIOs
const int PINS[] = {0, 1, 2, 3, 6, 7, 9, 10, 11, 12, 13, 14,
                    21, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47};
const int PIN_N = sizeof(PINS) / sizeof(PINS[0]);

bool prevState[sizeof(PINS)/sizeof(PINS[0])];

bool shouldSkip(int pin) {
    for (int i = 0; i < SKIP_N; i++)
        if (SKIP[i] == pin) return true;
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== Button Finder ===");
    Serial.println("Press each button and watch which GPIO goes LOW\n");

    for (int i = 0; i < PIN_N; i++) {
        int pin = PINS[i];
        if (shouldSkip(pin)) continue;
        pinMode(pin, INPUT_PULLUP);
        prevState[i] = HIGH;
    }
}

void loop() {
    for (int i = 0; i < PIN_N; i++) {
        int pin = PINS[i];
        if (shouldSkip(pin)) continue;

        bool state = digitalRead(pin);
        if (state == LOW && prevState[i] == HIGH) {
            Serial.printf(">>> GPIO %d went LOW  (button pressed)\n", pin);
        } else if (state == HIGH && prevState[i] == LOW) {
            Serial.printf("    GPIO %d went HIGH (button released)\n", pin);
        }
        prevState[i] = state;
    }
    delay(10);
}

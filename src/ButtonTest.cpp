// #include <Arduino.h>

// #define BUTTON_PIN 13  // GPIO pin for the button

// void setup() {
//     // Initialize serial communication for debugging
//     Serial.begin(115200);

//     // Configure the button pin with internal pull-up
//     pinMode(BUTTON_PIN, INPUT_PULLUP);

//     // Display a message on startup
//     Serial.println("Button test started!");
// }

// void loop() {
//     // Read the button state (LOW when pressed, HIGH when not pressed)
//     int buttonState = digitalRead(BUTTON_PIN);

//     // Print the button state to the serial monitor
//     if (buttonState == LOW) {
//         Serial.println("Button Pressed");
//     } else {
//         Serial.println("Button Released");
//     }

//     // Small delay to avoid spamming the serial monitor
//     delay(200);
// }

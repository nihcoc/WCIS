#include <ESP32Servo.h>

Servo myservo;

int pos = 0;

void setup() {
  myservo.attach(16);     // attach servo to GPIO 16
  pinMode(22, OUTPUT);    // you forgot this
  digitalWrite(22, HIGH); // set pin 22 HIGH
}

void loop() {
  for (pos = 0; pos <= 180; pos += 1) {
    myservo.write(pos);
    delay(15);
  }

  for (pos = 180; pos >= 0; pos -= 1) {
    myservo.write(pos);
    delay(15);
  }
}]#include <ESP32Servo.h>

Servo myservo;

int pos = 0;

void setup() {
  myservo.attach(16);     // attach servo to GPIO 16
  pinMode(22, OUTPUT);    // you forgot this
  digitalWrite(22, HIGH); // set pin 22 HIGH
}

void loop() {
  for (pos = 0; pos <= 180; pos += 1) {
    myservo.write(pos);
    delay(15);
  }

  for (pos = 180; pos >= 0; pos -= 1) {
    myservo.write(pos);
    delay(15);
  }
}

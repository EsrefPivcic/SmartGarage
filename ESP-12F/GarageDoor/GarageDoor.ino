/*
FIREBASE:
=====================================================
*/

#include <Arduino.h>
#if defined(ESP32)
#include <WiFi.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#endif
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#define WIFI_SSID ""
#define WIFI_PASSWORD ""

#define API_KEY ""
#define DATABASE_URL ""

FirebaseData fbdo;

FirebaseAuth auth;
FirebaseConfig config;

/*
=====================================================
*/

#include <Servo.h>

Servo servo;  // create servo object to control a servo

int pos = 0;  // variable to store the servo position

String detectionResult = "no car";
String ownerCar = "";
bool manual = false;
bool manualOpen = false;

bool open = false;

bool signupOK = false;

void setup() {
  Serial.begin(9600);

  servo.attach(D7);  // attaches the servo on pin D7 to the servo object

  //==============FIREBASE, WIFI:
  Serial.print("Wi-Fi and Firebase setup in 2 seconds...");
  delay(2000);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  /* Assign the api key (required) */
  config.api_key = API_KEY;

  /* Assign the RTDB URL (required) */
  config.database_url = DATABASE_URL;

  /* Sign up */
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("signup Ok");
    signupOK = true;
  } else {
    Serial.printf("%s\n", config.signer.signupError.message.c_str());
  }

  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.print("Finished Wi-Fi and Firebase setup.");
  //===============================
}

void loop() {
  if (Firebase.RTDB.getBool(&fbdo, "/garage/manual")) {
    if (fbdo.dataType() == "boolean") {
      manual = fbdo.boolData();
      Serial.println("manual:");
      Serial.println(manual);
    }
  } else {
    Serial.println(fbdo.errorReason());
  }

  if (manual == false) {
    if (Firebase.RTDB.getString(&fbdo, "/garage/result")) {
      if (fbdo.dataType() == "string") {
        detectionResult = fbdo.stringData();
      }
    } else {
      Serial.println(fbdo.errorReason());
    }

    if (Firebase.RTDB.getString(&fbdo, "/garage/ownercar")) {
      if (fbdo.dataType() == "string") {
        ownerCar = fbdo.stringData();
      }
    } else {
      Serial.println(fbdo.errorReason());
    }

    if (detectionResult == ownerCar) { 
      if (!open) {
        for (pos = 0; pos <= 180; pos += 1) {
          servo.write(pos);
          delay(10);        
        }
        open = true;
        if (Firebase.RTDB.setBool(&fbdo, "/garage/manualopen", true)) {
          Serial.println("Sent manualopen to firebase.");
        } else {
          Serial.println("FAILED send manualopen to firebase: " + fbdo.errorReason());
        }
      }
    } 
    else {
      if (open) {
        for (pos = 180; pos >= 0; pos -= 1) {
          servo.write(pos);                   
          delay(10);                          
        }
        open = false;
        if (Firebase.RTDB.setBool(&fbdo, "/garage/manualopen", false)) {
          Serial.println("Sent manualopen to firebase.");
        } else {
          Serial.println("FAILED send manualopen to firebase: " + fbdo.errorReason());
        }
      }
    }
  } else {
    if (Firebase.RTDB.getBool(&fbdo, "/garage/manualopen")) {
      if (fbdo.dataType() == "boolean") {
        manualOpen = fbdo.boolData();
      }
    } else {
      Serial.println(fbdo.errorReason());
    }

    if (manualOpen) {
      if (!open) {
        for (pos = 0; pos <= 180; pos += 1) {
          servo.write(pos);  
          delay(10);         
        }
        open = true;
      }
    } else {
      if (open) {
        for (pos = 180; pos >= 0; pos -= 1) {  
          servo.write(pos);                    
          delay(10);                           
        }
        open = false;
      }
    }
  }
}

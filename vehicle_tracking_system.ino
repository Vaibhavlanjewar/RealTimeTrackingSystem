#include <ESP8266WiFi.h>
#include <FirebaseESP8266.h>
#include <SoftwareSerial.h>

// 🔗 Firebase Credentials
#define FIREBASE_HOST "real-time-vehicle-tracki-a83e6-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "AIzaSyCPoo_6Y-XB69FM_qrIjErVADmnaZeG3-4"

// 🔗 WiFi Credentials
const char* ssid = "RN";
const char* password = "77777777";

// Firebase Objects
FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;

// A9G Module on D7 (RX), D8 (TX)
SoftwareSerial A9G(13, 15);

// 🌍 Convert NMEA to Decimal Degrees
float convertToDecimalDegrees(String raw, String dir) {
  if (raw.length() < 6) return 0.0;
  int degreeDigits = (dir == "N" || dir == "S") ? 2 : 3;
  float deg = raw.substring(0, degreeDigits).toFloat();
  float min = raw.substring(degreeDigits).toFloat();
  float dec = deg + (min / 60.0);
  if (dir == "S" || dir == "W") dec *= -1;
  return dec;
}

// 📏 Haversine Distance
double calculateDistance(float lat1, float lon1, float lat2, float lon2) {
  const double R = 6371000;
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat/2) * sin(dLat/2) +
             cos(radians(lat1)) * cos(radians(lat2)) *
             sin(dLon/2) * sin(dLon/2);
  double c = 2 * atan2(sqrt(a), sqrt(1-a));
  return R * c;
}

// 📱 Send SMS
void sendSMS(String num, String msg) {
  A9G.println("AT+CMGF=1");
  delay(1000);
  while (A9G.available()) Serial.write(A9G.read());
  A9G.println("AT+CMGS=\"" + num + "\"");
  delay(1000);
  while (A9G.available()) Serial.write(A9G.read());
  A9G.print(msg);
  delay(1000);
  A9G.write(26);  // Ctrl+Z
  delay(5000);
  while (A9G.available()) Serial.write(A9G.read());
}

// 📞 Make Call
void makeCall(String num) {
  A9G.println("ATD" + num + ";");
  delay(100000);
  A9G.println("ATH");
}

void setup() {
  Serial.begin(9600);
  A9G.begin(9600);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("\n✅ WiFi Connected");

  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (Firebase.ready()) {
    if (auth.token.uid != "") {
      Serial.println("User is authenticated");
    } else {
      Serial.println("User is NOT authenticated");
    }
  } else {
    Serial.println("❌ Firebase not ready");
  }

  A9G.println("AT+GPS=1");
  A9G.println("AT+GPSRD=1");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) return;

  String gpsRaw = "";
  unsigned long startTime = millis();
  while (millis() - startTime < 3000) {
    while (A9G.available()) gpsRaw += char(A9G.read());
  }

  int i = gpsRaw.indexOf("$GNRMC");
  if (i < 0) { delay(2000); return; }

  int eol = gpsRaw.indexOf('\n', i);
  String line = gpsRaw.substring(i, eol > 0 ? eol : gpsRaw.length());
  line.trim();

  int idx = 0, last = -1;
  int comma[12];
  while (idx < 12) {
    int next = line.indexOf(',', last + 1);
    if (next < 0) break;
    comma[idx++] = next;
    last = next;
  }

  Serial.print("Line: "); Serial.println(line);

  if (idx >= 3 && comma[2] > comma[1]) {
    String status = line.substring(comma[1] + 1, comma[2]);
    Serial.print("Status: "); Serial.println(status);

    if (status == "A" && idx >= 6) {
      float vLat = convertToDecimalDegrees(
        line.substring(comma[2] + 1, comma[3]),
        line.substring(comma[3] + 1, comma[4])
      );
      float vLon = convertToDecimalDegrees(
        line.substring(comma[4] + 1, comma[5]),
        line.substring(comma[5] + 1, comma[6])
      );

      if (vLat != 0 && vLon != 0) {
        Serial.printf("🚗 Vehicle: %.6f, %.6f\n", vLat, vLon);

        FirebaseJson gpsJson;
        gpsJson.set("latitude", vLat);
        gpsJson.set("longitude", vLon);

        static unsigned long lastUpdate = 0;
        if (millis() - lastUpdate > 5000) {
          if (Firebase.setJSON(firebaseData, "/vehicle/gps", gpsJson)) {
            Serial.println("✅ Data sent to Firebase successfully.");
          } else {
            Serial.print("❌ Failed to send data to Firebase. Error: ");
            Serial.println(firebaseData.errorReason());
          }
          lastUpdate = millis();
        }

        if (Firebase.getJSON(firebaseData, "/users")) {
          FirebaseJson& users = firebaseData.jsonObject();
          size_t count = users.iteratorBegin();
          FirebaseJsonData data;

          for (size_t j = 0; j < count; j++) {
            String uid, jsonStr;
            int type;
            users.iteratorGet(j, type, uid, jsonStr);
            if (type != FirebaseJson::JSON_OBJECT) continue;

            FirebaseJson user(jsonStr);

            if (!user.get(data, "location/lat")) continue;
            float uLat = data.floatValue;

            if (!user.get(data, "location/lng")) continue;
            float uLon = data.floatValue;

            if (!user.get(data, "phone")) continue;
            String phone = data.stringValue;
            if (phone.length() == 10 && !phone.startsWith("+91")) {
              phone = "+91" + phone;
            }

            if (!user.get(data, "online")) continue;
            bool isOnline = data.boolValue;

            double dist = calculateDistance(vLat, vLon, uLat, uLon); 
            Serial.printf("📍 %s — %.1f m\n", uid.c_str(), dist);

            if (dist <= 300.0 && isOnline) {
              bool enable_sms = false, enable_call = false;
              bool notified1 = false, notified2 = false;

              if (user.get(data, "enable_sms")) enable_sms = data.boolValue;
              if (user.get(data, "enable_call")) enable_call = data.boolValue;
              if (user.get(data, "notified1")) notified1 = data.boolValue;
              if (user.get(data, "notified2")) notified2 = data.boolValue;

              // ✅ SMS Notification
              if (enable_sms && !notified1) {
                sendSMS(phone, "Your College Bus Has Arrived!!! Come Quickly.");
                Serial.println("📱 SMS sent to " + phone);

                if (Firebase.setBool(firebaseData, "/users/" + uid + "/notified1", true)) {
                  Serial.println("✅ notified1 set to true for " + uid);
                } else {
                  Serial.print("❌ Failed to set notified1 for " + uid + ". Error: ");
                  Serial.println(firebaseData.errorReason());
                }
              }

              // ✅ Call Notification
              if (enable_call && !notified2) {
                makeCall(phone);
                Serial.println("📞 Call made to " + phone);

                if (Firebase.setBool(firebaseData, "/users/" + uid + "/notified2", true)) {
                  Serial.println("✅ notified2 set to true for " + uid);
                } else {
                  Serial.print("❌ Failed to set notified2 for " + uid + ". Error: ");
                  Serial.println(firebaseData.errorReason());
                }
              }
            }
          }
          users.iteratorEnd();
        } else {
          Serial.println("❌ Failed to get /users");
          Serial.println("Error " + String(firebaseData.errorCode()) + ": " + firebaseData.errorReason());
        }
      }
    }
  }
  delay(5000);
}

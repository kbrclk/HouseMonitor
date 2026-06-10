#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "DHT.h"
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>

// --- WiFi & API Credentials ---
const char* ssid = "BMEGOD";
const char* password = "bmegod12";
String apiKey = "36e6f54b0528b949819afca27c095a2f";
String lat = "47.68";
String lon = "19.13";

// --- InfluxDB Settings ---
#define INFLUXDB_URL "https://us-east-1-1.aws.cloud2.influxdata.com/"
#define INFLUXDB_TOKEN "8dBZbqg9k-nGjaSsjoZfyfTIguFEc1WAXcILFDjEwtX_RMuMJ1myAbz_kK4saJ5G4F96mv9eg_tgfqO6Bhj_bQ=="
#define INFLUXDB_ORG "BME"
#define INFLUXDB_BUCKET "weekend_house_reports"
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3" // Timezone for Hungary/Central Europe

// --- Pin Settings ---
const int micPin = 34; 
const int gasPin = 35;
const int dhtPin = 25;
const int redPin = 26;   
const int greenPin = 27; 
const int sampleWindow = 50; 

// --- Weather Data Globals ---
float extTemp = 0.0;
float extHum = 0.0;
float extPress = 0.0;
float extFeelsLike = 0.0;
float windSpeed = 0.0;

#define DHTTYPE DHT11
DHT dht(dhtPin, DHTTYPE);

// --- InfluxDB Client Setup ---
InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);
Point sensorData("environment_metrics"); // This is the "Measurement" name in Influx

unsigned long lastWeatherCheck = 0;
const unsigned long weatherInterval = 30000;

// --- Forward Declarations ---
float measureSound();
void getWeatherData();
void sendAlert(String type, float value);

void setup() {
  Serial.begin(115200);
  pinMode(redPin, OUTPUT);
  pinMode(greenPin, OUTPUT);

  dht.begin();
  
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(greenPin, LOW);
    digitalWrite(redPin, HIGH);
    delay(250);
    digitalWrite(redPin, LOW);
    delay(250);
    Serial.print(".");
  }

  digitalWrite(redPin, LOW);
  digitalWrite(greenPin, HIGH);
  Serial.println("\nWiFi Connected!");

  // --- Time Sync ---
  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

  // Check InfluxDB connection
  if (client.validateConnection()) {
    Serial.println("Connected to InfluxDB server");
  } else {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(client.getLastErrorMessage());
  }
  
  delay(2000); 
}

void loop() {
  // 1. Connection Status Light
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(greenPin, LOW);
    digitalWrite(redPin, HIGH);
  } else {
    digitalWrite(redPin, LOW);
    digitalWrite(greenPin, HIGH);
  }

  // 2. Measure Local Conditions
  float currentDB = measureSound();
  int gasValue = analogRead(gasPin);
  float localTemp = dht.readTemperature();
  float localHum = dht.readHumidity();

  Serial.printf("Local -> Sound: %.1f dB | Air Quality: %d | Temp: %.1f°C | Hum: %.1f%%\n", 
                currentDB, gasValue, localTemp, localHum);

  // 3. --- SEND DATA TO INFLUXDB ---
  sensorData.clearFields();

  // Local Sensor Fields
  sensorData.addField("sound_level", currentDB);
  sensorData.addField("gas_value", gasValue);
  sensorData.addField("temp_internal", localTemp);
  sensorData.addField("hum_internal", localHum);
  
  // External Weather Fields
  sensorData.addField("temp_external", extTemp);
  sensorData.addField("temp_feels_like", extFeelsLike);
  sensorData.addField("pressure_external", extPress);
  sensorData.addField("hum_external", extHum);
  sensorData.addField("wind_speed", windSpeed);

  if (!client.writePoint(sensorData)) {
    Serial.printf("InfluxDB write failed: %s\n", client.getLastErrorMessage().c_str());
  }

  // 4. Alerts
  if (currentDB > 75.0) sendAlert("NOISE", currentDB);
  if (gasValue > 1500) sendAlert("GAS/SMOKE", gasValue);

  // 5. Fetch Weather Data
  if (millis() - lastWeatherCheck > weatherInterval || lastWeatherCheck == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      getWeatherData();
    }
    lastWeatherCheck = millis();
  }

  delay(30000); // Send data every 0.5 minutes
}

// --- Sound Measurement Function ---
float measureSound() {
  unsigned long startMillis = millis();
  unsigned int peakToPeak = 0;
  unsigned int signalMax = 0;
  unsigned int signalMin = 4095;

  while (millis() - startMillis < sampleWindow) {
    int sample = analogRead(micPin);
    if (sample < 4096) {
      if (sample > signalMax) signalMax = sample;
      else if (sample < signalMin) signalMin = sample;
    }
  }
  peakToPeak = signalMax - signalMin;
  if (peakToPeak < 1) peakToPeak = 1;

  float unoEquivalentPeak = peakToPeak / 4.0;
  if (unoEquivalentPeak < 1.0) unoEquivalentPeak = 1.0; 

  return (20.0 * log10(unoEquivalentPeak)) + 30.0;
}

// --- Weather API Function ---
void getWeatherData() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://api.openweathermap.org/data/2.5/weather?lat=" + lat + "&lon=" + lon + "&appid=" + apiKey + "&units=metric";
    
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode > 0) {
      String payload = http.getString();
      StaticJsonDocument<1536> doc; // Increased size slightly for more data
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        // Extracting the new fields
        extTemp = doc["main"]["temp"];
        extFeelsLike = doc["main"]["feels_like"];
        extPress = doc["main"]["pressure"];
        extHum = doc["main"]["humidity"];
        windSpeed = doc["wind"]["speed"];
        
        Serial.println("\n--- EXTERNAL WEATHER SYNC ---");
        Serial.printf("Temp: %.1f°C | Feels Like: %.1f°C\n", extTemp, extFeelsLike);
        Serial.printf("Press: %.0f hPa | Hum: %.0f%% | Wind: %.1f m/s\n", extPress, extHum, windSpeed);
      }
    }
    http.end();
  }
}

// --- Alert Placeholder ---
void sendAlert(String type, float value) {
  Serial.printf("\n!!! [ALERT] %s: %.1f !!!\n", type.c_str(), value);
}
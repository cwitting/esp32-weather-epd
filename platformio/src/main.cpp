/* Main program for esp32-weather-epd.
 * Copyright (C) 2022-2023  Luke Marzen
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Arduino.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>
#include <time.h>
#include <WiFi.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "driver/adc.h"

#include "api_response.h"
#include "client_utils.h"
#include "config.h"
#include "display_utils.h"
#include "icons/icons_196x196.h"
#include "renderer.h"
#ifndef USE_HTTP
  #include <WiFiClientSecure.h>
#endif
#ifdef USE_HTTPS_WITH_CERT_VERIF
  #include "cert.h"
#endif

// too large to allocate locally on stack
static owm_resp_onecall_t       owm_onecall;
static owm_resp_air_pollution_t owm_air_pollution;

Preferences prefs;

/* Put esp32 into ultra low-power deep-sleep (<11μA).
 * Aligns wake time to the minute. Sleep times defined in config.cpp.
 */
void beginDeepSleep(unsigned long &startTime, tm *timeInfo)
{
  if (!getLocalTime(timeInfo))
  {
    Serial.println("Failed to obtain time before deep-sleep, referencing " \
                   "older time.");
  }

  uint64_t sleepDuration = 0;
  int extraHoursUntilWake = 0;
  int curHour = timeInfo->tm_hour;

  if (timeInfo->tm_min >= 58)
  { // if we are within 2 minutes of the next hour, then round up for the
    // purposes of bed time
    curHour = (curHour + 1) % 24;
    extraHoursUntilWake += 1;
  }

  if (BED_TIME < WAKE_TIME && curHour >= BED_TIME && curHour < WAKE_TIME)
  { // 0              B   v  W  24
    // |--------------zzzzZzz---|
    extraHoursUntilWake += WAKE_TIME - curHour;
  }
  else if (BED_TIME > WAKE_TIME && curHour < WAKE_TIME)
  { // 0 v W               B    24
    // |zZz----------------zzzzz|
    extraHoursUntilWake += WAKE_TIME - curHour;
  }
  else if (BED_TIME > WAKE_TIME && curHour >= BED_TIME)
  { // 0   W               B  v 24
    // |zzz----------------zzzZz|
    extraHoursUntilWake += WAKE_TIME - (curHour - 24);
  }
  else // This feature is disabled (BED_TIME == WAKE_TIME)
  {    // OR it is not past BED_TIME
    extraHoursUntilWake = 0;
  }

  if (extraHoursUntilWake == 0)
  { // align wake time to nearest multiple of SLEEP_DURATION
    sleepDuration = SLEEP_DURATION * 60ULL
                    - ((timeInfo->tm_min % SLEEP_DURATION) * 60ULL
                        + timeInfo->tm_sec);
  }
  else
  { // align wake time to the hour
    sleepDuration = extraHoursUntilWake * 3600ULL
                    - (timeInfo->tm_min * 60ULL + timeInfo->tm_sec);
  }

  // if we are within 2 minutes of the next alignment.
  if (sleepDuration <= 120ULL)
  {
    sleepDuration += SLEEP_DURATION * 60ULL;
  }

  // add extra delay to compensate for esp32's with fast RTCs.
  sleepDuration += 10ULL;

#if DEBUG_LEVEL >= 1
  printHeapUsage();
#endif

  esp_sleep_enable_timer_wakeup(sleepDuration * 1000000ULL);
  Serial.println("Awake for "
                 + String((millis() - startTime) / 1000.0, 3) + "s");
  Serial.println("Deep-sleep for " + String(sleepDuration) + "s");
  btStop();
  adc_power_off();
  esp_deep_sleep_start();
} // end beginDeepSleep


static bool mqtt_message_received = false;
static float inHumidity = 0.0f;

void mqtt_callback(char* topic, byte* message, unsigned int length) {
  mqtt_message_received = true;
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String jsonMsg;
  
  for (int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
    jsonMsg += (char)message[i];
  }
  Serial.println();

  DynamicJsonDocument doc(32 * 1024);

  DeserializationError error = deserializeJson(doc, jsonMsg);

  inHumidity = doc["humidity"];

  Serial.print("Humidity: ");
  Serial.println(inHumidity);

  // // Feel free to add more if statements to control more GPIOs with MQTT

  // // If a message is received on the topic esp32/output, you check if the message is either "on" or "off". 
  // // Changes the output state according to the message
  // if (String(topic) == "esp32/output") {
  //   Serial.print("Changing output to ");
  // }
}

/* Program entry point.
 */
void setup()
{
  btStop();
  adc_power_off();
  unsigned long startTime = millis();
  Serial.begin(115200);

#if DEBUG_LEVEL >= 1
  printHeapUsage();
#endif

  // Open namespace for read/write to non-volatile storage
  prefs.begin(NVS_NAMESPACE, false);

#if BATTERY_MONITORING
  // GET BATTERY VOLTAGE
  // DFRobot FireBeetle Esp32-E V1.0 has voltage divider (1M+1M), so readings
  // are multiplied by 2. Readings are divided by 1000 to convert mV to V.
  double batteryVoltage =
            static_cast<double>(analogRead(PIN_BAT_ADC)) / 1000.0 * (3.5 / 2.0);
            // use / 1000.0 * (3.3 / 2.0) multiplier above for firebeetle esp32
            // use / 1000.0 * (3.5 / 2.0) for firebeetle esp32-E
  Serial.println("Battery voltage: " + String(batteryVoltage, 2));

  // When the battery is low, the display should be updated to reflect that, but
  // only the first time we detect low voltage. The next time the display will
  // refresh is when voltage is no longer low. To keep track of that we will
  // make use of non-volatile storage.
  bool lowBat = prefs.getBool("lowBat", false);

  // low battery, deep-sleep now
  if (batteryVoltage <= LOW_BATTERY_VOLTAGE)
  {
    if (lowBat == false)
    { // battery is now low for the first time
      prefs.putBool("lowBat", true);
      prefs.end();
      initDisplay();
      do
      {
        drawError(battery_alert_0deg_196x196, "Low Battery", "");
      } while (display.nextPage());
      display.powerOff();
    }

    if (batteryVoltage <= CRIT_LOW_BATTERY_VOLTAGE)
    { // critically low battery
      // don't set esp_sleep_enable_timer_wakeup();
      // We won't wake up again until someone manually presses the RST button.
      Serial.println("Critically low battery voltage!");
      Serial.println("Hibernating without wake time!");
    }
    else if (batteryVoltage <= VERY_LOW_BATTERY_VOLTAGE)
    { // very low battery
      esp_sleep_enable_timer_wakeup(VERY_LOW_BATTERY_SLEEP_INTERVAL
                                    * 60ULL * 1000000ULL);
      Serial.println("Very low battery voltage!");
      Serial.println("Deep-sleep for "
                     + String(VERY_LOW_BATTERY_SLEEP_INTERVAL) + "min");
    }
    else
    { // low battery
      esp_sleep_enable_timer_wakeup(LOW_BATTERY_SLEEP_INTERVAL
                                    * 60ULL * 1000000ULL);
      Serial.println("Low battery voltage!");
      Serial.println("Deep-sleep for "
                    + String(LOW_BATTERY_SLEEP_INTERVAL) + "min");
    }
    btStop();
    adc_power_off();
    esp_deep_sleep_start();
  }
  // battery is no longer low, reset variable in non-volatile storage
  if (lowBat == true)
  {
    prefs.putBool("lowBat", false);
  }
#else
  double batteryVoltage = NAN;
#endif

  // All data should have been loaded from NVS. Close filesystem.
  prefs.end();

  String statusStr = {};
  String tmpStr = {};
  tm timeInfo = {};

  // START WIFI
  int wifiRSSI = 0; // “Received Signal Strength Indicator"
  wl_status_t wifiStatus = startWiFi(wifiRSSI);
  if (wifiStatus != WL_CONNECTED)
  { // WiFi Connection Failed
    killWiFi();
    initDisplay();
    if (wifiStatus == WL_NO_SSID_AVAIL)
    {
      Serial.println("Network Not Available");
      do
      {
        drawError(wifi_x_196x196, "Network Not", "Available");
      } while (display.nextPage());
    }
    else
    {
      Serial.println("WiFi Connection Failed");
      do
      {
        drawError(wifi_x_196x196, "WiFi Connection", "Failed");
      } while (display.nextPage());
    }
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }

  // TIME SYNCHRONIZATION
  configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
  bool timeConfigured = waitForSNTPSync(&timeInfo);
  if (!timeConfigured)
  { // Failed To Fetch The Time
    Serial.println("Time Synchronization Failed");
    killWiFi();
    initDisplay();
    do
    {
      drawError(wi_time_4_196x196, "Time Synchronization", "Failed");
    } while (display.nextPage());
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }

  // MAKE API REQUESTS
#ifdef USE_HTTP
  WiFiClient client;
#elif defined(USE_HTTPS_NO_CERT_VERIF)
  WiFiClientSecure client;
  client.setInsecure();
#elif defined(USE_HTTPS_WITH_CERT_VERIF)
  WiFiClientSecure client;
  client.setCACert(cert_Sectigo_RSA_Domain_Validation_Secure_Server_CA);
#endif

  // Listen on MQTT
  WiFiClient mqtt_web_client;
  Serial.println("Setting up mqtt");
  PubSubClient mqtt_client(mqtt_web_client);
  const char* mqtt_server = "192.168.0.111";
  mqtt_client.setServer(mqtt_server, 1883);
  mqtt_client.setCallback(mqtt_callback);
  if(!mqtt_client.connect("Esp32Client", "user", "password")) {
    Serial.println("Not able to connect");
  }
  mqtt_client.subscribe("zigbee2mqtt/Living Room Thermo");
  //

  int rxStatus = getOWMonecall(client, owm_onecall);
  if (rxStatus != HTTP_CODE_OK)
  {
    killWiFi();
    statusStr = "One Call " + OWM_ONECALL_VERSION + " API";
    tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
    initDisplay();
    do
    {
      drawError(wi_cloud_down_196x196, statusStr, tmpStr);
    } while (display.nextPage());
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }
  rxStatus = getOWMairpollution(client, owm_air_pollution);
  if (rxStatus != HTTP_CODE_OK)
  {
    killWiFi();
    statusStr = "Air Pollution API";
    tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
    initDisplay();
    do
    {
      drawError(wi_cloud_down_196x196, statusStr, tmpStr);
    } while (display.nextPage());
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }

  for (size_t i = 0; i < 20; i++)
  {
    mqtt_client.loop();
    if (mqtt_message_received) {
      break;
    }
    Serial.println("No message yet...");
    delay(500);
  }
  Serial.println("Mqtt Done");

  killWiFi(); // WiFi no longer needed

  // GET INDOOR TEMPERATURE AND HUMIDITY, start BME280...
  pinMode(PIN_BME_PWR, OUTPUT);
  digitalWrite(PIN_BME_PWR, HIGH);
  float inTemp     = NAN;
  Serial.print("Reading from BME280... ");
  TwoWire I2C_bme = TwoWire(0);
  Adafruit_BMP280 bme(&I2C_bme);

  I2C_bme.begin(PIN_BME_SDA, PIN_BME_SCL, 100000); // 100kHz
  if(bme.begin(BME_ADDRESS))
  {
    inTemp     = bme.readTemperature(); // Celsius
    // inHumidity = bme.readHumidity();    // %

    // check if BME readings are valid
    // note: readings are checked again before drawing to screen. If a reading
    //       is not a number (NAN) then an error occurred, a dash '-' will be
    //       displayed.
    if (std::isnan(inTemp))
    {
      statusStr = "BME read failed";
      Serial.println(statusStr);
    }
    else
    {
      Serial.println("Success");
    }
  }
  else
  {
    statusStr = "BME not found"; // check wiring
    Serial.println(statusStr);
  }

  digitalWrite(PIN_BME_PWR, LOW);

  String refreshTimeStr;
  getRefreshTimeStr(refreshTimeStr, timeConfigured, &timeInfo);
  String dateStr;
  getDateStr(dateStr, &timeInfo);

  int pwrPin = 25;
  pinMode(pwrPin, OUTPUT);
  digitalWrite(pwrPin, HIGH);
  // RENDER FULL REFRESH
  initDisplay();
  do
  {
    drawCurrentConditions(owm_onecall.current, owm_onecall.daily[0],
                          owm_air_pollution, inTemp, inHumidity);
    drawForecast(owm_onecall.daily, timeInfo);
    drawLocationDate(CITY_STRING, dateStr);
    drawOutlookGraph(owm_onecall.hourly, timeInfo);
#if !DISPLAY_ALERTS
    drawAlerts(owm_onecall.alerts, CITY_STRING, dateStr);
#endif
    drawStatusBar(statusStr, refreshTimeStr, wifiRSSI, batteryVoltage);
  } while (display.nextPage());
  display.powerOff();
  digitalWrite(pwrPin, LOW);
  pinMode(PIN_EPD_CS, OUTPUT);
  digitalWrite(PIN_EPD_CS, LOW);
  gpio_hold_en(GPIO_NUM_2); // Keep builtin LED low
  gpio_deep_sleep_hold_en();

  // DEEP-SLEEP
  beginDeepSleep(startTime, &timeInfo);
} // end setup

/* This will never run
 */
void loop()
{
} // end loop


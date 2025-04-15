// Import required libraries
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "SPIFFS.h"
#include "DHT20.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <Attribute_Request.h>
#include <PubSubClient.h>
#include <Arduino_MQTT_Client.h>
#include <OTA_Firmware_Update.h>
#include <ThingsBoard.h>

#include <Espressif_Updater.h>

WiFiClient espClient;
PubSubClient client(espClient);
AsyncWebServer server(80);

void processSharedAttributeUpdate(const JsonObjectConst &data)
{
  for (auto it = data.begin(); it != data.end(); ++it)
  {
    Serial.println(it->key().c_str());
  }
}



const char fwVersion[] = "fw_version";
uint64_t REQUEST_TIMEOUT_MICROSECONDS = 5000000ULL;
void requestTimedOut() {}

const std::vector<const char *> REQUESTED_CLIENT_ATTRIBUTES = {fwVersion};
Attribute_Request<2U, 5U> attr_request;
const Attribute_Request_Callback<5U> clientCallback(&processSharedAttributeUpdate, REQUEST_TIMEOUT_MICROSECONDS, &requestTimedOut, REQUESTED_CLIENT_ATTRIBUTES);
bool requestedShared = false;


void processSharedAttributeUpdate(const JsonObjectConst &data);

constexpr uint8_t MAX_SHARED_ATTR = 5U;
Shared_Attribute_Update<1U, MAX_SHARED_ATTR> sharedAttributeUpdate;

const char ledState[] = "fw_title";
constexpr std::array<const char *, 2U> SUBSCRIBED_SHARED_ATTRIBUTES = {ledState};
const Shared_Attribute_Callback<MAX_SHARED_ATTR> callback(&processSharedAttributeUpdate, SUBSCRIBED_SHARED_ATTRIBUTES);
bool sharedAttrSubscribed = false;

const char CURRENT_FIRMWARE_TITLE[] = "ota";
const char CURRENT_FIRMWARE_VERSION[] = "1.0";

// Maximum amount of retries we attempt to download each firmware chunck over MQTT
constexpr uint8_t FIRMWARE_FAILURE_RETRIES = 12U;

// Size of each firmware chunck downloaded over MQTT,
// increased packet size, might increase download speed
constexpr uint16_t FIRMWARE_PACKET_SIZE = 4096U;

// Replace with your network credentials
const char* ssid = "Wifi11";
const char* password = "123456789";

const char* mqttServer = "app.coreiot.io";
const int mqttPort = 1883;
const char* ACCESS_TOKEN = "l8wfuphtuoy2fpe21lio";

// Create AsyncWebServer object on port 80
// AsyncWebServer server(80);
DHT20 dht20;

// Variables to store temperature and humidity values
float temperature = 0.0;
float humidity = 0.0;


// Maximum size packets will ever be sent or received by the underlying MQTT client,
// if the size is to small messages might not be sent or received messages will be discarded
constexpr uint16_t MAX_MESSAGE_SEND_SIZE = 512U;
constexpr uint16_t MAX_MESSAGE_RECEIVE_SIZE = 512U;

// Baud rate for the debugging serial connection
// If the Serial output is mangled, ensure to change the monitor speed accordingly to this variable
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;

// Initalize the Mqtt client instance
Arduino_MQTT_Client mqttClient(espClient);
// Initialize used apis
OTA_Firmware_Update<> ota;
const std::array<IAPI_Implementation*, 3U> apis = {
    &sharedAttributeUpdate,
    &ota,
    &attr_request,
};
// Initialize ThingsBoard instance with the maximum needed buffer size
ThingsBoard tb(mqttClient, MAX_MESSAGE_RECEIVE_SIZE, MAX_MESSAGE_SEND_SIZE, Default_Max_Stack_Size, apis);
// Initalize the Updater client instance used to flash binary to flash memory


Espressif_Updater<> updater;


// Statuses for updating
bool currentFWSent = false;
bool updateRequestSent = false;

/// @brief Reconnects the WiFi uses InitWiFi if the connection has been removed
/// @return Returns true as soon as a connection has been established again
void InitWiFi() {
  Serial.println("Connecting to WiFi ...");
  // Attempting to establish a connection to the given WiFi network
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    // Delay 500ms until a connection has been successfully established
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected to WiFi");

}
bool reconnect() {
  // Check to ensure we aren't connected yet
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    return true;
  }

  // If we aren't establish a new connection to the given WiFi network
  InitWiFi();
  return true;
}


/// @brief Update starting callback method that will be called as soon as the shared attribute firmware keys have been received and processed
/// and the moment before we subscribe the necessary topics for the OTA firmware update.
/// Is meant to give a moment were any additional processes or communication with the cloud can be stopped to ensure the update process runs as smooth as possible.
/// To ensure that calling the ThingsBoardSized::Cleanup_Subscriptions() method can be used which stops any receiving of data over MQTT besides the one for the OTA firmware update,
/// if this method is used ensure to call all subscribe methods again so they can be resubscribed, in the method passed to the finished_callback if the update failed and we do not restart the device
void update_starting_callback() {
  // Nothing to do
}

/// @brief End callback method that will be called as soon as the OTA firmware update, either finished successfully or failed.
/// Is meant to allow to either restart the device if the udpate was successfull or to restart any stopped services before the update started in the subscribed update_starting_callback
/// @param success Either true (update successful) or false (update failed)
void finished_callback(const bool & success) {
  if (success) {
    Serial.println("Done, Reboot now");


    esp_restart();
    return;
  }
  Serial.println("Downloading firmware failed");
}

/// @brief Progress callback method that will be called every time our current progress of downloading the complete firmware data changed,
/// meaning it will be called if the amount of already downloaded chunks increased.
/// Is meant to allow to display a progress bar or print the current progress of the update into the console with the currently already downloaded amount of chunks and the total amount of chunks
/// @param current Already received and processs amount of chunks
/// @param total Total amount of chunks we need to receive and process until the update has completed
void progress_callback(const size_t & current, const size_t & total) {
  Serial.printf("Progress %.2f%%\n", static_cast<float>(current * 100U) / total);
}

 void connectMQTT() {
   while (!client.connected()) {
     Serial.println("Connecting...");
    if (client.connect("ESP32Client", ACCESS_TOKEN, NULL)) {
       Serial.println("MQTT connect successfully!");
     } else {
       Serial.print("Failed to connect");
       Serial.println(client.state());
       delay(2000);
     }
   }
 }

// Hàm gửi dữ liệu lên CoreIOT
void sendDataToCoreIOT() {
  if (!tb.connected()) {
    Serial.println("ThingsBoard client not connected!");
    return;
  }

  bool tempSent = tb.sendTelemetryData("temperature", temperature);
  bool humSent  = tb.sendTelemetryData("humidity", humidity);

  if (tempSent && humSent) {
    Serial.println("Data published to CoreIOT !");
  } else {
    Serial.println("Failed to publish telemetry to CoreIOT.");
  }
}


// Hàm đọc dữ liệu từ cảm biến
void readDHT20SensorTask(void *pvParameters) {
  while (true) {
    if (dht20.read() == DHT20_OK) {
      temperature = dht20.getTemperature();
      humidity = dht20.getHumidity();
      Serial.printf("Temperature: %.2f °C, Humidity: %.2f %%\n", temperature, humidity);
      sendDataToCoreIOT(); // Gửi dữ liệu sau khi đọc cảm biến
    } else {
      Serial.println("Failed to connect to DHT20");
    }
    
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void taskOTA(void* pvParameters){
  while(1) {
    if (!tb.connected()) {
      // Reconnect to the ThingsBoard server,
      // if a connection was disrupted or has not yet been established
      Serial.printf("Connecting to: (%s) with token (%s)\n", "app.coreiot.io", "l8wfuphtuoy2fpe21lio");
      if (!tb.connect(mqttServer, ACCESS_TOKEN, mqttPort)) {
        Serial.println("Failed to connect");
        return;
      }
      Serial.println("IOT connected");
    }  

    if (!requestedShared)
    {
      // Shared attributes we want to request from the server
      requestedShared = attr_request.Shared_Attributes_Request(clientCallback);
      if (!requestedShared)
      {
        Serial.println("Failed to request shared attributes");
      }
    }

    if (!sharedAttrSubscribed)
    {
      if (!sharedAttributeUpdate.Shared_Attributes_Subscribe(callback))
      {
        Serial.println("Failed to subscribe for shared attribute updates");
        return;
      }
      Serial.println("Subscribe shared attribute done");
      sharedAttrSubscribed = true;
    }

    if (!currentFWSent) {
      currentFWSent = ota.Firmware_Send_Info(CURRENT_FIRMWARE_TITLE, CURRENT_FIRMWARE_VERSION);
    }
  
    if (!updateRequestSent) {
      Serial.println("Firmware Update...");
      const OTA_Update_Callback callback(	CURRENT_FIRMWARE_TITLE, CURRENT_FIRMWARE_VERSION , &updater, &finished_callback, &progress_callback, &update_starting_callback, FIRMWARE_FAILURE_RETRIES, FIRMWARE_PACKET_SIZE);
      updateRequestSent = ota.Start_Firmware_Update(callback);
    }
  
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  InitWiFi();
   delay(7000);
  Wire.begin();
   if (!dht20.begin()) {
     Serial.println("DHT20 is unavailable");
     // while (true);
   }
   else Serial.println("DHT20 is available");

  
  
   client.setServer(mqttServer, mqttPort);
   connectMQTT();

   server.begin();

  xTaskCreate(readDHT20SensorTask, "ReadDHT20Sensor", 2048, NULL, 1, NULL);
  xTaskCreate(taskOTA, "taskOTA", 4096, NULL, 1, NULL);

}

void loop() {
  if (!reconnect()) {
    return;
  }

  tb.loop();
  delay(1000);
}

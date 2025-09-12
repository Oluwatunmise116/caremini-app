#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// LittleFS Configuration
#define REMINDERS_FILE "/reminders.json"

// OLED Display using U8g2 - 1.3 inch SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// Hardware pins for ESP32-C3
const int vibrationPin = 2;
const int buzzerPin = 3;

// BLE Server variables
BLEServer* pServer = NULL;
BLECharacteristic* pTimeCharacteristic = NULL;
BLECharacteristic* pReminderCharacteristic = NULL;
BLECharacteristic* pStatusCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// Time variables
struct TimeStruct {
  int hour;
  int minute;
  int second;
  int day;
  int month;
  int year;
};
TimeStruct currentTime = {12, 0, 0, 1, 1, 2024};
unsigned long lastTimeUpdate = 0;
bool timeSet = false;

// Reminder structure
struct Reminder {
  int id;
  int hour;
  int minute;
  String type;
  String message;
  bool active;
  bool triggered;
};

Reminder reminders[10];
int reminderCount = 0;
int nextReminderId = 1;

// Reminder alert variables
bool alertActive = false;
unsigned long alertStartTime = 0;
unsigned long lastBeepTime = 0;
unsigned long lastVibrationTime = 0;
String currentAlertMessage = "";
const unsigned long ALERT_DURATION = 60000;
const unsigned long BEEP_INTERVAL = 1000;
const unsigned long VIBRATION_INTERVAL = 2000;

// FreeRTOS variables
TaskHandle_t TimeTaskHandle = NULL;
TaskHandle_t DisplayTaskHandle = NULL;
TaskHandle_t BLETaskHandle = NULL;
SemaphoreHandle_t timeMutex = NULL;
SemaphoreHandle_t displayMutex = NULL;
volatile bool displayUpdateFlag = false;

// BLE UUIDs
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define TIME_CHARACTERISTIC_UUID "12345678-1234-1234-1234-123456789abd"
#define REMINDER_CHARACTERISTIC_UUID "12345678-1234-1234-1234-123456789abe"
#define STATUS_CHARACTERISTIC_UUID "12345678-1234-1234-1234-123456789abf"

// Function declarations
void handleTimeUpdate(String timeData);
void handleReminderUpdate(String reminderData);
void handleBLEConnection();
void checkReminders();
void triggerAlert(Reminder reminder);
void handleAlerts();
void updateDisplay();
void displayWaitingMessage();
void drawBluetoothIcon(int x, int y);
void drawClockIcon(int x, int y);
void saveRemindersToFS();
void loadRemindersFromFS();
void listFiles();
void timeTask(void *parameter);
void displayTask(void *parameter);
void bleTask(void *parameter);
void deleteReminder(int id);
void sendReminderList();

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("BLE Device Connected");
    displayUpdateFlag = true;
  };

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("BLE Device Disconnected");
    displayUpdateFlag = true;
  }
};

class MyCharacteristicCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue();
    
    if (pCharacteristic->getUUID().toString() == TIME_CHARACTERISTIC_UUID) {
      handleTimeUpdate(value);
    } 
    else if (pCharacteristic->getUUID().toString() == REMINDER_CHARACTERISTIC_UUID) {
      handleReminderUpdate(value);
    }
  }
};

void handleTimeUpdate(String timeData) {
  Serial.println("Received time data: " + timeData);
  
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, timeData);
  
  if (doc.containsKey("hour") && doc.containsKey("minute")) {
    if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      currentTime.hour = doc["hour"];
      currentTime.minute = doc["minute"];
      currentTime.second = doc["second"];
      currentTime.day = doc["day"];
      currentTime.month = doc["month"];
      currentTime.year = doc["year"];
      
      timeSet = true;
      lastTimeUpdate = millis();
      
      xSemaphoreGive(timeMutex);
      
      Serial.printf("Time updated: %02d:%02d:%02d %02d/%02d/%d\n", 
                    currentTime.hour, currentTime.minute, currentTime.second,
                    currentTime.day, currentTime.month, currentTime.year);
      
      displayUpdateFlag = true;
    }
  }
}

void handleReminderUpdate(String reminderData) {
  Serial.println("Received reminder data: " + reminderData);
  
  DynamicJsonDocument doc(2048);
  deserializeJson(doc, reminderData);
  
  if (doc.containsKey("action")) {
    String action = doc["action"];
    
    if (action == "add") {
      if (reminderCount < 10) {
        reminders[reminderCount].id = nextReminderId++;
        reminders[reminderCount].hour = doc["hour"];
        reminders[reminderCount].minute = doc["minute"];
        reminders[reminderCount].type = doc["type"].as<String>();
        reminders[reminderCount].message = doc["message"].as<String>();
        reminders[reminderCount].active = true;
        reminders[reminderCount].triggered = false;
        
        Serial.printf("Added reminder ID %d: %s at %02d:%02d - %s\n", 
                      reminders[reminderCount].id,
                      reminders[reminderCount].type.c_str(),
                      reminders[reminderCount].hour,
                      reminders[reminderCount].minute,
                      reminders[reminderCount].message.c_str());
        
        reminderCount++;
        saveRemindersToFS();
        sendReminderList();
      }
    }
    else if (action == "delete") {
      int deleteId = doc["id"];
      deleteReminder(deleteId);
    }
    else if (action == "clear") {
      reminderCount = 0;
      saveRemindersToFS();
      Serial.println("All reminders cleared");
      sendReminderList();
    }
    else if (action == "list") {
      sendReminderList();
    }
  }
}

void deleteReminder(int id) {
  for (int i = 0; i < reminderCount; i++) {
    if (reminders[i].id == id) {
      for (int j = i; j < reminderCount - 1; j++) {
        reminders[j] = reminders[j + 1];
      }
      reminderCount--;
      saveRemindersToFS();
      Serial.printf("Deleted reminder ID %d\n", id);
      sendReminderList();
      return;
    }
  }
  Serial.printf("Reminder ID %d not found\n", id);
}

void sendReminderList() {
  if (!deviceConnected) {
    Serial.println("Cannot send reminder list - device not connected");
    return;
  }
  
  Serial.printf("Sending reminder list to web app - %d reminders\n", reminderCount);
  
  DynamicJsonDocument doc(2048);
  doc["action"] = "reminder_list";
  doc["count"] = reminderCount;
  JsonArray reminderArray = doc.createNestedArray("reminders");
  
  for (int i = 0; i < reminderCount; i++) {
    JsonObject reminder = reminderArray.createNestedObject();
    reminder["id"] = reminders[i].id;
    reminder["hour"] = reminders[i].hour;
    reminder["minute"] = reminders[i].minute;
    reminder["type"] = reminders[i].type;
    reminder["message"] = reminders[i].message;
    
    Serial.printf("Adding to list: ID %d, %02d:%02d, %s, %s\n", 
                  reminders[i].id, reminders[i].hour, reminders[i].minute,
                  reminders[i].type.c_str(), reminders[i].message.c_str());
  }
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  Serial.println("JSON to send: " + jsonString);
  
  if (jsonString.length() > 500) {
    Serial.println("JSON too long, sending simplified version");
    doc.clear();
    doc["action"] = "reminder_list";
    doc["count"] = min(reminderCount, 3);
    JsonArray simpleArray = doc.createNestedArray("reminders");
    
    for (int i = 0; i < reminderCount && i < 3; i++) {
      JsonObject reminder = simpleArray.createNestedObject();
      reminder["id"] = reminders[i].id;
      reminder["hour"] = reminders[i].hour;
      reminder["minute"] = reminders[i].minute;
      reminder["type"] = reminders[i].type;
      String shortMsg = reminders[i].message;
      if (shortMsg.length() > 20) {
        shortMsg = shortMsg.substring(0, 20) + "...";
      }
      reminder["message"] = shortMsg;
    }
    
    serializeJson(doc, jsonString);
  }
  
  pStatusCharacteristic->setValue(jsonString.c_str());
  pStatusCharacteristic->notify();
  Serial.println("Reminder list sent successfully");
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Alzheimer's Watch with FreeRTOS on ESP32-C3...");
  
  // Create mutexes
  timeMutex = xSemaphoreCreateMutex();
  displayMutex = xSemaphoreCreateMutex();
  
  if (timeMutex == NULL || displayMutex == NULL) {
    Serial.println("Failed to create mutexes!");
    while(1) {
      delay(1000);
    }
  }
  Serial.println("Mutexes created successfully");
  
  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS initialization failed!");
    while(1) {
      delay(1000);
    }
  } else {
    Serial.println("LittleFS initialized successfully");
  }
  
  listFiles();
  loadRemindersFromFS();
  
  // Initialize hardware
  pinMode(vibrationPin, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(vibrationPin, LOW);
  digitalWrite(buzzerPin, LOW);
  
  // Initialize OLED
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.clearDisplay();
  displayWaitingMessage();
  
  // Initialize BLE
  BLEDevice::init("AlzheimerWatch");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  pTimeCharacteristic = pService->createCharacteristic(
                      TIME_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_WRITE |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  
  pReminderCharacteristic = pService->createCharacteristic(
                      REMINDER_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_WRITE
                    );
  
  pStatusCharacteristic = pService->createCharacteristic(
                      STATUS_CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  
  pTimeCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
  pReminderCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
  
  pTimeCharacteristic->addDescriptor(new BLE2902());
  pStatusCharacteristic->addDescriptor(new BLE2902());
  
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x0);
  BLEDevice::startAdvertising();
  
  Serial.println("BLE Server started");
  
  // Create FreeRTOS tasks
  xTaskCreate(timeTask, "TimeTask", 4096, NULL, 3, &TimeTaskHandle);
  xTaskCreate(displayTask, "DisplayTask", 4096, NULL, 2, &DisplayTaskHandle);
  xTaskCreate(bleTask, "BLETask", 4096, NULL, 1, &BLETaskHandle);
  
  Serial.println("FreeRTOS tasks created successfully for ESP32-C3");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

void handleBLEConnection() {
  if (!deviceConnected && oldDeviceConnected) {
    Serial.println("Device disconnected, restarting advertising");
    delay(500);
    pServer->startAdvertising();
    oldDeviceConnected = deviceConnected;
  }
  
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
    Serial.println("Device connected - will send reminder list shortly");
  }
}

void checkReminders() {
  if (!timeSet) return;
  
  if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < reminderCount; i++) {
      if (reminders[i].active && !reminders[i].triggered) {
        if (currentTime.hour == reminders[i].hour && 
            currentTime.minute == reminders[i].minute && 
            currentTime.second == 0) {
          
          triggerAlert(reminders[i]);
          reminders[i].triggered = true;
          
          Serial.printf("Reminder triggered: %s - %s\n", 
                        reminders[i].type.c_str(), 
                        reminders[i].message.c_str());
        }
      }
      
      if (reminders[i].triggered && 
          (currentTime.hour != reminders[i].hour || currentTime.minute != reminders[i].minute)) {
        reminders[i].triggered = false;
      }
    }
    xSemaphoreGive(timeMutex);
  }
}

void triggerAlert(Reminder reminder) {
  alertActive = true;
  alertStartTime = millis();
  currentAlertMessage = reminder.message;
  
  Serial.printf("ALERT: %s reminder - %s\n", 
                reminder.type.c_str(), reminder.message.c_str());
  
  if (deviceConnected) {
    String alertMsg = "ALERT: " + reminder.type + " - " + reminder.message;
    pStatusCharacteristic->setValue(alertMsg.c_str());
    pStatusCharacteristic->notify();
  }
}

void handleAlerts() {
  if (!alertActive) return;
  
  unsigned long currentMillis = millis();
  
  if (currentMillis - alertStartTime >= ALERT_DURATION) {
    alertActive = false;
    currentAlertMessage = "";
    digitalWrite(vibrationPin, LOW);
    digitalWrite(buzzerPin, LOW);
    Serial.println("Alert ended");
    displayUpdateFlag = true;
    return;
  }
  
  if (currentMillis - lastVibrationTime >= VIBRATION_INTERVAL) {
    digitalWrite(vibrationPin, !digitalRead(vibrationPin));
    lastVibrationTime = currentMillis;
  }
  
  if (currentMillis - lastBeepTime >= BEEP_INTERVAL) {
    digitalWrite(buzzerPin, HIGH);
    delay(200);
    digitalWrite(buzzerPin, LOW);
    lastBeepTime = currentMillis;
  }
}

void drawBluetoothIcon(int x, int y) {
  u8g2.drawLine(x+2, y, x+2, y+8);
  u8g2.drawLine(x+2, y, x+5, y+3);
  u8g2.drawLine(x+2, y+8, x+5, y+5);
  u8g2.drawLine(x+1, y+2, x+3, y+4);
  u8g2.drawLine(x+1, y+6, x+3, y+4);
  u8g2.drawPixel(x+5, y+3);
  u8g2.drawPixel(x+5, y+5);
}

void drawClockIcon(int x, int y) {
  u8g2.drawCircle(x+4, y+4, 4);
  u8g2.drawLine(x+4, y+4, x+4, y+2);
  u8g2.drawLine(x+4, y+4, x+6, y+4);
  u8g2.drawPixel(x+4, y+4);
}

void updateDisplay() {
  if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    u8g2.clearBuffer();
    
    if (!timeSet) {
      displayWaitingMessage();
    } else {
      if (alertActive) {
        drawClockIcon(5, 5);
        u8g2.setFont(u8g2_font_6x10_tf);
        u8g2.drawStr(20, 12, currentAlertMessage.c_str());
        
        u8g2.setFont(u8g2_font_logisoso24_tn);
        String timeStr = "";
        if (currentTime.hour < 10) timeStr += "0";
        timeStr += String(currentTime.hour) + ":";
        if (currentTime.minute < 10) timeStr += "0";
        timeStr += String(currentTime.minute);
        
        int timeWidth = u8g2.getStrWidth(timeStr.c_str());
        int timeX = (128 - timeWidth) / 2 + 5;
        u8g2.drawStr(timeX, 45, timeStr.c_str());
        
      } else {
        if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          u8g2.setFont(u8g2_font_logisoso32_tn);
          
          String timeStr = "";
          if (currentTime.hour < 10) timeStr += "0";
          timeStr += String(currentTime.hour) + ":";
          if (currentTime.minute < 10) timeStr += "0";
          timeStr += String(currentTime.minute);
          
          int timeWidth = u8g2.getStrWidth(timeStr.c_str());
          int timeX = ((128 - timeWidth) / 2) - 10; // Shifted 10 pixels left
          u8g2.drawStr(timeX, 35, timeStr.c_str());
          
          u8g2.setFont(u8g2_font_6x10_tf);
          String secondStr = ":";
          if (currentTime.second < 10) secondStr += "0";
          secondStr += String(currentTime.second);
          u8g2.drawStr(timeX + timeWidth, 35, secondStr.c_str());
          
          String dateStr = "";
          if (currentTime.day < 10) dateStr += "0";
          dateStr += String(currentTime.day) + "/";
          if (currentTime.month < 10) dateStr += "0";
          dateStr += String(currentTime.month) + "/" + String(currentTime.year);
          
          int dateWidth = u8g2.getStrWidth(dateStr.c_str());
          int dateX = (128 - dateWidth) / 2;
          u8g2.drawStr(dateX, 55, dateStr.c_str());
          
          xSemaphoreGive(timeMutex);
        }
      }
      
      if (deviceConnected) {
        drawBluetoothIcon(5, 55);
      }
    }
    
    u8g2.sendBuffer();
    xSemaphoreGive(displayMutex);
  }
}

void displayWaitingMessage() {
  u8g2.setFont(u8g2_font_6x10_tf);
  
  String line1 = "Waiting for";
  String line2 = "phone sync...";
  
  int line1Width = u8g2.getStrWidth(line1.c_str());
  int line2Width = u8g2.getStrWidth(line2.c_str());
  
  int line1X = (128 - line1Width) / 2;
  int line2X = (128 - line2Width) / 2;
  
  u8g2.drawStr(line1X, 25, line1.c_str());
  u8g2.drawStr(line2X, 40, line2.c_str());
}

// LittleFS Functions
void saveRemindersToFS() {
  Serial.println("Saving reminders to LittleFS...");
  
  DynamicJsonDocument doc(2048);
  doc["reminderCount"] = reminderCount;
  doc["nextReminderId"] = nextReminderId;
  
  JsonArray reminderArray = doc.createNestedArray("reminders");
  
  for (int i = 0; i < reminderCount; i++) {
    JsonObject reminder = reminderArray.createNestedObject();
    reminder["id"] = reminders[i].id;
    reminder["hour"] = reminders[i].hour;
    reminder["minute"] = reminders[i].minute;
    reminder["type"] = reminders[i].type;
    reminder["message"] = reminders[i].message;
    reminder["active"] = reminders[i].active;
    reminder["triggered"] = reminders[i].triggered;
  }
  
  File file = LittleFS.open(REMINDERS_FILE, "w");
  if (!file) {
    Serial.println("Failed to open reminders file for writing");
    return;
  }
  
  if (serializeJson(doc, file) == 0) {
    Serial.println("Failed to write JSON to file");
  } else {
    Serial.printf("Successfully saved %d reminders to LittleFS\n", reminderCount);
  }
  
  file.close();
}

void loadRemindersFromFS() {
  Serial.println("Loading reminders from LittleFS...");
  
  if (!LittleFS.exists(REMINDERS_FILE)) {
    Serial.println("Reminders file doesn't exist, starting fresh");
    reminderCount = 0;
    nextReminderId = 1;
    return;
  }
  
  File file = LittleFS.open(REMINDERS_FILE, "r");
  if (!file) {
    Serial.println("Failed to open reminders file for reading");
    reminderCount = 0;
    nextReminderId = 1;
    return;
  }
  
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.printf("Failed to parse reminders JSON: %s\n", error.c_str());
    reminderCount = 0;
    nextReminderId = 1;
    return;
  }
  
  reminderCount = doc["reminderCount"] | 0;
  nextReminderId = doc["nextReminderId"] | 1;
  
  if (reminderCount > 10) {
    Serial.println("Invalid reminder count, resetting");
    reminderCount = 0;
    nextReminderId = 1;
    return;
  }
  
  JsonArray reminderArray = doc["reminders"];
  for (int i = 0; i < reminderCount && i < 10; i++) {
    JsonObject reminder = reminderArray[i];
    reminders[i].id = reminder["id"];
    reminders[i].hour = reminder["hour"];
    reminders[i].minute = reminder["minute"];
    reminders[i].type = reminder["type"].as<String>();
    reminders[i].message = reminder["message"].as<String>();
    reminders[i].active = reminder["active"] | true;
    reminders[i].triggered = reminder["triggered"] | false;
    
    Serial.printf("Loaded reminder ID %d: %s at %02d:%02d - %s\n",
                  reminders[i].id, reminders[i].type.c_str(),
                  reminders[i].hour, reminders[i].minute,
                  reminders[i].message.c_str());
  }
  
  Serial.printf("Successfully loaded %d reminders from LittleFS\n", reminderCount);
}

void listFiles() {
  Serial.println("Files in LittleFS:");
  File root = LittleFS.open("/");
  if (!root) {
    Serial.println("Failed to open root directory");
    return;
  }
  
  File file = root.openNextFile();
  bool foundFiles = false;
  
  while (file) {
    Serial.printf("  %s (%zu bytes)\n", file.name(), file.size());
    foundFiles = true;
    file.close();
    file = root.openNextFile();
  }
  
  if (!foundFiles) {
    Serial.println("  No files found in LittleFS");
  }
  
  root.close();
}

// FreeRTOS Task Functions
void timeTask(void *parameter) {
  Serial.println("Time task started on ESP32-C3");
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  while (true) {
    if (timeSet) {
      if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        currentTime.second++;
        if (currentTime.second >= 60) {
          currentTime.second = 0;
          currentTime.minute++;
          if (currentTime.minute >= 60) {
            currentTime.minute = 0;
            currentTime.hour++;
            if (currentTime.hour >= 24) {
              currentTime.hour = 0;
            }
          }
        }
        
        xSemaphoreGive(timeMutex);
        displayUpdateFlag = true;
        checkReminders();
      }
    }
    
    handleAlerts();
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
    taskYIELD();
  }
}

void displayTask(void *parameter) {
  Serial.println("Display task started on ESP32-C3");
  
  while (true) {
    if (displayUpdateFlag || (xTaskGetTickCount() % pdMS_TO_TICKS(500) == 0)) {
      updateDisplay();
      displayUpdateFlag = false;
    }
    
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void bleTask(void *parameter) {
  Serial.println("BLE task started on ESP32-C3");
  static bool reminderListSent = false;
  
  while (true) {
    handleBLEConnection();
    
    if (deviceConnected && !reminderListSent) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      Serial.println("Sending stored reminders to web app...");
      sendReminderList();
      reminderListSent = true;
    }
    
    if (!deviceConnected) {
      reminderListSent = false;
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

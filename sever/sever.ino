#include <WiFi.h>
#include <PN532_HSU.h>
#include <PN532.h>
#include <ArduinoJson.h>
#include <RpcServer.h>
#include <RpcSerialTransport.h>

const char* ssid     = "your wifi name";
const char* password = "password";

WiFiServer tcpServer(8080);
WiFiClient client;

RpcServer<8> rpc;
StaticJsonDocument<512> rpcResultDoc;

PN532_HSU pn532hsu(Serial1);
PN532 nfc(pn532hsu);

bool tagPresent = false;
String currentUidStr = "";
unsigned long lastSeenTime = 0;
unsigned long lastPollTime = 0;

const uint16_t NFC_READ_TIMEOUT_MS = 500;

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial1.begin(115200, SERIAL_8N1, 33, 32);
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("ไม่พบ PN532!");
    while (1) delay(1000);
  }
  nfc.SAMConfig();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected. IP: " + String(WiFi.localIP().toString()));

  tcpServer.begin();
  Serial.println("NTAG215 JSON-RPC Server started on port 8080");

  setupRpcMethods();
}

// ฟังก์ชันเขียน NDEF Record พร้อมล้างข้อมูลเก่าและใส่ Terminator (0xFE) ให้ถูกต้อง
bool writeNdefRecord(char typeChar, const uint8_t* payload, uint16_t payloadLen) {
  uint16_t recordLen;
  if (payloadLen < 255) {
    recordLen = 1 + 1 + 1 + 1 + payloadLen; 
  } else {
    recordLen = 1 + 1 + 4 + 1 + payloadLen;
  }

  uint16_t totalLen = 2 + recordLen + 1; // TLV header (2) + record + terminator (1)
  uint16_t pagesNeeded = (totalLen + 3) / 4;
  uint16_t allocLen = pagesNeeded * 4;

  uint8_t buf[allocLen];
  memset(buf, 0, allocLen); // เคลียร์ค่าว่างเป็น 0 ป้องกันข้อมูลขยะตกค้าง

  buf[0] = 0x03; // NDEF Message TLV Tag
  buf[1] = (uint8_t)recordLen; 

  if (payloadLen < 255) {
    buf[2] = 0xD1; // MB=1, ME=1, SR=1, TNF=001 (Well-known)
    buf[3] = 0x01; // Type Length = 1
    buf[4] = (uint8_t)payloadLen;
    buf[5] = (uint8_t)typeChar;
    memcpy(buf + 6, payload, payloadLen);
    buf[6 + payloadLen] = 0xFE; // NDEF Terminator TLV
  } else {
    buf[2] = 0xC1; 
    buf[3] = 0x01;
    buf[4] = (uint8_t)(payloadLen >> 24);
    buf[5] = (uint8_t)(payloadLen >> 16);
    buf[6] = (uint8_t)(payloadLen >> 8);
    buf[7] = (uint8_t)(payloadLen & 0xFF);
    buf[8] = (uint8_t)typeChar;
    memcpy(buf + 9, payload, payloadLen);
    buf[9 + payloadLen] = 0xFE;
  }

  // เขียนข้อมูลลง NTAG เริ่มตั้งแต่ Page 4 เป็นต้นไป
  for (uint16_t p = 0; p < pagesNeeded; p++) {
    if (!nfc.mifareultralight_WritePage(4 + p, buf + p * 4)) return false;
  }
  
  // เขียนหน้าถัดไปเพิ่มอีก 1 หน้าเพื่อเคลียร์ข้อมูลเก่าที่อาจหลงเหลือจากการเขียนข้อความที่ยาวกว่า
  uint8_t emptyPage[4] = {0xFE, 0x00, 0x00, 0x00};
  nfc.mifareultralight_WritePage(4 + pagesNeeded, emptyPage);

  return true;
}

bool writeNdefUriToNtag(String url) {
  uint8_t prefixCode = 0x00;
  if (url.startsWith("https://www.")) { prefixCode = 0x02; url = url.substring(12); }
  else if (url.startsWith("http://www."))  { prefixCode = 0x01; url = url.substring(11); }
  else if (url.startsWith("https://"))     { prefixCode = 0x04; url = url.substring(8); }
  else if (url.startsWith("http://"))      { prefixCode = 0x03; url = url.substring(7); }
  else if (url.startsWith("tel:"))         { prefixCode = 0x05; url = url.substring(4); }
  else if (url.startsWith("mailto:"))      { prefixCode = 0x06; url = url.substring(7); }

  uint16_t payloadLen = 1 + url.length();
  uint8_t payload[payloadLen];
  payload[0] = prefixCode;
  for (uint16_t i = 0; i < url.length(); i++) payload[1 + i] = (uint8_t)url[i];

  return writeNdefRecord('U', payload, payloadLen);
}

bool writeNdefTextToNtag(String text, String langCode = "en") {
  uint16_t payloadLen = 1 + langCode.length() + text.length();
  uint8_t payload[payloadLen];
  payload[0] = (uint8_t)langCode.length();
  uint16_t idx = 1;
  for (uint16_t i = 0; i < langCode.length(); i++) payload[idx++] = (uint8_t)langCode[i];
  for (uint16_t i = 0; i < text.length(); i++) payload[idx++] = (uint8_t)text[i];

  return writeNdefRecord('T', payload, payloadLen);
}

// ฟังก์ชันอ่าน NDEF ที่กรองอักขระ null terminator ออกอย่างสะอาดหมดจด
bool readNdefFromNtag(String &outType, String &outContent) {
  uint8_t page4[4];
  if (!nfc.mifareultralight_ReadPage(4, page4)) {
    return false;
  }

  if (page4[0] != 0x03) {
    outType = "empty";
    outContent = "";
    return true;
  }

  uint16_t msgLen = page4[1];
  if (msgLen == 0 || msgLen == 0xFF) {
    outType = "empty";
    outContent = "";
    return true;
  }

  uint16_t totalBytesNeeded = 2 + msgLen;
  uint16_t maxPages = (totalBytesNeeded + 3) / 4;
  uint8_t buf[maxPages * 4];
  memset(buf, 0, maxPages * 4);

  for (uint8_t p = 0; p < maxPages; p++) {
    uint8_t pageBuf[4];
    if (nfc.mifareultralight_ReadPage(4 + p, pageBuf)) {
      memcpy(buf + (p * 4), pageBuf, 4);
    }
  }

  uint8_t* ndefMsg = buf + 2;
  uint8_t flags = ndefMsg[0];
  uint8_t tnf = flags & 0x07;
  bool sr = (flags & 0x10) != 0;
  
  uint8_t typeLen = ndefMsg[1];
  uint32_t payloadLen = 0;
  uint16_t idx = 2;

  if (sr) {
    payloadLen = ndefMsg[idx];
    idx += 1;
  } else {
    payloadLen = ((uint32_t)ndefMsg[idx] << 24) | ((uint32_t)ndefMsg[idx + 1] << 16) |
                 ((uint32_t)ndefMsg[idx + 2] << 8)  | ndefMsg[idx + 3];
    idx += 4;
  }

  if (flags & 0x08) {
    idx += 1; 
  }

  String typeStr = "";
  for (uint8_t i = 0; i < typeLen; i++) {
    typeStr += (char)ndefMsg[idx + i];
  }
  idx += typeLen;

  uint8_t* payloadPtr = ndefMsg + idx;

  if (tnf == 0x01 && typeStr == "U") {
    static const char* prefixTable[] = {
      "", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:"
    };
    uint8_t prefixCode = (payloadLen > 0) ? payloadPtr[0] : 0;
    String prefix = (prefixCode < 7) ? prefixTable[prefixCode] : "";
    String rest = "";
    for (uint32_t i = 1; i < payloadLen; i++) {
      char c = (char)payloadPtr[i];
      if (c != '\0') rest += c;
    }
    outType = "uri";
    outContent = prefix + rest;
  } 
  else if (tnf == 0x01 && typeStr == "T") {
    uint8_t statusByte = (payloadLen > 0) ? payloadPtr[0] : 0;
    uint8_t langLen = statusByte & 0x3F;
    String text = "";
    for (uint32_t i = 1 + langLen; i < payloadLen; i++) {
      char c = (char)payloadPtr[i];
      if (c != '\0') text += c; // กรองอักขระว่างทิ้ง ป้องกันปัญหาข้อความเพี้ยน
    }
    outType = "text";
    outContent = text;
  } 
  else {
    outType = "unknown";
    outContent = "";
  }

  return true;
}

void setupRpcMethods() {
  rpc.addMethod("ping", []() -> JsonVariant {
    rpcResultDoc.clear();
    rpcResultDoc["status"] = "success";
    rpcResultDoc["message"] = "pong";
    return rpcResultDoc.as<JsonVariant>();
  });

  rpc.addMethod("read", [](JsonVariantConst params) -> JsonVariant {
    (void)params;
    rpcResultDoc.clear();

    uint8_t check_uid[7];
    uint8_t check_len;
    bool cardDetectedNow = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, check_uid, &check_len, NFC_READ_TIMEOUT_MS);
    
    if (cardDetectedNow) {
      tagPresent = true;
      lastSeenTime = millis();
      String tempStr = "";
      for (uint8_t i = 0; i < check_len; i++) {
        if (check_uid[i] < 0x10) tempStr += "0";
        tempStr += String(check_uid[i], HEX);
      }
      tempStr.toUpperCase();
      currentUidStr = tempStr;
    }

    if (tagPresent && currentUidStr != "") {
      rpcResultDoc["status"] = "success";
      rpcResultDoc["uid"] = currentUidStr;

      String ndefType, ndefContent;
      if (readNdefFromNtag(ndefType, ndefContent)) {
        rpcResultDoc["content_type"] = ndefType;
        rpcResultDoc["content"] = ndefContent;
        rpcResultDoc["message"] = "Read NDEF successfully";
      } else {
        rpcResultDoc["content_type"] = "error";
        rpcResultDoc["content"] = "";
        rpcResultDoc["message"] = "Failed to read NDEF content";
      }
    } else {
      rpcResultDoc["status"] = "error";
      rpcResultDoc["message"] = "No NTAG tag detected. Please place tag on reader.";
    }

    return rpcResultDoc.as<JsonVariant>();
  });

  rpc.addMethod("write", [](JsonVariantConst params) -> JsonVariant {
    rpcResultDoc.clear();

    uint8_t check_uid[7];
    uint8_t check_len;
    bool cardDetectedNow = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, check_uid, &check_len, NFC_READ_TIMEOUT_MS);
    
    if (cardDetectedNow) {
      tagPresent = true;
      lastSeenTime = millis();
      String tempStr = "";
      for (uint8_t i = 0; i < check_len; i++) {
        if (check_uid[i] < 0x10) tempStr += "0";
        tempStr += String(check_uid[i], HEX);
      }
      tempStr.toUpperCase();
      currentUidStr = tempStr;
    }

    if (tagPresent && currentUidStr != "") {
      String dataToWrite = params["data"] | "";
      String recordType = params["type"] | "";

      bool looksLikeUri = dataToWrite.startsWith("http://")  ||
                           dataToWrite.startsWith("https://") ||
                           dataToWrite.startsWith("www.")     ||
                           dataToWrite.startsWith("tel:")     ||
                           dataToWrite.startsWith("mailto:");

      bool asUri;
      if (recordType == "uri") asUri = true;
      else if (recordType == "text") asUri = false;
      else asUri = looksLikeUri;

      bool ok = asUri ? writeNdefUriToNtag(dataToWrite) : writeNdefTextToNtag(dataToWrite);

      if (ok) {
        rpcResultDoc["status"] = "success";
        rpcResultDoc["message"] = "Written to NTAG215: " + dataToWrite;
      } else {
        rpcResultDoc["status"] = "error";
        rpcResultDoc["message"] = "Failed to write NDEF data";
      }
    } else {
      rpcResultDoc["status"] = "error";
      rpcResultDoc["message"] = "No NTAG tag detected. Please place tag on reader.";
    }

    return rpcResultDoc.as<JsonVariant>();
  });
}

void loop() {
  if (!client || !client.connected()) {
    WiFiClient newClient = tcpServer.available();
    if (newClient) {
      client = newClient;
      Serial.println("New client connected!");
    }
  }

  if (millis() - lastPollTime > 300) {
    lastPollTime = millis();

    uint8_t uid_buff[7];
    uint8_t uid_len;
    bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid_buff, &uid_len, NFC_READ_TIMEOUT_MS);

    if (success) {
      tagPresent = true;
      lastSeenTime = millis();

      String tempStr = "";
      for (uint8_t i = 0; i < uid_len; i++) {
        if (uid_buff[i] < 0x10) tempStr += "0";
        tempStr += String(uid_buff[i], HEX);
      }
      tempStr.toUpperCase();
      currentUidStr = tempStr;
    } else {
      if (millis() - lastSeenTime > 3000) {
        tagPresent = false;
        currentUidStr = "";
      }
    }
  }

  if (client && client.connected() && client.available()) {
    RpcSerialTransport transport(client);
    String response = rpc.handleRequest(transport);
    if (response.length() > 0) {
      Serial.println("RPC Response: " + response);
      transport.write(response);
    }
  }
}
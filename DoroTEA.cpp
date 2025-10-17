#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

#define SD_CS_PIN 5
#define I2S_BCK_PIN 27 
#define I2S_WS_PIN 26 
#define I2S_DATA_PIN 25

const char* ssid = "DOROTEA";
const char* password = "abacate1";

const char* API_SERVER_IP = "192.168.40.129";
const int API_SERVER_PORT = 5000;

WebServer server(80);
const char* DOWNLOAD_DIR = "/downloadable_files";

File fsUploadFile;

unsigned long lastHumorCheck = 0;
const long checkInterval = 3000;
String humorStatusUrl = "";
String latestHumor = "neutral";
int musicSource = 0;
bool isPlaying = false;
String currentMusicPath = "";

AudioFileSourceSD *fileSD = nullptr;
AudioGeneratorMP3 *mp3 = nullptr;
AudioOutputI2S *out = nullptr;

void audio_info(void *cbData, const char *info, bool is_metatdata, const char *name) {
    if (!is_metatdata) {
        Serial.printf("AUDIO INFO: %s\n", info);
    }
}

void initializeI2S() {
    if (!out) {
        out = new AudioOutputI2S();
        out->SetPinout(I2S_BCK_PIN, I2S_WS_PIN, I2S_DATA_PIN);
        out->SetGain(1.0); //volume
    }
}

void stopCurrentMusic() {
    if (mp3) {
        Serial.println("DEBUG: Parando a reproducao atual.");
        mp3->stop();
        delete mp3;
        mp3 = nullptr;
    }
    
    if (fileSD) {
        delete fileSD;
        fileSD = nullptr;
    }

    isPlaying = false;
    currentMusicPath = "";
    musicSource = 0; 
    Serial.println("DEBUG: Musica parada com sucesso.");
}

void playMusicByPath(String localPath, int source) {
    if (isPlaying && currentMusicPath.equalsIgnoreCase(localPath)) {
        return; 
    }
    
    if (source == 2 && musicSource == 1) { 
        Serial.println("INFO: Modo Manual ativo (1). Ignorando play automatico (2).");
        return;
    }
    
    stopCurrentMusic(); 

    if (!SD.exists(localPath)) {
        Serial.println("ERRO: Arquivo nao encontrado no SD: " + localPath);
        return;
    }

    initializeI2S(); 
    fileSD = new AudioFileSourceSD(localPath.c_str());
    
    if (!fileSD->isOpen()) {
        Serial.println("ERRO: Falha ao abrir AudioFileSourceSD: " + localPath);
        stopCurrentMusic();
        return;
    }

    mp3 = new AudioGeneratorMP3();
    mp3->RegisterMetadataCB(audio_info, nullptr);

    if (mp3->begin(fileSD, out)) { 
        isPlaying = true;
        currentMusicPath = localPath; 
        musicSource = source;
        Serial.printf("SUCESSO: Tocando musica local do SD: %s. Origem: %d\n", localPath.c_str(), source);
    } else {
        Serial.println("ERRO: Falha ao iniciar MP3 do SD: " + localPath);
        stopCurrentMusic(); 
    }
}

String resolveMusicIdToPath(String id) {
    if (id.equalsIgnoreCase("default") || id.length() == 0) {
        return "";
    }
    
    String musicId = id;

    if (!musicId.endsWith(".mp3") && !musicId.endsWith(".MP3")) {
        musicId += ".mp3";
    }
    
    String customPath = String(DOWNLOAD_DIR) + "/" + musicId;
    if (SD.exists(customPath)) {
        return customPath;
    }
    
    String defaultPath = "/" + musicId;
    if (SD.exists(defaultPath)) {
        return defaultPath;
    }
    
    Serial.println("AVISO: ID de musica nao mapeado ou nao encontrado no SD: " + id);
    return "";
}

void handleStatus() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String response = "{\"status\":\"OK\", \"mode\":" + String(musicSource) + ", \"playing\":" + (isPlaying ? "true" : "false") + ", \"path\":\"" + currentMusicPath + "\"}";
    server.send(200, "application/json", response);
}

void handleManualPlay() {
    server.sendHeader("Access-Control-Allow-Origin", "*");

    String musicId = server.arg("id"); 
    if (server.hasArg("plain")) { 
        String body = server.arg("plain");
        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, body);
        if (!error) {
            musicId = doc["id"].as<String>(); 
        }
    }

    if (musicId.length() > 0) {
        String path = resolveMusicIdToPath(musicId);
        
        if (path.length() > 0) {
            playMusicByPath(path, 1); 
            server.send(200, "text/plain", "OK: Tocando: " + path);
            Serial.println("COMANDO MANUAL: Iniciar " + path);
        } else {
            Serial.println("ERRO: ID/Caminho desconhecido para modo manual: " + musicId);
            server.send(404, "text/plain", "Not Found: ID/Caminho desconhecido.");
        }
    } else {
        server.send(400, "text/plain", "Bad Request: 'id' not provided.");
    }
}

void handleStop() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    stopCurrentMusic();
    Serial.println("COMANDO STOP RECEBIDO: Musica parada.");
    server.send(200, "text/plain", "OK: Music stopped.");
}

void handleFileUpload() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Headers", "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    
    if (server.uri() != "/upload") {
        return;
    }
    
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        String filePath = String(DOWNLOAD_DIR) + "/" + filename;
        
        Serial.printf("RECEBENDO: %s\n", filePath.c_str());

        if (SD.exists(filePath)) {
            SD.remove(filePath); 
        }
        
        fsUploadFile = SD.open(filePath, FILE_WRITE);
        if (!fsUploadFile) {
            Serial.println("ERRO: Falha ao abrir arquivo no SD para upload.");
            server.send(500, "text/plain", "Erro interno ao criar arquivo no SD.");
        }
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (fsUploadFile) {
            fsUploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (fsUploadFile) {
            fsUploadFile.close();
            Serial.printf("SUCESSO: Arquivo recebido e salvo em: %s, Tamanho: %d bytes\n", upload.filename.c_str(), upload.totalSize);
            server.send(200, "text/plain", "Upload Concluido!");
        } else {
            server.send(500, "text/plain", "Erro ao fechar o arquivo.");
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Serial.println("AVISO: Upload abortado pelo cliente.");
        if (fsUploadFile) fsUploadFile.close();
        server.send(400, "text/plain", "Upload abortado.");
    }
}

void applyHumorLogic(String humor, String selectedMusicId) {
    if (musicSource == 1) {
        Serial.println("INFO: MODO MANUAL ATIVO. Ignorando checagem de humor.");
        return; 
    }

    if (humor.equalsIgnoreCase("angry") || humor.equalsIgnoreCase("sad") || humor.equalsIgnoreCase("fear")) {
        
        String targetMusicPath = resolveMusicIdToPath(selectedMusicId);
        
        if (targetMusicPath.length() > 0) {
            playMusicByPath(targetMusicPath, 2); 
        } else {
            if (isPlaying && musicSource == 2) {
                stopCurrentMusic();
            }
            Serial.println("AVISO: Musica selecionada (ID: " + selectedMusicId + ") nao encontrada no SD.");
        }
    } 
    else { 
        if (isPlaying && musicSource == 2) { 
            stopCurrentMusic();
            Serial.println("ACAO: Humor NEUTRO/POSITIVO. Parando musica automatica.");
        }
    }
}

void fetchLatestHumor() {
    HTTPClient http;
    http.begin(humorStatusUrl);
    http.setTimeout(5000);
    
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument doc(512); 
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            String newHumor = doc["ultima_emocao"].as<String>(); 
            String selectedMusicId = doc["musica_selecionada"].as<String>(); 
            
            if (newHumor.length() > 0) {
                if (newHumor != latestHumor || (musicSource == 2 && !isPlaying)) { 
                    latestHumor = newHumor;
                    applyHumorLogic(latestHumor, selectedMusicId);
                }
            } else {
                Serial.println("API Humor Recebida: Nenhuma emocao valida.");
            }
        } else {
            Serial.println("ERRO: Falha ao desserializar JSON da API de humor. " + String(error.c_str()));
        }
    } else {
        Serial.println("ERRO: Falha na requisicao HTTP. Codigo: " + String(httpCode));
    }
    http.end();
}

void setup() {
    Serial.begin(115200);
    
    humorStatusUrl = "http://" + String(API_SERVER_IP) + ":" + String(API_SERVER_PORT) + "/ultima_emocao";
    
    WiFi.begin(ssid, password);
    Serial.print("Conectando ao WiFi...");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConectado! Endereco IP: " + WiFi.localIP().toString());

    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("ERRO: Falha ao inicializar o Cartao SD! Verifique o pino CS.");
    } else {
        Serial.println("SUCESSO: Cartao SD inicializado.");
        if (!SD.exists(DOWNLOAD_DIR)) {
            SD.mkdir(DOWNLOAD_DIR);
            Serial.println("Diretorio de downloads criado: " + String(DOWNLOAD_DIR));
        }
    }
    
    server.on("/", HTTP_OPTIONS, []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
        server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Cache-Control, Pragma");
        server.send(200, "text/plain", "");
    });

    server.on("/status", HTTP_GET, handleStatus);
    server.on("/play", HTTP_POST, handleManualPlay); 
    server.on("/play", HTTP_GET, handleManualPlay); 
    server.on("/stop", HTTP_POST, handleStop);
    server.on("/upload", HTTP_POST, []() {
        server.send(200, "text/plain", "Upload processado.");
    }, handleFileUpload); 

    server.begin();
    Serial.println("Servidor HTTP do ESP32 iniciado na porta 80.");
}

void loop() {
    server.handleClient(); 
    if (mp3 && mp3->isRunning()) {
        if (!mp3->loop()) { 
            stopCurrentMusic();
            Serial.println("INFO: A musica terminou de tocar.");
        }
    }
    
    unsigned long currentMillis = millis();
    if (currentMillis - lastHumorCheck >= checkInterval) {
        lastHumorCheck = currentMillis;
        fetchLatestHumor();
    }
}

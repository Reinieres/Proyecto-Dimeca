#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <AccelStepper.h>

// Pines
const int stepPin1 = 16; // Motor Paso 1 (Tornillo Dosificador de Tolva)
const int dirPin1  = 17;

const int stepPin2 = 18; // Motor Paso 2 (Agitador)
const int dirPin2  = 19; 

const int pinRelayDC = 4; // Relé del Motor DC (Aspersor centrífugo)
const int ledEstado = 5;

// Red
const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_server = "broker.hivemq.com";

WiFiClient espClient;
PubSubClient client(espClient);

// Piscina ID
int numeroPiscina = 1; 
String topicSensores = "camaronera/piscina" + String(numeroPiscina) + "/sensores";
String topicControl  = "camaronera/piscina" + String(numeroPiscina) + "/control";


// Configuramos AccelStepper con los nombres CORRECTOS de los pines
AccelStepper motorTolva(AccelStepper::DRIVER, stepPin1, dirPin1);
AccelStepper motorAgitador(AccelStepper::DRIVER, stepPin2, dirPin2);

// Variables de la "Receta" (Valores seguros por defecto)
float tiempoON  = 20.0; 
float tiempoOFF = 1.0;  
float velocidadTolva    = 500; 
float velocidadAgitador = 300;
int velocidadMotorDC    = 255; 

// Variables de control de la máquina de estados
unsigned long tiempoInicioFase = 0;
unsigned long ultimoEnvioTelemetria = 0;
bool sistemaIniciado = false;
bool motorGirando = false;

// Configuraciones de Red
void setup_wifi() {
  Serial.print("Conectando a Wokwi-GUEST");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Conectado!");
}

void reconnect() {
  while (!client.connected()) {
    // Generador de ID único para que HiveMQ no desconecte la placa
    String clientId = "ESP32_Reinier_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("Conectado al Broker MQTT (HiveMQ) de forma estable!");
      client.subscribe(topicControl.c_str());
    } else {
      delay(5000);
    }
  }
}

// Recepción de Ordenes de Node Red
void callback(char* topic, byte* payload, unsigned int length) {
  String mensaje = "";
  for (int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }
  
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, mensaje);

  if (!error) {
    String accion = doc["accion"];

    // Apagado
    if (accion == "APAGAR") {
      sistemaIniciado = false;
      motorGirando = false;
      
      analogWrite(pinRelayDC, 0); 
      motorTolva.setSpeed(0);     
      motorAgitador.setSpeed(0);  
      
      Serial.println(">>> SISTEMA APAGADO <<<");
    }
    
    // Iniciar o Aplicar cambios
    else if (accion == "INICIAR_CICLO") {
      
      // FILTRO DE SEGURIDAD: Solo actualiza si el valor es mayor a 0
      if (doc.containsKey("tiempo_on") && (float)doc["tiempo_on"] > 0) {
        tiempoON = doc["tiempo_on"];
      }
      if (doc.containsKey("tiempo_off") && (float)doc["tiempo_off"] > 0) {
        tiempoOFF = doc["tiempo_off"];
      }
      if (doc.containsKey("velocidad_1")) velocidadTolva    = doc["velocidad_1"];
      if (doc.containsKey("velocidad_2")) velocidadAgitador = doc["velocidad_2"];
      if (doc.containsKey("velocidad_dc")) velocidadMotorDC = doc["velocidad_dc"];
      
      // Aplicar velocidades
      motorTolva.setSpeed(velocidadTolva);
      motorAgitador.setSpeed(velocidadAgitador);

      if (!sistemaIniciado) {
        sistemaIniciado = true;
        motorGirando = true;
        tiempoInicioFase = millis();
        analogWrite(pinRelayDC, velocidadMotorDC);
        Serial.println(">>> NUEVO CICLO INICIADO <<<");
      } 
      else {
        if (motorGirando) {
          analogWrite(pinRelayDC, velocidadMotorDC); 
        }
        Serial.println(">>> PARAMETROS ACTUALIZADOS ESTABLEMENTE <<<");
      }
    }
  }
}

// void setup
void setup() {
  Serial.begin(115200);
  
  // Configuramos el pin del Relé como salida
  pinMode(pinRelayDC, OUTPUT);
  pinMode(ledEstado, OUTPUT);

  // Configuración de límites de los motores de paso
  motorTolva.setMaxSpeed(2000); 
  motorTolva.setSpeed(0);
  
  motorAgitador.setMaxSpeed(2000);
  motorAgitador.setSpeed(0);

  setup_wifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
}

// Conexión con red
void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); 

  //LA MÁQUINA DE ESTADOS
  if (sistemaIniciado) {
    
    // ESTADO 1: FASE ACTIVA (MOTORES ON)
    if (motorGirando) {
      
      // Encendemos el aspersor (Relé del Motor DC)
      analogWrite(pinRelayDC, velocidadMotorDC);
      digitalWrite(ledEstado, HIGH);
      // Hacemos avanzar ambos motores de paso sin usar delay()
      motorTolva.runSpeed(); 
      motorAgitador.runSpeed(); 

      // Evaluamos el tiempo de apagado
      if (millis() - tiempoInicioFase >= (tiempoON * 1000UL)) {
        motorGirando = false;
        tiempoInicioFase = millis();
        Serial.println("Fase activa terminada. Entrando en pausa...");
      }
    } 
    
    // ESTADO 2: FASE DE ESPERA (MOTORES OFF)
    else {
      // Apagamos el aspersor (Relé del Motor DC)
      analogWrite(pinRelayDC, 0);
      digitalWrite(ledEstado, LOW);
      
      // Evaluamos el tiempo de encendido para el próximo ciclo
      if (millis() - tiempoInicioFase >= (tiempoOFF * 60000UL)) {
        motorGirando = true;
        tiempoInicioFase = millis();
        Serial.println("Pausa terminada. Reiniciando ciclo de alimentación...");
      }
    }
  }
  // ========================================================

  // ENVÍO DE TELEMETRÍA
  if (millis() - ultimoEnvioTelemetria > 2000) {
    ultimoEnvioTelemetria = millis();
    
    float voltaje = 23.5 + (random(0, 10) / 10.0);
    float corrienteDC = motorGirando ? (2.0 + (random(0, 5) / 10.0)) : 0.0;
    float corrientePaso1 = motorGirando ? (1.5 + (random(0, 3) / 10.0)) : 0.0;
    float corrientePaso2 = motorGirando ? (1.2 + (random(0, 3) / 10.0)) : 0.0;
    int rpmPaso1 = motorGirando ? velocidadTolva : 0;
    int rpmPaso2 = motorGirando ? velocidadAgitador : 0;

    String payload = "{";
    payload += "\"voltaje\": " + String(voltaje, 1) + ", ";
    payload += "\"corriente_dc\": " + String(corrienteDC, 1) + ", ";
    payload += "\"corriente_paso1\": " + String(corrientePaso1, 1) + ", ";
    payload += "\"corriente_paso2\": " + String(corrientePaso2, 1) + ", ";
    payload += "\"rpm_paso1\": " + String(rpmPaso1) + ", ";
    payload += "\"rpm_paso2\": " + String(rpmPaso2);
    payload += "}";

    client.publish(topicSensores.c_str(), payload.c_str());
  }
}

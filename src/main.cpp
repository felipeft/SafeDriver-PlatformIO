#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// Configuração do Wi-Fi
const char* ssid = "iPhone de Felipe";
const char* password = "23082001";

// Instancia servidor na porta 80
WebServer server(80);

// Pinos do Motor 1 (Esquerdo)
const int pwmMotor1 = 25;
const int frenteMotor1 = 26;
const int trasMotor1 = 27;

// Pinos do Motor 2 (Direito)
const int pwmMotor2 = 33;
const int frenteMotor2 = 12;
const int trasMotor2 = 13;

// Variáveis globais de controle do Motor
int velocidadeCrua = 0;    // Valor bruto do slider (-255 a 255 para manual, 0 a 255 para autônomo)
int velocidadeMapeada = 0; // Velocidade ajustada para os motores (90 a 255)
float currentJoystickX = 0.0; // Posição X do joystick (-1.0 a 1.0)
bool modoAutonomoAtivo = false;     // true = AUTO, false = MANUAL
bool isAccelButtonActive = false;   // Novo: Estado do botão acelerar (true = pressionado, false = solto)

// --- Configuração dos Sensores Ultrassônicos ---
// Pinos do Sensor Frontal
const int TRIG_PIN_FRONT = 23; 
const int ECHO_PIN_FRONT = 22; 
// Pinos do Sensor Traseiro
const int TRIG_PIN_REAR = 2; 
const int ECHO_PIN_REAR = 15; 
// Pinos do Sensor Direito
const int TRIG_PIN_RIGHT = 4; 
const int ECHO_PIN_RIGHT = 18; 
// Pinos do Sensor Esquerdo
const int TRIG_PIN_LEFT = 19; 
const int ECHO_PIN_LEFT = 21; 

// Nova distância de parada para o modo autônomo
const int DISTANCIA_PARADA_AUTONOMO_CM = 50; 

// Variáveis de leitura dos sensores
int currentDistanceFrontCm = 0; 
int currentDistanceRearCm = 0;
int currentDistanceRightCm = 0;
int currentDistanceLeftCm = 0;

unsigned long lastSensorReadTime = 0;
const long sensorReadInterval = 150; // Intervalo entre o início de cada ciclo de leitura de TODOS os sensores

// Constants for joystick control
const float JOYSTICK_DEADZONE_X = 0.15; // Deadzone for horizontal joystick. Normalized 0 to 1.
const float PIVOT_START_THRESHOLD_NORM = 0.5; // Normalized point in turning range where pivot starts (0 to 1)

// --- Declarações de Funções ---
String getHtml();
void parar();
void frente();
void tras(); // Nova função para ir para trás
void virarDireita(); // Nova função para virar à direita
void virarEsquerda(); // Nova função para virar à esquerda
int lerSensorUltrassonico(int trigPin, int echoPin);
void handleDataUpdate();

// Função para mapear o valor absoluto do slider para a faixa de velocidade desejada
// Agora, o valor de entrada (x) será o ABSOLUTO da velocidadeCrua para o modo manual
// E o próprio velocidadeCrua para o modo autônomo (já que será 0-255)
long mapValue(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min; 
}

void setup() {
  Serial.begin(115200);

  // Configuração dos pinos dos motores
  pinMode(frenteMotor1, OUTPUT);
  pinMode(trasMotor1, OUTPUT);
  pinMode(frenteMotor2, OUTPUT);
  pinMode(trasMotor2, OUTPUT);

  ledcAttachPin(pwmMotor1, 0); 
  ledcAttachPin(pwmMotor2, 1); 
  ledcSetup(0, 1000, 8);      
  ledcSetup(1, 1000, 8);      

  // Configuração dos pinos dos sensores ultrassônicos
  pinMode(TRIG_PIN_FRONT, OUTPUT);
  pinMode(ECHO_PIN_FRONT, INPUT);
  pinMode(TRIG_PIN_REAR, OUTPUT);
  pinMode(ECHO_PIN_REAR, INPUT);
  pinMode(TRIG_PIN_RIGHT, OUTPUT);
  pinMode(ECHO_PIN_RIGHT, INPUT);
  pinMode(TRIG_PIN_LEFT, OUTPUT);
  pinMode(ECHO_PIN_LEFT, INPUT);

  // Conexão Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi conectado!");
  Serial.print("IP do ESP32: ");
  Serial.println(WiFi.localIP());

  // Rotas do servidor web
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getHtml());
  });

  server.on("/controle", HTTP_GET, []() {
    // Processa a mudança de modo ANTES de processar a velocidade/direção
    // para que a lógica de mapeamento de velocidade seja correta
    if (server.hasArg("mode")) {
      String modeStr = server.arg("mode");
      bool newMode = (modeStr == "autonomo");
      if (newMode != modoAutonomoAtivo) {
        modoAutonomoAtivo = newMode;
        if (modoAutonomoAtivo) {
          Serial.println("Modo: AUTONOMO ATIVADO");
        } else {
          Serial.println("Modo: MANUAL ATIVADO");
          currentJoystickX = 0.0; // Reseta joystick X ao voltar para manual
          isAccelButtonActive = false; // Reseta o estado do botão ao voltar para manual
        }
        parar(); // Para o carro ao trocar de modo
      }
    }

    // Processa velocidade
    if (server.hasArg("vel")) {
      velocidadeCrua = server.arg("vel").toInt();
      // Lógica de mapeamento de velocidade adaptada ao modo
      if (velocidadeCrua == 0) {
        velocidadeMapeada = 0;
      } else {
        if (modoAutonomoAtivo) {
          // No modo autônomo, velocidadeCrua já é 0-255, mapeamos direto
          velocidadeMapeada = mapValue(velocidadeCrua, 0, 255, 90, 255);
        } else {
          // No modo manual, usamos o valor absoluto e mapeamos
          velocidadeMapeada = mapValue(abs(velocidadeCrua), 0, 255, 90, 255);
        }
      }
    }

    // Processa a posição X do joystick - APENAS no modo MANUAL
    if (server.hasArg("joyX") && !modoAutonomoAtivo) {
      currentJoystickX = server.arg("joyX").toFloat();
    } else if (modoAutonomoAtivo) {
      currentJoystickX = 0.0; // Zera o joystick X no modo autônomo
    }

    // Processa o estado do botão de aceleração - APENAS no modo MANUAL
    if (server.hasArg("accelBtnState") && !modoAutonomoAtivo) {
      isAccelButtonActive = (server.arg("accelBtnState") == "true");
    }
    
    server.send(200, "text/plain", "OK");
  });

  server.on("/data", HTTP_GET, handleDataUpdate);

  server.begin();
  Serial.println("Servidor web iniciado");
}

void loop() {
  server.handleClient(); 
  
  // Leitura sequencial dos 4 sensores com intervalos curtos entre eles para evitar interferência
  // O sensorReadInterval será o tempo entre o início de cada ciclo de leitura de TODOS os sensores
  if (millis() - lastSensorReadTime >= sensorReadInterval) {
    currentDistanceFrontCm = lerSensorUltrassonico(TRIG_PIN_FRONT, ECHO_PIN_FRONT);
    delay(5); // Pequeno atraso entre leituras para evitar crosstalk
    currentDistanceRearCm = lerSensorUltrassonico(TRIG_PIN_REAR, ECHO_PIN_REAR);
    delay(5);
    currentDistanceRightCm = lerSensorUltrassonico(TRIG_PIN_RIGHT, ECHO_PIN_RIGHT);
    delay(5);
    currentDistanceLeftCm = lerSensorUltrassonico(TRIG_PIN_LEFT, ECHO_PIN_LEFT);
    
    lastSensorReadTime = millis(); // Atualiza o tempo da última leitura *após* ler todos os sensores

    Serial.print("Distancias: F="); Serial.print(currentDistanceFrontCm);
    Serial.print(" R="); Serial.print(currentDistanceRearCm);
    Serial.print(" D="); Serial.print(currentDistanceRightCm);
    Serial.print(" E="); Serial.print(currentDistanceLeftCm);
    Serial.println(" cm");
  }
  
  if (modoAutonomoAtivo) {
    // Lógica para o modo AUTÔNOMO
    Serial.print("MODO: AUTONOMO, "); 
    Serial.print("Velocidade Crua: "); Serial.print(velocidadeCrua); 
    Serial.print(", Velocidade Mapeada: "); Serial.print(velocidadeMapeada); 

    // No modo autônomo, só se move para frente com velocidade positiva
    if (velocidadeMapeada > 0) { // Se o slider está em "frente" (valor positivo e mapeado)
        // Prioridade 1: Ir para frente se o caminho estiver livre
        // Se o sensor frontal identifica um objeto a 30 cm ou menos OU se a leitura falhou (-1)
        if (currentDistanceFrontCm == -1 || currentDistanceFrontCm <= DISTANCIA_PARADA_AUTONOMO_CM) {
            Serial.print("AUTONOMO: OBSTACULO FRONTAL DETECTADO! Parado a ");
            if (currentDistanceFrontCm != -1) {
              Serial.print(currentDistanceFrontCm);
            } else {
              Serial.print("Falha na leitura");
            }
            Serial.println(" cm. Avaliando rota alternativa.");
            
            parar(); // Para o carro imediatamente ao detectar obstáculo frontal

            // Considerar -1 (leitura inválida) como um obstáculo para fins de decisão
            int distLeft = (currentDistanceLeftCm == -1 || currentDistanceLeftCm <= DISTANCIA_PARADA_AUTONOMO_CM) ? 0 : currentDistanceLeftCm;
            int distRight = (currentDistanceRightCm == -1 || currentDistanceRightCm <= DISTANCIA_PARADA_AUTONOMO_CM) ? 0 : currentDistanceRightCm;
            int distRear = (currentDistanceRearCm == -1 || currentDistanceRearCm <= DISTANCIA_PARADA_AUTONOMO_CM) ? 0 : currentDistanceRearCm;

            // Prioridade 2: Virar para o lado com mais espaço
            if (distLeft > 0 && distLeft >= distRight) { // Virar Esquerda se tiver mais espaço ou igual ao direito (e > 0)
                Serial.println("AUTONOMO: Virando à esquerda.");
                virarEsquerda();
            } else if (distRight > 0 && distRight > distLeft) { // Virar Direita se tiver mais espaço
                Serial.println("AUTONOMO: Virando à direita.");
                virarDireita();
            } else if (distRear > 0) { // Prioridade 3: Ir para trás se laterais bloqueadas
                Serial.println("AUTONOMO: Movendo para trás.");
                tras(); 
            } else { // Pior caso: todas as direções bloqueadas ou falhas de leitura
                parar(); // Permanece imovel
                Serial.println("AUTONOMO: BLOQUEADO EM TODAS AS DIREÇÕES. Parado.");
            }
        } else { // Não há obstáculo frontal dentro da distância de parada
          frente(); 
          Serial.print("AUTONOMO: Indo para frente. Distancia Frontal: "); 
          Serial.print(currentDistanceFrontCm);
          Serial.print(" cm. Velocidade Mapeada: ");
          Serial.println(velocidadeMapeada);
        }
    } 
    else { // Se velocidadeMapeada == 0 (slider em zero no modo autônomo)
        parar();
        Serial.println("AUTONOMO: Parado (velocidade zero do slider)."); 
    }
  } else {
    // Lógica para o modo MANUAL (prioridade total do comando web)
    Serial.print("MODO: MANUAL, "); 
    Serial.print("Velocidade Crua: "); Serial.print(velocidadeCrua); 
    Serial.print(", Velocidade Mapeada: "); Serial.print(velocidadeMapeada); 
    Serial.print(", Joystick X: "); Serial.print(currentJoystickX); 
    Serial.print(", Botao Acelerar Ativo: "); Serial.println(isAccelButtonActive ? "SIM" : "NAO");
    
    // NOVO: Somente move os motores se o botão de aceleração estiver pressionado
    if (isAccelButtonActive) {
        int actualSpeedLeft = 0;
        int actualSpeedRight = 0;
        bool directionLeftForward = false;
        bool directionRightForward = false;

        float baseSpeed = (float)velocidadeMapeada; // Base speed for motors (0-255)
        float joystickX = currentJoystickX; // -1.0 (full left) to 1.0 (full right)

        // Handle vertical slider deadzone: if velocidadeCrua is 0, stop motors regardless of joystickX
        if (velocidadeCrua == 0) {
            parar();
            return; // Exit this section if no speed is commanded
        }

        // Initialize directions based on velocidadeCrua
        directionLeftForward = (velocidadeCrua > 0);
        directionRightForward = (velocidadeCrua > 0);

        // Calculate effective joystick X considering deadzone
        float effectiveJoystickX = 0;
        if (abs(joystickX) > JOYSTICK_DEADZONE_X) {
            // Map joystickX from (DEADZONE, 1) to (0, 1) or (-1, -DEADZONE) to (-1, 0)
            effectiveJoystickX = (joystickX - (joystickX > 0 ? JOYSTICK_DEADZONE_X : -JOYSTICK_DEADZONE_X)) / (1.0 - JOYSTICK_DEADZONE_X);
        }
        // Now effectiveJoystickX ranges roughly from -1.0 to 1.0, with 0 for the deadzone.

        // Straight movement (within deadzone of joystickX or no turn commanded)
        if (abs(effectiveJoystickX) < 0.001) { // Check against a small epsilon for float comparison
            actualSpeedLeft = baseSpeed;
            actualSpeedRight = baseSpeed;
        } else if (effectiveJoystickX < 0) { // Turning Left
            actualSpeedRight = baseSpeed; // Right motor always runs at base speed

            if (abs(effectiveJoystickX) < PIVOT_START_THRESHOLD_NORM) { // Wide turn left
                // Left motor speed reduces as effectiveJoystickX moves towards -PIVOT_START_THRESHOLD_NORM
                actualSpeedLeft = baseSpeed * (1.0 - (abs(effectiveJoystickX) / PIVOT_START_THRESHOLD_NORM));
            } else { // Extreme pivot left
                // Left motor reverses. Speed increases as joystick moves further left (towards -1.0)
                directionLeftForward = !directionLeftForward; // Toggle direction

                // Normalize effectiveJoystickX for the pivot range
                // Input range: PIVOT_START_THRESHOLD_NORM to 1.0 (absolute value)
                // Output speed: 0 to baseSpeed
                float pivotNormalizedFactor = (abs(effectiveJoystickX) - PIVOT_START_THRESHOLD_NORM) / (1.0 - PIVOT_START_THRESHOLD_NORM);
                actualSpeedLeft = baseSpeed * pivotNormalizedFactor;
            }
        } else { // Turning Right (effectiveJoystickX > 0)
            actualSpeedLeft = baseSpeed; // Left motor always runs at base speed

            if (effectiveJoystickX < PIVOT_START_THRESHOLD_NORM) { // Wide turn right
                actualSpeedRight = baseSpeed * (1.0 - (effectiveJoystickX / PIVOT_START_THRESHOLD_NORM));
            } else { // Extreme pivot right
                // Right motor reverses. Speed increases as joystick moves further right (towards 1.0)
                directionRightForward = !directionRightForward; // Toggle direction

                // Normalize effectiveJoystickX for the pivot range
                // Input range: PIVOT_START_THRESHOLD_NORM to 1.0
                // Output speed: 0 to baseSpeed
                float pivotNormalizedFactor = (effectiveJoystickX - PIVOT_START_THRESHOLD_NORM) / (1.0 - PIVOT_START_THRESHOLD_NORM);
                actualSpeedRight = baseSpeed * pivotNormalizedFactor;
            }
        }

        // Ensure speeds are within 0-255 range
        actualSpeedLeft = constrain(actualSpeedLeft, 0, 255);
        actualSpeedRight = constrain(actualSpeedRight, 0, 255);

        // Apply the calculated speeds and directions to the motors
        digitalWrite(frenteMotor1, directionLeftForward ? HIGH : LOW);
        digitalWrite(trasMotor1, directionLeftForward ? LOW : HIGH);
        ledcWrite(0, actualSpeedLeft);

        digitalWrite(frenteMotor2, directionRightForward ? HIGH : LOW);
        digitalWrite(trasMotor2, directionRightForward ? LOW : HIGH);
        ledcWrite(1, actualSpeedRight);

    } else { // Botão acelerar NÃO está pressionado
        parar();
    }
  }
}

// Funções de controle de motor (sem alterações, pois já usam velocidadeMapeada)
void parar() {
  digitalWrite(frenteMotor1, LOW);
  digitalWrite(trasMotor1, LOW);
  digitalWrite(frenteMotor2, LOW);
  digitalWrite(trasMotor2, LOW);
  ledcWrite(0, 0); 
  ledcWrite(1, 0); 
}

void frente() {
  digitalWrite(frenteMotor1, HIGH);
  digitalWrite(trasMotor1, LOW);
  digitalWrite(frenteMotor2, HIGH);
  digitalWrite(trasMotor2, LOW);
  ledcWrite(0, velocidadeMapeada); 
  ledcWrite(1, velocidadeMapeada); 
}

// Implementação das novas funções de movimento
void tras() {
  digitalWrite(frenteMotor1, LOW);
  digitalWrite(trasMotor1, HIGH);
  digitalWrite(frenteMotor2, LOW);
  digitalWrite(trasMotor2, HIGH);
  ledcWrite(0, velocidadeMapeada); 
  ledcWrite(1, velocidadeMapeada); 
}

void virarDireita() {
  // Para virar à direita no lugar (pivot), motor esquerdo para frente, motor direito para trás
  digitalWrite(frenteMotor1, HIGH); // Motor Esquerdo para frente
  digitalWrite(trasMotor1, LOW);
  digitalWrite(frenteMotor2, LOW); // Motor Direito para trás
  digitalWrite(trasMotor2, HIGH);
  ledcWrite(0, velocidadeMapeada); 
  ledcWrite(1, velocidadeMapeada); 
}

void virarEsquerda() {
  // Para virar à esquerda no lugar (pivot), motor esquerdo para trás, motor direito para frente
  digitalWrite(frenteMotor1, LOW); // Motor Esquerdo para trás
  digitalWrite(trasMotor1, HIGH);
  digitalWrite(frenteMotor2, HIGH); // Motor Direito para frente
  digitalWrite(trasMotor2, LOW);
  ledcWrite(0, velocidadeMapeada); 
  ledcWrite(1, velocidadeMapeada); 
}

int lerSensorUltrassonico(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000); 

  if (duration == 0) { 
      return -1; // Indica que a leitura falhou (timeout)
  }

  int distanceCm = duration / 58;

  if (distanceCm >= 2 && distanceCm <= 400) { // Range típico do HC-SR04
    return distanceCm;
  } else {
    return -1; // Fora do range válido ou erro
  }
}

void handleDataUpdate() {
  StaticJsonDocument<256> doc; // Aumentado o tamanho para acomodar mais dados dos sensores
  doc["distanceFront"] = (currentDistanceFrontCm == -1) ? "---" : String(currentDistanceFrontCm);
  doc["distanceRear"] = (currentDistanceRearCm == -1) ? "---" : String(currentDistanceRearCm);
  doc["distanceRight"] = (currentDistanceRightCm == -1) ? "---" : String(currentDistanceRightCm);
  doc["distanceLeft"] = (currentDistanceLeftCm == -1) ? "---" : String(currentDistanceLeftCm);
  doc["mode"] = modoAutonomoAtivo ? "autonomo" : "manual";

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}
String getHtml() {
  return R"rawliteral(
    <!DOCTYPE html>
<html>
<head>
    <title>Controle do Carrinho</title>
    <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, minimum-scale=1, user-scalable=no, viewport-fit=cover">
    <meta charset="UTF-8">
    <style>
        /* Estilos para limitar a proporção da tela */
        body {
            font-family: Arial, sans-serif;
            text-align: center;
            margin: 0;
            /* Adiciona safe-area-insets para iOS */
            padding-top: constant(safe-area-inset-top);
            padding-top: env(safe-area-inset-top);
            padding-right: constant(safe-area-inset-right);
            padding-right: env(safe-area-inset-right);
            padding-bottom: constant(safe-area-inset-bottom);
            padding-bottom: env(safe-area-inset-bottom);
            padding-left: constant(safe-area-inset-left);
            padding-left: env(safe-area-inset-left);

            background-color: #f0f0f0;
            display: flex;
            justify-content: center;
            align-items: center;
            min-height: 100vh;
            box-sizing: border-box;
            overflow: hidden; /* Garante que o conteúdo não vaze fora da proporção */
            font-size: 3vw; /* Tamanho base da fonte responsivo */

            /* Evita rolagem elástica em iOS que pode interferir com o layout */
            -webkit-overflow-scrolling: touch;
        }

        .viewport-limiter {
            width: 100vw;
            max-width: calc(100vh * (19.5 / 9));
            height: 100vh;
            max-height: calc(100vw * (9 / 19.5));

            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: space-between;

            background-color: #ffffff;
            box-shadow: 0 0 10px rgba(0,0,0,0.2);
            box-sizing: border-box;
            padding: 2vh 2vw;
            /* **ALTERAÇÃO AQUI**: Reduzido o valor para "subir" mais o conteúdo */
            padding-bottom: calc(2vh + env(safe-area-inset-bottom) + 5px); /* Valores menores para subir mais */
            overflow: hidden;
        }

        h1 {
            color: #333;
            margin-bottom: 2vh;
            font-size: 1.5em;
        }

        .mode-toggle {
            display: flex;
            justify-content: center;
            align-items: center;
            margin-bottom: 2vh;
            width: 100%;
        }
        .mode-toggle label {
            margin: 0 1vw;
            font-size: 0.8em;
            color: #555;
        }
        .toggle-switch {
            position: relative;
            display: inline-block;
            --switch-height: 4.5vh; /* Variável para altura do switch */
            --switch-width: calc(var(--switch-height) * 1.8); /* Largura baseada na altura para proporção */

            width: var(--switch-width);
            height: var(--switch-height);
            min-width: 50px;
            min-height: 28px;
            max-width: 60px;
            max-height: 34px;
        }
        .toggle-switch input {
            opacity: 0;
            width: 0;
            height: 0;
        }
        .slider-round {
            position: absolute;
            cursor: pointer;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background-color: #ccc;
            -webkit-transition: .4s;
            transition: .4s;
            border-radius: 999px;
            display: flex;
            align-items: center;
            justify-content: flex-start;
            padding: 2px;
            box-sizing: border-box;
        }
        .slider-round:before {
            position: absolute;
            content: "";
            height: 0;
            width: 0;
            left: 0;
            bottom: 0;
            background-color: white;
            -webkit-transition: .4s;
            transition: .4s;
            border-radius: 50%;
        }
        input:checked + .slider-round {
            background-color: #2196F3;
            justify-content: flex-end;
        }
        input:focus + .slider-round {
            box-shadow: 0 0 1px #2196F3;
        }

        .main-control-area {
            display: flex;
            justify-content: space-evenly;
            align-items: flex-start;
            width: 100%;
            max-width: 900px;
            flex-grow: 1;
            padding: 1vh 1vw;
            box-sizing: border-box;
            flex-wrap: wrap;
            align-content: center;
        }

        /* Estilo para o Joystick */
        .joystick-container {
            position: relative;
            width: 30vh; /* Changed from 40vh */
            height: 30vh; /* Changed from 40vh */
            min-width: 220px; /* Changed from 280px */
            min-height: 220px; /* Changed from 280px */
            max-width: 320px; /* Changed from 400px */
            max-height: 320px; /* Changed from 400px */
            border: 2px solid #555;
            border-radius: 50%;
            background-color: #e0e0e0;
            display: flex;
            justify-content: center;
            align-items: center;
            flex-shrink: 0;
            -webkit-touch-callout: none;
            -webkit-user-select: none;
            -khtml-user-select: none;
            -moz-user-select: none;
            -ms-user-select: none;
            user-select: none;
            margin-right: 2vw;
            margin-bottom: 2vh;
            touch-action: none; /* Adicionado para priorizar eventos de toque */
        }
        .joystick-handle {
            width: 11vh; /* Changed from 15vh */
            height: 11vh; /* Changed from 15vh */
            min-width: 75px; /* Changed from 100px */
            min-height: 75px; /* Changed from 100px */
            max-width: 120px; /* Changed from 150px */
            max-height: 120px; /* Changed from 150px */
            background-color: #007bff;
            border-radius: 50%;
            position: absolute;
            cursor: grab;
            touch-action: none;
            box-shadow: 2px 2px 5px rgba(0,0,0,0.3);
        }

        /* Container para o slider e os botões de acelerar/freio */
        .accel-group-container {
            display: flex;
            flex-direction: row;
            align-items: center;
            gap: 5vw;
            min-height: 30vh;
            position: relative;
            margin-left: auto;
            flex-wrap: wrap;
            justify-content: center;
        }

        /* Container para os botões de Acelerar e Freio */
        .button-group {
            display: flex;
            flex-direction: row;
            gap: 1.5vw;
            margin-right: 1vw;
            flex-shrink: 0;
            margin-bottom: 2vh;
        }

        /* Estilo para o Slider Vertical (Acelerador) */
        .vertical-slider-container {
            display: flex;
            flex-direction: column;
            align-items: center;
            position: relative;
            flex-shrink: 0;
            width: 10vw;
            height: 35vh;
            min-width: 60px;
            min-height: 200px;
            max-height: 300px;
            justify-content: center;
            overflow: hidden;
        }
        #accel_slider {
            -webkit-appearance: none;
            width: 30vh;
            height: 2vw;
            min-width: 180px;
            min-height: 10px;
            max-width: 250px;
            max-height: 15px;
            outline: none;
            opacity: 0.7;
            transition: opacity .2s;
            border-radius: 5px;
            transform: translate(-50%, -50%) rotate(270deg);
            position: absolute;
            top: 50%;
            left: 50%;
        }

        /* Estilos específicos para o background do slider */
        #accel_slider.manual-mode-slider {
            background: linear-gradient(to right, #f44336 0%, #f44336 49%, #ccc 49%, #ccc 51%, #4CAF50 51%, #4CAF50 100%);
        }
        #accel_slider.autonomous-mode-slider {
            background: linear-gradient(to right, #ccc 0%, #ccc 1%, #4CAF50 1%, #4CAF50 100%);
        }

        /* Estilização do thumb para cross-browser compatibility */
        #accel_slider::-webkit-slider-thumb {
            -webkit-appearance: none;
            appearance: none;
            width: 4vh;
            height: 4vh;
            min-width: 28px;
            min-height: 28px;
            max-width: 35px;
            max-height: 35px;
            border-radius: 50%;
            background: #007bff;
            cursor: grab;
            box-shadow: 0px 0px 5px rgba(0,0,0,0.3);
        }
        #accel_slider::-moz-range-thumb {
            width: 4vh;
            height: 4vh;
            min-width: 28px;
            min-height: 28px;
            max-width: 35px;
            max-height: 35px;
            border-radius: 50%;
            background: #007bff;
            cursor: grab;
            box-shadow: 0px 0px 5px rgba(0,0,0,0.3);
        }
        #accel_slider::-ms-thumb {
            width: 4vh;
            height: 4vh;
            min-width: 28px;
            min-height: 28px;
            max-width: 35px;
            max-height: 35px;
            border-radius: 50%;
            background: #007bff;
            cursor: grab;
            box-shadow: 0px 0px 5px rgba(0,0,0,0.3);
        }

        /* Estilo da região morta do slider */
        #accel_slider.deadzone-thumb::-webkit-slider-thumb {
            background: #999;
        }
        #accel_slider.deadzone-thumb::-moz-range-thumb {
            background: #999;
        }
        #accel_slider.deadzone-thumb::-ms-thumb {
            background: #999;
        }

        /* Botões de Acelerar e Freio */
        .control-button {
            padding: 2vh 3vw;
            font-size: 1em;
            color: white;
            border: none;
            border-radius: 10px;
            cursor: pointer;
            box-shadow: 2px 2px 5px rgba(0,0,0,0.2);
            transition: background-color 0.1s ease;
            -webkit-tap-highlight-color: transparent;
            width: 15vw;
            min-width: 100px;
            max-width: 150px;
        }
        #accelButton {
            background-color: #2196F3;
        }
        #accelButton:active {
            background-color: #0d8bf2;
            box-shadow: inset 1px 1px 3px rgba(0,0,0,0.3);
        }
        #accelButton:hover { background-color: #0b7dda; }

        #brakeButton {
            background-color: #f44336;
        }
        #brakeButton:active {
            background-color: #da190b;
            box-shadow: inset 1px 1px 3px rgba(0,0,0,0.3);
        }
        #brakeButton:hover { background-color: #cc0000; }

        /* Informações do sensor e modo */
        .info-display {
            font-size: 0.8em;
            color: #555;
            flex-shrink: 0;
            padding-bottom: 0.5vh;
        }
        .info-display p {
            margin: 0.5vh 0; /* Espaçamento entre as linhas de distância */
        }
        .distance-value { /* Nova classe para os spans de valor */
            font-size: 1.2em;
            font-weight: bold;
            color: #007bff;
        }

        /* Media Queries para responsividade */
        @media (max-width: 768px) and (orientation: portrait) {
            body {
                font-size: 4vw;
            }
            .main-control-area {
                flex-direction: column;
                align-items: center;
                gap: 3vh;
                justify-content: center;
            }
            .joystick-container {
                order: 1;
                width: 40vw; /* Changed from 50vw */
                height: 40vw; /* Changed from 50vw */
                min-width: 220px; /* Changed from 280px */
                min-height: 220px; /* Changed from 280px */
                margin-right: 0;
                margin-bottom: 3vh;
            }
            .joystick-handle {
                width: 15vw; /* Changed from 20vw */
                height: 15vw; /* Changed from 20vw */
                min-width: 75px; /* Changed from 100px */
                min-height: 75px; /* Changed from 100px */
            }
            .accel-group-container {
                order: 2;
                flex-direction: column;
                gap: 2vh;
                min-height: auto;
                align-items: center;
                margin-left: 0;
            }
            .button-group {
                flex-direction: row;
                margin-right: 0;
                gap: 3vw;
            }
            .vertical-slider-container {
                margin-left: 0;
                width: 80%;
                height: 8vw;
                min-width: 200px;
                min-height: 40px;
            }
            #accel_slider {
                transform: rotate(0deg);
                width: 100%;
                height: 1.5vh;
                min-width: 200px;
                position: relative;
                top: auto;
                left: auto;
                margin: 0;
                transform: none;
            }
            #accel_slider::-webkit-slider-thumb,
            #accel_slider::-moz-range-thumb,
            #accel_slider::-ms-thumb {
                width: 6vw;
                height: 6vw;
                min-width: 28px;
                min-height: 28px;
            }
            .control-button {
                padding: 2.5vh 4vw;
                width: 25vw;
            }
            .mode-toggle label {
                font-size: 1em;
            }
            .toggle-switch {
                --switch-height: 8vw;
                --switch-width: calc(var(--switch-height) * 1.8);
                width: var(--switch-width);
                height: var(--switch-height);
                min-width: 50px;
                min-height: 28px;
            }
            .slider-round {
                display: flex;
                align-items: center;
                justify-content: flex-start;
                padding: 2px;
                box-sizing: border-box;
            }
            .slider-round:before {
                height: 0;
                width: 0;
                left: 0;
                bottom: 0;
            }
            input:checked + .slider-round {
                justify-content: flex-end;
            }
            .viewport-limiter {
                /* **ALTERAÇÃO AQUI**: Reduzido o valor em portrait mode também para "subir" mais o conteúdo */
                padding-bottom: calc(1vh + env(safe-area-inset-bottom) + 10px); /* Valores menores para subir mais */
            }
            .info-display {
                padding-bottom: 0.5vh;
            }
        }
    </style>
</head>
<body>
    <div class="viewport-limiter">
        <h1>Controle Remoto SafeDriver</h1>

        <div class="mode-toggle">
            <label for="modeSwitch">Manual</label>
            <label class="toggle-switch">
                <input type="checkbox" id="modeSwitch">
                <span class="slider-round"></span>
            </label>
            <label for="modeSwitch">Autônomo</label>
        </div>

        <div class="main-control-area">
            <div class="joystick-container" id="joystick">
                <div class="joystick-handle" id="joystickHandle"></div>
            </div>

            <div class="accel-group-container">
                <div class="button-group">
                    <button id="brakeButton" class="control-button">Freio</button>
                    <button id="accelButton" class="control-button">Acelerar</button>
                </div>
                <div class="vertical-slider-container">
                    <input type="range" id="accel_slider" min="-255" max="255" value="0" class="manual-mode-slider">
                </div>
            </div>
        </div>

        <div class="info-display">
            <p>Distância do Sensor Frontal: <span id="distance_front_val" class="distance-value">--</span> cm</p>
            <p>Distância do Sensor Traseiro: <span id="distance_rear_val" class="distance-value">--</span> cm</p>
            <p>Distância do Sensor Direito: <span id="distance_right_val" class="distance-value">--</span> cm</p>
            <p>Distância do Sensor Esquerdo: <span id="distance_left_val" class="distance-value">--</span> cm</p>
        </div>
    </div>

    <script>
        const joystick = document.getElementById('joystick');
        const joystickHandle = document.getElementById('joystickHandle');
        const accelSlider = document.getElementById('accel_slider');
        const modeSwitch = document.getElementById("modeSwitch");
        const accelButton = document.getElementById('accelButton');
        const brakeButton = document.getElementById('brakeButton');
        // IDs atualizados para os valores de distância
        const distanceFrontVal = document.getElementById("distance_front_val");
        const distanceRearVal = document.getElementById("distance_rear_val");
        const distanceRightVal = document.getElementById("distance_right_val");
        const distanceLeftVal = document.getElementById("distance_left_val");

        let isDragging = false;
        let joystickCenterX;
        let joystickCenterY;
        let joystickRadius;

        let currentJoystickX = 0;
        let currentSpeedValue = 0;
        let isAccelButtonPressed = false; // Estado do botão Acelerar no frontend

        let isAutonomousMode = false; // Estado inicial: Manual

        let commandSendTimeout;
        const COMMAND_SEND_DEBOUNCE_MS = 50;
        const DEADZONE_THRESHOLD = 40;
        
        // Novo: Para rastrear o toque ativo do joystick
        let activeTouchId = null;

        // --- Funções Auxiliares ---
        function updateJoystickGeometry() {
            const joystickRect = joystick.getBoundingClientRect();
            joystickCenterX = joystickRect.width / 2;
            joystickCenterY = joystickRect.height / 2;
            joystickRadius = joystickRect.width / 2 - joystickHandle.offsetWidth / 2;
        }

        function resetSliderToZero() {
            accelSlider.value = 0;
            currentSpeedValue = 0;
            accelSlider.dispatchEvent(new Event('input'));
        }

        // Função para atualizar o slider com base no modo
        function updateSliderForMode() {
            if (isAutonomousMode) {
                accelSlider.min = "0";
                accelSlider.max = "255";
                accelSlider.classList.remove('manual-mode-slider');
                accelSlider.classList.add('autonomous-mode-slider');
                if (parseInt(accelSlider.value) === 0) {
                    accelSlider.classList.add('deadzone-thumb');
                } else {
                    accelSlider.classList.remove('deadzone-thumb');
                }
            } else {
                accelSlider.min = "-255";
                accelSlider.max = "255";
                accelSlider.classList.remove('autonomous-mode-slider');
                accelSlider.classList.add('manual-mode-slider');
                let rawValue = parseInt(accelSlider.value);
                if (rawValue > -DEADZONE_THRESHOLD && rawValue < DEADZONE_THRESHOLD) {
                    accelSlider.classList.add('deadzone-thumb');
                } else {
                    accelSlider.classList.remove('deadzone-thumb');
                }
            }
            resetSliderToZero();
        }

        // --- Funções de Envio de Comando ---
        function sendCommand() {
            clearTimeout(commandSendTimeout);
            commandSendTimeout = setTimeout(() => {
                let vel = currentSpeedValue;
                let joyX = currentJoystickX; // This is the value from -1.0 to 1.0

                const modeParam = isAutonomousMode ? '&mode=autonomo' : '&mode=manual';
                // Incluir o estado do botão acelerar na URL
                const accelBtnStateParam = `&accelBtnState=${isAccelButtonPressed ? 'true' : 'false'}`;
                
                // Envia joystickX diretamente
                fetch(`/controle?vel=${vel}&joyX=${joyX}${modeParam}${accelBtnStateParam}`)
                    .then(response => {
                        if (!response.ok) {
                            console.error('Erro ao enviar comando:', response.statusText);
                        }
                    })
                    .catch(error => {
                        console.error('Erro de rede:', error);
                    });
            }, COMMAND_SEND_DEBOUNCE_MS);
        }

        // --- Lógica do Joystick ---
        function onStart(e) {
            if (isAutonomousMode) return;
            e.preventDefault(); // Prevent default browser actions (like scrolling)

            // Se for um evento de toque, armazena o identificador do toque que iniciou o joystick
            if (e.changedTouches && e.changedTouches.length > 0) {
                activeTouchId = e.changedTouches[0].identifier;
            } else {
                activeTouchId = 'mouse'; // Usa um identificador especial para eventos de mouse
            }
            
            isDragging = true;
            joystickHandle.style.cursor = 'grabbing';
            document.body.addEventListener('mousemove', onMove);
            document.body.addEventListener('touchmove', onMove, { passive: false });
            document.body.addEventListener('mouseup', onEnd);
            document.body.addEventListener('touchend', onEnd);
            document.body.addEventListener('touchcancel', onEnd);
            onMove(e); // Atualiza a posição inicial
        }

        function onMove(e) {
            if (!isDragging || isAutonomousMode) return;

            let clientX_val;
            let targetTouch = null;

            if (e.touches) { // Se for um evento de toque
                // Encontra o toque que corresponde ao nosso activeTouchId
                for (let i = 0; i < e.touches.length; i++) {
                    if (e.touches[i].identifier === activeTouchId) {
                        targetTouch = e.touches[i];
                        break;
                    }
                }
                if (!targetTouch) return; // Se o toque rastreado não for encontrado, ignora este evento
                clientX_val = targetTouch.clientX;
            } else { // Evento de mouse
                if (activeTouchId !== 'mouse') return; // Se não for interação do mouse, ignora
                clientX_val = e.clientX;
            }

            const joystickRect = joystick.getBoundingClientRect();
            let x = clientX_val - (joystickRect.left + joystickCenterX);

            const distance = Math.abs(x);
            if (distance > joystickRadius) {
                x = x > 0 ? joystickRadius : -joystickRadius;
            }

            joystickHandle.style.transform = `translate(${x}px, 0px)`;

            currentJoystickX = x / joystickRadius;
            sendCommand();
        }

        function onEnd(e) {
            if (isAutonomousMode) return;

            // Para eventos de toque, verifica se o toque que terminou é o que estamos rastreando
            if (e.changedTouches && e.changedTouches.length > 0) {
                let touchEnded = false;
                for (let i = 0; i < e.changedTouches.length; i++) {
                    if (e.changedTouches[i].identifier === activeTouchId) {
                        touchEnded = true;
                        break;
                    }
                }
                if (!touchEnded) return; // Se o toque rastreado não terminou, não para o arrasto
            } else if (activeTouchId !== 'mouse') { // Evento de mouse, mas estávamos rastreando um toque
                return;
            }
            
            isDragging = false;
            activeTouchId = null; // Reseta o ID do toque ativo
            joystickHandle.style.cursor = 'grab';
            joystickHandle.style.transform = `translate(0px, 0px)`; // Volta o joystick para o centro

            currentJoystickX = 0; // Reseta a posição X do joystick para 0 (centro)
            
            document.body.removeEventListener('mousemove', onMove);
            document.body.removeEventListener('touchmove', onMove);
            document.body.removeEventListener('mouseup', onEnd);
            document.body.removeEventListener('touchend', onEnd);
            document.body.removeEventListener('touchcancel', onEnd);
            sendCommand(); 
        }

        joystickHandle.addEventListener('mousedown', onStart);
        joystickHandle.addEventListener('touchstart', onStart, { passive: false });

        // --- Lógica do Slider de Aceleração (Vertical) ---
        accelSlider.oninput = () => {
            let rawValue = parseInt(accelSlider.value);
            currentSpeedValue = rawValue;

            if (!isAutonomousMode) {
                if (rawValue > -DEADZONE_THRESHOLD && rawValue < DEADZONE_THRESHOLD) {
                    accelSlider.classList.add('deadzone-thumb');
                } else {
                    accelSlider.classList.remove('deadzone-thumb');
                }
            } else {
                if (rawValue === 0) {
                    accelSlider.classList.add('deadzone-thumb');
                } else {
                    accelSlider.classList.remove('deadzone-thumb');
                }
            }

            // O slider apenas define a velocidade, o movimento é feito pelo botão Acelerar
            sendCommand(); 
        };

        // --- Lógica dos Botões Acelerar e Freio ---
        accelButton.addEventListener('mousedown', () => {
            if (!isAutonomousMode) { // Apenas no modo manual
                isAccelButtonPressed = true;
                sendCommand(); // Envia o estado do botão
            }
        });
        accelButton.addEventListener('mouseup', () => {
            if (!isAutonomousMode) {
                isAccelButtonPressed = false;
                sendCommand(); // Envia o estado do botão
            }
        });
        accelButton.addEventListener('touchstart', (e) => {
            if (!isAutonomousMode) {
                e.preventDefault(); // Previne o scroll da página em dispositivos móveis
                isAccelButtonPressed = true;
                sendCommand(); // Envia o estado do botão
            }
        }, { passive: false });
        accelButton.addEventListener('touchend', () => {
            if (!isAutonomousMode) {
                isAccelButtonPressed = false;
                sendCommand(); // Envia o estado do botão
            }
        });
        accelButton.addEventListener('touchcancel', () => {
            if (!isAutonomousMode) {
                isAccelButtonPressed = false;
                sendCommand(); // Envia o estado do botão
            }
        });

        brakeButton.addEventListener('mousedown', () => {
            if (!isAutonomousMode) {
                accelSlider.value = 0; // Set slider to 0
                currentSpeedValue = 0; // Update current speed value
                accelSlider.dispatchEvent(new Event('input')); // Trigger input event to update visual and send command
                isAccelButtonPressed = false; // Ensure no acceleration command is sent, but this button is for stopping
                sendCommand(); // Send the stop command (will also send accelBtnState=false)
            }
        });
        brakeButton.addEventListener('mouseup', () => {
            if (!isAutonomousMode) {
                // No action needed on mouseup, as the button's only function is to set speed to 0
                sendCommand();
            }
        });
        brakeButton.addEventListener('touchstart', (e) => {
            if (!isAutonomousMode) {
                e.preventDefault();
                accelSlider.value = 0; // Set slider to 0
                currentSpeedValue = 0; // Update current speed value
                accelSlider.dispatchEvent(new Event('input')); // Trigger input event
                isAccelButtonPressed = false; // Ensure no acceleration command is sent
                sendCommand(); // Send the stop command
            }
        }, { passive: false });
        brakeButton.addEventListener('touchend', () => {
            if (!isAutonomousMode) {
                // No action needed on touchend
                sendCommand();
            }
        });
        brakeButton.addEventListener('touchcancel', () => {
            if (!isAutonomousMode) {
                // No action needed on touchcancel
                sendCommand();
            }
        });

        // --- Lógica do Switch de Modo ---
        modeSwitch.addEventListener('change', () => {
            isAutonomousMode = modeSwitch.checked;
            updateSliderForMode();
            // Ao mudar o modo, resetar o estado do botão de aceleração no frontend
            isAccelButtonPressed = false; 
            if (isAutonomousMode) {
                joystick.style.pointerEvents = 'none'; // Desabilita o joystick
                joystickHandle.style.opacity = '0.5';
                accelButton.style.pointerEvents = 'none'; // Desabilita botões
                brakeButton.style.pointerEvents = 'none';
                accelButton.style.opacity = '0.5';
                brakeButton.style.opacity = '0.5';
            } else {
                joystick.style.pointerEvents = 'auto'; // Habilita o joystick
                joystickHandle.style.opacity = '1';
                accelButton.style.pointerEvents = 'auto'; // Habilita botões
                brakeButton.style.pointerEvents = 'auto';
                accelButton.style.opacity = '1';
                brakeButton.style.opacity = '1';
            }
            // Envia o comando de mudança de modo (e o estado do botão acelerar)
            sendCommand();
        });

        // --- Atualização de Dados (Sensor) ---
        function fetchDataUpdate() {
            fetch('/data')
                .then(response => response.json())
                .then(data => {
                    distanceFrontVal.textContent = data.distanceFront;
                    distanceRearVal.textContent = data.distanceRear;
                    distanceRightVal.textContent = data.distanceRight;
                    distanceLeftVal.textContent = data.distanceLeft;
                    
                    // Atualiza o estado do switch se houver discrepância (em caso de reinício, por exemplo)
                    const newMode = (data.mode === "autonomo");
                    if (newMode !== isAutonomousMode) {
                        isAutonomousMode = newMode;
                        modeSwitch.checked = newMode;
                        updateSliderForMode();
                        // Resetar estado do botão no frontend se o modo for alterado externamente
                        isAccelButtonPressed = false; 
                        if (isAutonomousMode) {
                            joystick.style.pointerEvents = 'none';
                            joystickHandle.style.opacity = '0.5';
                            accelButton.style.pointerEvents = 'none';
                            brakeButton.style.pointerEvents = 'none';
                            accelButton.style.opacity = '0.5';
                            brakeButton.style.opacity = '0.5';
                        } else {
                            joystick.style.pointerEvents = 'auto';
                            joystickHandle.style.opacity = '1';
                            accelButton.style.pointerEvents = 'auto';
                            brakeButton.style.pointerEvents = 'auto';
                            accelButton.style.opacity = '1';
                            brakeButton.style.opacity = '1';
                        }
                    }
                })
                .catch(error => {
                    console.error('Erro ao buscar dados:', error);
                    distanceFrontVal.textContent = "Erro";
                    distanceRearVal.textContent = "Erro";
                    distanceRightVal.textContent = "Erro";
                    distanceLeftVal.textContent = "Erro";
                });
        }

        // --- Inicialização ---
        window.addEventListener('load', () => {
            updateJoystickGeometry();
            updateSliderForMode(); // Configura o slider no carregamento inicial
            fetchDataUpdate(); // Faz a primeira leitura do sensor e modo
            setInterval(fetchDataUpdate, 1000); // Atualiza a cada 1 segundo
        });
        window.addEventListener('resize', updateJoystickGeometry); // Reajusta o joystick no redimensionamento

        // Evita o menu de contexto do clique direito em dispositivos móveis
        document.addEventListener('contextmenu', e => e.preventDefault());
    </script>
</body>
</html>
)rawliteral";
}
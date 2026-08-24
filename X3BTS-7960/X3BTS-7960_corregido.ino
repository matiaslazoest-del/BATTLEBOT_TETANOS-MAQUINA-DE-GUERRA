/**
 * @file BattleBot_Torneo_Definitivo_3Motores.ino
 * @brief Firmware de Combate: Seguridad MAC, Tracción Diferencial + Arma Progresiva (3er BTS7960)
 * @note Mantiene intactos los pines de tracción anteriores y añade el control del arma en los gatillos L2/R2.
 * Corregido: Ejecución continua de rampa para respuesta inmediata sin lag.
 */

#include <Bluepad32.h>

// ==========================================
// CANDADO DE SEGURIDAD MAC (Mando verificado)
// ==========================================
const uint8_t CONTROL_AUTORIZADO[] = {0xF4, 0x93, 0x9F, 0xBF, 0x35, 0xA3};

// ==========================================
// CONFIGURACIÓN DE PINES (GPIO ESP32)
// ==========================================
// Driver Izquierdo (BTS7960 - Tracción)
#define L_RPWM 25  
#define L_LPWM 26  
#define L_REN  27  
#define L_LEN  14  

// Driver Derecho (BTS7960 - Tracción)
#define R_RPWM 32  
#define R_LPWM 33  
#define R_REN  12  
#define R_LEN  13  

// Driver 3: Arma Activa (BTS7960 - NUEVO)
#define W_RPWM 18  // Señal PWM Avance del Arma (GPIO 18)
#define W_LPWM 19  // Señal PWM Reversa del Arma (GPIO 19)
#define W_REN  21  // Habilitador Avance del Arma (GPIO 21)
#define W_LEN  22  // Habilitador Reversa del Arma (GPIO 22)

// CANALES PWM (Lógica LEDC v2.x)
#define CH_L_RPWM  0  
#define CH_L_LPWM  1  
#define CH_R_RPWM  2  
#define CH_R_LPWM  3  
#define CH_W_RPWM  4  // Canal 4 para avance del arma
#define CH_W_LPWM  5  // Canal 5 para reversa del arma

// ==========================================
// PARÁMETROS DE COMBATE
// ==========================================
const int PWM_FREQ   = 20000;  
const int PWM_RES    = 8;      
const int DEADZONE   = 35;     
const int MAX_RAMP   = 20;     // Rampa de tracción (suave y rápida)
const int W_MAX_RAMP = 15;     // Rampa más suave para el arma (protege de sobrecorrientes masivas)

// Variables de estado PWM (Rampa)
int currentLeftPWM   = 0;
int currentRightPWM  = 0;
int currentWeaponPWM = 0;

ControllerPtr miControl = nullptr;

void setMotors(int targetLeft, int targetRight, int targetWeapon);

// ==========================================
// FILTRO DE SEGURIDAD BLUETOOTH
// ==========================================
void onConnectedController(ControllerPtr ctl) {
    ControllerProperties properties = ctl->getProperties();
    const uint8_t* mac = properties.btaddr;
    
    bool esElControlCorrecto = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != CONTROL_AUTORIZADO[i]) {
            esElControlCorrecto = false;
            break;
        }
    }

    if (esElControlCorrecto) {
        Serial.println("\n[ACCESO CONCEDIDO] ¡Mando oficial conectado con éxito!");
        miControl = ctl;
        ctl->setColorLED(0, 255, 0); 
        ctl->playDualRumble(0, 300, 0xFF, 0xFF);
    } else {
        Serial.printf("\n[BLOQUEADO] Intruso rechazado. MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ctl->disconnect(); 
    }
}

void onDisconnectedController(ControllerPtr ctl) {
    if (miControl == ctl) {
        Serial.println("\n[FAILSAFE] Mando desconectado. Freno total inmediato de seguridad.");
        miControl = nullptr;
        // Frenado físico absoluto de motores y arma
        ledcWrite(CH_L_RPWM, 0); ledcWrite(CH_L_LPWM, 0);
        ledcWrite(CH_R_RPWM, 0); ledcWrite(CH_R_LPWM, 0);
        ledcWrite(CH_W_RPWM, 0); ledcWrite(CH_W_LPWM, 0);
        currentLeftPWM = 0; currentRightPWM = 0; currentWeaponPWM = 0;
    }
}

// ==========================================
// CONFIGURACIÓN DE HARDWARE (SETUP)
// ==========================================
void setup() {
    Serial.begin(115200);
    Serial.println("\nINICIANDO SISTEMA voltron DE 3 MOTORES...");

    // Configurar pines de habilitación como salidas
    pinMode(L_REN, OUTPUT); pinMode(L_LEN, OUTPUT);
    pinMode(R_REN, OUTPUT); pinMode(R_LEN, OUTPUT);
    pinMode(W_REN, OUTPUT); pinMode(W_LEN, OUTPUT);
    
    // Habilitar los tres puentes H permanentemente
    digitalWrite(L_REN, HIGH); digitalWrite(L_LEN, HIGH);
    digitalWrite(R_REN, HIGH); digitalWrite(R_LEN, HIGH);
    digitalWrite(W_REN, HIGH); digitalWrite(W_LEN, HIGH);

    // Inicializar canales de hardware LEDC
    ledcSetup(CH_L_RPWM, PWM_FREQ, PWM_RES); 
    ledcSetup(CH_L_LPWM, PWM_FREQ, PWM_RES); 
    ledcSetup(CH_R_RPWM, PWM_FREQ, PWM_RES); 
    ledcSetup(CH_R_LPWM, PWM_FREQ, PWM_RES); 
    ledcSetup(CH_W_RPWM, PWM_FREQ, PWM_RES); 
    ledcSetup(CH_W_LPWM, PWM_FREQ, PWM_RES); 

    // Asociar pines físicos a canales lógicos
    ledcAttachPin(L_RPWM, CH_L_RPWM);
    ledcAttachPin(L_LPWM, CH_L_LPWM);
    ledcAttachPin(R_RPWM, CH_R_RPWM);
    ledcAttachPin(R_LPWM, CH_R_LPWM);
    ledcAttachPin(W_RPWM, CH_W_RPWM);
    ledcAttachPin(W_LPWM, CH_W_LPWM);

    // Detener todo al arrancar
    setMotors(0, 0, 0);

    BP32.setup(&onConnectedController, &onDisconnectedController);
    BP32.enableVirtualDevice(false);
}

// ==========================================
// LAZO DE CONTROL PRINCIPAL (LOOP)
// ==========================================
void loop() {
    BP32.update(); 
    
    // CORREGIDO: Se elimina el filtro hasData() para permitir que las rampas de aceleración
    // calculen la velocidad de manera fluida y continua en cada ciclo del procesador.
    if (miControl && miControl->isConnected()) {
        // --- 1. LECTURA DE TRACCIÓN (Joysticks) ---
        int rawY = -miControl->axisX(); 
        int rawX =  miControl->axisY();  

        int forward = 0;
        int turn = 0;

        if (abs(rawY) > DEADZONE) forward = map(rawY, -511, 512, -255, 255);
        if (abs(rawX) > DEADZONE) turn    = map(rawX, -511, 512, -255, 255);

        int targetLeft  = forward + turn;
        int targetRight = forward - turn;

        // --- 2. LECTURA DE ARMA PROGRESIVA (Gatillos L2 / R2) ---
        int rawThrottle = miControl->throttle(); // Gatillo R2
        int rawBrake    = miControl->brake();    // Gatillo L2
        int targetWeapon = 0;

        if (rawThrottle > 50) {
            targetWeapon = map(rawThrottle, 50, 1023, 0, 255); // Giro adelante progresivo
        } else if (rawBrake > 50) {
            targetWeapon = -map(rawBrake, 50, 1023, 0, 255);   // Giro atrás progresivo
        }

        // Ejecutar envío de potencias controlado por rampa
        setMotors(targetLeft, targetRight, targetWeapon);
    } 
    // FAILSAFE DE CORTE DIRECTO EN AUSENCIA DE MANDO
    else if (miControl == nullptr) {
        ledcWrite(CH_L_RPWM, 0); ledcWrite(CH_L_LPWM, 0);
        ledcWrite(CH_R_RPWM, 0); ledcWrite(CH_R_LPWM, 0);
        ledcWrite(CH_W_RPWM, 0); ledcWrite(CH_W_LPWM, 0);
        currentLeftPWM = 0; currentRightPWM = 0; currentWeaponPWM = 0;
    }

    delay(10); 
}

// ==========================================
// GESTIÓN ELECTRÓNICA DE RAMPAS INDEPENDIENTES
// ==========================================
void setMotors(int targetLeft, int targetRight, int targetWeapon) {
    targetLeft   = constrain(targetLeft, -255, 255);
    targetRight  = constrain(targetRight, -255, 255);
    targetWeapon = constrain(targetWeapon, -255, 255);

    // Rampa Motor Izquierdo
    if (targetLeft > currentLeftPWM)       currentLeftPWM = min(currentLeftPWM + MAX_RAMP, targetLeft);
    else if (targetLeft < currentLeftPWM)  currentLeftPWM = max(currentLeftPWM - MAX_RAMP, targetLeft);

    // Rampa Motor Derecho
    if (targetRight > currentRightPWM)      currentRightPWM = min(currentRightPWM + MAX_RAMP, targetRight);
    else if (targetRight < currentRightPWM) currentRightPWM = max(currentRightPWM - MAX_RAMP, targetRight);

    // Rampa Motor de Arma
    if (targetWeapon > currentWeaponPWM)      currentWeaponPWM = min(currentWeaponPWM + W_MAX_RAMP, targetWeapon);
    else if (targetWeapon < currentWeaponPWM) currentWeaponPWM = max(currentWeaponPWM - W_MAX_RAMP, targetWeapon);

    // Escritura Puente H Izquierdo
    if (currentLeftPWM >= 0) {
        ledcWrite(CH_L_RPWM, currentLeftPWM);
        ledcWrite(CH_L_LPWM, 0);
    } else {
        ledcWrite(CH_L_RPWM, 0);
        ledcWrite(CH_L_LPWM, -currentLeftPWM);
    }

    // Escritura Puente H Derecho
    if (currentRightPWM >= 0) {
        ledcWrite(CH_R_RPWM, currentRightPWM);
        ledcWrite(CH_R_LPWM, 0);
    } else {
        ledcWrite(CH_R_RPWM, 0);
        ledcWrite(CH_R_LPWM, -currentRightPWM);
    }

    // Escritura Puente H de Arma (Driver 3)
    if (currentWeaponPWM >= 0) {
        ledcWrite(CH_W_RPWM, currentWeaponPWM);
        ledcWrite(CH_W_LPWM, 0);
    } else {
        ledcWrite(CH_W_RPWM, 0);
        ledcWrite(CH_W_LPWM, -currentWeaponPWM);
    }
    // ===============================
// MONITOR SERIAL DE MOTORES
// ===============================
Serial.print("Motor Izq: ");
if (currentLeftPWM > 0) {
    Serial.print("ADELANTE ");
    Serial.print(currentLeftPWM);
} else if (currentLeftPWM < 0) {
    Serial.print("ATRAS ");
    Serial.print(-currentLeftPWM);
} else {
    Serial.print("DETENIDO");
}

Serial.print(" | Motor Der: ");
if (currentRightPWM > 0) {
    Serial.print("ADELANTE ");
    Serial.print(currentRightPWM);
} else if (currentRightPWM < 0) {
    Serial.print("ATRAS ");
    Serial.print(-currentRightPWM);
} else {
    Serial.print("DETENIDO");
}

Serial.print(" | Arma: ");
if (currentWeaponPWM > 0) {
    Serial.print("ADELANTE ");
    Serial.print(currentWeaponPWM);
} else if (currentWeaponPWM < 0) {
    Serial.print("REVERSA ");
    Serial.print(-currentWeaponPWM);
} else {
    Serial.print("DETENIDA");
}

Serial.println();
}
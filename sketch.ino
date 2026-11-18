/*
 * ============================================================================
 *  IntegraMed - Porta-Remedios Inteligente (Edge Computing + MQTT)
 *  Plataforma: ESP32 (Wokwi) | Linguagem: C++ (Arduino)
 *  Global Solution FIAP 2026 - CitizenTech
 * ----------------------------------------------------------------------------
 *  ARQUITETURA (2 nucleos):
 *     Nucleo 1 (loop)        : BORDA - sensores, decisao, LEDs, buzzer, LCD.
 *                              Usa apenas millis(); nunca espera pela rede.
 *     Nucleo 0 (tarefaRede)  : Wi-Fi + MQTT. Se a rede cair ou o broker
 *                              demorar, a borda continua funcionando.
 *     Comunicacao entre os dois: fila de comandos (rede -> borda) e
 *                              buffer JSON protegido por mutex (borda -> rede).
 *
 *  CICLO DE BORDA:
 *     medir -> filtrar (media movel) -> processar (indice, tendencia,
 *     histerese) -> decidir estado -> atuar -> publicar
 *
 *  Pinagem:
 *     DHT22 GPIO15 | LDR (AO) GPIO34 | Botao verde GPIO4 | Botao azul GPIO5
 *     LED verde GPIO12 | amarelo GPIO14 | vermelho GPIO27 | Buzzer GPIO26
 *     LCD I2C (0x27): SDA GPIO21 / SCL GPIO22
 *
 *  IMPORTANTE: o LDR e calibrado no boot. Inicie a simulacao com a tampa
 *  FECHADA (slider de luz do LDR no valor "escuro/normal").
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// ============================================================================
// 1. CONFIGURACOES
// ============================================================================
#define PIN_DHT          15
#define PIN_LDR          34
#define PIN_BTN_TOMEI     4
#define PIN_BTN_TESTE     5
#define PIN_LED_VERDE    12
#define PIN_LED_AMARELO  14
#define PIN_LED_VERMELHO 27
#define PIN_BUZZER       26

const char* WIFI_SSID    = "Wokwi-GUEST";
const char* WIFI_PASS    = "";
const char* MQTT_BROKER  = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char* TOPICO_PUB    = "fiap/gs2026/integramed/033/remedios";
const char* TOPICO_SUB    = "fiap/gs2026/integramed/033/comandos";
const char* TOPICO_STATUS = "fiap/gs2026/integramed/033/status";   // Last Will
const char* DEVICE_ID     = "integramed-033";

// --- Limiares com histerese (entra no limite, sai abaixo do limite de saida) ---
const float TEMP_ALERTA_IN   = 25.0, TEMP_ALERTA_OUT   = 24.0;
const float TEMP_CRITICA_IN  = 30.0, TEMP_CRITICA_OUT  = 29.0;
const float UMID_ALERTA_IN   = 70.0, UMID_ALERTA_OUT   = 68.0;

// --- Tendencia: subida de >= 1.0 C em 30 s (15 amostras de 2 s) ja avisa ---
#define JANELA_TENDENCIA 15
const float TENDENCIA_LIMITE   = 1.0;
const float TENDENCIA_TEMP_MIN = 22.0;   // so avisa se ja estiver perto do limite

// --- Atraso e luz ---
const unsigned long ATRASO_CRITICO_MS = 15000UL;   // 15 s sem tomar -> vermelho
const unsigned long LUZ_MAX_SEG       = 10;        // tampa aberta > 10 s -> amarelo

// --- LDR: calibrado no boot. Tampa "aberta" = leitura se afasta da base ---
const int LDR_DELTA_ABRIR  = 500;   // diferenca (ADC 0..4095) para considerar ABERTA
const int LDR_DELTA_FECHAR = 300;   // diferenca para voltar a FECHADA (histerese)

// --- Filtro e limites fisicos do DHT22 ---
#define JANELA 5
const float TEMP_MIN_FISICA = -40.0, TEMP_MAX_FISICA = 80.0;
const float UMID_MIN_FISICA = 0.0,   UMID_MAX_FISICA = 100.0;
const uint8_t MAX_FALHAS_DHT = 5;

// --- Periodos (millis) ---
const unsigned long PERIODO_DHT_MS      = 2000;
const unsigned long PERIODO_LCD_MS      = 500;
const unsigned long PERIODO_MQTT_MS     = 3000;
const unsigned long PERIODO_RECONECT_MS = 5000;
const unsigned long DEBOUNCE_MS         = 40;

// --- Buzzer ---
const int FREQ_LEMBRETE = 1500, FREQ_CRITICA = 2500;
const unsigned long BIP_ON_MS = 150, BIP_PERIODO_MS = 1500;

// --- LCD: a linha 2 alterna entre o status e o nome do remedio ---
const unsigned long LCD_FASE_STATUS_MS = 3000;
const unsigned long LCD_FASE_NOME_MS   = 3000;

// --- Fila de comandos MQTT (rede -> borda) ---
#define TAM_COMANDO 96
#define TAM_JSON    520

enum EstadoBorda { ESTADO_VERDE, ESTADO_AMARELO, ESTADO_VERMELHO };
enum AlarmeTipo  { ALARME_OFF, ALARME_LEMBRETE, ALARME_TERMICO, ALARME_LUZ,
                   ALARME_TENDENCIA, ALARME_CRITICO, ALARME_TERMICO_CRITICO };

const char* nomeAlarme(AlarmeTipo a) {
  switch (a) {
    case ALARME_LEMBRETE:        return "LEMBRETE";
    case ALARME_TERMICO:         return "TERMICO";
    case ALARME_LUZ:             return "LUZ";
    case ALARME_TENDENCIA:       return "TENDENCIA";
    case ALARME_CRITICO:         return "CRITICO";
    case ALARME_TERMICO_CRITICO: return "TERMICO_CRITICO";
    default:                     return "OFF";
  }
}

// ============================================================================
// 2. OBJETOS E VARIAVEIS GLOBAIS
// ============================================================================
DHT dht(PIN_DHT, DHT22);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// --- Sincronizacao entre os nucleos ---
QueueHandle_t     filaComandos = NULL;     // comandos recebidos pelo MQTT
SemaphoreHandle_t mutexJson    = NULL;     // protege o buffer de telemetria
char jsonPendente[TAM_JSON];               // ultimo JSON montado pela borda
volatile bool jsonNovo = false;            // ha JSON aguardando publicacao
volatile bool mqttReconectou = false;      // aviso da rede para a borda

// --- Media movel de 5 amostras ---
struct MediaMovel {
  float buf[JANELA];
  uint8_t idx = 0, n = 0;
  void add(float v) { buf[idx] = v; idx = (idx + 1) % JANELA; if (n < JANELA) n++; }
  float media() const {
    if (n == 0) return NAN;
    float s = 0; for (uint8_t i = 0; i < n; i++) s += buf[i];
    return s / n;
  }
};
MediaMovel filtroTemp, filtroUmid;
float temperatura = NAN, umidade = NAN;
bool sensorOk = false;
uint8_t falhasDht = 0;

// --- Historico para tendencia (temperaturas ja filtradas) ---
float histTemp[JANELA_TENDENCIA];
uint8_t histIdx = 0, histN = 0;
float tendencia = 0.0;   // variacao em ~30 s (graus C)

// --- Relogio interno (segundos absolutos) ---
uint32_t relogioAbs = 7UL * 3600 + 59UL * 60 + 45;   // inicia 07:59:45 (demo)
unsigned long ultimoTick = 0;
uint32_t diaAtual = 0;

// --- Agenda / posologia ---
char     nomeRemedio[26] = "Dipirona 500mg";
uint8_t  intervaloHoras  = 8;
uint32_t proximaDoseAbs  = 8UL * 3600;
bool     doseDevida      = false;
unsigned long inicioAlarmeMs = 0;
uint16_t dosesHoje = 0;

// --- Tampa / luz ---
int  ldrBase = 0;               // leitura de referencia (tampa fechada)
bool tampaAberta = false;
unsigned long tAberturaMs = 0;

// --- Estado, histerese e atuadores ---
EstadoBorda estado = ESTADO_VERDE;
AlarmeTipo  alarme = ALARME_OFF;
bool latchAlerta = false, latchCritico = false;
int indiceConservacao = 100;
int freqAtual = 0;

// --- Botoes ---
struct Botao { uint8_t pino; bool ultimaLeitura; bool estavel; unsigned long tMudanca; };
Botao btnTomei = {PIN_BTN_TOMEI, HIGH, HIGH, 0};
Botao btnTeste = {PIN_BTN_TESTE, HIGH, HIGH, 0};

// --- Temporizadores ---
unsigned long tDht = 0, tLcd = 0, tMqtt = 0;
bool forcarPublicacao = false;

// --- LCD (alternancia status/nome) ---
bool lcdMostrarNome = false;
unsigned long lcdTFase = 0;
uint8_t lcdScrollTick = 0;

// ============================================================================
// 3. TEMPO E AGENDA
// ============================================================================
void fmtHMS(uint32_t abs, char* out) {   // out >= 9 bytes
  unsigned long s = abs % 86400UL;
  snprintf(out, 9, "%02lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
}
void fmtHM(uint32_t abs, char* out) {    // out >= 6 bytes
  unsigned long s = abs % 86400UL;
  snprintf(out, 6, "%02lu:%02lu", s / 3600, (s % 3600) / 60);
}

// Agenda a proxima dose para HH:MM (hoje, ou amanha se o horario ja passou)
void agendarPara(int hh, int mm) {
  uint32_t base = (relogioAbs / 86400UL) * 86400UL + hh * 3600UL + mm * 60UL;
  if (base <= relogioAbs) base += 86400UL;
  proximaDoseAbs = base;
  doseDevida = false;
}

// Relogio nao-bloqueante: 1 tick por segundo real
void atualizarRelogio() {
  unsigned long agora = millis();
  while (agora - ultimoTick >= 1000UL) {
    ultimoTick += 1000UL;
    relogioAbs++;
    if (relogioAbs / 86400UL != diaAtual) { diaAtual = relogioAbs / 86400UL; dosesHoje = 0; }
  }
}

// Confirma a dose SOMENTE se houver dose pendente (evita pular a agenda
// por um aperto acidental). Soma a posologia para agendar a proxima.
bool confirmarDose(const char* origem) {
  if (!doseDevida) {
    Serial.printf("[DOSE] ignorada (%s): nenhuma dose pendente\n", origem);
    return false;
  }
  dosesHoje++;
  proximaDoseAbs += (uint32_t)intervaloHoras * 3600UL;
  while (proximaDoseAbs <= relogioAbs) proximaDoseAbs += (uint32_t)intervaloHoras * 3600UL;
  doseDevida = false;
  forcarPublicacao = true;
  char hm[6]; fmtHM(proximaDoseAbs, hm);
  Serial.printf("[DOSE] confirmada via %s. Proxima: %s\n", origem, hm);
  return true;
}

void avancarParaProximaDose() {
  if (proximaDoseAbs > relogioAbs) relogioAbs = proximaDoseAbs;
  forcarPublicacao = true;
  Serial.println(F("[TESTE] relogio avancado para a proxima dose"));
}

void verificarAgenda() {
  if (!doseDevida && relogioAbs >= proximaDoseAbs) {
    doseDevida = true;
    inicioAlarmeMs = millis();
    forcarPublicacao = true;
    Serial.println(F("[AGENDA] hora do remedio!"));
  }
}

// ============================================================================
// 4. BUZZER (compativel com Arduino-ESP32 core 2.x e 3.x)
// ============================================================================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  void buzzerInit()        { ledcAttach(PIN_BUZZER, 2000, 8); }
  void buzzerTom(int freq) { ledcWriteTone(PIN_BUZZER, freq); }
#else
  #define CANAL_BUZZER 0
  void buzzerInit()        { ledcSetup(CANAL_BUZZER, 2000, 8); ledcAttachPin(PIN_BUZZER, CANAL_BUZZER); }
  void buzzerTom(int freq) { ledcWriteTone(CANAL_BUZZER, freq); }
#endif

// ============================================================================
// 5. SENSORES E PROCESSAMENTO LOCAL
// ============================================================================
// Atualiza o historico e calcula a tendencia (variacao nos ultimos ~30 s)
void atualizarTendencia() {
  histTemp[histIdx] = temperatura;
  histIdx = (histIdx + 1) % JANELA_TENDENCIA;
  if (histN < JANELA_TENDENCIA) histN++;
  if (histN == JANELA_TENDENCIA) tendencia = temperatura - histTemp[histIdx];  // histIdx = mais antigo
  else tendencia = 0.0;
}

// Le o DHT22, valida (isnan + limites fisicos) e aplica a media movel
void lerDHT() {
  float t = dht.readTemperature();
  float u = dht.readHumidity();
  bool valido = !isnan(t) && !isnan(u) &&
                t >= TEMP_MIN_FISICA && t <= TEMP_MAX_FISICA &&
                u >= UMID_MIN_FISICA && u <= UMID_MAX_FISICA;
  if (valido) {
    falhasDht = 0; sensorOk = true;
    filtroTemp.add(t); filtroUmid.add(u);
    temperatura = filtroTemp.media();
    umidade = filtroUmid.media();
    atualizarTendencia();
  } else {
    if (falhasDht < 255) falhasDht++;
    Serial.println(F("[DHT] leitura invalida descartada"));
    if (falhasDht >= MAX_FALHAS_DHT) sensorOk = false;
  }
}

// Calibra o LDR no boot: media de 16 leituras = referencia da tampa FECHADA
void calibrarLDR() {
  long soma = 0;
  for (int i = 0; i < 16; i++) soma += analogRead(PIN_LDR);
  ldrBase = (int)(soma / 16);
  tampaAberta = false;
  Serial.printf("[LDR] base calibrada: %d (tampa fechada)\n", ldrBase);
}

// Detecta o instante de ABERTURA da tampa (borda de subida), com histerese.
// Funciona nos dois sentidos de variacao do LDR (mais ou menos luz).
bool detectarAberturaTampa() {
  int diff = abs(analogRead(PIN_LDR) - ldrBase);
  bool aberta = tampaAberta ? (diff > LDR_DELTA_FECHAR) : (diff > LDR_DELTA_ABRIR);
  bool evento = (aberta && !tampaAberta);
  if (evento) tAberturaMs = millis();
  tampaAberta = aberta;
  return evento;
}

unsigned long luzExpostaSeg() {
  return tampaAberta ? (millis() - tAberturaMs) / 1000UL : 0;
}

bool botaoPressionado(Botao& b) {
  bool leitura = digitalRead(b.pino);
  unsigned long agora = millis();
  if (leitura != b.ultimaLeitura) { b.tMudanca = agora; b.ultimaLeitura = leitura; }
  if ((agora - b.tMudanca) > DEBOUNCE_MS && leitura != b.estavel) {
    b.estavel = leitura;
    if (b.estavel == LOW) return true;
  }
  return false;
}

// Indice de Conservacao (0-100%): penaliza calor, umidade e exposicao a luz
int calcularIndice() {
  if (!sensorOk) return 0;
  float penT = (temperatura > 22.0) ? min(60.0f, (temperatura - 22.0f) * 7.5f) : 0;
  float penU = (umidade > 60.0)     ? min(25.0f, (umidade - 60.0f) * 1.25f)    : 0;
  float penL = (float)min(15UL, luzExpostaSeg() * 3UL / 2UL);
  int idx = (int)(100.0f - penT - penU - penL);
  return constrain(idx, 0, 100);
}

// ============================================================================
// 6. MAQUINA DE ESTADOS (3 estados + histerese)
// ============================================================================
void atualizarHisterese() {
  if (!sensorOk) { latchAlerta = false; latchCritico = false; return; }

  if (!latchCritico && temperatura > TEMP_CRITICA_IN)       latchCritico = true;
  else if (latchCritico && temperatura < TEMP_CRITICA_OUT)  latchCritico = false;

  bool entra = (temperatura > TEMP_ALERTA_IN) || (umidade > UMID_ALERTA_IN);
  bool sai   = (temperatura < TEMP_ALERTA_OUT) && (umidade < UMID_ALERTA_OUT);
  if (!latchAlerta && entra) latchAlerta = true;
  else if (latchAlerta && sai) latchAlerta = false;
}

void decidirEstado() {
  atualizarHisterese();
  bool atrasoCritico = doseDevida && (millis() - inicioAlarmeMs > ATRASO_CRITICO_MS);
  bool luzExposta    = !doseDevida && luzExpostaSeg() >= LUZ_MAX_SEG;
  bool tendenciaAlta = sensorOk && tendencia >= TENDENCIA_LIMITE && temperatura > TENDENCIA_TEMP_MIN;

  if (latchCritico || atrasoCritico) {
    estado = ESTADO_VERMELHO;
    alarme = atrasoCritico ? ALARME_CRITICO : ALARME_TERMICO_CRITICO;
  } else if (doseDevida || latchAlerta || luzExposta || tendenciaAlta) {
    estado = ESTADO_AMARELO;
    if (doseDevida)         alarme = ALARME_LEMBRETE;
    else if (latchAlerta)   alarme = ALARME_TERMICO;
    else if (luzExposta)    alarme = ALARME_LUZ;
    else                    alarme = ALARME_TENDENCIA;
  } else {
    estado = ESTADO_VERDE;
    alarme = ALARME_OFF;
  }
  indiceConservacao = calcularIndice();
}

const char* nomeEstado() {
  switch (estado) {
    case ESTADO_VERDE:   return "VERDE";
    case ESTADO_AMARELO: return "AMARELO";
    default:             return "VERMELHO";
  }
}

// ============================================================================
// 7. ATUADORES
// ============================================================================
void atualizarAtuadores() {
  digitalWrite(PIN_LED_VERDE,    estado == ESTADO_VERDE);
  digitalWrite(PIN_LED_AMARELO,  estado == ESTADO_AMARELO);
  digitalWrite(PIN_LED_VERMELHO, estado == ESTADO_VERMELHO);

  int freqAlvo = 0;
  if (estado == ESTADO_VERMELHO)       freqAlvo = FREQ_CRITICA;                        // continuo
  else if (estado == ESTADO_AMARELO)   freqAlvo = ((millis() % BIP_PERIODO_MS) < BIP_ON_MS) ? FREQ_LEMBRETE : 0;  // bips

  if (freqAlvo != freqAtual) { buzzerTom(freqAlvo); freqAtual = freqAlvo; }
}

// ============================================================================
// 8. DISPLAY LCD
// ============================================================================
void lcdLinha(uint8_t linha, const char* texto) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", texto);
  lcd.setCursor(0, linha);
  lcd.print(buf);
}

// Mostra o nome do remedio imediatamente (usado quando chega uma nova RECEITA)
void lcdMostrarNomeAgora() {
  lcdMostrarNome = true;
  lcdTFase = millis();
  lcdScrollTick = 0;
}

void atualizarLCD() {
  char hms[9], hm[6], l1[24], l2[24];
  fmtHMS(relogioAbs, hms);
  fmtHM(proximaDoseAbs, hm);

  // --- Linha 1: relogio + temperatura ---
  if (sensorOk) snprintf(l1, sizeof(l1), "%s %4.1fC", hms, temperatura);
  else          snprintf(l1, sizeof(l1), "%s --.-C", hms);
  lcdLinha(0, l1);

  // --- Texto de status (maximo 16 caracteres) ---
  if (estado == ESTADO_VERMELHO)
    snprintf(l2, sizeof(l2), "%s", doseDevida ? "ATRASO CRITICO!" : "PERIGO TERMICO!");
  else if (doseDevida)
    snprintf(l2, sizeof(l2), "TOME O REMEDIO!");
  else if (alarme == ALARME_LUZ)
    snprintf(l2, sizeof(l2), "FECHE A TAMPA!");
  else if (alarme == ALARME_TENDENCIA)
    snprintf(l2, sizeof(l2), "TEMP. SUBINDO");
  else if (estado == ESTADO_AMARELO)
    snprintf(l2, sizeof(l2), "CALOR/UMIDADE");
  else
    snprintf(l2, sizeof(l2), "Prox %s C%3d%%", hm, indiceConservacao);

  // --- Linha 2: alterna entre o status e o nome do remedio ---
  unsigned long agora = millis();
  size_t len = strlen(nomeRemedio);

  if (!lcdMostrarNome) {
    if (agora - lcdTFase >= LCD_FASE_STATUS_MS) {
      lcdMostrarNome = true; lcdTFase = agora; lcdScrollTick = 0;
    }
  } else if (len <= 16) {
    if (agora - lcdTFase >= LCD_FASE_NOME_MS) { lcdMostrarNome = false; lcdTFase = agora; }
  } else {
    // Nome longo: pausa no inicio, rola 1 coluna por atualizacao, pausa no fim
    if (lcdScrollTick > (len - 16) + 4) { lcdMostrarNome = false; lcdTFase = agora; }
  }

  if (lcdMostrarNome) {
    char janela[17] = {0};
    size_t off = 0;
    if (len > 16) {
      int p = (int)lcdScrollTick - 2;
      off = (size_t)constrain(p, 0, (int)(len - 16));
      lcdScrollTick++;
    }
    strncpy(janela, nomeRemedio + off, 16);
    lcdLinha(1, janela);
  } else {
    lcdLinha(1, l2);
  }
}

// ============================================================================
// 9. COMANDOS REMOTOS (processados na BORDA, nunca na tarefa de rede)
// ============================================================================
void trimInPlace(char* s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == '\t')) s[--n] = 0;
  size_t i = 0; while (s[i] == ' ' || s[i] == '\t') i++;
  if (i) memmove(s, s + i, strlen(s + i) + 1);
}

// RECEITA:Nome|HH:MM|Intervalo
bool processarReceita(char* corpo) {
  char* p1 = strchr(corpo, '|');
  if (!p1) return false;
  char* p2 = strchr(p1 + 1, '|');
  if (!p2) return false;
  *p1 = 0; *p2 = 0;
  char* nome = corpo; char* hora = p1 + 1; char* inter = p2 + 1;
  trimInPlace(nome); trimInPlace(hora); trimInPlace(inter);

  int hh, mm, h = atoi(inter);
  if (strlen(nome) == 0 || sscanf(hora, "%d:%d", &hh, &mm) != 2) return false;
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || h < 1 || h > 24) return false;

  size_t j = 0;   // remove aspas/barras para manter o JSON valido
  for (size_t i = 0; nome[i] && j < sizeof(nomeRemedio) - 1; i++)
    if (nome[i] != '"' && nome[i] != '\\') nomeRemedio[j++] = nome[i];
  nomeRemedio[j] = 0;

  intervaloHoras = (uint8_t)h;
  dosesHoje = 0;
  agendarPara(hh, mm);
  lcdMostrarNomeAgora();   // o paciente ve o novo nome no display na hora
  Serial.printf("[RECEITA] %s as %02d:%02d a cada %dh\n", nomeRemedio, hh, mm, h);
  return true;
}

void imprimirAjuda() {
  Serial.println(F("Comandos: TOMAR|TOMEI | PROXIMA|ALARME | HH:MM | STATUS | AJUDA"));
  Serial.println(F("          RECEITA:Nome|HH:MM|Horas  (ex: RECEITA:Amoxicilina 500mg|08:00|8)"));
}

// Interpreta um comando de texto (chamado pelo loop da borda)
void processarComando(char* msg) {
  trimInPlace(msg);
  Serial.printf("[MQTT] comando: '%s'\n", msg);

  char up[TAM_COMANDO];
  for (size_t i = 0; i <= strlen(msg); i++) up[i] = toupper((unsigned char)msg[i]);

  int hh, mm;
  if (!strcmp(up, "TOMAR") || !strcmp(up, "TOMEI")) {
    confirmarDose("MQTT");
  } else if (!strcmp(up, "PROXIMA") || !strcmp(up, "ALARME")) {
    avancarParaProximaDose();
  } else if (!strcmp(up, "STATUS")) {
    forcarPublicacao = true;
  } else if (!strcmp(up, "AJUDA")) {
    imprimirAjuda();
  } else if (!strncmp(up, "RECEITA:", 8)) {
    if (processarReceita(msg + 8)) forcarPublicacao = true;
    else Serial.println(F("[MQTT] RECEITA invalida. Use RECEITA:Nome|HH:MM|Horas"));
  } else if (strchr(up, ':') && sscanf(up, "%d:%d", &hh, &mm) == 2 &&
             hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
    agendarPara(hh, mm);
    forcarPublicacao = true;
    Serial.printf("[MQTT] proxima dose agendada para %02d:%02d\n", hh, mm);
  } else {
    Serial.println(F("[MQTT] comando desconhecido (envie AJUDA)"));
  }
}

// Callback do PubSubClient (roda na tarefa de rede): apenas ENFILEIRA o comando.
// Assim nenhuma variavel da borda e alterada fora do nucleo 1 (sem condicao de corrida).
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  char msg[TAM_COMANDO];
  memset(msg, 0, sizeof(msg));
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);
  xQueueSend(filaComandos, msg, 0);   // se a fila estiver cheia, descarta
}

// Esvazia a fila de comandos (chamado a cada ciclo do loop)
void processarFilaComandos() {
  char msg[TAM_COMANDO];
  while (xQueueReceive(filaComandos, msg, 0) == pdTRUE) processarComando(msg);
}

// ============================================================================
// 10. TELEMETRIA (montada na borda, publicada pela tarefa de rede)
// ============================================================================
void montarJson(char* json, size_t n) {
  char hms[9], hm[6], tStr[12], uStr[12];
  fmtHMS(relogioAbs, hms);
  fmtHM(proximaDoseAbs, hm);
  if (sensorOk) { snprintf(tStr, sizeof(tStr), "%.1f", temperatura); snprintf(uStr, sizeof(uStr), "%.1f", umidade); }
  else          { strcpy(tStr, "null"); strcpy(uStr, "null"); }

  snprintf(json, n,
    "{\"id\":\"%s\",\"relogio\":\"%s\",\"proxima_dose\":\"%s\",\"posologia\":\"%u/%uh\","
    "\"remedio\":\"%s\",\"estado\":\"%s\",\"alarme\":\"%s\",\"tampa\":\"%s\","
    "\"doses_hoje\":%u,\"temperatura\":%s,\"umidade\":%s,"
    "\"indice_conservacao\":%d,\"tendencia\":%.1f}",
    DEVICE_ID, hms, hm, intervaloHoras, intervaloHoras, nomeRemedio, nomeEstado(),
    nomeAlarme(alarme), tampaAberta ? "ABERTA" : "FECHADA", dosesHoje, tStr, uStr,
    indiceConservacao, tendencia);
}

// Entrega o JSON para a tarefa de rede (nao bloqueia: espera no maximo 10 ms)
void enviarParaRede() {
  char json[TAM_JSON];
  montarJson(json, sizeof(json));
  if (xSemaphoreTake(mutexJson, pdMS_TO_TICKS(10)) == pdTRUE) {
    strlcpy(jsonPendente, json, sizeof(jsonPendente));
    jsonNovo = true;
    xSemaphoreGive(mutexJson);
  }
}

// ============================================================================
// 11. TAREFA DE REDE (nucleo 0): Wi-Fi + MQTT
// ============================================================================
void manterConexoes() {
  static unsigned long tReconect = 0;
  unsigned long agora = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (agora - tReconect >= PERIODO_RECONECT_MS) {
      tReconect = agora;
      Serial.println(F("[WiFi] (re)conectando..."));
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
    }
    return;
  }

  if (!mqtt.connected() && agora - tReconect >= PERIODO_RECONECT_MS) {
    tReconect = agora;
    char cid[40];
    snprintf(cid, sizeof(cid), "%s-%04X", DEVICE_ID, (uint16_t)esp_random());
    Serial.print(F("[MQTT] conectando... "));
    // Last Will: se cair sem avisar, o broker publica "offline" (retido)
    if (mqtt.connect(cid, NULL, NULL, TOPICO_STATUS, 1, true, "offline")) {
      mqtt.subscribe(TOPICO_SUB);
      mqtt.publish(TOPICO_STATUS, "online", true);
      Serial.println(F("OK"));
      mqttReconectou = true;
    } else {
      Serial.printf("falhou rc=%d\n", mqtt.state());
    }
  }
}

// Esta tarefa roda isolada da borda: mesmo que o connect() demore, os LEDs,
// o buzzer, o LCD e os botoes continuam respondendo no nucleo 1.
// (vTaskDelay aqui apenas cede CPU a tarefa; o loop da borda nao usa delay.)
void tarefaRede(void* param) {
  char json[TAM_JSON];
  for (;;) {
    manterConexoes();
    if (mqtt.connected()) {
      mqtt.loop();
      if (jsonNovo) {
        bool tem = false;
        if (xSemaphoreTake(mutexJson, pdMS_TO_TICKS(20)) == pdTRUE) {
          if (jsonNovo) { strlcpy(json, jsonPendente, sizeof(json)); jsonNovo = false; tem = true; }
          xSemaphoreGive(mutexJson);
        }
        if (tem) {
          if (mqtt.publish(TOPICO_PUB, json)) Serial.printf("[MQTT] pub: %s\n", json);
          else Serial.println(F("[MQTT] falha ao publicar"));
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ============================================================================
// 12. SETUP E LOOP (BORDA - nucleo 1)
// ============================================================================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_AMARELO, OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  pinMode(PIN_BTN_TOMEI, INPUT_PULLUP);
  pinMode(PIN_BTN_TESTE, INPUT_PULLUP);
  pinMode(PIN_LDR, INPUT);
  buzzerInit();
  buzzerTom(0);

  dht.begin();
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcdLinha(0, "IntegraMed");
  lcdLinha(1, "Iniciando...");

  calibrarLDR();

  // --- Rede em tarefa propria (nucleo 0) ---
  filaComandos = xQueueCreate(4, TAM_COMANDO);
  mutexJson    = xSemaphoreCreateMutex();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(callbackMQTT);
  mqtt.setBufferSize(640);
  mqtt.setSocketTimeout(2);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  xTaskCreatePinnedToCore(tarefaRede, "rede", 8192, NULL, 1, NULL, 0);

  ultimoTick = millis();
  diaAtual = relogioAbs / 86400UL;
  lerDHT();
  Serial.println(F("IntegraMed pronto. Envie AJUDA no topico de comandos."));
  imprimirAjuda();
}

void loop() {
  unsigned long agora = millis();
  static EstadoBorda estadoAnterior = ESTADO_VERDE;
  static AlarmeTipo  alarmeAnterior = ALARME_OFF;

  // --- Comandos remotos recebidos pela tarefa de rede ---
  processarFilaComandos();
  if (mqttReconectou) { mqttReconectou = false; forcarPublicacao = true; }

  // --- Relogio e sensores ---
  atualizarRelogio();
  if (agora - tDht >= PERIODO_DHT_MS) { tDht = agora; lerDHT(); }

  // --- Entradas do paciente ---
  bool abriuTampa = detectarAberturaTampa();
  if (botaoPressionado(btnTomei)) confirmarDose("botao verde");
  if (botaoPressionado(btnTeste)) avancarParaProximaDose();
  if (abriuTampa && doseDevida)   confirmarDose("tampa (LDR)");

  // --- Processamento local, decisao e atuacao ---
  verificarAgenda();
  decidirEstado();
  atualizarAtuadores();
  if (agora - tLcd >= PERIODO_LCD_MS) { tLcd = agora; atualizarLCD(); }

  // --- Publicacao por excecao: muda o estado/alarme -> publica na hora ---
  if (estado != estadoAnterior || alarme != alarmeAnterior) {
    estadoAnterior = estado;
    alarmeAnterior = alarme;
    forcarPublicacao = true;
    Serial.printf("[ESTADO] %s (%s)\n", nomeEstado(), nomeAlarme(alarme));
  }

  // --- Telemetria periodica (3 s) ou imediata por evento ---
  if (forcarPublicacao || agora - tMqtt >= PERIODO_MQTT_MS) {
    tMqtt = agora;
    forcarPublicacao = false;
    enviarParaRede();
  }
}

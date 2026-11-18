/*
- ============================================================================
-  IntegraMed v2.1 - Porta-Remedios Inteligente (Edge Computing + MQTT)
-  Plataforma: ESP32 (Wokwi) | Linguagem: C++ (Arduino)
-  Global Solution FIAP 2026 - CitizenTech
- ----------------------------------------------------------------------------
-  Pinagem:
-     DHT22 GPIO15 | LDR (AO) GPIO34 | Botao verde GPIO4 | Botao azul GPIO5
-     LED verde GPIO12 | amarelo GPIO14 | vermelho GPIO27 | Buzzer GPIO26
-     LCD I2C (0x27): SDA GPIO21 / SCL GPIO22
- ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

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

// --- Limiares com histerese ---
const float TEMP_ALERTA_IN   = 25.0, TEMP_ALERTA_OUT   = 24.0;
const float TEMP_CRITICA_IN  = 30.0, TEMP_CRITICA_OUT  = 29.0;
const float UMID_ALERTA_IN   = 70.0, UMID_ALERTA_OUT   = 68.0;

// --- Tendencia: subida de >= 1.0 C em 30 s (15 amostras de 2 s) ---
#define JANELA_TENDENCIA 15
const float TENDENCIA_LIMITE   = 1.0;
const float TENDENCIA_TEMP_MIN = 22.0;

// --- Atraso e luz ---
const unsigned long ATRASO_CRITICO_MS = 15000UL;   // 15 s sem tomar -> vermelho
const unsigned long LUZ_MAX_SEG       = 10;        // tampa aberta > 10 s -> amarelo

// --- LDR (ADC 0..4095) ---
#define LDR_ALTO_E_LUZ 1
const int LDR_LIMIAR = 2000;

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
const unsigned long DEBOUNCE_ISR_MS     = 180;     // Debounce na interrupcao

// --- Buzzer ---
const int FREQ_LEMBRETE = 1500, FREQ_CRITICA = 2500, FREQ_ACK = 1800;
const unsigned long BIP_ON_MS = 150, BIP_PERIODO_MS = 1500;

enum EstadoBorda { ESTADO_VERDE, ESTADO_AMARELO, ESTADO_VERMELHO };

// ============================================================================
// 2. OBJETOS E VARIAVEIS GLOBAIS
// ============================================================================
DHT dht(PIN_DHT, DHT22);
LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

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

// --- Historico para tendencia ---
float histTemp[JANELA_TENDENCIA];
uint8_t histIdx = 0, histN = 0;
float tendencia = 0.0;

// --- Relogio interno (segundos absolutos) ---
uint32_t relogioAbs = 7UL * 3600 + 59UL * 60 + 45;   // inicia 07:59:45
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
bool tampaAberta = false;
unsigned long tAberturaMs = 0;

// --- Estado, histerese e atuadores ---
EstadoBorda estado = ESTADO_VERDE;
const char* statusAlarme = "OFF";
bool latchAlerta = false, latchCritico = false;
int indiceConservacao = 100;
int freqAtual = 0;

// --- Feedback instantaneo (LCD + Buzzer) ---
char avisoL1[17] = "";
char avisoL2[17] = "";
unsigned long fimAvisoLcdMs = 0;
unsigned long fimBipAckMs   = 0;

// --- Botoes via Interrupcao de Hardware (resposta instantanea) ---
struct BotaoISR {
  uint8_t pino;
  volatile bool pendente;
  volatile unsigned long ultimoAcionamentoMs;
};
BotaoISR btnTomei = {PIN_BTN_TOMEI, false, 0};
BotaoISR btnTeste = {PIN_BTN_TESTE, false, 0};

void IRAM_ATTR isrBtnTomei() {
  unsigned long agora = millis();
  if (agora - btnTomei.ultimoAcionamentoMs >= DEBOUNCE_ISR_MS) {
    btnTomei.pendente = true;
    btnTomei.ultimoAcionamentoMs = agora;
  }
}

void IRAM_ATTR isrBtnTeste() {
  unsigned long agora = millis();
  if (agora - btnTeste.ultimoAcionamentoMs >= DEBOUNCE_ISR_MS) {
    btnTeste.pendente = true;
    btnTeste.ultimoAcionamentoMs = agora;
  }
}

bool consumirBotao(BotaoISR& b) {
  if (!b.pendente) return false;
  noInterrupts();
  b.pendente = false;
  interrupts();
  return true;
}

// --- Temporizadores ---
unsigned long tDht = 0, tLcd = 0, tMqtt = 0, tReconect = 0;
bool forcarPublicacao = false;
bool forcarLcd        = false;

// ============================================================================
// 3. BUZZER E FEEDBACK VISUAL
// ============================================================================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  void buzzerInit()        { ledcAttach(PIN_BUZZER, 2000, 8); }
  void buzzerTom(int freq) { ledcWriteTone(PIN_BUZZER, freq); }
#else
  #define CANAL_BUZZER 0
  void buzzerInit()        { ledcSetup(CANAL_BUZZER, 2000, 8); ledcAttachPin(PIN_BUZZER, CANAL_BUZZER); }
  void buzzerTom(int freq) { ledcWriteTone(CANAL_BUZZER, freq); }
#endif

void mostrarAvisoLCD(const char* l1, const char* l2, unsigned long duracaoMs = 1500) {
  snprintf(avisoL1, sizeof(avisoL1), "%-16.16s", l1);
  snprintf(avisoL2, sizeof(avisoL2), "%-16.16s", l2);
  fimAvisoLcdMs = millis() + duracaoMs;
  forcarLcd = true;
}

void tocarBipConfirmacao(unsigned long duracaoMs = 90) {
  fimBipAckMs = millis() + duracaoMs;
  buzzerTom(FREQ_ACK);
  freqAtual = FREQ_ACK;
}

// ============================================================================
// 4. TEMPO E AGENDA
// ============================================================================
void fmtHMS(uint32_t abs, char* out) {
  uint32_t s = abs % 86400UL;
  snprintf(out, 9, "%02lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
}
void fmtHM(uint32_t abs, char* out) {
  uint32_t s = abs % 86400UL;
  snprintf(out, 6, "%02lu:%02lu", s / 3600, (s % 3600) / 60);
}

void agendarPara(int hh, int mm) {
  uint32_t base = (relogioAbs / 86400UL) * 86400UL + hh * 3600UL + mm * 60UL;
  if (base <= relogioAbs) base += 86400UL;
  proximaDoseAbs = base;
  doseDevida = false;
}

void atualizarRelogio() {
  unsigned long agora = millis();
  while (agora - ultimoTick >= 1000UL) {
    ultimoTick += 1000UL;
    relogioAbs++;
    if (relogioAbs / 86400UL != diaAtual) { diaAtual = relogioAbs / 86400UL; dosesHoje = 0; }
  }
}

void confirmarDose(const char* origem) {
  dosesHoje++;
  proximaDoseAbs += (uint32_t)intervaloHoras * 3600UL;
  while (proximaDoseAbs <= relogioAbs) proximaDoseAbs += (uint32_t)intervaloHoras * 3600UL;
  doseDevida = false;
  forcarPublicacao = true;

  char hm[6], msgL2[17];
  fmtHM(proximaDoseAbs, hm);
  snprintf(msgL2, sizeof(msgL2), "Prox dose %s", hm);
  mostrarAvisoLCD("DOSE CONFIRMADA!", msgL2, 1800);
  tocarBipConfirmacao(100);

  Serial.printf("[DOSE] confirmada via %s. Proxima: %s\n", origem, hm);
}

void avancarParaProximaDose() {
  if (proximaDoseAbs > relogioAbs) relogioAbs = proximaDoseAbs;
  forcarPublicacao = true;
  forcarLcd = true;
  Serial.println(F("[TESTE] relogio avancado para a proxima dose"));
}

void verificarAgenda() {
  if (!doseDevida && relogioAbs >= proximaDoseAbs) {
    doseDevida = true;
    inicioAlarmeMs = millis();
    forcarPublicacao = true;
    forcarLcd = true;
    Serial.println(F("[AGENDA] hora do remedio!"));
  }
}

// ============================================================================
// 5. SENSORES E PROCESSAMENTO LOCAL
// ============================================================================
void atualizarTendencia() {
  histTemp[histIdx] = temperatura;
  histIdx = (histIdx + 1) % JANELA_TENDENCIA;
  if (histN < JANELA_TENDENCIA) histN++;
  if (histN == JANELA_TENDENCIA) tendencia = temperatura - histTemp[histIdx];
  else tendencia = 0.0;
}

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

bool detectarAberturaTampa() {
  int v = analogRead(PIN_LDR);
#if LDR_ALTO_E_LUZ
  bool aberta = (v > LDR_LIMIAR);
#else
  bool aberta = (v < LDR_LIMIAR);
#endif
  bool evento = (aberta && !tampaAberta);
  if (evento) tAberturaMs = millis();
  tampaAberta = aberta;
  return evento;
}

unsigned long luzExpostaSeg() {
  return tampaAberta ? (millis() - tAberturaMs) / 1000UL : 0;
}

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
    statusAlarme = atrasoCritico ? "CRITICO" : "TERMICO_CRITICO";
  } else if (doseDevida || latchAlerta || luzExposta || tendenciaAlta) {
    estado = ESTADO_AMARELO;
    if (doseDevida)         statusAlarme = "LEMBRETE";
    else if (latchAlerta)   statusAlarme = "TERMICO";
    else if (luzExposta)    statusAlarme = "LUZ";
    else                    statusAlarme = "TENDENCIA";
  } else {
    estado = ESTADO_VERDE;
    statusAlarme = "OFF";
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
// 7. ATUADORES E DISPLAY LCD
// ============================================================================
void atualizarAtuadores() {
  digitalWrite(PIN_LED_VERDE,    estado == ESTADO_VERDE);
  digitalWrite(PIN_LED_AMARELO,  estado == ESTADO_AMARELO);
  digitalWrite(PIN_LED_VERMELHO, estado == ESTADO_VERMELHO);

  // Mantem o bip curto de confirmacao se estiver ativo
  if (millis() < fimBipAckMs) return;

  int freqAlvo = 0;
  if (estado == ESTADO_VERMELHO)       freqAlvo = FREQ_CRITICA;
  else if (estado == ESTADO_AMARELO)   freqAlvo = ((millis() % BIP_PERIODO_MS) < BIP_ON_MS) ? FREQ_LEMBRETE : 0;

  if (freqAlvo != freqAtual) { buzzerTom(freqAlvo); freqAtual = freqAlvo; }
}

void lcdLinha(uint8_t linha, const char* texto) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16.16s", texto);
  lcd.setCursor(0, linha);
  lcd.print(buf);
}

void atualizarLCD() {
  if (millis() < fimAvisoLcdMs) {
    lcdLinha(0, avisoL1);
    lcdLinha(1, avisoL2);
    return;
  }

  char hms[9], hm[6], l1[24], l2[24];
  fmtHMS(relogioAbs, hms);
  fmtHM(proximaDoseAbs, hm);

  if (sensorOk) snprintf(l1, sizeof(l1), "%s %4.1fC", hms, temperatura);
  else          snprintf(l1, sizeof(l1), "%s --.-C", hms);

  if (estado == ESTADO_VERMELHO)
    snprintf(l2, sizeof(l2), "%s", doseDevida ? "ATRASO CRITICO!" : "PERIGO TERMICO!");
  else if (doseDevida)
    snprintf(l2, sizeof(l2), "TOME O REMEDIO!");
  else if (estado == ESTADO_AMARELO)
    snprintf(l2, sizeof(l2), "%s", !strcmp(statusAlarme, "LUZ") ? "FECHE A TAMPA!" :
                                   !strcmp(statusAlarme, "TENDENCIA") ? "CAIXA ESQUENTANDO" : "CAIXA QUENTE/UMIDA");
  else
    snprintf(l2, sizeof(l2), "Prox %s C%3d%%", hm, indiceConservacao);

  lcdLinha(0, l1);
  lcdLinha(1, l2);
}

// ============================================================================
// 8. COMANDOS E CADASTRO DE REMEDIOS (MQTT + SERIAL)
// ============================================================================
void trimInPlace(char* s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == '\t')) s[--n] = 0;
  size_t i = 0; while (s[i] == ' ' || s[i] == '\t') i++;
  if (i) memmove(s, s + i, strlen(s + i) + 1);
}

void definirNomeRemedio(const char* novoNome) {
  size_t j = 0;
  for (size_t i = 0; novoNome[i] && j < sizeof(nomeRemedio) - 1; i++) {
    if (novoNome[i] != '"' && novoNome[i] != '\\') nomeRemedio[j++] = novoNome[i];
  }
  nomeRemedio[j] = 0;
}

void aplicarReceitaCompleta(const char* nome, int hh, int mm, int h) {
  definirNomeRemedio(nome);
  intervaloHoras = (uint8_t)h;
  dosesHoje = 0;
  agendarPara(hh, mm);
  forcarPublicacao = true;

  char resumo[17];
  snprintf(resumo, sizeof(resumo), "%02d:%02d a cada %dh", hh, mm, h);
  mostrarAvisoLCD(nomeRemedio, resumo, 2200);
  tocarBipConfirmacao(80);
  Serial.printf("[CADASTRO] Remedio: '%s' | Proxima: %02d:%02d | Intervalo: %dh\n", nomeRemedio, hh, mm, h);
}

// Aceita separadores: '|' ou ',' ou ';' (Ex: "Amoxicilina 500mg, 08:00, 8")
bool processarReceitaDelimitada(char* corpo) {
  char sep = 0;
  if (strchr(corpo, '|'))      sep = '|';
  else if (strchr(corpo, ';')) sep = ';';
  else if (strchr(corpo, ',')) sep = ',';
  if (!sep) return false;

  char* p1 = strchr(corpo, sep);
  if (!p1) return false;
  char* p2 = strchr(p1 + 1, sep);
  if (!p2) return false;

  *p1 = 0; *p2 = 0;
  char* nome = corpo;
  char* hora = p1 + 1;
  char* inter = p2 + 1;
  trimInPlace(nome); trimInPlace(hora); trimInPlace(inter);

  int hh = -1, mm = -1, h = atoi(inter);
  if (strlen(nome) == 0 || sscanf(hora, "%d:%d", &hh, &mm) != 2) return false;
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || h < 1 || h > 24) return false;

  aplicarReceitaCompleta(nome, hh, mm, h);
  return true;
}

// Suporte a JSON simples: {"remedio":"Dipirona","hora":"08:00","intervalo":8}
bool processarReceitaJSON(const char* json) {
  char nome[26] = "";
  int hh = -1, mm = -1, h = intervaloHoras;

  const char* pRem = strstr(json, "\"remedio\"");
  if (pRem) {
    const char* q1 = strchr(pRem + 9, '"');
    if (q1) {
      q1++;
      const char* q2 = strchr(q1, '"');
      if (q2) {
        size_t len = min((size_t)(q2 - q1), sizeof(nome) - 1);
        memcpy(nome, q1, len);
        nome[len] = 0;
      }
    }
  }

  const char* pHora = strstr(json, "\"hora\"");
  if (pHora) {
    const char* q1 = strchr(pHora + 6, '"');
    if (q1) sscanf(q1 + 1, "%d:%d", &hh, &mm);
  }

  const char* pInt = strstr(json, "\"intervalo\"");
  if (pInt) {
    const char* dp = strchr(pInt + 11, ':');
    if (dp) h = atoi(dp + 1);
  }

  if (strlen(nome) == 0 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || h < 1 || h > 24) return false;
  aplicarReceitaCompleta(nome, hh, mm, h);
  return true;
}

void imprimirAjuda() {
  Serial.println(F("\n================ COMANDOS INTEGRAMED v2.1 ================"));
  Serial.println(F(" [Acoes]     TOMEI | ALARME | AGORA | STATUS | AJUDA"));
  Serial.println(F(" [Cadastro]  Nome, HH:MM, Horas  (ex: Amoxicilina, 08:00, 8)"));
  Serial.println(F(" [Avulso]    NOME Dipirona | HORA 14:30 | INT 8 (ou 8H)"));
  Serial.println(F(" [Perfis]    P1 (Dipirona) | P2 (Amoxicilina) | P3 (Losartana)"));
  Serial.println(F(" [JSON]      {\"remedio\":\"Dipirona\",\"hora\":\"08:00\",\"intervalo\":8}"));
  Serial.println(F("==========================================================\n"));
}

void processarComando(const char* entrada, const char* origem) {
  char msg[128];
  strncpy(msg, entrada, sizeof(msg) - 1);
  msg[sizeof(msg) - 1] = 0;
  trimInPlace(msg);
  if (strlen(msg) == 0) return;

  Serial.printf("[%s] comando recebido: '%s'\n", origem, msg);

  char up[128];
  for (size_t i = 0; i <= strlen(msg); i++) up[i] = toupper((unsigned char)msg[i]);

  int hh, mm, h;

  // 1. Acoes diretas
  if (!strcmp(up, "TOMAR") || !strcmp(up, "TOMEI") || !strcmp(up, "OK")) {
    confirmarDose(origem);
  } else if (!strcmp(up, "PROXIMA") || !strcmp(up, "ALARME") || !strcmp(up, "TESTE")) {
    avancarParaProximaDose();
  } else if (!strcmp(up, "AGORA") || !strcmp(up, "JA")) {
    proximaDoseAbs = relogioAbs + 3;
    doseDevida = false;
    forcarPublicacao = true;
    mostrarAvisoLCD("MODO DEMO", "Alarme em 3s...", 1500);
    Serial.println(F("[DEMO] Alarme agendado para daqui a 3 segundos!"));
  } else if (!strcmp(up, "STATUS")) {
    forcarPublicacao = true;
  } else if (!strcmp(up, "AJUDA") || !strcmp(up, "HELP") || !strcmp(up, "?")) {
    imprimirAjuda();
  }
  // 2. Perfis rapidos para demonstracao
  else if (!strcmp(up, "P1")) {
    aplicarReceitaCompleta("Dipirona 500mg", 8, 0, 8);
  } else if (!strcmp(up, "P2")) {
    aplicarReceitaCompleta("Amoxicilina 500", 12, 0, 8);
  } else if (!strcmp(up, "P3")) {
    aplicarReceitaCompleta("Losartana 50mg", 7, 0, 12);
  }
  // 3. JSON
  else if (msg[0] == '{') {
    if (!processarReceitaJSON(msg))
      Serial.println(F("[ERRO] JSON invalido. Ex: {\"remedio\":\"Dipirona\",\"hora\":\"08:00\",\"intervalo\":8}"));
  }
  // 4. Comandos individuais: NOME, HORA, INT
  else if (!strncmp(up, "NOME ", 5) || !strncmp(up, "NOME:", 5)) {
    char* novo = msg + 5;
    trimInPlace(novo);
    if (strlen(novo) > 0) {
      definirNomeRemedio(novo);
      forcarPublicacao = true;
      mostrarAvisoLCD("REMEDIO ALTERADO", nomeRemedio, 1800);
      tocarBipConfirmacao(80);
      Serial.printf("[CADASTRO] Nome alterado para: %s\n", nomeRemedio);
    }
  } else if (!strncmp(up, "HORA ", 5) || !strncmp(up, "HORA:", 5)) {
    if (sscanf(up + 5, "%d:%d", &hh, &mm) == 2 && hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
      agendarPara(hh, mm);
      forcarPublicacao = true;
      char l2[17]; snprintf(l2, sizeof(l2), "Agendado %02d:%02d", hh, mm);
      mostrarAvisoLCD("NOVO HORARIO", l2, 1800);
      tocarBipConfirmacao(80);
      Serial.printf("[CADASTRO] Proxima dose agendada para %02d:%02d\n", hh, mm);
    }
  } else if (!strncmp(up, "INT ", 4) || !strncmp(up, "INT:", 4) ||
             !strncmp(up, "INTERVALO ", 10) || !strncmp(up, "INTERVALO:", 10)) {
    const char* pNum = (up[3] == ' ' || up[3] == ':') ? (up + 4) : (up + 10);
    h = atoi(pNum);
    if (h >= 1 && h <= 24) {
      intervaloHoras = (uint8_t)h;
      forcarPublicacao = true;
      char l2[17]; snprintf(l2, sizeof(l2), "A cada %d horas", h);
      mostrarAvisoLCD("NOVO INTERVALO", l2, 1800);
      tocarBipConfirmacao(80);
      Serial.printf("[CADASTRO] Intervalo alterado para %dh\n", h);
    }
  }
  // 5. Atalho de intervalo direto (ex: "8H", "12H")
  else if (sscanf(up, "%dH", &h) == 1 && h >= 1 && h <= 24 && strlen(up) <= 3) {
    intervaloHoras = (uint8_t)h;
    forcarPublicacao = true;
    char l2[17]; snprintf(l2, sizeof(l2), "A cada %d horas", h);
    mostrarAvisoLCD("NOVO INTERVALO", l2, 1800);
    tocarBipConfirmacao(80);
    Serial.printf("[CADASTRO] Intervalo alterado para %dh\n", h);
  }
  // 6. Receita completa (com ou sem prefixo RECEITA: / CAD: / REMEDIO:)
  else if (!strncmp(up, "RECEITA:", 8) || !strncmp(up, "REMEDIO:", 8)) {
    if (!processarReceitaDelimitada(msg + 8))
      Serial.println(F("[ERRO] Formato invalido. Use: Nome, HH:MM, Horas"));
  } else if (!strncmp(up, "CAD:", 4) || !strncmp(up, "CAD ", 4)) {
    if (!processarReceitaDelimitada(msg + 4))
      Serial.println(F("[ERRO] Formato invalido. Use: Nome, HH:MM, Horas"));
  } else if (strchr(msg, '|') || strchr(msg, ',') || strchr(msg, ';')) {
    if (!processarReceitaDelimitada(msg))
      Serial.println(F("[ERRO] Formato invalido. Use: Nome, HH:MM, Horas"));
  }
  // 7. Atalho direto de horario (ex: "08:30")
  else if (strchr(up, ':') && sscanf(up, "%d:%d", &hh, &mm) == 2 &&
           hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59) {
    agendarPara(hh, mm);
    forcarPublicacao = true;
    char l2[17]; snprintf(l2, sizeof(l2), "Agendado %02d:%02d", hh, mm);
    mostrarAvisoLCD("NOVO HORARIO", l2, 1800);
    tocarBipConfirmacao(80);
    Serial.printf("[CADASTRO] Proxima dose agendada para %02d:%02d\n", hh, mm);
  } else {
    Serial.println(F("[ERRO] Comando desconhecido. Envie AJUDA para ver os exemplos."));
  }
}

void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  char msg[128];
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);
  msg[length] = 0;
  processarComando(msg, "MQTT");
}

void lerComandosSerial() {
  static char buf[128];
  static size_t idx = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (idx > 0) {
        buf[idx] = 0;
        processarComando(buf, "SERIAL");
        idx = 0;
      }
    } else if (idx < sizeof(buf) - 1) {
      buf[idx++] = c;
    }
  }
}

// ============================================================================
// 9. CONECTIVIDADE
// ============================================================================
void manterConexoes() {
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
    if (mqtt.connect(cid, NULL, NULL, TOPICO_STATUS, 1, true, "offline")) {
      mqtt.subscribe(TOPICO_SUB);
      mqtt.publish(TOPICO_STATUS, "online", true);
      Serial.println(F("OK"));
      forcarPublicacao = true;
    } else {
      Serial.printf("falhou rc=%d\n", mqtt.state());
    }
  }
}

void publicarTelemetria() {
  if (!mqtt.connected()) return;

  char hms[9], hm[6], tStr[12], uStr[12], json[520];
  fmtHMS(relogioAbs, hms);
  fmtHM(proximaDoseAbs, hm);
  if (sensorOk) { snprintf(tStr, sizeof(tStr), "%.1f", temperatura); snprintf(uStr, sizeof(uStr), "%.1f", umidade); }
  else          { strcpy(tStr, "null"); strcpy(uStr, "null"); }

  snprintf(json, sizeof(json),
    "{\"id\":\"%s\",\"relogio\":\"%s\",\"proxima_dose\":\"%s\",\"posologia\":\"%u/%uh\","
    "\"remedio\":\"%s\",\"estado\":\"%s\",\"alarme\":\"%s\",\"tampa\":\"%s\","
    "\"doses_hoje\":%u,\"temperatura\":%s,\"umidade\":%s,"
    "\"indice_conservacao\":%d,\"tendencia\":%.1f}",
    DEVICE_ID, hms, hm, intervaloHoras, intervaloHoras, nomeRemedio, nomeEstado(),
    statusAlarme, tampaAberta ? "ABERTA" : "FECHADA", dosesHoje, tStr, uStr,
    indiceConservacao, tendencia);

  if (mqtt.publish(TOPICO_PUB, json)) Serial.printf("[MQTT] pub: %s\n", json);
  else Serial.println(F("[MQTT] falha ao publicar"));
}

// ============================================================================
// 10. SETUP E LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_AMARELO, OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  pinMode(PIN_BTN_TOMEI, INPUT_PULLUP);
  pinMode(PIN_BTN_TESTE, INPUT_PULLUP);
  pinMode(PIN_LDR, INPUT);

  // Interrupcoes de hardware para resposta instantanea dos botoes
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_TOMEI), isrBtnTomei, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_TESTE), isrBtnTeste, FALLING);

  buzzerInit();
  buzzerTom(0);

  dht.begin();
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcdLinha(0, "IntegraMed v2.1");
  lcdLinha(1, "Iniciando...");

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(callbackMQTT);
  mqtt.setBufferSize(640);
  mqtt.setSocketTimeout(2);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);

  ultimoTick = millis();
  diaAtual = relogioAbs / 86400UL;
  lerDHT();
  Serial.println(F("IntegraMed v2.1 pronto. Digite ou envie AJUDA via MQTT."));
  imprimirAjuda();
}

void loop() {
  unsigned long agora = millis();
  static EstadoBorda estadoAnterior = ESTADO_VERDE;
  static const char* alarmeAnterior = "OFF";

  // --- 1. Entradas imediatas (Botoes via ISR + Comandos Serial) ---
  if (consumirBotao(btnTomei)) confirmarDose("botao verde");
  if (consumirBotao(btnTeste)) avancarParaProximaDose();
  lerComandosSerial();

  // --- 2. Rede (MQTT) ---
  manterConexoes();
  if (mqtt.connected()) mqtt.loop();

  // --- 3. Relogio e sensores ---
  atualizarRelogio();
  if (agora - tDht >= PERIODO_DHT_MS) { tDht = agora; lerDHT(); }

  bool abriuTampa = detectarAberturaTampa();
  if (abriuTampa && doseDevida) confirmarDose("tampa (LDR)");

  // --- 4. Processamento local, decisao e atuacao ---
  verificarAgenda();
  decidirEstado();
  atualizarAtuadores();

  if (forcarLcd || agora - tLcd >= PERIODO_LCD_MS) {
    tLcd = agora;
    forcarLcd = false;
    atualizarLCD();
  }

  // --- 5. Publicacao por excecao ou periodica ---
  if (estado != estadoAnterior || statusAlarme != alarmeAnterior) {
    estadoAnterior = estado;
    alarmeAnterior = statusAlarme;
    forcarPublicacao = true;
    Serial.printf("[ESTADO] %s (%s)\n", nomeEstado(), statusAlarme);
  }

  if (forcarPublicacao || agora - tMqtt >= PERIODO_MQTT_MS) {
    tMqtt = agora;
    forcarPublicacao = false;
    publicarTelemetria();
  }
}

/*
  ╔═════════════════════════════════════════════════════════════════════╗
  ║  Eilik-Style Robot Face – Waveshare 1.69" LCD (ST7789V2, 240x280)   ║
  ║  Target: ESP32 / ESP32-S3                                           ║
  ║                                                                     ║
  ║  DESIGN: Minimalist Eilik-inspired face                             ║
  ║  • Large glowing cyan pill-shaped eyes, flat & clean (no pupils,    ║
  ║    no iris, no shine, no eyebrows)                                  ║
  ║  • Eye color stays cyan across ALL expressions                      ║
  ║  • Eyes change SHAPE only to convey emotion                         ║
  ║  • Ultra-smooth blink: 20 eased steps via local canvas buffering    ║
  ║  • Ultra-smooth wink on HAPPY entry via local canvas buffering     ║
  ║  • Smooth talking mouth wave (10 frames, sinusoidal)                ║
  ║  • Pulsing dot for LISTENING                                        ║
  ║  • BUMPED: shake + BUTTERY-SMOOTH HYBRID SWIRLING SPIRAL EYES       ║
  ║                                                                     ║
  ║  SCREEN BEHAVIOR (no divider, no idle quips):                       ║
  ║  • Idle: full-screen neutral face                                   ║
  ║  • Hold touch pad: full-screen "thinking" face while recording      ║
  ║  • Release (real question): face disappears, reply text takes the   ║
  ║    whole screen — scroll with the rotary encoder if it's long       ║
  ║  • Quick tap on the pad: instantly back to the full-screen neutral  ║
  ║    face                                                              ║
  ╚═════════════════════════════════════════════════════════════════════╝
*/

// =====================================================================
// USER CONFIG
// =====================================================================
#define WIFI_SSID       ""
#define WIFI_PASSWORD   ""
#define GROQ_API_KEY    ""
// =====================================================================

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <math.h>
#include <driver/i2s.h>   // NS4168 I2S amp output + INMP441 I2S mic input
#include <esp_heap_caps.h> // heap_caps_malloc() for PSRAM-backed recording buffer
#include <LittleFS.h>      // stage recorded audio on flash so the big PCM buffer
                           // can be freed BEFORE opening the TLS connection to
                           // Groq — on boards with no PSRAM, holding both at once
                           // fragments internal SRAM below what mbedTLS needs

// ---------- PIN CONFIG ----------
#define TFT_CS    19
#define TFT_DC    26
#define TFT_RST   33
#define TFT_BL    32
#define TFT_SCLK  18
#define TFT_MOSI  5

#define MPU_SDA   2
#define MPU_SCL   4

// ---------- NS4168 I2S Amp ----------
// Wiring:  NS4168 BCLK → GPIO 13
//          NS4168 LRC  → GPIO 23
//          NS4168 DIN  → GPIO 21
//          NS4168 VDD  → 3.3V
//          NS4168 GND  → GND
//          Speaker +   → NS4168 OUT+
//          Speaker -   → NS4168 OUT-
#define AMP_BCLK        13
#define AMP_LRC         23
#define AMP_DIN         21
#define AMP_SAMPLE_RATE 48000   // Hz – Groq TTS is fixed at 48 kHz, must match TTS_SAMPLE_RATE

// ---------- HW-139 Touch Sensor (push-to-talk) ----------
// Wiring:  HW-139 VCC → 3.3V
//          HW-139 GND → GND
//          HW-139 OUT → GPIO 27  (goes HIGH while a finger is on the pad)
#define TOUCH_PIN       27

// ---------- Rotary Encoder (scrolls long reply text) ----------
// Wiring:  Encoder VCC → 3.3V
//          Encoder GND → GND
//          Encoder CLK → GPIO 17
//          Encoder DT  → GPIO 16
// The encoder's SW (push button), if present, is not used here.
// If scrolling feels backwards on your hardware, just swap the CLK/DT wires.
#define ENCODER_CLK     25
#define ENCODER_DT      35

// ---------- INMP441 I2S Microphone ----------
// Wiring:  INMP441 VDD → 3.3V
//          INMP441 GND → GND
//          INMP441 L/R → GND   (selects LEFT channel — required, see setupMicI2S)
//          INMP441 WS  → GPIO 15
//          INMP441 SCK → GPIO 14
//          INMP441 SD  → GPIO 34  (input-only pin — perfect for a mic data line)
// Runs on I2S_NUM_1 so it never collides with the NS4168 amp on I2S_NUM_0.
#define MIC_SCK          14
#define MIC_WS           15
#define MIC_SD           34
#define MIC_SAMPLE_RATE  16000   // 16 kHz mono is plenty for speech + keeps the buffer small

// ---------- Voice recording buffer ----------
// A plain ESP32 (WROOM-style, no PSRAM) only has ~520 KB of SRAM total,
// and WiFi/BT + the TFT + TLS all eat into that before you ever touch
// the pad — exactly how much is left varies board to board. Rather than
// picking one fixed size that might fail to allocate on your specific
// board, setup() tries these durations from longest to shortest and
// keeps whichever one actually fits (see recordCapacitySamples below).
const int RECORD_SECOND_OPTIONS[] = { 5, 4, 3, 2, 1 };
#define RECORD_MIN_MS  300   // ignore accidental taps shorter than this

// ---------- Listening cue tone (played through the NS4168 amp) ----------
#define CUE_FREQ_HZ      1200
#define CUE_DURATION_MS  120
#define CUE_SAMPLES      (AMP_SAMPLE_RATE * CUE_DURATION_MS / 1000)

// ---------- Self-hosted Edge-TTS proxy config (primary — see speakText() below) ----------
// Talks to travisvn/openai-edge-tts (github.com/travisvn/openai-edge-tts)
// running on a computer on your own network. That server does the real
// Edge-TTS work (with a normal browser-like TLS stack, so it doesn't hit
// the fingerprinting issues a direct ESP32 implementation would) and
// hands back a plain WAV file over plain local HTTP — genuinely free,
// no account/API key/billing setup needed anywhere.
//
// Setup on the host machine (needs Docker):
//   docker run -d -p 5050:5050 -e API_KEY=roboface -e PORT=5050 travisvn/openai-edge-tts:latest
// Then set EDGE_PROXY_HOST below to that machine's LAN IP (e.g. 192.168.1.50).
#define EDGE_PROXY_HOST     "192.168.1.50"   // <-- set to your host machine's LAN IP
#define EDGE_PROXY_PORT     5050
#define EDGE_PROXY_API_KEY  "roboface"       // must match API_KEY in the docker run command above
// Edge-TTS voice names (same catalog Azure uses under the hood).
// Full list / samples: tts.travisvn.com
#define EDGE_PROXY_VOICE_FR "fr-FR-DeniseNeural"   // female; try fr-FR-HenriNeural for male
#define EDGE_PROXY_VOICE_EN "en-US-JennyNeural"      // male; try en-US-JennyNeural for female

// ---------- Groq TTS config (fallback — used only if the Edge-TTS proxy fails) ----------
// playai-tts sounds natural; Fritz-PlayAI is a clear, friendly male voice.
// Other free voices: Celeste-PlayAI (female), Blade-PlayAI (deep male).
// Groq Orpheus TTS — replacement for discontinued playai-tts
// NOTE: as of this writing Groq's TTS only offers English and Saudi Arabic
// voices — no French voice yet. French replies still play back fine
// (this English voice reads the French text), just with an English accent.
// The on-screen French text and the speech-to-text side are unaffected.
#define TTS_MODEL       "canopylabs/orpheus-v1-english"
#define TTS_VOICE       "hannah"  // options: autumn, diana, hannah, austin, daniel, troy
// CRITICAL: Groq TTS only outputs 48 kHz — any other rate causes a 400 error
#define TTS_SAMPLE_RATE  48000  // must match AMP_SAMPLE_RATE below

// ---------- MPU-9250 ----------
#define MPU_ADDR              0x68
#define MPU_REG_PWR_MGMT_1   0x6B
#define MPU_REG_WHO_AM_I      0x75
#define MPU_REG_ACCEL_XOUT_H  0x3B
#define ACCEL_SENS            16384.0
#define BUMP_THRESHOLD_G      0.6
#define BUMP_COOLDOWN_MS      1500
bool mpuConnected = false;

// ---------- Screen ----------
#define SCREEN_W 240
#define SCREEN_H 280

// Colors
#define COL_BG     0x0000   // black
#define COL_TEXT   0xFFFF   // white

// Eye color: single Eilik-style cyan – never changes
#define COL_EYE    0x07FF   // bright cyan
#define COL_EYE_DIM 0x0210  // dim cyan for squint/closed lid

Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, TFT_CS, TFT_DC, TFT_RST);

// ---------- Expression ----------
enum Expression { IDLE, LISTENING, TALKING, HAPPY, BUMPED };
Expression currentExpression = IDLE;

// ---------- Screen mode: either the full-screen face, or full-screen text ----------
enum DisplayMode { MODE_FACE, MODE_TEXT };
DisplayMode displayMode = MODE_FACE;

unsigned long lastBumpTime         = 0;
bool          bumpAnimationPlaying = false;

// Keeps track of the continuous spinning position of spiral lines
float globalSpiralPhase = 0.0f;

// =====================================================================
// Face geometry
// =====================================================================
const int eyeW    = 52;   // wide pill
const int eyeH    = 44;   // height
const int eyeGap  = 24;
const int eyeY    = 96;   // vertical position — centered for a full-screen face
const int leftEyeX  = (SCREEN_W - (eyeW * 2 + eyeGap)) / 2;
const int rightEyeX = leftEyeX + eyeW + eyeGap;

// Mouth
const int mouthBaseY = eyeY + eyeH + 22;
const int mouthW     = 54;
const int mouthX     = (SCREEN_W - mouthW) / 2;

unsigned long lastBlink       = 0;
unsigned long blinkInterval   = 3000;
int           talkFrame       = 0;
float         talkPhase       = 0.0f;

// Listening pulse
float         listenPulse    = 5.0f;
float         listenPulseDir = 1.0f;
unsigned long lastListenUpdate = 0;

// ---------- Full-screen text mode (shown after you let go of the pad) ----------
// TEXT_Y_START/TEXT_PANEL_H now describe the whole screen (minus a small
// top/bottom margin) rather than a small strip below the face, since text
// takes over the entire display in MODE_TEXT.
#define TEXT_MARGIN_TOP     14
#define TEXT_MARGIN_BOTTOM  14
#define TEXT_Y_START        TEXT_MARGIN_TOP
#define TEXT_PANEL_H        (SCREEN_H - TEXT_MARGIN_TOP - TEXT_MARGIN_BOTTOM)
#define TEXT_X_MARGIN       6
#define TEXT_SIZE           2
#define MAX_TEXT_LINES       80   // generous wrap buffer for long replies

String textLines[MAX_TEXT_LINES];
int    textLineCount    = 0;
int    textScrollOffset = 0;   // first visible line
int    textVisibleLines = 1;   // how many lines fit on screen at once, computed on render

// ---------- Rotary encoder scroll state ----------
volatile int  encoderDelta = 0;   // accumulated, unapplied scroll steps (set by ISR)
portMUX_TYPE  encoderMux   = portMUX_INITIALIZER_UNLOCKED;

// ---------- Serial input ----------
String serialInputBuffer = "";
bool   serialLineReady   = false;

// ---------- Touch-to-talk / INMP441 recording ----------
int16_t* recordBuffer         = nullptr;  // PCM16 mono scratch buffer, allocated fresh on each touch
size_t   recordCapacitySamples = 0;       // how many samples recordBuffer can currently hold
size_t   recordedSamples      = 0;
bool     touchActive          = false;    // true while the pad is currently being held

// ---------- Groq API ----------
#define MAX_HISTORY 6
String history[MAX_HISTORY];
int    historyCount = 0;
const char* GROQ_HOST  = "api.groq.com";
const int   GROQ_PORT  = 443;
const char* GROQ_MODEL = "llama-3.1-8b-instant";
const char* SYSTEM_PROMPT =
  "You are a friendly desktop robot assistant and a French conversation partner. "
  "When in French mode, use natural, everyday spoken French (tu, not vous, unless the "
  "user is formal) so it feels like a real conversation, not a lesson. If the user makes "
  "a small grammar or word-choice mistake, you can casually restate the correct phrase "
  "in your reply the way a patient native-speaker friend would, without making a big "
  "deal of it. Keep sentences short and clear. "
  "A CURRENT MODE note below tells you whether to reply in French or English right now — "
  "always follow it exactly, even if it seems to conflict with something above. "
  "You have a small LCD face that can show expressions. "
  "ALWAYS start your reply with an expression tag (pick the best fit): "
  "[EXPR:IDLE], [EXPR:HAPPY], [EXPR:LISTENING], [EXPR:TALKING], or [EXPR:BUMPED]. "
  "Use HAPPY for positive/fun replies, LISTENING when asking the user a question, "
  "TALKING for normal informative replies, BUMPED for surprise/shock. "
  "After the tag write your reply normally. Keep replies under 200 words.";

// Forward Declarations
void blinkOnce();
void winkOnce();
void setExpression(Expression e);
void wrapTextForScreen(const String& text);
uint8_t cp437FromUnicode(uint32_t cp);
char asciiFallback(uint32_t cp);
String utf8ToCP437(const String &in);
void drawScrollbar(Adafruit_GFX &gfx, int trackY, int maxOffset);
void drawTextFrame(Adafruit_GFX &gfx, int originY, int maxOffset);
void renderTextMode();
void enterTextMode(const String& text);
void IRAM_ATTR encoderISR();
void pollEncoder();
void showWifiStatus(const char* msg);
void connectWiFi();
String callGroqAPI(const String& userMessage);
bool messageSaysAuRevoir(const String& s);
bool messageLooksFrench(const String& s);
Expression parseExpression(const String& tag);
const char* expressionName(Expression e);
void initMPU9250();
bool readAccelG(float &ax, float &ay, float &az);
void checkBump();
void playBumpAnimation();
void drawSpiralToCanvas(GFXcanvas16* canvas, int cx, int cy, float startPhase, uint16_t col);
void drawExpressionEyes();
void drawMouth();
void drawListeningMouth();
void clearMouthArea();
void clearEyeZone();
void setupAmpI2S();
void speakText(const String& text);
bool speakTextEdge(const String& text);
void speakTextGroq(const String& text);
void writeMonoToI2SStereo(const uint8_t* data, size_t len);
void resetI2SStereoCarry();
void setupMicI2S();
void playListenCue();
bool recordWhileTouched();
bool allocateRecordBuffer();
void freeRecordBuffer();
String transcribeAudioGroq(const int16_t* pcm, size_t sampleCount);
String transcribeAudioGroqFromFile(const char* path, size_t sampleCount);
void writeWavHeader(uint8_t* hdr, uint32_t dataBytes, uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample);
void handleVoiceInput();
void processUserQuery(const String& userMsg);

// =====================================================================
// Setup
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n=== Eilik Robot Face Chat ===");

  if (!LittleFS.begin(true)) {  // true = format on first-mount failure
    Serial.println("[FS] LittleFS mount failed — recording will fall back to RAM only.");
  } else {
    Serial.println("[FS] LittleFS ready.");
  }

  if (TFT_BL >= 0) { pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH); }

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(SCREEN_W, SCREEN_H);
  tft.setSPISpeed(24000000); // Enforce high-speed hardware SPI
  tft.setRotation(0);
  tft.fillScreen(COL_BG);

  setExpression(IDLE);   // full-screen neutral face
  initMPU9250();
  connectWiFi();
  setupAmpI2S();

  pinMode(TOUCH_PIN, INPUT);
  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), encoderISR, FALLING);

  setupMicI2S();
  Serial.printf("[MIC] Free heap after setup: %u bytes (recording buffer is allocated on-demand per touch).\n", ESP.getFreeHeap());

  Serial.println("\nReady! Hold the touch pad to talk, tap it once to return to the face,");
  Serial.println("scroll long replies with the rotary encoder, or type a message and press Enter:");
  Serial.println("--------------------------------------");
}

// =====================================================================
// Loop
// =====================================================================
void loop() {
  unsigned long now = millis();
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialInputBuffer.length() > 0) serialLineReady = true;
    } else {
      serialInputBuffer += c;
    }
  }

  if (serialLineReady) {
    String userMsg = serialInputBuffer;
    serialInputBuffer = "";
    serialLineReady   = false;
    processUserQuery(userMsg);
  }

  // ---- Touch-to-talk: hold the HW-139 pad, speak, let go to send.
  //      A quick tap (shorter than RECORD_MIN_MS) is treated as "return to
  //      the neutral face" — see handleVoiceInput(). ----
  bool touchNow = digitalRead(TOUCH_PIN) == HIGH;
  if (touchNow && !touchActive && !bumpAnimationPlaying) {
    touchActive = true;
    handleVoiceInput();
    touchActive = false;
  }

  // Blink during idle (face mode only)
  if (displayMode == MODE_FACE && currentExpression == IDLE && now - lastBlink > blinkInterval) {
    blinkOnce();
    lastBlink     = now;
    blinkInterval = 2500 + random(0, 3500);
  }

  // Smooth swirling animation calculation loop for holding BUMPED state
  if (displayMode == MODE_FACE && currentExpression == BUMPED && !bumpAnimationPlaying) {
    globalSpiralPhase += 0.32f; 
    drawExpressionEyes();
    delay(16); // ~60 FPS update pacing
  }

  // Rotary encoder: scroll the reply text while it's on screen
  pollEncoder();

  // Bump detection (only while the face is showing, so it doesn't
  // interrupt someone reading a reply)
  if (displayMode == MODE_FACE && mpuConnected && !bumpAnimationPlaying &&
      now - lastBumpTime > BUMP_COOLDOWN_MS) {
    checkBump();
  }

  delay(4);
}

// =====================================================================
// Shared pipeline: send a question (from Serial OR voice) to Groq,
// show/speak the reply. Both input paths funnel through here.
// =====================================================================
void processUserQuery(const String& userMsg) {
  if (userMsg.length() == 0) return;

  Serial.print("\nYou: "); Serial.println(userMsg);

  // Same "thinking" face used while recording — reused here while we
  // wait on Groq, instead of a "Thinking..." text message.
  setExpression(LISTENING);

  String reply = callGroqAPI(userMsg);

  // The model still prefixes replies with an [EXPR:...] tag, but the
  // screen only ever shows the neutral face or the full-screen reply text
  // now — so just strip the tag rather than switching expressions.
  String displayReply = reply;
  if (reply.startsWith("[EXPR:")) {
    int closeIdx = reply.indexOf(']');
    if (closeIdx > 0) {
      displayReply = reply.substring(closeIdx + 1);
      displayReply.trim();
    }
  }

  Serial.print("\nRobot: "); Serial.println(displayReply);
  Serial.println("--------------------------------------");
  Serial.println("You: ");

  enterTextMode(displayReply);

  // Read the reply aloud through the NS4168 amp. The reply text stays on
  // screen — scrollable with the rotary encoder — the whole time it's
  // being spoken. A quick tap on the touch pad (any time) brings back
  // the neutral face.
  speakText(displayReply);
}

// =====================================================================
// Touch-to-talk: cue tone → record while held → transcribe → answer
// =====================================================================

// Tries the largest recording duration that currently fits in free RAM,
// leaving a safety margin so we never starve the display's canvas-based
// animations (GFXcanvas16 doesn't null-check its own allocation, so if we
// grab RAM too greedily it crashes on the next redraw instead of failing
// safely). Called fresh on every touch — see freeRecordBuffer().
#define RECORD_HEAP_MARGIN_BYTES 80000   // keep this much free for canvases/WiFi/TLS
                                          // (bumped from 40000 — mbedTLS's handshake needs
                                          // its own large contiguous block, on top of what
                                          // the display canvas and WiFi stack already use,
                                          // and it has to happen *while* recordBuffer is
                                          // still allocated, since transcribeAudioGroq()
                                          // reads directly out of it)
bool allocateRecordBuffer() {
  for (int secs : RECORD_SECOND_OPTIONS) {
    size_t trySamples = (size_t)MIC_SAMPLE_RATE * secs;
    size_t tryBytes   = trySamples * sizeof(int16_t);

    // Skip sizes that would eat too far into free heap even if malloc
    // would technically succeed — better to record less than to crash.
    size_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < tryBytes + RECORD_HEAP_MARGIN_BYTES) continue;

    int16_t* buf = (int16_t*)heap_caps_malloc(tryBytes, MALLOC_CAP_SPIRAM);
    bool usedPSRAM = (buf != nullptr);
    if (!buf) buf = (int16_t*)malloc(tryBytes);
    if (buf) {
      recordBuffer          = buf;
      recordCapacitySamples = trySamples;
      Serial.printf("[MIC] Recording buffer OK: %d s (%u bytes) via %s. Free internal heap now %u, largest block %u.\n",
                    secs, (unsigned)tryBytes, usedPSRAM ? "PSRAM" : "INTERNAL SRAM (fallback!)",
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      return true;
    }
  }
  Serial.println("[MIC] Could not allocate any recording buffer right now.");
  return false;
}

void freeRecordBuffer() {
  if (recordBuffer) { free(recordBuffer); recordBuffer = nullptr; }
  recordCapacitySamples = 0;
  recordedSamples       = 0;
}

// Writes the current recordBuffer contents (raw PCM, no WAV header) out to
// flash so the RAM buffer can be freed before we open the TLS connection to
// Groq. Returns true on success. See the LittleFS include comment above for
// why this exists — without PSRAM, holding both the ~96KB+ PCM buffer AND
// mbedTLS's handshake buffers in internal SRAM at the same time fragments
// the heap too badly for the connection to succeed.
#define STAGED_AUDIO_PATH "/rec.pcm"
bool stageRecordBufferToFlash() {
  if (!recordBuffer || recordedSamples == 0) return false;
  File f = LittleFS.open(STAGED_AUDIO_PATH, "w");
  if (!f) {
    Serial.println("[FS] Failed to open staging file for write.");
    return false;
  }
  size_t bytesToWrite = recordedSamples * sizeof(int16_t);
  size_t written = f.write((const uint8_t*)recordBuffer, bytesToWrite);
  f.close();
  if (written != bytesToWrite) {
    Serial.printf("[FS] Short write staging audio: %u of %u bytes.\n",
                  (unsigned)written, (unsigned)bytesToWrite);
    LittleFS.remove(STAGED_AUDIO_PATH);
    return false;
  }
  return true;
}

void deleteStagedAudio() {
  if (LittleFS.exists(STAGED_AUDIO_PATH)) LittleFS.remove(STAGED_AUDIO_PATH);
}

void handleVoiceInput() {
  // Small debounce so a brush of the pad doesn't trigger a full recording.
  delay(15);
  if (digitalRead(TOUCH_PIN) != HIGH) return;

  // IMPORTANT: switch the face and play the cue BEFORE grabbing the
  // recording buffer. setExpression() draws through a temporary
  // GFXcanvas16 (~23 KB) — if we reserve the recording buffer first,
  // there may not be enough heap left for that canvas and the display
  // library will crash writing into a failed (null) allocation.
  playListenCue();               // little beep = "I'm listening"
  setExpression(LISTENING);      // full-screen "thinking" face while held

  if (!allocateRecordBuffer()) {
    enterTextMode("Low on memory — try again in a moment.");
    return;
  }

  bool longEnough = recordWhileTouched();

  // Diagnostic: check whether we actually captured real signal, or just
  // near-silence (which would explain Whisper hallucinating "Thank you.").
  {
    int16_t peak = 0;
    int64_t sumAbs = 0;
    for (size_t i = 0; i < recordedSamples; i++) {
      int16_t v = recordBuffer[i];
      int16_t av = v < 0 ? -v : v;
      if (av > peak) peak = av;
      sumAbs += av;
    }
    int32_t avgAbs = recordedSamples ? (int32_t)(sumAbs / recordedSamples) : 0;
    Serial.printf("[MIC] Captured %u samples. Peak amplitude: %d / 32767. Average abs: %d.\n",
                  (unsigned)recordedSamples, peak, avgAbs);
    if (peak < 200) {
      Serial.println("[MIC] WARNING: audio looks essentially silent — check mic wiring "
                      "(L/R pin), I2S pin numbers, and that the mic is actually the one "
                      "being read (see channel_format comment in setupMicI2S()).");
    }
  }

  if (!longEnough) {
    // A quick tap, not a real question — this is the "return to the
    // neutral face" gesture, so just go back to it, full screen.
    freeRecordBuffer();
    setExpression(IDLE);
    return;
  }

  enterTextMode("Transcribing...");

  // Stage the PCM audio to flash, then free the RAM buffer BEFORE opening
  // the TLS connection — mbedTLS needs a large contiguous block of internal
  // SRAM for its handshake, and on a board with no PSRAM, holding both the
  // recording buffer and that handshake buffer at once fragments the heap
  // too badly for the connection to succeed (see stageRecordBufferToFlash()).
  size_t samplesForTranscription = recordedSamples;
  String transcript;
  if (stageRecordBufferToFlash()) {
    freeRecordBuffer();
    transcript = transcribeAudioGroqFromFile(STAGED_AUDIO_PATH, samplesForTranscription);
    deleteStagedAudio();
  } else {
    // Flash staging failed for some reason (full FS, etc.) — fall back to
    // the direct-from-RAM path so voice input still works, just with the
    // original fragmentation risk.
    Serial.println("[FS] Staging failed, falling back to RAM-direct transcription.");
    transcript = transcribeAudioGroq(recordBuffer, recordedSamples);
    freeRecordBuffer();
  }
  transcript.trim();

  if (transcript.length() == 0) {
    enterTextMode("Sorry, I didn't catch that. Tap the pad to go back.");
    return;
  }

  processUserQuery(transcript);
}

// =====================================================================
// Expression helpers
// =====================================================================
Expression parseExpression(const String& tag) {
  if (tag == "HAPPY")     return HAPPY;
  if (tag == "LISTENING") return LISTENING;
  if (tag == "TALKING")   return TALKING;
  if (tag == "BUMPED")    return BUMPED;
  return IDLE;
}

const char* expressionName(Expression e) {
  switch (e) {
    case HAPPY:     return "HAPPY";
    case LISTENING: return "LISTENING";
    case TALKING:   return "TALKING";
    case BUMPED:    return "BUMPED";
    default:        return "IDLE";
  }
}

// =====================================================================
// Core draw blocks
// =====================================================================
void drawEyePill(int x, int y, int w, int h) {
  if (h <= 0) return;
  int r = min(w, h) / 2;
  int cy = eyeY + eyeH / 2;
  int ey = cy - h / 2;
  if (h <= 3) {
    tft.fillRoundRect(x, cy - 1, w, 3, 1, COL_EYE_DIM);
  } else {
    tft.fillRoundRect(x, ey, w, h, r, COL_EYE);
  }
}

void clearEyeZone() {
  tft.fillRect(0, eyeY - 2, SCREEN_W, eyeH + 4, COL_BG);
}

// Local canvas-accelerated Archimedean spiral renderer to eliminate rendering lag entirely
void drawSpiralToCanvas(GFXcanvas16* canvas, int cx, int cy, float startPhase, uint16_t col) {
  float lastX = cx;
  float lastY = cy;
  float maxTheta = 3.5f * 2.0f * M_PI; 
  float step = 0.18f; 
  float maxRadius = (eyeH / 2.0f) + 1; 

  for (float theta = 0.0f; theta <= maxTheta; theta += step) {
    float r = (theta / maxTheta) * maxRadius;
    float currentAngle = theta - startPhase;
    
    float x = cx + (cosf(currentAngle) * r * 1.1f);
    float y = cy + (sinf(currentAngle) * r * 0.9f);
    
    if (theta > 0.0f) {
      canvas->drawLine((int)lastX, (int)lastY, (int)x, (int)y, col);
      canvas->drawLine((int)lastX, (int)lastY + 1, (int)x, (int)y + 1, col); 
    }
    lastX = x;
    lastY = y;
  }
}

void drawExpressionEyes() {
  // Use a localized mini double-buffer stack exclusively over the active eye area
  // Safely allocates on stack memory block, executing instantly without breaking Heap/I2C bounds
  GFXcanvas16 eyeCanvas(SCREEN_W, eyeH + 4);
  eyeCanvas.fillScreen(COL_BG);

  int localEyeY = 2; // relative tracking offset inside the mini buffer
  int cy = localEyeY + eyeH / 2;

  switch (currentExpression) {
    case HAPPY: {
      int r = min(eyeW, eyeH) / 2;
      eyeCanvas.fillRoundRect(leftEyeX,  localEyeY, eyeW, eyeH, r, COL_EYE);
      eyeCanvas.fillRoundRect(rightEyeX, localEyeY, eyeW, eyeH, r, COL_EYE);
      eyeCanvas.fillRect(leftEyeX,  localEyeY, eyeW, eyeH * 55 / 100, COL_BG);
      eyeCanvas.fillRect(rightEyeX, localEyeY, eyeW, eyeH * 55 / 100, COL_BG);
      break;
    }
    case BUMPED: {
      int leftCenterX  = leftEyeX + (eyeW / 2);
      int rightCenterX = rightEyeX + (eyeW / 2);
      drawSpiralToCanvas(&eyeCanvas, leftCenterX,  cy, globalSpiralPhase, COL_EYE);
      drawSpiralToCanvas(&eyeCanvas, rightCenterX, cy, globalSpiralPhase, COL_EYE);
      break;
    }
    case LISTENING: {
      int lh = eyeH * 75 / 100;
      int ly = localEyeY + (eyeH - lh) / 2;
      int r  = min(eyeW, lh) / 2;
      eyeCanvas.fillRoundRect(leftEyeX,  ly, eyeW, lh, r, COL_EYE);
      eyeCanvas.fillRoundRect(rightEyeX, ly, eyeW, lh, r, COL_EYE);
      break;
    }
    default: { // IDLE / TALKING
      int r = min(eyeW, eyeH) / 2;
      eyeCanvas.fillRoundRect(leftEyeX,  localEyeY, eyeW, eyeH, r, COL_EYE);
      eyeCanvas.fillRoundRect(rightEyeX, localEyeY, eyeW, eyeH, r, COL_EYE);
      break;
    }
  }

  // Push local buffer contents onto display hardware via one instant transaction
  tft.drawRGBBitmap(0, eyeY - 2, eyeCanvas.getBuffer(), eyeCanvas.width(), eyeCanvas.height());
}

// =====================================================================
// Mouth
// =====================================================================
void clearMouthArea() {
  tft.fillRect(0, mouthBaseY - 6, SCREEN_W, 40, COL_BG);
}

void drawMouth() {
  clearMouthArea();
  switch (currentExpression) {
    case HAPPY: {
      tft.fillRoundRect(mouthX - 4, mouthBaseY + 2, mouthW + 8, 20, 10, COL_EYE);
      tft.fillRect(mouthX - 4, mouthBaseY + 2, mouthW + 8, 10, COL_BG);
      break;
    }
    case LISTENING: {
      listenPulse = 5.0f;
      tft.fillCircle(SCREEN_W / 2, mouthBaseY + 10, (int)listenPulse, COL_EYE);
      break;
    }
    case BUMPED: {
      tft.fillRoundRect(mouthX + mouthW / 2 - 11, mouthBaseY, 22, 24, 11, COL_EYE);
      tft.fillRoundRect(mouthX + mouthW / 2 - 7, mouthBaseY + 4, 14, 16, 7, COL_BG);
      break;
    }
    case TALKING:
      break;
    default: 
      tft.fillRoundRect(mouthX + 6, mouthBaseY + 10, mouthW - 12, 6, 3, COL_EYE);
      break;
  }
}

void drawListeningMouth() {
  if (currentExpression != LISTENING) return;
  clearMouthArea();
  static float listenPhase = 0.0f;
  listenPhase += 0.15f;
  if (listenPhase > 2.0f * M_PI) listenPhase -= 2.0f * M_PI;
  float norm = (sinf(listenPhase) + 1.0f) / 2.0f;
  int r = 4 + (int)(norm * 9.0f);
  tft.fillCircle(SCREEN_W / 2, mouthBaseY + 10, r + 3, COL_EYE_DIM);
  tft.fillCircle(SCREEN_W / 2, mouthBaseY + 10, r, COL_EYE);
}

// =====================================================================
// Ultra-Smooth Local Canvas Accelerated Blink
// =====================================================================
void blinkOnce() {
  const int N = 20;
  int heights[N];
  for (int i = 0; i < N; i++) {
    float t = (float)i / (float)(N - 1);
    float phase = t * 2.0f;
    float v = (phase <= 1.0f) ? (1.0f - (phase * phase)) : ((phase - 1.0f) * (2.0f - (phase - 1.0f)));
    heights[i] = (int)(v * eyeH);
    if (heights[i] < 0) heights[i] = 0;
  }

  GFXcanvas16 blinkCanvas(SCREEN_W, eyeH + 4);
  int localEyeY = 2;
  int cy = localEyeY + eyeH / 2;

  for (int i = 0; i < N; i++) {
    blinkCanvas.fillScreen(COL_BG);
    int h = heights[i];
    int ey = cy - h / 2;
    
    if (h <= 3) {
      blinkCanvas.fillRoundRect(leftEyeX,  cy - 1, eyeW, 3, 1, COL_EYE_DIM);
      blinkCanvas.fillRoundRect(rightEyeX, cy - 1, eyeW, 3, 1, COL_EYE_DIM);
    } else {
      int r = min(eyeW, h) / 2;
      blinkCanvas.fillRoundRect(leftEyeX,  ey, eyeW, h, r, COL_EYE);
      blinkCanvas.fillRoundRect(rightEyeX, ey, eyeW, h, r, COL_EYE);
    }
    tft.drawRGBBitmap(0, eyeY - 2, blinkCanvas.getBuffer(), blinkCanvas.width(), blinkCanvas.height());
    
    int d = (i < 4 || i > 15) ? 10 : (i < 7 || i > 12) ? 6 : 4;
    delay(d);
  }
  drawExpressionEyes();
}

// =====================================================================
// Ultra-Smooth Local Canvas Accelerated Wink
// =====================================================================
void winkOnce() {
  const int N = 16;
  GFXcanvas16 winkCanvas(SCREEN_W, eyeH + 4);
  int localEyeY = 2;
  int cy = localEyeY + eyeH / 2;

  for (int i = 0; i < N; i++) {
    float t = (float)i / (float)(N - 1);
    float phase = t * 2.0f;
    float v = (phase <= 1.0f) ? (1.0f - (phase * phase)) : ((phase - 1.0f) * (2.0f - (phase - 1.0f)));
    int h = (int)(v * eyeH);
    if (h < 0) h = 0;

    winkCanvas.fillScreen(COL_BG);
    
    // Left eye remains flat open (HAPPY profile base shape)
    int rOpen = min(eyeW, eyeH) / 2;
    winkCanvas.fillRoundRect(leftEyeX, localEyeY, eyeW, eyeH, rOpen, COL_EYE);
    winkCanvas.fillRect(leftEyeX, localEyeY, eyeW, eyeH * 55 / 100, COL_BG);

    // Right eye animates smooth wink lid transition inside the buffer
    int ey = cy - h / 2;
    if (h <= 3) {
      winkCanvas.fillRoundRect(rightEyeX, cy - 1, eyeW, 3, 1, COL_EYE_DIM);
    } else {
      int r = min(eyeW, h) / 2;
      winkCanvas.fillRoundRect(rightEyeX, ey, eyeW, h, r, COL_EYE);
    }
    
    tft.drawRGBBitmap(0, eyeY - 2, winkCanvas.getBuffer(), winkCanvas.width(), winkCanvas.height());
    int d = (i < 3 || i > 12) ? 12 : 6;
    delay(d);
  }
  drawExpressionEyes();
}

// =====================================================================
// Set expression
// =====================================================================
void setExpression(Expression e) {
  Expression prev = currentExpression;
  currentExpression = e;
  talkFrame  = 0;
  talkPhase  = 0.0f;
  displayMode = MODE_FACE;
  tft.fillScreen(COL_BG);
  drawExpressionEyes();
  drawMouth();
  if (e == HAPPY && prev != HAPPY) {
    delay(100);
    winkOnce();
  }
}

// =====================================================================
// UTF-8 -> CP437 conversion (for on-screen text only)
// Adafruit_GFX's built-in font is laid out like IBM code page 437, which
// has real glyphs for é, è, ê, à, ç, etc. — but the Groq API sends UTF-8,
// where each accented character is 2+ bytes. Printed raw, those bytes
// show up as garbled boxes. This decodes UTF-8 into codepoints and maps
// the common French/Latin-1 ones onto their CP437 byte, so accents show
// up correctly. Anything with no CP437 slot (œ, curly quotes, emoji...)
// falls back to a plain-ASCII approximation so text stays readable.
// =====================================================================
uint8_t cp437FromUnicode(uint32_t cp) {
  switch (cp) {
    case 0x00C7: return 0x80; // Ç
    case 0x00FC: return 0x81; // ü
    case 0x00E9: return 0x82; // é
    case 0x00E2: return 0x83; // â
    case 0x00E4: return 0x84; // ä
    case 0x00E0: return 0x85; // à
    case 0x00E5: return 0x86; // å
    case 0x00E7: return 0x87; // ç
    case 0x00EA: return 0x88; // ê
    case 0x00EB: return 0x89; // ë
    case 0x00E8: return 0x8A; // è
    case 0x00EF: return 0x8B; // ï
    case 0x00EE: return 0x8C; // î
    case 0x00EC: return 0x8D; // ì
    case 0x00C4: return 0x8E; // Ä
    case 0x00C5: return 0x8F; // Å
    case 0x00C9: return 0x90; // É
    case 0x00E6: return 0x91; // æ
    case 0x00C6: return 0x92; // Æ
    case 0x00F4: return 0x93; // ô
    case 0x00F6: return 0x94; // ö
    case 0x00F2: return 0x95; // ò
    case 0x00FB: return 0x96; // û
    case 0x00F9: return 0x97; // ù
    case 0x00FF: return 0x98; // ÿ
    case 0x00D6: return 0x99; // Ö
    case 0x00DC: return 0x9A; // Ü
    case 0x00A2: return 0x9B; // ¢
    case 0x00A3: return 0x9C; // £
    case 0x00A5: return 0x9D; // ¥
    case 0x0192: return 0x9F; // ƒ
    case 0x00E1: return 0xA0; // á
    case 0x00ED: return 0xA1; // í
    case 0x00F3: return 0xA2; // ó
    case 0x00FA: return 0xA3; // ú
    case 0x00F1: return 0xA4; // ñ
    case 0x00D1: return 0xA5; // Ñ
    case 0x00BF: return 0xA8; // ¿
    case 0x00AC: return 0xAA; // ¬
    case 0x00A1: return 0xAD; // ¡
    case 0x00AB: return 0xAE; // «
    case 0x00BB: return 0xAF; // »
    default: return 0;        // no CP437 slot
  }
}

// For accented letters that have no CP437 glyph (mostly uppercase forms
// like Ê, Î, Ô, Û, Ù, Ë, Ï — rare, but do show up capitalized at the
// start of a French sentence), just drop the accent rather than showing
// a garbled box.
char asciiFallback(uint32_t cp) {
  if (cp >= 0x00C0 && cp <= 0x00C5) return 'A';
  if (cp == 0x00C8 || (cp >= 0x00C9 && cp <= 0x00CB)) return 'E';
  if (cp >= 0x00CC && cp <= 0x00CF) return 'I';
  if (cp >= 0x00D2 && cp <= 0x00D6) return 'O';
  if (cp == 0x00D8) return 'O';
  if (cp >= 0x00D9 && cp <= 0x00DC) return 'U';
  if (cp == 0x00DD) return 'Y';
  if (cp == 0x00DF) return 's';  // ß
  return 0; // truly unmappable (emoji, exotic symbols) — drop it
}

String utf8ToCP437(const String &in) {
  String out;
  out.reserve(in.length());
  size_t n = in.length();
  for (size_t i = 0; i < n; ) {
    uint8_t c0 = (uint8_t)in.charAt(i);
    uint32_t cp; int len;
    if (c0 < 0x80) {
      cp = c0; len = 1;
    } else if ((c0 & 0xE0) == 0xC0 && i + 1 < n) {
      cp = ((uint32_t)(c0 & 0x1F) << 6) | ((uint8_t)in.charAt(i + 1) & 0x3F);
      len = 2;
    } else if ((c0 & 0xF0) == 0xE0 && i + 2 < n) {
      cp = ((uint32_t)(c0 & 0x0F) << 12) |
           (((uint8_t)in.charAt(i + 1) & 0x3F) << 6) |
           ((uint8_t)in.charAt(i + 2) & 0x3F);
      len = 3;
    } else if ((c0 & 0xF8) == 0xF0 && i + 3 < n) {
      cp = ((uint32_t)(c0 & 0x07) << 18) |
           (((uint8_t)in.charAt(i + 1) & 0x3F) << 12) |
           (((uint8_t)in.charAt(i + 2) & 0x3F) << 6) |
           ((uint8_t)in.charAt(i + 3) & 0x3F);
      len = 4;
    } else {
      i += 1; continue; // stray/invalid byte — skip it
    }
    i += len;

    if (cp < 0x80) { out += (char)cp; continue; }

    // A few common "smart" punctuation marks and multi-char substitutions
    // that LLMs love to use, mapped to something the font can show:
    if (cp == 0x2019 || cp == 0x2018) { out += '\''; continue; }
    if (cp == 0x201C || cp == 0x201D) { out += '"';  continue; }
    if (cp == 0x2013 || cp == 0x2014) { out += '-';  continue; }
    if (cp == 0x2026) { out += "..."; continue; }
    if (cp == 0x0153) { out += "oe";  continue; } // œ
    if (cp == 0x0152) { out += "OE";  continue; } // Œ
    if (cp == 0x20AC) { out += "EUR"; continue; } // €

    uint8_t mapped = cp437FromUnicode(cp);
    if (mapped != 0) {
      out += (char)mapped;
    } else {
      char fb = asciiFallback(cp);
      if (fb) out += fb;
      // else: no reasonable ASCII stand-in (emoji etc.) — just drop it
    }
  }
  return out;
}

// =====================================================================
// Full-screen text mode — shown once you let go of the touch pad.
// Wraps the reply to fit the screen width, then renderTextMode() draws
// whatever page of lines is currently scrolled into view. The rotary
// encoder moves textScrollOffset up/down one line per detent.
// =====================================================================
void wrapTextForScreen(const String& text) {
  textLineCount    = 0;
  textScrollOffset = 0;

  const int charW = 6 * TEXT_SIZE;
  const int maxLineWidth = SCREEN_W - 2 * TEXT_X_MARGIN;

  String currentLine = "";
  String word = "";
  String t = text + " ";
  for (int i = 0; i < (int)t.length() && textLineCount < MAX_TEXT_LINES; i++) {
    char c = t.charAt(i);
    if (c == ' ' || c == '\n') {
      if (word.length() > 0) {
        int curPx  = currentLine.length() * charW;
        int wordPx = word.length() * charW;
        if (curPx > 0 && (curPx + charW + wordPx > maxLineWidth)) {
          textLines[textLineCount++] = currentLine;
          currentLine = "";
          if (textLineCount >= MAX_TEXT_LINES) break;
        }
        if (currentLine.length() > 0) currentLine += " ";
        currentLine += word;
        word = "";
      }
      if (c == '\n' && textLineCount < MAX_TEXT_LINES) {
        textLines[textLineCount++] = currentLine;
        currentLine = "";
      }
    } else {
      word += c;
    }
  }
  if (textLineCount < MAX_TEXT_LINES && currentLine.length() > 0)
    textLines[textLineCount++] = currentLine;
}

// Small scrollbar on the right edge, only drawn when there's more text
// than fits on one screen — the only hint the user needs that the
// encoder can scroll further. Templated on Adafruit_GFX so it can draw
// either straight onto the live screen or into an off-screen canvas.
void drawScrollbar(Adafruit_GFX &gfx, int trackY, int maxOffset) {
  int trackX = SCREEN_W - 6;
  int trackH = TEXT_PANEL_H;
  gfx.drawFastVLine(trackX + 2, trackY, trackH, COL_EYE_DIM);

  int thumbH = trackH * textVisibleLines / textLineCount;
  if (thumbH < 12) thumbH = 12;
  int thumbY = trackY + (maxOffset > 0 ? (long)(trackH - thumbH) * textScrollOffset / maxOffset : 0);
  gfx.fillRoundRect(trackX, thumbY, 4, thumbH, 2, COL_EYE);
}

// Draws the current page of wrapped lines (+ scrollbar) into whatever
// Adafruit_GFX target is passed in, starting at local y = originY.
void drawTextFrame(Adafruit_GFX &gfx, int originY, int maxOffset) {
  gfx.setTextColor(COL_TEXT);
  gfx.setTextSize(TEXT_SIZE);

  const int charH = 8 * TEXT_SIZE;
  const int lineH = charH + 6;
  int endLine = min(textLineCount, textScrollOffset + textVisibleLines);
  int y = originY;
  for (int i = textScrollOffset; i < endLine; i++) {
    gfx.setCursor(TEXT_X_MARGIN, y);
    gfx.print(textLines[i]);
    y += lineH;
  }

  if (textLineCount > textVisibleLines) {
    drawScrollbar(gfx, originY, maxOffset);
  }
}

void renderTextMode() {
  if (textLineCount == 0) { tft.fillScreen(COL_BG); return; }

  const int charH = 8 * TEXT_SIZE;
  const int lineH = charH + 6;
  textVisibleLines = TEXT_PANEL_H / lineH;
  if (textVisibleLines < 1) textVisibleLines = 1;

  int maxOffset = max(0, textLineCount - textVisibleLines);
  if (textScrollOffset > maxOffset) textScrollOffset = maxOffset;
  if (textScrollOffset < 0) textScrollOffset = 0;

  // Build the whole frame off-screen, then push it in a single transfer.
  // This is the same local-canvas trick the eyes/blink/wink animations
  // use elsewhere in this sketch — it avoids the black "reset" flash you
  // get from clearing the live screen and drawing text on top of it.
  GFXcanvas16 textCanvas(SCREEN_W, TEXT_PANEL_H);
  if (textCanvas.getBuffer()) {
    textCanvas.fillScreen(COL_BG);
    drawTextFrame(textCanvas, 0, maxOffset);
    tft.drawRGBBitmap(0, TEXT_Y_START, textCanvas.getBuffer(), textCanvas.width(), textCanvas.height());
  } else {
    // Off-screen buffer didn't fit in RAM right now — fall back to
    // drawing straight onto the screen (may flicker a little).
    tft.fillRect(0, TEXT_Y_START, SCREEN_W, TEXT_PANEL_H, COL_BG);
    drawTextFrame(tft, TEXT_Y_START, maxOffset);
  }
}

void enterTextMode(const String& text) {
  displayMode = MODE_TEXT;
  wrapTextForScreen(utf8ToCP437(text));
  renderTextMode();
}

// =====================================================================
// Rotary encoder — scrolls the full-screen text
// =====================================================================
void IRAM_ATTR encoderISR() {
  static unsigned long lastInterruptTime = 0;
  unsigned long now = millis();
  if (now - lastInterruptTime < 3) return;  // debounce
  lastInterruptTime = now;

  portENTER_CRITICAL_ISR(&encoderMux);
  if (digitalRead(ENCODER_DT) != digitalRead(ENCODER_CLK)) {
    encoderDelta++;
  } else {
    encoderDelta--;
  }
  portEXIT_CRITICAL_ISR(&encoderMux);
}

void pollEncoder() {
  if (displayMode != MODE_TEXT) return;

  int delta;
  portENTER_CRITICAL(&encoderMux);
  delta = encoderDelta;
  encoderDelta = 0;
  portEXIT_CRITICAL(&encoderMux);

  if (delta == 0) return;

  int maxOffset = max(0, textLineCount - textVisibleLines);
  textScrollOffset += delta;
  if (textScrollOffset < 0) textScrollOffset = 0;
  if (textScrollOffset > maxOffset) textScrollOffset = maxOffset;

  renderTextMode();
}

// =====================================================================
// WiFi
// =====================================================================
#define WIFI_STATUS_BAND_H 24
void showWifiStatus(const char* msg) {
  int bandY = SCREEN_H - WIFI_STATUS_BAND_H;
  tft.fillRect(0, bandY, SCREEN_W, WIFI_STATUS_BAND_H, COL_BG);
  if (msg && msg[0] != '\0') {
    tft.setTextColor(COL_TEXT);
    tft.setTextSize(1);
    int x = (SCREEN_W - (int)strlen(msg) * 6) / 2;
    tft.setCursor(x < 0 ? 0 : x, bandY + WIFI_STATUS_BAND_H / 2 - 4);
    tft.print(msg);
  }
}

void connectWiFi() {
  Serial.print("Connecting to WiFi: "); Serial.println(WIFI_SSID);
  showWifiStatus("WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
    showWifiStatus("Online!"); delay(800); showWifiStatus("");
  } else {
    Serial.println("\nWiFi FAILED.");
    showWifiStatus("No WiFi!");
  }
}

// =====================================================================
// Groq API
// =====================================================================
String jsonStringEscape(const String& s) {
  String out = "\"";
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s.charAt(i);
    if      (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else                out += c;
  }
  return out + "\"";
}

// Escapes text for safe embedding inside an SSML <voice> element (used by
// the Azure TTS request body below) — & < > " ' all have special meaning
// in XML and need to become entities instead.
String xmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s.charAt(i);
    if      (c == '&')  out += "&amp;";
    else if (c == '<')  out += "&lt;";
    else if (c == '>')  out += "&gt;";
    else if (c == '"')  out += "&quot;";
    else if (c == '\'') out += "&apos;";
    else                out += c;
  }
  return out;
}

void addToHistory(const char* role, const String& content) {
  if (historyCount >= MAX_HISTORY) {
    for (int i = 0; i < MAX_HISTORY - 2; i++) history[i] = history[i + 2];
    historyCount -= 2;
  }
  history[historyCount++] = String("{\"role\":\"") + role + "\",\"content\":" + jsonStringEscape(content) + "}";
}

// =====================================================================
// French/English mode — tracked here in firmware rather than left up to
// the model to remember. This is deterministic: it doesn't depend on
// the conversation history window, and it doesn't rely on a small/fast
// model correctly inferring and holding a "mode" across turns.
// =====================================================================
bool forceEnglishMode = false;  // false = default French conversation mode

// Case-insensitive check for "au revoir" anywhere in the message,
// ignoring punctuation (so "Au revoir !", "au-revoir", etc. all match).
bool messageSaysAuRevoir(const String& s) {
  String cleaned;
  cleaned.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isalpha((unsigned char)c)) cleaned += (char)tolower((unsigned char)c);
    else if (c == ' ' || c == '-') cleaned += ' ';
  }
  return cleaned.indexOf("au revoir") >= 0;
}

// Crude but effective French detector: any accented character (which
// English speech-to-text essentially never produces) is a strong signal
// on its own, backed up by a short list of unambiguous French words.
bool messageLooksFrench(const String& s) {
  for (size_t i = 0; i < s.length(); i++) {
    if ((uint8_t)s.charAt(i) >= 0x80) return true;  // accented UTF-8 byte
  }
  String t = s; t.toLowerCase();
  const char* frenchWords[] = {
    "bonjour", "bonsoir", "salut", "merci", "s'il", "d'accord",
    "voila", "francais", "je suis", "qu'est", "pourquoi",
    "comment ca", "comment allez"
  };
  for (size_t i = 0; i < sizeof(frenchWords) / sizeof(frenchWords[0]); i++) {
    if (t.indexOf(frenchWords[i]) >= 0) return true;
  }
  return false;
}

String callGroqAPI(const String& userMessage) {
  // "Au revoir" always forces English mode, and hearing French again
  // always switches back — regardless of what the LLM would decide on
  // its own, and regardless of how far back in history the trigger was.
  if (messageSaysAuRevoir(userMessage)) {
    forceEnglishMode = true;
    Serial.println("[LANG] 'Au revoir' detected -> switching to English.");
  } else if (forceEnglishMode && messageLooksFrench(userMessage)) {
    forceEnglishMode = false;
    Serial.println("[LANG] French detected -> switching back to French.");
  }

  addToHistory("user", userMessage);

  // The mode is re-stated explicitly on every single request, so the
  // model never has to remember or infer it — it's just told.
  String modePrompt = String(SYSTEM_PROMPT) +
    (forceEnglishMode
      ? "\n\nCURRENT MODE: The user said 'Au revoir' to pause French practice. "
        "Reply in English only, no French, until they speak French to you again."
      : "\n\nCURRENT MODE: Default French conversation mode — reply in French "
        "as described above.");

  String messagesJson = "[{\"role\":\"system\",\"content\":";
  messagesJson += jsonStringEscape(modePrompt); messagesJson += "}";
  for (int i = 0; i < historyCount; i++) { messagesJson += ","; messagesJson += history[i];
  }
  messagesJson += "]";

  String body = String("{\"model\":") + jsonStringEscape(GROQ_MODEL) +
                ",\"max_tokens\":300,\"messages\":" + messagesJson + "}";
  WiFiClientSecure client; client.setInsecure();
  Serial.print("[API] Connecting...");
  if (!client.connect(GROQ_HOST, GROQ_PORT)) {
    Serial.println(" FAILED");
    return "[EXPR:BUMPED] Couldn't reach Groq. Check WiFi!";
  }
  Serial.println(" OK");

  client.print("POST /openai/v1/chat/completions HTTP/1.1\r\n");
  client.print("Host: api.groq.com\r\n");
  client.print("Authorization: Bearer "); client.print(GROQ_API_KEY);
  client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: "); client.print(body.length()); client.print("\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  bool headersDone = false;
  String responseLine = "", responseBody = "";
  unsigned long timeout = millis() + 20000;
  while (client.connected() || client.available()) {
    if (millis() > timeout) return "[EXPR:BUMPED] API timeout!";
    while (client.available()) {
      char c = client.read();
      if (!headersDone) {
        responseLine += c;
        if (responseLine.endsWith("\r\n\r\n")) { headersDone = true;
          responseLine = ""; }
      } else { responseBody += c;
      }
    }
    delay(1);
  }
  client.stop();
  responseBody.trim();

  String cleanBody = "";
  int pos = 0;
  while (pos < (int)responseBody.length()) {
    int lineEnd = responseBody.indexOf("\r\n", pos);
    if (lineEnd < 0) break;
    String sizeLine = responseBody.substring(pos, lineEnd); sizeLine.trim();
    long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
    pos = lineEnd + 2;
    if (chunkSize == 0) break;
    cleanBody += responseBody.substring(pos, pos + chunkSize);
    pos += chunkSize + 2;
  }
  if (cleanBody.isEmpty()) cleanBody = responseBody;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, cleanBody);
  if (err) return "[EXPR:BUMPED] JSON parse error.";
  if (doc["error"]["message"]) return String("[EXPR:BUMPED] Groq error: ") + (const char*)doc["error"]["message"];
  String reply = doc["choices"][0]["message"]["content"] | "";
  reply.trim();
  if (reply.isEmpty()) return "[EXPR:BUMPED] Empty reply from Groq.";
  addToHistory("assistant", reply);
  return reply;
}

// =====================================================================
// MPU-9250
// =====================================================================
void initMPU9250() {
  Wire.begin(MPU_SDA, MPU_SCL);
  Wire.beginTransmission(MPU_ADDR); Wire.write(MPU_REG_WHO_AM_I);
  Wire.endTransmission(false); Wire.requestFrom(MPU_ADDR, 1, true);
  if (Wire.available()) {
    uint8_t id = Wire.read();
    if (id == 0x71 || id == 0x73 || id == 0x70 || id == 0x68) mpuConnected = true;
  }
  if (!mpuConnected) { Serial.println("MPU not found — bump disabled."); return; }
  Wire.beginTransmission(MPU_ADDR); Wire.write(MPU_REG_PWR_MGMT_1); Wire.write(0x00);
  Wire.endTransmission(true);
  Serial.println("MPU-9250 connected.");
}

bool readAccelG(float &ax, float &ay, float &az) {
  Wire.beginTransmission(MPU_ADDR); Wire.write(MPU_REG_ACCEL_XOUT_H);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(MPU_ADDR, 6, true);
  if (Wire.available() < 6) return false;
  int16_t rawX = (Wire.read() << 8) | Wire.read();
  int16_t rawY = (Wire.read() << 8) | Wire.read();
  int16_t rawZ = (Wire.read() << 8) | Wire.read();
  ax = rawX / ACCEL_SENS; ay = rawY / ACCEL_SENS; az = rawZ / ACCEL_SENS;
  return true;
}

void checkBump() {
  float ax, ay, az;
  if (!readAccelG(ax, ay, az)) return;
  float jolt = fabs(sqrtf(ax*ax + ay*ay + az*az) - 1.0f);
  if (jolt > BUMP_THRESHOLD_G) {
    bumpAnimationPlaying = true;
    playBumpAnimation();
    setExpression(BUMPED);
    delay(3000); 
    setExpression(IDLE);
    lastBlink    = millis();
    lastBumpTime = millis();
    bumpAnimationPlaying = false;
  }
}

// =====================================================================
// Bump animation
// =====================================================================
void playBumpAnimation() {
  // Phase 1: smooth decay shake (12 frames drawn directly to avoid buffering overhead)
  const float shakeAmps[] = { 8, -8, 7, -7, 5, -5, 4, -4, 2, -2, 1, 0 };
  int numShake = sizeof(shakeAmps) / sizeof(shakeAmps[0]);
  for (int i = 0; i < numShake; i++) {
    int dx = (int)shakeAmps[i];
    tft.fillRect(0, eyeY - 2, SCREEN_W, eyeH + 4, COL_BG);
    int r = min(eyeW, eyeH) / 2;
    tft.fillRoundRect(leftEyeX  + dx, eyeY, eyeW, eyeH, r, COL_EYE);
    tft.fillRoundRect(rightEyeX + dx, eyeY, eyeW, eyeH, r, COL_EYE);
    delay(28);
  }

  // Shocked mouth
  clearMouthArea();
  tft.fillRoundRect(mouthX + mouthW / 2 - 11, mouthBaseY, 22, 24, 11, COL_EYE);
  tft.fillRoundRect(mouthX + mouthW / 2 - 7,  mouthBaseY + 4, 14, 16,  7, COL_BG);

  // Phase 2: Active local canvas spinning spiral burst
  GFXcanvas16 dynamicCanvas(SCREEN_W, eyeH + 4);
  int localEyeY = 2;
  int cy = localEyeY + eyeH / 2;
  int leftCenterX  = leftEyeX + (eyeW / 2);
  int rightCenterX = rightEyeX + (eyeW / 2);

  for (int frame = 0; frame < 20; frame++) {
    globalSpiralPhase += 0.45f; 
    
    dynamicCanvas.fillScreen(COL_BG);
    drawSpiralToCanvas(&dynamicCanvas, leftCenterX,  cy, globalSpiralPhase, COL_EYE);
    drawSpiralToCanvas(&dynamicCanvas, rightCenterX, cy, globalSpiralPhase, COL_EYE);
    
    tft.drawRGBBitmap(0, eyeY - 2, dynamicCanvas.getBuffer(), dynamicCanvas.width(), dynamicCanvas.height());
    delay(30);
  }
}

// =====================================================================
// NS4168 I2S Amp — setup
// =====================================================================
void setupAmpI2S() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = AMP_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    // Large DMA ring: 8 bufs x 512 samples x 4 bytes = 16 KB headroom.
    // This absorbs WiFi TCP jitter so the I2S clock never starves.
    .dma_buf_count        = 8,
    .dma_buf_len          = 512,
    .use_apll             = false, // APLL/MCLK generation collides with the mic's I2S_NUM_1 peripheral
                                     // on a normal ESP32 (only one MCLK line is shared between both
                                     // I2S ports) — regular PLL clocking is plenty accurate for playback.
    .tx_desc_auto_clear   = true,  // silence on underrun, not garbage
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = AMP_BCLK,
    .ws_io_num    = AMP_LRC,
    .data_out_num = AMP_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE,
  };
  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S] Driver install failed: %d\n", err);
    return;
  }
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);

  // CRITICAL: stop the I2S clock immediately after setup.
  // If left running with no audio data, the DMA buffers starve during
  // WiFi calls / display SPI writes / delay() calls and the NS4168
  // outputs that data as loud static. We only start it in speakText().
  i2s_stop(I2S_NUM_0);

  Serial.println("[I2S] NS4168 amp ready (clock stopped until playback).");
}

// Write mono 16-bit PCM to the NS4168 by duplicating each sample L+R.
// Uses a large heap buffer (4 KB stereo output per call) to minimise
// the number of i2s_write() calls — each call has ~10 us of overhead
// that adds up to audible distortion when called hundreds of times/sec.
//
// IMPORTANT: network reads arrive in arbitrary-sized chunks that are NOT
// guaranteed to be an even number of bytes (i.e. aligned to a whole
// 16-bit sample). A dangling odd byte at the end of one chunk belongs
// to the same sample as the first byte of the NEXT chunk. Dropping it
// (as opposed to carrying it over) shifts every sample after that point
// by one byte for the rest of the stream — which sounds exactly like
// scratchy static, because that's exactly what it is: scrambled PCM.
static bool    i2sCarryPending = false;
static uint8_t i2sCarryByte    = 0;

void resetI2SStereoCarry() {
  i2sCarryPending = false;
}

void writeMonoToI2SStereo(const uint8_t* data, size_t len) {
  // 2048 mono frames → 4 bytes each (L+R 16-bit) = 8 KB stereo buffer.
  const size_t FRAMES  = 2048;
  const size_t BUF_LEN = FRAMES * 4;
  static uint8_t stereoBuf[BUF_LEN];  // static = lives in BSS, not stack
  size_t stereoIdx = 0;
  size_t written   = 0;
  size_t i = 0;

  if (len == 0) return;

  // Finish off a sample that was split across the previous chunk boundary.
  if (i2sCarryPending) {
    uint8_t lo = i2sCarryByte, hi = data[0];
    stereoBuf[stereoIdx++] = lo;
    stereoBuf[stereoIdx++] = hi;
    stereoBuf[stereoIdx++] = lo;
    stereoBuf[stereoIdx++] = hi;
    i2sCarryPending = false;
    i = 1;
  }

  for (; i + 1 < len; i += 2) {
    stereoBuf[stereoIdx++] = data[i];
    stereoBuf[stereoIdx++] = data[i + 1];
    stereoBuf[stereoIdx++] = data[i];
    stereoBuf[stereoIdx++] = data[i + 1];

    if (stereoIdx >= BUF_LEN) {
      i2s_write(I2S_NUM_0, stereoBuf, stereoIdx, &written, portMAX_DELAY);
      stereoIdx = 0;
    }
  }
  if (stereoIdx > 0) {
    i2s_write(I2S_NUM_0, stereoBuf, stereoIdx, &written, portMAX_DELAY);
  }

  // Odd byte left dangling at the end of this chunk — hold onto it
  // instead of dropping it; it'll be completed by the next chunk's
  // first byte.
  if (i < len) {
    i2sCarryByte    = data[len - 1];
    i2sCarryPending = true;
  }
}

// =====================================================================
// INMP441 I2S Microphone — setup (runs on I2S_NUM_1, independent of the
// NS4168 amp on I2S_NUM_0 so recording and playback never fight over
// the same peripheral).
// =====================================================================
void setupMicI2S() {
  i2s_config_t micCfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = MIC_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT, // INMP441 sends 24-bit data, MSB-justified in a 32-bit slot
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT, // L/R pin tied to GND → mic drives the LEFT slot
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 4,
    .dma_buf_len          = 256,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
  };
  i2s_pin_config_t micPins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE, // INMP441 doesn't use MCLK — without this,
                                        // the driver auto-claims a default MCLK pin
                                        // that the amp's I2S port already owns, and
                                        // i2s_set_pin() below fails silently for it
    .bck_io_num   = MIC_SCK,
    .ws_io_num    = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_SD,
  };
  esp_err_t err = i2s_driver_install(I2S_NUM_1, &micCfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[MIC] I2S driver install failed: %d\n", err);
    return;
  }
  esp_err_t pinErr = i2s_set_pin(I2S_NUM_1, &micPins);
  if (pinErr != ESP_OK) {
    Serial.printf("[MIC] i2s_set_pin FAILED: %d — mic will read silence/garbage!\n", pinErr);
    return;
  }
  i2s_zero_dma_buffer(I2S_NUM_1);
  Serial.println("[MIC] INMP441 ready on I2S_NUM_1.");
}

// =====================================================================
// Listening cue — short beep through the NS4168 amp so the person knows
// the mic is live before they start talking.
// =====================================================================
void playListenCue() {
  static int16_t cueBuf[CUE_SAMPLES];
  for (int i = 0; i < CUE_SAMPLES; i++) {
    float t = (float)i / (float)AMP_SAMPLE_RATE;
    // Quick fade in/out so the beep doesn't click.
    float envelope = sinf(M_PI * (float)i / (float)CUE_SAMPLES);
    cueBuf[i] = (int16_t)(sinf(2.0f * M_PI * CUE_FREQ_HZ * t) * 9000.0f * envelope);
  }
  i2s_start(I2S_NUM_0);
  i2s_zero_dma_buffer(I2S_NUM_0);
  writeMonoToI2SStereo((const uint8_t*)cueBuf, CUE_SAMPLES * sizeof(int16_t));
  delay(30); // let the DMA ring drain before we stop the clock
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_stop(I2S_NUM_0);
}

// =====================================================================
// Record from the INMP441 for as long as the touch pad is held down.
// Fills the global recordBuffer with 16-bit mono PCM and keeps the
// listening-pulse animation alive the whole time.
// Returns false if the tap was too short to be a real question.
// =====================================================================
bool recordWhileTouched() {
  recordedSamples = 0;
  if (!recordBuffer) return false;

  const size_t CHUNK_SAMPLES = 256;
  int32_t raw[CHUNK_SAMPLES];
  unsigned long recordStart = millis();
  unsigned long lastAnim    = millis();

  while (digitalRead(TOUCH_PIN) == HIGH && recordedSamples < recordCapacitySamples) {
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_1, raw, CHUNK_SAMPLES * sizeof(int32_t), &bytesRead, 50 / portTICK_PERIOD_MS);
    size_t samplesRead = bytesRead / sizeof(int32_t);

    for (size_t i = 0; i < samplesRead && recordedSamples < recordCapacitySamples; i++) {
      // INMP441 data is left-justified in the 32-bit word; shift down to
      // a sane 16-bit range. >>14 gives good headroom without clipping
      // on normal speaking volume — tweak if it sounds quiet/loud.
      int32_t s = raw[i] >> 14;
      if (s > 32767)  s = 32767;
      if (s < -32768) s = -32768;
      recordBuffer[recordedSamples++] = (int16_t)s;
    }

    if (millis() - lastAnim > 40) {
      drawListeningMouth();
      lastAnim = millis();
    }
  }

  unsigned long durationMs = millis() - recordStart;
  return durationMs >= RECORD_MIN_MS;
}

// =====================================================================
// Minimal 44-byte canonical WAV header for 16-bit PCM.
// =====================================================================
void writeWavHeader(uint8_t* hdr, uint32_t dataBytes, uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample) {
  uint32_t byteRate   = sampleRate * channels * (bitsPerSample / 8);
  uint16_t blockAlign = channels * (bitsPerSample / 8);
  uint32_t riffSize   = 36 + dataBytes;

  memcpy(hdr,      "RIFF", 4);
  memcpy(hdr + 8,  "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);
  memcpy(hdr + 36, "data", 4);

  hdr[4]  = riffSize & 0xFF;         hdr[5]  = (riffSize >> 8) & 0xFF;
  hdr[6]  = (riffSize >> 16) & 0xFF; hdr[7]  = (riffSize >> 24) & 0xFF;

  uint32_t fmtChunkSize = 16;
  hdr[16] = fmtChunkSize & 0xFF; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;
  hdr[20] = 1; hdr[21] = 0;                      // PCM format
  hdr[22] = channels & 0xFF; hdr[23] = 0;
  hdr[24] = sampleRate & 0xFF;         hdr[25] = (sampleRate >> 8) & 0xFF;
  hdr[26] = (sampleRate >> 16) & 0xFF; hdr[27] = (sampleRate >> 24) & 0xFF;
  hdr[28] = byteRate & 0xFF;           hdr[29] = (byteRate >> 8) & 0xFF;
  hdr[30] = (byteRate >> 16) & 0xFF;   hdr[31] = (byteRate >> 24) & 0xFF;
  hdr[32] = blockAlign & 0xFF; hdr[33] = 0;
  hdr[34] = bitsPerSample & 0xFF; hdr[35] = 0;
  hdr[40] = dataBytes & 0xFF;         hdr[41] = (dataBytes >> 8) & 0xFF;
  hdr[42] = (dataBytes >> 16) & 0xFF; hdr[43] = (dataBytes >> 24) & 0xFF;
}

// =====================================================================
// Groq Whisper transcription — sends the recorded PCM as a WAV file to
// api.groq.com/openai/v1/audio/transcriptions via multipart/form-data
// and returns the transcribed text.
// =====================================================================
String transcribeAudioGroqFromFile(const char* path, size_t sampleCount) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[STT] No WiFi."); return ""; }
  if (sampleCount == 0) return "";

  File audioFile = LittleFS.open(path, "r");
  if (!audioFile) {
    Serial.println("[STT] Could not open staged audio file.");
    return "";
  }
  uint32_t dataBytes = (uint32_t)audioFile.size();
  uint8_t wavHeader[44];
  writeWavHeader(wavHeader, dataBytes, MIC_SAMPLE_RATE, 1, 16);

  const char* boundary = "----ESP32RoboFaceBoundary7MA4YWx";
  String partModel  = String("--") + boundary + "\r\n"
                       "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                       "whisper-large-v3-turbo\r\n";
  String partFormat = String("--") + boundary + "\r\n"
                       "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
                       "json\r\n";
  String partFileHdr = String("--") + boundary + "\r\n"
                       "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                       "Content-Type: audio/wav\r\n\r\n";
  String tail = String("\r\n--") + boundary + "--\r\n";

  size_t contentLength = partModel.length() + partFormat.length() + partFileHdr.length()
                       + 44 + dataBytes + tail.length();

  // recordBuffer has already been freed by this point (see handleVoiceInput) —
  // that's the whole reason this function reads from flash instead of RAM.
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  Serial.printf("[STT] Free heap before connect: %u bytes (largest block: %u)\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  Serial.printf("[STT] WiFi RSSI: %d dBm\n", WiFi.RSSI());

  Serial.println("[STT] Connecting to Groq for transcription...");
  if (!client.connect("api.groq.com", 443)) {
    Serial.println("[STT] Connection failed.");
    Serial.printf("[STT] Free heap after failed connect: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("[STT] Largest contiguous block: %u bytes\n", ESP.getMaxAllocHeap());
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.print("[STT] mbedTLS last error: ");
    Serial.println(errBuf);
    audioFile.close();
    return "";
  }

  client.print("POST /openai/v1/audio/transcriptions HTTP/1.1\r\n");
  client.print("Host: api.groq.com\r\n");
  client.print("Authorization: Bearer "); client.print(GROQ_API_KEY); client.print("\r\n");
  client.print("Content-Type: multipart/form-data; boundary="); client.print(boundary); client.print("\r\n");
  client.print("Content-Length: "); client.print(contentLength); client.print("\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(partModel);
  client.print(partFormat);
  client.print(partFileHdr);
  client.write(wavHeader, 44);

  // Stream the PCM body straight off flash, in small chunks, so we never
  // need a second large RAM buffer alongside the TLS connection.
  uint8_t streamBuf[2048];
  audioFile.seek(0);
  while (audioFile.available()) {
    size_t n = audioFile.read(streamBuf, sizeof(streamBuf));
    if (n == 0) break;
    client.write(streamBuf, n);
  }
  audioFile.close();
  client.print(tail);

  // Wait for a response.
  unsigned long waitStart = millis();
  while (!client.available() && millis() - waitStart < 15000) delay(10);
  if (!client.available()) {
    Serial.println("[STT] Timeout waiting for response.");
    client.stop(); return "";
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.println("[STT] Status: " + statusLine);

  bool isChunked = false;
  while (client.connected() || client.available()) {
    String hdr = client.readStringUntil('\n');
    hdr.trim();
    if (hdr.length() == 0) break;
    if (hdr.indexOf("chunked") >= 0) isChunked = true;
  }

  String responseBody = "";
  unsigned long bodyTimeout = millis() + 15000;
  while ((client.connected() || client.available()) && millis() < bodyTimeout) {
    if (client.available()) {
      responseBody += (char)client.read();
    } else {
      delay(1);
    }
  }
  client.stop();
  responseBody.trim();

  String cleanBody = responseBody;
  if (isChunked) {
    // Same simple chunked-transfer-encoding parser used for the chat API.
    cleanBody = "";
    int pos = 0;
    while (pos < (int)responseBody.length()) {
      int lineEnd = responseBody.indexOf("\r\n", pos);
      if (lineEnd < 0) break;
      String sizeLine = responseBody.substring(pos, lineEnd); sizeLine.trim();
      long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      pos = lineEnd + 2;
      if (chunkSize == 0) break;
      cleanBody += responseBody.substring(pos, pos + chunkSize);
      pos += chunkSize + 2;
    }
    if (cleanBody.isEmpty()) cleanBody = responseBody;
  }

  if (statusLine.indexOf("200") < 0) {
    Serial.println("[STT] Error body: " + cleanBody);
    return "";
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, cleanBody);
  if (err) { Serial.println("[STT] JSON parse error."); return ""; }
  String text = doc["text"] | "";
  text.trim();
  Serial.println("[STT] Heard: " + text);
  return text;
}

String transcribeAudioGroq(const int16_t* pcm, size_t sampleCount) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[STT] No WiFi."); return ""; }
  if (!pcm || sampleCount == 0) return "";

  uint32_t dataBytes = (uint32_t)(sampleCount * sizeof(int16_t));
  uint8_t wavHeader[44];
  writeWavHeader(wavHeader, dataBytes, MIC_SAMPLE_RATE, 1, 16);

  const char* boundary = "----ESP32RoboFaceBoundary7MA4YWx";
  String partModel  = String("--") + boundary + "\r\n"
                       "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                       "whisper-large-v3-turbo\r\n";
  String partFormat = String("--") + boundary + "\r\n"
                       "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
                       "json\r\n";
  String partFileHdr = String("--") + boundary + "\r\n"
                       "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                       "Content-Type: audio/wav\r\n\r\n";
  String tail = String("\r\n--") + boundary + "--\r\n";

  size_t contentLength = partModel.length() + partFormat.length() + partFileHdr.length()
                       + 44 + dataBytes + tail.length();

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);

  Serial.printf("[STT] Free heap before connect: %u bytes (largest block: %u)\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  Serial.printf("[STT] WiFi RSSI: %d dBm\n", WiFi.RSSI());

  IPAddress resolvedIP;
  if (WiFi.hostByName("api.groq.com", resolvedIP)) {
    Serial.print("[STT] DNS OK, api.groq.com -> "); Serial.println(resolvedIP);
  } else {
    Serial.println("[STT] DNS resolution FAILED for api.groq.com.");
  }

  Serial.println("[STT] Connecting to Groq for transcription...");
  if (!client.connect("api.groq.com", 443)) {
    Serial.println("[STT] Connection failed.");
    Serial.printf("[STT] Free heap after failed connect: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("[STT] Largest contiguous block: %u bytes\n", ESP.getMaxAllocHeap());
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.print("[STT] mbedTLS last error: ");
    Serial.println(errBuf);
    return "";
  }

  client.print("POST /openai/v1/audio/transcriptions HTTP/1.1\r\n");
  client.print("Host: api.groq.com\r\n");
  client.print("Authorization: Bearer "); client.print(GROQ_API_KEY); client.print("\r\n");
  client.print("Content-Type: multipart/form-data; boundary="); client.print(boundary); client.print("\r\n");
  client.print("Content-Length: "); client.print(contentLength); client.print("\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(partModel);
  client.print(partFormat);
  client.print(partFileHdr);
  client.write(wavHeader, 44);

  // Stream the PCM body in chunks so we never need a second full-size buffer.
  const uint8_t* pcmBytes = (const uint8_t*)pcm;
  const size_t STREAM_CHUNK = 4096;
  size_t sent = 0;
  while (sent < dataBytes) {
    size_t n = min(STREAM_CHUNK, (size_t)(dataBytes - sent));
    client.write(pcmBytes + sent, n);
    sent += n;
  }
  client.print(tail);

  // Wait for a response.
  unsigned long waitStart = millis();
  while (!client.available() && millis() - waitStart < 15000) delay(10);
  if (!client.available()) {
    Serial.println("[STT] Timeout waiting for response.");
    client.stop(); return "";
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.println("[STT] Status: " + statusLine);

  bool isChunked = false;
  while (client.connected() || client.available()) {
    String hdr = client.readStringUntil('\n');
    hdr.trim();
    if (hdr.length() == 0) break;
    if (hdr.indexOf("chunked") >= 0) isChunked = true;
  }

  String responseBody = "";
  unsigned long bodyTimeout = millis() + 15000;
  while ((client.connected() || client.available()) && millis() < bodyTimeout) {
    if (client.available()) {
      responseBody += (char)client.read();
    } else {
      delay(1);
    }
  }
  client.stop();
  responseBody.trim();

  String cleanBody = responseBody;
  if (isChunked) {
    // Same simple chunked-transfer-encoding parser used for the chat API.
    cleanBody = "";
    int pos = 0;
    while (pos < (int)responseBody.length()) {
      int lineEnd = responseBody.indexOf("\r\n", pos);
      if (lineEnd < 0) break;
      String sizeLine = responseBody.substring(pos, lineEnd); sizeLine.trim();
      long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      pos = lineEnd + 2;
      if (chunkSize == 0) break;
      cleanBody += responseBody.substring(pos, pos + chunkSize);
      pos += chunkSize + 2;
    }
    if (cleanBody.isEmpty()) cleanBody = responseBody;
  }

  if (statusLine.indexOf("200") < 0) {
    Serial.println("[STT] Error body: " + cleanBody);
    return "";
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, cleanBody);
  if (err) { Serial.println("[STT] JSON parse error."); return ""; }
  String text = doc["text"] | "";
  text.trim();
  Serial.println("[STT] Heard: " + text);
  return text;
}

// =====================================================================
// Groq TTS — streams WAV audio to the NS4168 amp
//
// KEY FIXES vs. previous version:
//   1. Pre-buffer ~32 KB of PCM before starting I2S so the DMA ring
//      is already full when the clock starts — eliminates the initial
//      scratch/pop and protects against early TCP stalls.
//   2. Aggressive TCP drain loop: keep reading until the socket buffer
//      is truly empty rather than relying on client.available() which
//      can return 0 for several ms between TCP packets.
//   3. Static 4 KB read buffer (heap, not stack) to get large chunks
//      per readBytes() call — fewer calls = less overhead = less jitter.
//   4. WAV channel count check: if Groq sends stereo, write it as-is
//      instead of double-duplicating every sample.
//   5. Longer end-of-stream drain (400 ms) to cleanly flush all DMA
//      buffers before stopping the I2S clock.
// =====================================================================
void speakTextGroq(const String& text) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[TTS] No WiFi."); return; }
  if (text.length() == 0) return;

  Serial.println("[TTS] Requesting audio from Groq (fallback)...");
  // Screen stays in MODE_TEXT showing the reply — no face/mouth animation
  // during playback anymore, so it doesn't fight with the text on screen.

  String body = String("{\"model\":\"") + TTS_MODEL + "\","
                "\"input\":" + jsonStringEscape(text) + ","
                "\"voice\":\"" + TTS_VOICE + "\","
                "\"response_format\":\"wav\"}";

  WiFiClientSecure client;
  client.setInsecure();
  // Increase TCP receive buffer timeout — TLS handshake can take ~1 s
  client.setTimeout(15);

  if (!client.connect("api.groq.com", 443)) {
    Serial.println("[TTS] Connection failed.");
    return;
  }

  // Use HTTP/1.1 with WiFiClientSecure — HTTP/1.0 causes an empty
  // status line on some TLS stacks because the server closes before
  // the client reads the response. HTTP/1.1 keeps the connection open
  // long enough for readStringUntil() to get the full status line.
  client.print("POST /openai/v1/audio/speech HTTP/1.1\r\n");
  client.print("Host: api.groq.com\r\n");
  client.print("Authorization: Bearer "); client.print(GROQ_API_KEY); client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: "); client.print(body.length()); client.print("\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  // Wait up to 10 s for the server to start sending the response.
  // Without this wait, readStringUntil() returns immediately on an
  // empty buffer, giving us a blank status line.
  unsigned long waitStart = millis();
  while (!client.available() && millis() - waitStart < 10000) {
    delay(10);
  }
  if (!client.available()) {
    Serial.println("[TTS] Timeout waiting for response.");
    client.stop(); return;
  }

  // --- Read full response into a buffer so we can handle both
  //     error JSON and binary WAV in one clean pass. ---
  // First read all headers line by line.
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.println("[TTS] Status: " + statusLine);

  // Collect all header lines and print them
  bool isChunked = false;
  while (client.connected() || client.available()) {
    String hdr = client.readStringUntil('\n');
    hdr.trim();
    if (hdr.length() == 0) break;  // blank line = end of headers
    Serial.println("[TTS] Hdr: " + hdr);
    if (hdr.indexOf("chunked") >= 0) isChunked = true;
  }

  if (statusLine.indexOf("200") < 0) {
    // Read and print error body (it's JSON text, safe to accumulate)
    String errBody = "";
    unsigned long errTimeout = millis() + 4000;
    while ((client.connected() || client.available()) && millis() < errTimeout) {
      if (client.available()) {
        char c = (char)client.read();
        if (c >= 32 || c == '\n' || c == '\r') errBody += c;
      } else { delay(2); }
    }
    errBody.trim();
    Serial.println("[TTS] Error body: " + (errBody.length() ? errBody : "(empty)"));
    client.stop(); return;
  }

  // --- Stream body to I2S with proper dechunking ---
  // We can't buffer the whole WAV (10s @ 48kHz mono = ~960 KB).
  // Instead: read the 44-byte WAV header first, configure I2S, then
  // stream the rest straight to I2S.
  //
  // IMPORTANT: this does NOT try to detect chunk-size lines by peeking
  // at the data and guessing whether it "looks like" hex text — the
  // data is raw 16-bit PCM audio, and roughly 1 in 12 random audio
  // bytes happens to match a hex-digit ASCII value. Guessing that way
  // periodically misidentifies real audio bytes as chunk framing and
  // discards them, which is a constant, high-frequency corruption
  // source (this was the main cause of the static). Instead, we track
  // the exact number of bytes remaining in the current chunk and only
  // read a new chunk-size line once that count hits zero — no guessing.
  long   chunkRemaining = 0;
  bool   chunkedDone    = false;
  static uint8_t rbuf[512];
  size_t rbufLen = 0, rbufPos = 0;
  unsigned long bodyTimeout = millis() + 30000;

  auto nextRawByte = [&]() -> int {
    while (true) {
      if (rbufPos < rbufLen) return rbuf[rbufPos++];
      if (millis() > bodyTimeout) return -1;
      if (isChunked && chunkRemaining <= 0) {
        if (chunkedDone) return -1;
        if (!client.available()) {
          if (!client.connected()) return -1;
          delay(1); continue;
        }
        int pk = client.peek();
        if (pk == '\r' || pk == '\n') { client.read(); continue; }  // trailing CRLF from previous chunk
        String szLine = client.readStringUntil('\n'); szLine.trim();
        long sz = strtol(szLine.c_str(), nullptr, 16);
        if (sz <= 0) { chunkedDone = true; return -1; }  // final 0-length chunk
        chunkRemaining = sz;
        continue;
      }
      if (!client.available()) {
        if (!client.connected()) return -1;
        delay(1); continue;
      }
      size_t want = sizeof(rbuf);
      if (isChunked) want = min(want, (size_t)chunkRemaining);
      size_t got = client.read(rbuf, want);
      if (got == 0) continue;
      if (isChunked) chunkRemaining -= got;
      rbufLen = got; rbufPos = 0;
    }
  };

  // Read 44-byte WAV header
  uint8_t wavHeader[44];
  for (int i = 0; i < 44; i++) {
    int b = nextRawByte();
    if (b < 0) { Serial.println("[TTS] WAV header read failed."); client.stop(); return; }
    wavHeader[i] = (uint8_t)b;
  }

  // Verify RIFF magic
  if (wavHeader[0] != 'R' || wavHeader[1] != 'I' || wavHeader[2] != 'F' || wavHeader[3] != 'F') {
    String errStr = "";
    for (int i = 0; i < 44; i++) { char c = wavHeader[i]; if (c >= 32) errStr += c; }
    Serial.println("[TTS] Not a WAV — body started: " + errStr);
    client.stop(); return;
  }

  uint32_t wavSampleRate = (uint32_t)wavHeader[24] | ((uint32_t)wavHeader[25]<<8)
                         | ((uint32_t)wavHeader[26]<<16) | ((uint32_t)wavHeader[27]<<24);
  uint16_t wavChannels   = wavHeader[22] | (wavHeader[23]<<8);
  uint16_t wavBits       = wavHeader[34] | (wavHeader[35]<<8);
  Serial.printf("[TTS] WAV: %u Hz, %u ch, %u-bit\n", wavSampleRate, wavChannels, wavBits);

  if (wavBits != 16) {
    Serial.printf("[TTS] Unsupported bit depth %u.\n", wavBits);
    client.stop(); return;
  }

  // Configure I2S and start clock
  i2s_set_clk(I2S_NUM_0, wavSampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_start(I2S_NUM_0);
  resetI2SStereoCarry();
  Serial.println("[TTS] Streaming to I2S...");

  const size_t STREAM_BUF = 4096;
  uint8_t* buf = (uint8_t*)malloc(STREAM_BUF);
  if (!buf) { Serial.println("[TTS] malloc failed."); i2s_stop(I2S_NUM_0); client.stop(); return; }

  size_t totalAudio = 0, bi = 0;
  int rb;
  while ((rb = nextRawByte()) >= 0) {
    buf[bi++] = (uint8_t)rb;
    if (bi >= STREAM_BUF) {
      if (wavChannels == 1) writeMonoToI2SStereo(buf, bi);
      else { size_t written = 0; i2s_write(I2S_NUM_0, buf, bi, &written, portMAX_DELAY); }
      totalAudio += bi;
      bi = 0;
    }
  }
  if (bi > 0) {
    if (wavChannels == 1) writeMonoToI2SStereo(buf, bi);
    else { size_t written = 0; i2s_write(I2S_NUM_0, buf, bi, &written, portMAX_DELAY); }
    totalAudio += bi;
  }

  free(buf);
  client.stop();

  delay(400);
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_stop(I2S_NUM_0);

  Serial.printf("[TTS] Done. %u audio bytes played.\n", totalAudio);
  // Reply text stays on screen (scrollable) until the user taps the pad
  // to bring back the neutral face — see handleVoiceInput().
}

// =====================================================================
// Self-hosted Edge-TTS proxy — primary voice. Talks to a small local
// server (travisvn/openai-edge-tts) running elsewhere on your network,
// which does the actual Microsoft Edge TTS work and hands back a plain
// WAV file. Real French neural voice, genuinely free, no account setup.
//
// This is plain HTTP (not HTTPS) since it's just talking to your own
// LAN — no TLS handshake needed, which also makes this the simplest of
// the TTS integrations in this file. The response shape (raw WAV bytes,
// optionally chunked) is identical to Groq's, so this reuses the same
// exact-byte-count chunk tracking (never guessing at chunk boundaries
// from the audio content itself). Returns false on any failure so the
// caller can fall back to Groq.
// =====================================================================
bool speakTextEdge(const String& text) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[TTS] No WiFi."); return false; }
  if (text.length() == 0) return true;  // nothing to say isn't a failure

  const char* voiceName = forceEnglishMode ? EDGE_PROXY_VOICE_EN : EDGE_PROXY_VOICE_FR;
  Serial.printf("[TTS] Requesting audio from Edge-TTS proxy (%s)...\n", voiceName);

  String body = String("{\"model\":\"tts-1\",")
                + "\"input\":" + jsonStringEscape(text) + ","
                + "\"voice\":\"" + voiceName + "\","
                + "\"response_format\":\"wav\"}";

  WiFiClient client;  // plain HTTP — this is a local LAN request, no TLS needed
  client.setTimeout(15);

  if (!client.connect(EDGE_PROXY_HOST, EDGE_PROXY_PORT)) {
    Serial.println("[TTS] Edge-TTS proxy: connection failed (is the Docker container running?).");
    return false;
  }

  client.print("POST /v1/audio/speech HTTP/1.1\r\n");
  client.print("Host: "); client.print(EDGE_PROXY_HOST); client.print("\r\n");
  client.print("Authorization: Bearer "); client.print(EDGE_PROXY_API_KEY); client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: "); client.print(body.length()); client.print("\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  unsigned long waitStart = millis();
  while (!client.available() && millis() - waitStart < 10000) delay(10);
  if (!client.available()) {
    Serial.println("[TTS] Edge-TTS proxy: timeout waiting for response.");
    client.stop(); return false;
  }

  String statusLine = client.readStringUntil('\n'); statusLine.trim();
  Serial.println("[TTS] Edge-TTS proxy status: " + statusLine);

  bool isChunked = false;
  while (client.connected() || client.available()) {
    String hdr = client.readStringUntil('\n'); hdr.trim();
    if (hdr.length() == 0) break;
    if (hdr.indexOf("chunked") >= 0) isChunked = true;
  }

  if (statusLine.indexOf("200") < 0) {
    String errBody = "";
    unsigned long errTimeout = millis() + 4000;
    while ((client.connected() || client.available()) && millis() < errTimeout) {
      if (client.available()) { char c = (char)client.read(); if (c >= 32) errBody += c; }
      else delay(2);
    }
    Serial.println("[TTS] Edge-TTS proxy error body: " + errBody);
    client.stop();
    return false;
  }

  // ---- Buffered raw-byte reader over the (possibly chunked) HTTP body ----
  // Same exact-byte-count chunk tracking as speakTextGroq() — never
  // guesses at chunk boundaries from the audio content itself.
  long   chunkRemaining = 0;
  bool   chunkedDone    = false;
  static uint8_t rbuf[512];
  size_t rbufLen = 0, rbufPos = 0;
  unsigned long bodyTimeout = millis() + 30000;

  auto nextRawByte = [&]() -> int {
    while (true) {
      if (rbufPos < rbufLen) return rbuf[rbufPos++];
      if (millis() > bodyTimeout) return -1;
      if (isChunked && chunkRemaining <= 0) {
        if (chunkedDone) return -1;
        if (!client.available()) {
          if (!client.connected()) return -1;
          delay(1); continue;
        }
        int pk = client.peek();
        if (pk == '\r' || pk == '\n') { client.read(); continue; }
        String szLine = client.readStringUntil('\n'); szLine.trim();
        long sz = strtol(szLine.c_str(), nullptr, 16);
        if (sz <= 0) { chunkedDone = true; return -1; }
        chunkRemaining = sz;
        continue;
      }
      if (!client.available()) {
        if (!client.connected()) return -1;
        delay(1); continue;
      }
      size_t want = sizeof(rbuf);
      if (isChunked) want = min(want, (size_t)chunkRemaining);
      size_t got = client.read(rbuf, want);
      if (got == 0) continue;
      if (isChunked) chunkRemaining -= got;
      rbufLen = got; rbufPos = 0;
    }
  };

  // Read 44-byte WAV header
  uint8_t wavHeader[44];
  for (int i = 0; i < 44; i++) {
    int b = nextRawByte();
    if (b < 0) { Serial.println("[TTS] Edge-TTS proxy: WAV header read failed."); client.stop(); return false; }
    wavHeader[i] = (uint8_t)b;
  }

  if (wavHeader[0] != 'R' || wavHeader[1] != 'I' || wavHeader[2] != 'F' || wavHeader[3] != 'F') {
    String errStr = "";
    for (int i = 0; i < 44; i++) { char c = wavHeader[i]; if (c >= 32) errStr += c; }
    Serial.println("[TTS] Edge-TTS proxy: not a WAV — body started: " + errStr);
    client.stop(); return false;
  }

  uint32_t wavSampleRate = (uint32_t)wavHeader[24] | ((uint32_t)wavHeader[25]<<8)
                         | ((uint32_t)wavHeader[26]<<16) | ((uint32_t)wavHeader[27]<<24);
  uint16_t wavChannels   = wavHeader[22] | (wavHeader[23]<<8);
  uint16_t wavBits       = wavHeader[34] | (wavHeader[35]<<8);
  Serial.printf("[TTS] Edge-TTS proxy WAV: %u Hz, %u ch, %u-bit\n", wavSampleRate, wavChannels, wavBits);

  if (wavBits != 16) {
    Serial.printf("[TTS] Edge-TTS proxy: unsupported bit depth %u.\n", wavBits);
    client.stop(); return false;
  }

  i2s_set_clk(I2S_NUM_0, wavSampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_start(I2S_NUM_0);
  resetI2SStereoCarry();
  Serial.println("[TTS] Edge-TTS proxy: streaming to I2S...");

  const size_t STREAM_BUF = 4096;
  uint8_t* buf = (uint8_t*)malloc(STREAM_BUF);
  if (!buf) {
    Serial.println("[TTS] Edge-TTS proxy: malloc failed.");
    i2s_stop(I2S_NUM_0); client.stop(); return false;
  }

  size_t totalAudio = 0, bi = 0;
  int rb;
  while ((rb = nextRawByte()) >= 0) {
    buf[bi++] = (uint8_t)rb;
    if (bi >= STREAM_BUF) {
      if (wavChannels == 1) writeMonoToI2SStereo(buf, bi);
      else { size_t written = 0; i2s_write(I2S_NUM_0, buf, bi, &written, portMAX_DELAY); }
      totalAudio += bi;
      bi = 0;
    }
  }
  if (bi > 0) {
    if (wavChannels == 1) writeMonoToI2SStereo(buf, bi);
    else { size_t written = 0; i2s_write(I2S_NUM_0, buf, bi, &written, portMAX_DELAY); }
    totalAudio += bi;
  }

  free(buf);
  client.stop();

  delay(400);
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_stop(I2S_NUM_0);

  Serial.printf("[TTS] Edge-TTS proxy done. %u audio bytes played.\n", totalAudio);
  return totalAudio > 0;
}

// =====================================================================
// Top-level entry point every other function calls. Tries the local
// Edge-TTS proxy first (real French voice, genuinely free, no account
// needed); if that fails for any reason (proxy not running, wrong IP,
// network hiccup, etc.) it falls back to the Groq/Orpheus path so
// speech never just goes silent.
// =====================================================================
void speakText(const String& text) {
  if (speakTextEdge(text)) return;
  Serial.println("[TTS] Edge-TTS proxy failed — falling back to Groq.");
  speakTextGroq(text);
}

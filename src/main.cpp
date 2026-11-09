#include "mcu-max.h"
#include "opening_book.h" // Nạp sách khai cuộc tự động sinh từ PGN
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

// ========== CẤU HÌNH PHẦN CỨNG ==========
TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(TOUCH_CS);

// ========== BẢNG MÀU NEON CYBERPUNK (BGR565) ==========
#define CYBER_BG 0x0000          // Deep Space (Nền đen tuyệt đối)
#define CYBER_DARK_SQ 0x3003     // Cyber Purple (Ô tối bàn cờ)
#define CYBER_LIGHT_SQ 0xB7C5    // Neon Mint (Ô sáng bàn cờ)
#define CYBER_MAGENTA 0x781F     // Hot Magenta (Chiếu tướng, Active)
#define CYBER_CYAN 0xFF80        // Electric Cyan (Viền HUD, Normal)
#define CYBER_YELLOW 0x06FF      // Cyber Yellow (Highlight nước đi)
#define CYBER_GREY 0x4208        // Cyber Grey (Văn bản phụ)
#define CYBER_RED 0x001F         // Đỏ Neon BGR (Cảnh báo chiếu tướng)
#define CYBER_NO_TIME_TXT 0x053F // Cam Neon BGR (Chữ NO TIME)
#define CYBER_HIST_NUM 0x06BF    // Vàng Neon BGR (Số thứ tự move)
#define CYBER_HIST_BLK 0xBDF7    // Xám Ánh Kim BGR (Nước đi quân đen)
#define CYBER_ORANGE 0x03DF      // Cam Neon rực rỡ (BGR565) - Nút Chess Timer!

// ========== TRẠNG THÁI GAME ==========
enum GameState : uint8_t {
  STATE_MENU_MODE,
  STATE_MENU_LEVEL,
  STATE_MENU_COLOR,
  STATE_MENU_TIME,
  STATE_MENU_CUSTOM_TIME,
  STATE_PLAYING,
  STATE_GAME_OVER,
  STATE_REVIEW,
  STATE_TIMER_RUNNING
};
static GameState currentState = STATE_MENU_MODE;
static const char *gameOverMsg = "";
static const char *drawReason = "";

static bool isSerialDebugActive = false;
static int reviewMoveIndex = 0;

// ========== CHẾ ĐỘ CHƠI & THỜI GIAN ==========
enum GameMode : uint8_t { MODE_PVB, MODE_PVP, MODE_TIMER };
static GameMode currentGameMode = MODE_PVB;

enum TimeMode : uint8_t {
  TIME_UNTIMED,
  TIME_BLITZ_3_0,
  TIME_BLITZ_3_2,
  TIME_RAPID_10_0,
  TIME_RAPID_15_10,
  TIME_CUSTOM
};
static TimeMode currentTimeMode = TIME_UNTIMED;

static bool timerActiveWhite = true;
static bool timerPaused = false;

// ========== CẤU HÌNH BÀN CỜ & CẤP ĐỘ BOT ==========
#define SQUARE_SIZE 30
#define BOARD_SIZE 240

#define SIDE_WHITE MCUMAX_SIDE_WHITE
#define SIDE_BLACK MCUMAX_SIDE_BLACK

struct BotLevelConfig {
  const char *name;
  uint32_t nodes;
  uint8_t depth;
  bool probabilistic;
};

static const BotLevelConfig BOT_LEVELS[4] = {
    {"easy", 6000, 4, true},
    {"medium", 20000, 5, true},
    {"hard", 75000, 6, true},
    {"extreme", 600000, 7, false},
};

static uint8_t playerSide = SIDE_WHITE;
static uint8_t botSide = SIDE_BLACK;
static uint8_t displaySide = SIDE_WHITE;

static uint32_t engineNodes = 20000;
static uint32_t engineDepth = 5;

// Lịch sử ván đấu tối ưu RAM: gộp quân cờ + phong cấp vào 1 byte meta,
// thay String bằng mảng tĩnh 8 byte và chỉ lưu đồng hồ của bên vừa đi.
#define MAX_MOVES 4096
struct MoveRecord {
  mcumax_square from;
  mcumax_square to;
  uint8_t meta; // bits 0..2: promotion_piece, bits 3..7: movingPiece (0..31)
};

static MoveRecord moveHistory[MAX_MOVES];
static char moveSANHistory[MAX_MOVES][8];
static uint32_t moveTimeHistory[MAX_MOVES];
static volatile int16_t moveDelta[MAX_MOVES]; // -1: chưa đánh giá, >= 0: độ chênh lệch cp vs best move
static volatile uint8_t moveEvalClass[MAX_MOVES];
static volatile bool isAnimating = false;
static volatile int currentMoveCount = 0;

static inline mcumax_move getHistoryMove(int idx) {
  return {moveHistory[idx].from, moveHistory[idx].to,
          (mcumax_piece)(moveHistory[idx].meta & 0x07)};
}
static inline mcumax_piece getHistoryPiece(int idx) {
  return (mcumax_piece)((moveHistory[idx].meta >> 3) & 0x1F);
}

// Trạng thái tính toán đánh giá ngầm trên Core 0
static TaskHandle_t bgEvalTaskHandle = NULL;
static volatile bool bgEvalCore0Running = true;
static volatile int bgEvalCurrentPly = -1;
static uint8_t calcDots = 0;
static unsigned long lastCalcAnim = 0;

// Cửa sổ xác nhận thoát khi bấm MENU
static bool showQuitConfirm = false;

enum MoveEvalClass : uint8_t {
  EVAL_NONE,
  EVAL_BRILLIANT,  // Thí quân chiến thuật chuẩn xác ("!!", Xanh lam sáng 0x03FF)
  EVAL_GREAT,      // Nước đi duy nhất / cứu ván ("!", Cyan CYBER_CYAN)
  EVAL_GOOD,       // Nước đi tối ưu / bài bản (Ngôi sao 5 cánh, Xanh lá cây 0x07E0)
  EVAL_INACCURACY, // Nước cờ thiếu chính xác ("?", Cam CYBER_ORANGE)
  EVAL_BLUNDER     // Sai lầm nghiêm trọng ("??", Đỏ CYBER_RED)
};

static inline MoveEvalClass getMoveEvalClass(int16_t delta) {
  if (delta < 0) return EVAL_NONE;
  if (delta <= 50) return EVAL_GOOD;
  if (delta < 200) return EVAL_INACCURACY;
  return EVAL_BLUNDER;
}

static bool isSacrificeMove(mcumax_move m, uint8_t side, int32_t bestScore) {
  if (bestScore < -50) return false;
  mcumax_piece p = mcumax_get_piece(m.from);
  mcumax_piece cap = mcumax_get_piece(m.to);

  auto pieceVal = [](mcumax_piece pc) -> int {
    switch (pc & 0x07) {
      case MCUMAX_PAWN_UPSTREAM:
      case MCUMAX_PAWN_DOWNSTREAM: return 1;
      case MCUMAX_KNIGHT:
      case MCUMAX_BISHOP: return 3;
      case MCUMAX_ROOK: return 5;
      case MCUMAX_QUEEN: return 9;
      default: return 0;
    }
  };

  int valP = pieceVal(p);
  int valCap = pieceVal(cap);

  if (valP < 3) return false;
  if (valCap >= valP) return false;

  uint8_t oppSide = side ^ 0x18;
  if (!mcumax_is_square_attacked(m.to, oppSide)) return false;

  return true;
}

static mcumax_square kingInCheckSquare = MCUMAX_SQUARE_INVALID;
static mcumax_square selectedSquare = MCUMAX_SQUARE_INVALID;
static mcumax_move validMoves[32]; // Một quân cờ có tối đa 27 nước hợp lệ
static uint32_t validMovesCount = 0;

// Cờ đồng bộ cho phép Undo / Menu bất đồng bộ khi BOT đang suy nghĩ
static volatile bool pendingMenuAction = false;
static volatile bool pendingUndoAction = false;

// Quản lý cảm ứng dùng chung giữa loop() và engine_callback()
static bool touchActive = false;
static unsigned long lastTouchRelease = 0;

// ========== BIẾN QUẢN LÝ THỜI GIAN PvP & TIMER MODE ==========
static uint32_t whiteTimeLeft = 0;
static uint32_t blackTimeLeft = 0;
static unsigned long lastTimerUpdate = 0;
static uint32_t timeIncrementMs = 0;
static bool whiteClockFlash = false;
static bool blackClockFlash = false;
static unsigned long lastFlashToggle = 0;
static bool pvpTimerStarted = false;
static bool timerOnlyStarted = false;

static int customMinutes = 10;
static int customIncrement = 5;

// ========== BIẾN CHO BOT THINKING & REVIEW ==========
static volatile bool botThinking = false;
static volatile bool botStopped = false;
static uint8_t thinkingDots = 0;
static unsigned long lastThinkingAnim = 0;

static bool isCheckmateFlag = false;
static bool isStalemateFlag = false;
static mcumax_square specialKingSquare = MCUMAX_SQUARE_INVALID;

// ========== TEXTURES CÁC QUÂN CỜ (24x24, 72 BYTES/QUÂN TRONG FLASH) ==========
static const uint8_t PROGMEM tex_pawn[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00,
    0x00, 0x3c, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x3c, 0x00,
    0x00, 0x18, 0x00, 0x00, 0x18, 0x00, 0x00, 0x18, 0x00, 0x00, 0x3c, 0x00,
    0x00, 0x3c, 0x00, 0x00, 0x7e, 0x00, 0x00, 0xff, 0x00, 0x01, 0xff, 0x80,
    0x01, 0xff, 0x80, 0x01, 0xff, 0x80, 0x01, 0xff, 0x80, 0x03, 0xff, 0xc0,
    0x0f, 0xff, 0xf0, 0x1f, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t PROGMEM tex_knight[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x3f, 0x00,
    0x00, 0x7f, 0x80, 0x00, 0xff, 0xc0, 0x01, 0xff, 0xc0, 0x03, 0xff, 0xc0,
    0x03, 0xef, 0xc0, 0x07, 0xdf, 0x80, 0x0f, 0xbf, 0x00, 0x1f, 0x7e, 0x00,
    0x1e, 0xfc, 0x00, 0x1d, 0xfc, 0x00, 0x03, 0xfc, 0x00, 0x03, 0xf8, 0x00,
    0x03, 0xf0, 0x00, 0x07, 0xf0, 0x00, 0x07, 0xf8, 0x00, 0x0f, 0xfc, 0x00,
    0x1f, 0xff, 0x80, 0x3f, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t PROGMEM tex_bishop[] = {
    0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x18, 0x00, 0x00, 0x7e, 0x00,
    0x00, 0x18, 0x00, 0x00, 0x18, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x7e, 0x00,
    0x00, 0xff, 0x00, 0x01, 0xff, 0x80, 0x01, 0xe7, 0x80, 0x01, 0xc3, 0x80,
    0x01, 0x81, 0x80, 0x00, 0xff, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x3c, 0x00,
    0x00, 0x3c, 0x00, 0x00, 0x7e, 0x00, 0x00, 0xff, 0x00, 0x03, 0xff, 0xc0,
    0x0f, 0xff, 0xf0, 0x3f, 0xff, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t PROGMEM tex_rook[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x10, 0x1b, 0x1b, 0x10,
    0x1f, 0xff, 0xf0, 0x1f, 0xff, 0xf0, 0x0f, 0xff, 0xe0, 0x07, 0xff, 0xc0,
    0x07, 0xff, 0xc0, 0x07, 0xff, 0xc0, 0x07, 0xff, 0xc0, 0x07, 0xff, 0xc0,
    0x07, 0xff, 0xc0, 0x07, 0xff, 0xc0, 0x07, 0xff, 0xc0, 0x07, 0xff, 0xc0,
    0x07, 0xff, 0xc0, 0x0f, 0xff, 0xe0, 0x0f, 0xff, 0xe0, 0x1f, 0xff, 0xf0,
    0x3f, 0xff, 0xf8, 0x3f, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t PROGMEM tex_queen[] = {
    0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x38, 0x28, 0x38, 0x28, 0x28, 0x28,
    0x2c, 0x28, 0x68, 0x26, 0x38, 0xc8, 0x23, 0x7d, 0x88, 0x21, 0xff, 0x08,
    0x20, 0xff, 0x08, 0x20, 0xff, 0x08, 0x10, 0xff, 0x10, 0x18, 0xff, 0x30,
    0x08, 0xff, 0x20, 0x0c, 0xff, 0x60, 0x0e, 0xff, 0xc0, 0x07, 0xff, 0x80,
    0x07, 0xff, 0x80, 0x03, 0xff, 0x00, 0x03, 0xff, 0x00, 0x07, 0xff, 0x80,
    0x1f, 0xff, 0xf0, 0x3f, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t PROGMEM tex_king[] = {
    0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x38, 0x00, 0x00, 0xfe, 0x00,
    0x00, 0x38, 0x00, 0x00, 0x38, 0x00, 0x18, 0x38, 0x30, 0x3c, 0xff, 0x78,
    0x3e, 0xff, 0xf8, 0x3f, 0xff, 0xf8, 0x3f, 0xff, 0xf8, 0x3f, 0xff, 0xf8,
    0x1f, 0xff, 0xf0, 0x1f, 0xff, 0xf0, 0x0f, 0xff, 0xe0, 0x0f, 0xff, 0xe0,
    0x07, 0xff, 0xc0, 0x07, 0xff, 0xc0, 0x07, 0xff, 0xc0, 0x07, 0xff, 0xc0,
    0x1f, 0xff, 0xf0, 0x3f, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const uint8_t *const PIECE_TEX[8] = {
    nullptr,  tex_pawn,   tex_pawn, tex_knight,
    tex_king, tex_bishop, tex_rook, tex_queen};

// ========== NGUYÊN MẪU CÁC HÀM CHÍNH ==========
static void drawBoard();
static void drawSidebar();
static void drawPvPClocks();
static void drawModeMenu();
static void updateTimerDisplay(bool forceRedraw = false);
static void drawReviewEvalBadge();
static void drawReviewCalculatingDots(uint8_t dots);
static void drawQuitConfirmModal();
static void drawStarIcon(int cx, int cy, uint16_t color);
static float calculateSingleMoveAccuracy(int16_t delta);
static float getPlayerAccuracy(int *outCount = nullptr);
static inline bool isPlyPlayerMove(int ply);
static void bgEvalTask(void *pvParameters);

// ==================== CẢM ỨNG & HỆ TỌA ĐỘ ====================
static bool getStableTouchPoint(int &outX, int &outY) {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    if (p.z >= 200) {
      outX = constrain((int)map(p.x, 300, 3800, 0, 320), 0, 319);
      outY = constrain((int)map(p.y, 300, 3800, 0, 240), 0, 239);
      return true;
    }
  }
  return false;
}

static inline uint8_t getInteractiveSide() {
  return (currentGameMode == MODE_PVP) ? mcumax_get_current_side() : playerSide;
}

static inline mcumax_square displayToSquare(int displayFile, int displayRank) {
  int rank = (displaySide == SIDE_WHITE) ? displayRank : (7 - displayRank);
  int file = (displaySide == SIDE_WHITE) ? displayFile : (7 - displayFile);
  return (mcumax_square)((rank << 4) | file);
}

static inline bool squareToDisplay(mcumax_square sq, int &displayFile,
                                   int &displayRank) {
  if (sq & 0x88)
    return false;
  int rank = sq >> 4;
  int file = sq & 0x07;
  displayFile = (displaySide == SIDE_WHITE) ? file : (7 - file);
  displayRank = (displaySide == SIDE_WHITE) ? rank : (7 - rank);
  return true;
}

static inline uint16_t getSquareBgColor(mcumax_square sq) {
  bool isDark = (((sq >> 4) + (sq & 0x07)) & 1) != 0;
  return isDark ? CYBER_DARK_SQ : CYBER_LIGHT_SQ;
}

static inline bool shouldRotatePiece180(bool isWhite) {
  return (currentGameMode == MODE_PVP) && !isWhite;
}

// ==================== ĐỒ HỌA QUÂN CỜ & HIỆU ỨNG ====================
static void unpackBitmapRows(const uint8_t *tex, uint32_t row[28]) {
  memset(row, 0, sizeof(uint32_t) * 28);
  for (int v = 0; v < 24; v++) {
    const uint8_t *p = tex + v * 3;
    row[2 + v] = (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]) << 1;
  }
}

static void drawPieceAtPixel(int x, int y, const uint8_t *tex, bool isWhite,
                             bool rot180 = false) {
  if (!tex)
    return;
  uint32_t row[28];
  unpackBitmapRows(tex, row);

  uint16_t fillColor = isWhite ? 0xFFFF : 0x0000;
  uint16_t outColor = isWhite ? 0x0000 : 0xFFFF;

  tft.startWrite();
  for (int ry = 0; ry < 26; ry++) {
    int idx = rot180 ? (26 - ry) : (1 + ry);
    uint32_t fill = row[idx];
    uint32_t outline =
        ((fill << 1) | (fill >> 1) | row[idx - 1] | row[idx + 1]) & ~fill;
    uint32_t combined = fill | outline;
    if (!combined)
      continue;
    int py = y + 2 + ry;
    for (int rx = 0; rx < 26; rx++) {
      uint32_t mask = rot180 ? (1U << rx) : (1U << (25 - rx));
      if (combined & mask) {
        tft.drawPixel(x + 2 + rx, py, (fill & mask) ? fillColor : outColor);
      }
    }
  }
  tft.endWrite();
}

static inline void drawPieceTexture(int x, int y, mcumax_piece p) {
  bool isWhite = mcumax_is_piece_white(p);
  drawPieceAtPixel(x, y, PIECE_TEX[p & 0x07], isWhite,
                   shouldRotatePiece180(isWhite));
}

static void drawPieceRotatedAngle(int cx, int cy, const uint8_t *tex,
                                  float angleDegrees, bool isWhite,
                                  uint16_t bgColor) {
  uint16_t sqBuf[SQUARE_SIZE * SQUARE_SIZE];
  for (int i = 0; i < SQUARE_SIZE * SQUARE_SIZE; i++)
    sqBuf[i] = bgColor;

  float rad = angleDegrees * (3.14159265f / 180.0f);
  int32_t cosFP = (int32_t)lroundf(cosf(rad) * 1024.0f);
  int32_t sinFP = (int32_t)lroundf(sinf(rad) * 1024.0f);

  uint16_t fillColor = isWhite ? 0xFFFF : 0x0000;
  uint16_t outColor = isWhite ? 0x0000 : 0xFFFF;

  for (int dy = -13; dy <= 13; dy++) {
    for (int dx = -13; dx <= 13; dx++) {
      int srcX = (dx * cosFP + dy * sinFP + 11776 + 512) >> 10;
      int srcY = (-dx * sinFP + dy * cosFP + 11776 + 512) >> 10;

      if ((unsigned)srcX < 24U && (unsigned)srcY < 24U) {
        bool pixel = (tex[srcY * 3 + (srcX >> 3)] >> (7 - (srcX & 7))) & 1;
        if (pixel) {
          sqBuf[(15 + dy) * SQUARE_SIZE + (15 + dx)] = fillColor;
        } else {
          bool neighbor = false;
          for (int ny = -1; ny <= 1 && !neighbor; ny++) {
            int nY = srcY + ny;
            if ((unsigned)nY >= 24U)
              continue;
            for (int nx = -1; nx <= 1; nx++) {
              int nX = srcX + nx;
              if ((unsigned)nX < 24U &&
                  ((tex[nY * 3 + (nX >> 3)] >> (7 - (nX & 7))) & 1)) {
                neighbor = true;
                break;
              }
            }
          }
          if (neighbor)
            sqBuf[(15 + dy) * SQUARE_SIZE + (15 + dx)] = outColor;
        }
      }
    }
  }

  tft.setAddrWindow(cx, cy, SQUARE_SIZE, SQUARE_SIZE);
  tft.pushColors(sqBuf, SQUARE_SIZE * SQUARE_SIZE, true);
}

static uint16_t blendColors(uint16_t color1, uint16_t color2, float ratio) {
  if (ratio <= 0.0f)
    return color1;
  if (ratio >= 1.0f)
    return color2;

  int c1_1 = (color1 >> 11) & 0x1F;
  int c1_2 = (color1 >> 5) & 0x3F;
  int c1_3 = color1 & 0x1F;

  int c2_1 = (color2 >> 11) & 0x1F;
  int c2_2 = (color2 >> 5) & 0x3F;
  int c2_3 = color2 & 0x1F;

  uint16_t r1 = (uint16_t)(c1_1 + (c2_1 - c1_1) * ratio);
  uint16_t r2 = (uint16_t)(c1_2 + (c2_2 - c1_2) * ratio);
  uint16_t r3 = (uint16_t)(c1_3 + (c2_3 - c1_3) * ratio);

  return (r1 << 11) | (r2 << 5) | r3;
}

static void animateKingRotation(mcumax_square kingSq, float startAngle,
                                float endAngle) {
  if (kingSq == MCUMAX_SQUARE_INVALID)
    return;
  mcumax_piece p = mcumax_get_piece(kingSq);
  if ((p & 0x07) != MCUMAX_KING)
    return;

  int displayFile, displayRank;
  if (!squareToDisplay(kingSq, displayFile, displayRank))
    return;

  int x = displayFile * SQUARE_SIZE;
  int y = displayRank * SQUARE_SIZE;
  uint16_t bg = getSquareBgColor(kingSq);
  bool isWhite = mcumax_is_piece_white(p);
  float baseAngle = shouldRotatePiece180(isWhite) ? 180.0f : 0.0f;

  isAnimating = true;
  unsigned long startTime = millis();
  const unsigned long duration = 200;
  unsigned long lastFrame = 0;
  const unsigned long frameInterval = 16;

  while (millis() - startTime < duration) {
    if (millis() - lastFrame >= frameInterval) {
      lastFrame = millis();
      float t = (float)(millis() - startTime) / duration;
      if (t > 1.0f)
        t = 1.0f;
      float easeT = (t < 0.5f) ? (2.0f * t * t) : (1.0f - powf(-2.0f * t + 2.0f, 2.0f) / 2.0f);
      float angle = baseAngle + startAngle + (endAngle - startAngle) * easeT;
      drawPieceRotatedAngle(x, y, tex_king, angle, isWhite, bg);
    }
    yield();
  }
  drawPieceRotatedAngle(x, y, tex_king, baseAngle + endAngle, isWhite, bg);
  isAnimating = false;
}

static void animateCheckmateSymbol(mcumax_square kingSq, char symbol = '#') {
  if (kingSq == MCUMAX_SQUARE_INVALID)
    return;
  mcumax_piece p = mcumax_get_piece(kingSq);
  if ((p & 0x07) != MCUMAX_KING)
    return;

  int displayFile, displayRank;
  if (!squareToDisplay(kingSq, displayFile, displayRank))
    return;

  int x = displayFile * SQUARE_SIZE;
  int y = displayRank * SQUARE_SIZE;
  char symStr[2] = {symbol, '\0'};

  unsigned long startTime = millis();
  const unsigned long duration = 250;
  uint16_t lastColor = 0xFFFF;
  while (millis() - startTime < duration) {
    float t = (float)(millis() - startTime) / duration;
    if (t > 1.0f)
      t = 1.0f;
    uint16_t blendedColor = blendColors(CYBER_BG, CYBER_MAGENTA, t);
    if (blendedColor != lastColor) {
      lastColor = blendedColor;
      tft.fillRect(x + SQUARE_SIZE - 12, y + 2, 10, 10, CYBER_BG);
      tft.setTextColor(blendedColor);
      tft.setTextSize(1);
      tft.setCursor(x + SQUARE_SIZE - 10, y + 4);
      tft.print(symStr);
    }
    yield();
  }
  tft.fillRect(x + SQUARE_SIZE - 12, y + 2, 10, 10, CYBER_BG);
  tft.setTextColor(CYBER_MAGENTA);
  tft.setTextSize(1);
  tft.setCursor(x + SQUARE_SIZE - 10, y + 4);
  tft.print(symStr);
}

static bool isEnPassantTarget(mcumax_square sq) {
  if (selectedSquare == MCUMAX_SQUARE_INVALID)
    return false;
  mcumax_square ep = mcumax_get_en_passant_square();
  if (ep == MCUMAX_SQUARE_INVALID || sq != ep)
    return false;
  mcumax_piece p = mcumax_get_piece(selectedSquare);
  if (!mcumax_is_piece_side(p, getInteractiveSide()))
    return false;
  uint8_t type = p & 0x07;
  if (type != MCUMAX_PAWN_UPSTREAM && type != MCUMAX_PAWN_DOWNSTREAM)
    return false;

  for (uint32_t i = 0; i < validMovesCount; i++) {
    if (validMoves[i].from == selectedSquare && validMoves[i].to == sq)
      return true;
  }
  return false;
}

static void drawSingleSquare(int displayFile, int displayRank,
                             bool highlight = false,
                             mcumax_square ignoreSq1 = MCUMAX_SQUARE_INVALID,
                             mcumax_square ignoreSq2 = MCUMAX_SQUARE_INVALID) {
  int x = displayFile * SQUARE_SIZE;
  int y = displayRank * SQUARE_SIZE;
  mcumax_square sq = displayToSquare(displayFile, displayRank);

  if ((currentState == STATE_REVIEW || currentState == STATE_GAME_OVER) &&
      (currentState == STATE_GAME_OVER || reviewMoveIndex == currentMoveCount) &&
      sq == specialKingSquare && (isCheckmateFlag || isStalemateFlag)) {
    mcumax_piece p = mcumax_get_piece(sq);
    if ((p & 0x07) == MCUMAX_KING) {
      bool isWhite = mcumax_is_piece_white(p);
      float baseAngle = shouldRotatePiece180(isWhite) ? 180.0f : 0.0f;
      drawPieceRotatedAngle(x, y, tex_king, baseAngle + 90.0f, isWhite,
                            getSquareBgColor(sq));
      tft.fillRect(x + SQUARE_SIZE - 12, y + 2, 10, 10, CYBER_BG);
      tft.setTextColor(CYBER_MAGENTA);
      tft.setTextSize(1);
      tft.setCursor(x + SQUARE_SIZE - 10, y + 4);
      tft.print(isStalemateFlag ? '=' : '#');
      return;
    }
  }

  uint16_t bgColor = getSquareBgColor(sq);
  if (sq == kingInCheckSquare) {
    bgColor = CYBER_RED;
  } else if (currentState == STATE_PLAYING) {
    if (sq == selectedSquare) {
      bgColor = CYBER_YELLOW;
    } else if (selectedSquare != MCUMAX_SQUARE_INVALID && !highlight) {
      if (isEnPassantTarget(sq)) {
        bgColor = 0x03FF;
      } else {
        for (uint32_t i = 0; i < validMovesCount; i++) {
          if (validMoves[i].to == sq) {
            bgColor = (!mcumax_is_empty_square(mcumax_get_piece(sq)))
                          ? CYBER_MAGENTA
                          : 0x1FC0;
            break;
          }
        }
      }
    }
  }

  uint16_t sqBuf[SQUARE_SIZE * SQUARE_SIZE];
  for (int i = 0; i < SQUARE_SIZE * SQUARE_SIZE; i++)
    sqBuf[i] = bgColor;

  if (sq != ignoreSq1 && sq != ignoreSq2) {
    mcumax_piece p = mcumax_get_piece(sq);
    const uint8_t *tex = PIECE_TEX[p & 0x07];
    if (tex) {
      bool isWhite = mcumax_is_piece_white(p);
      bool rot180 = shouldRotatePiece180(isWhite);
      uint16_t fillColor = isWhite ? 0xFFFF : 0x0000;
      uint16_t outColor = isWhite ? 0x0000 : 0xFFFF;
      uint32_t row[28];
      unpackBitmapRows(tex, row);

      for (int ry = 0; ry < 26; ry++) {
        int idx = rot180 ? (26 - ry) : (1 + ry);
        uint32_t fill = row[idx];
        uint32_t outline =
            ((fill << 1) | (fill >> 1) | row[idx - 1] | row[idx + 1]) & ~fill;
        uint32_t combined = fill | outline;
        if (!combined)
          continue;
        uint16_t *dstRow = sqBuf + (2 + ry) * SQUARE_SIZE + 2;
        for (int rx = 0; rx < 26; rx++) {
          uint32_t mask = rot180 ? (1U << rx) : (1U << (25 - rx));
          if (combined & mask) {
            dstRow[rx] = (fill & mask) ? fillColor : outColor;
          }
        }
      }
    }
  }

  tft.setAddrWindow(x, y, SQUARE_SIZE, SQUARE_SIZE);
  tft.pushColors(sqBuf, SQUARE_SIZE * SQUARE_SIZE, true);
}

static void drawStarIcon(int cx, int cy, uint16_t color) {
  static const uint16_t starMask[9] = {
      0b000010000,
      0b000010000,
      0b000111000,
      0b111111111,
      0b011111110,
      0b001111100,
      0b011010110,
      0b110000011,
      0b100000001
  };
  tft.startWrite();
  for (int r = 0; r < 9; r++) {
    uint16_t row = starMask[r];
    for (int c = 0; c < 9; c++) {
      if ((row >> (8 - c)) & 1) {
        tft.drawPixel(cx - 4 + c, cy - 4 + r, color);
      }
    }
  }
  tft.endWrite();
}

static void compositeSpriteToBuffer(uint16_t *sqBuf, int sqX0, int sqY0,
                                    int piecePixelX, int piecePixelY,
                                    const uint32_t pieceRows[28],
                                    uint16_t fillColor, uint16_t outColor,
                                    bool rot180) {
  int pX0 = piecePixelX + 2;
  int pY0 = piecePixelY + 2;
  if (pX0 + 26 <= sqX0 || pX0 >= sqX0 + SQUARE_SIZE ||
      pY0 + 26 <= sqY0 || pY0 >= sqY0 + SQUARE_SIZE)
    return;

  for (int ry = 0; ry < 26; ry++) {
    int screenY = pY0 + ry;
    if (screenY < sqY0 || screenY >= sqY0 + SQUARE_SIZE)
      continue;
    int by = screenY - sqY0;
    int idx = rot180 ? (26 - ry) : (1 + ry);
    uint32_t fill = pieceRows[idx];
    uint32_t outline =
        ((fill << 1) | (fill >> 1) | pieceRows[idx - 1] | pieceRows[idx + 1]) & ~fill;
    uint32_t combined = fill | outline;
    if (!combined)
      continue;
    uint16_t *dstRow = sqBuf + by * SQUARE_SIZE;
    for (int rx = 0; rx < 26; rx++) {
      int screenX = pX0 + rx;
      if (screenX < sqX0 || screenX >= sqX0 + SQUARE_SIZE)
        continue;
      int bx = screenX - sqX0;
      uint32_t mask = rot180 ? (1U << rx) : (1U << (25 - rx));
      if (combined & mask) {
        dstRow[bx] = (fill & mask) ? fillColor : outColor;
      }
    }
  }
}

static void drawSingleSquareAnimated(int displayFile, int displayRank,
                                     mcumax_square ignoreSq1, mcumax_square ignoreSq2,
                                     int piece1X, int piece1Y, const uint32_t rows1[28],
                                     uint16_t fill1, uint16_t out1, bool rot1,
                                     int piece2X = -999, int piece2Y = -999,
                                     const uint32_t rows2[28] = nullptr,
                                     uint16_t fill2 = 0, uint16_t out2 = 0, bool rot2 = false) {
  int x = displayFile * SQUARE_SIZE;
  int y = displayRank * SQUARE_SIZE;
  mcumax_square sq = displayToSquare(displayFile, displayRank);
  uint16_t bgColor = getSquareBgColor(sq);

  uint16_t sqBuf[SQUARE_SIZE * SQUARE_SIZE];
  for (int i = 0; i < SQUARE_SIZE * SQUARE_SIZE; i++)
    sqBuf[i] = bgColor;

  if (sq != ignoreSq1 && sq != ignoreSq2) {
    mcumax_piece p = mcumax_get_piece(sq);
    const uint8_t *tex = PIECE_TEX[p & 0x07];
    if (tex) {
      bool isWhite = mcumax_is_piece_white(p);
      bool rot180 = shouldRotatePiece180(isWhite);
      uint32_t row[28];
      unpackBitmapRows(tex, row);
      compositeSpriteToBuffer(sqBuf, x, y, x, y, row,
                              isWhite ? 0xFFFF : 0x0000,
                              isWhite ? 0x0000 : 0xFFFF,
                              rot180);
    }
  }

  if (rows1) {
    compositeSpriteToBuffer(sqBuf, x, y, piece1X, piece1Y, rows1, fill1, out1, rot1);
  }
  if (rows2) {
    compositeSpriteToBuffer(sqBuf, x, y, piece2X, piece2Y, rows2, fill2, out2, rot2);
  }

  tft.setAddrWindow(x, y, SQUARE_SIZE, SQUARE_SIZE);
  tft.pushColors(sqBuf, SQUARE_SIZE * SQUARE_SIZE, true);
}

static void drawBoard() {
  tft.startWrite();
  for (int displayRank = 0; displayRank < 8; displayRank++) {
    for (int displayFile = 0; displayFile < 8; displayFile++) {
      drawSingleSquare(displayFile, displayRank);
    }
  }
  tft.endWrite();
}

// ==================== TRẠNG THÁI VÁN CỜ & DI CHUYỂN ====================
static void rebuildBoardToPly(int plyCount) {
  mcumax_init();
  for (int i = 0; i < plyCount; i++) {
    mcumax_play_move(getHistoryMove(i));
  }
  kingInCheckSquare = mcumax_get_check_square(mcumax_get_current_side());
  if ((currentState == STATE_REVIEW || currentState == STATE_GAME_OVER) &&
      plyCount == currentMoveCount && (isCheckmateFlag || isStalemateFlag)) {
    specialKingSquare = mcumax_find_king(mcumax_get_current_side());
  } else {
    specialKingSquare = MCUMAX_SQUARE_INVALID;
  }
}

static void checkGameState() {
  mcumax_game_state_info_t info = mcumax_evaluate_game_state();

  isCheckmateFlag = false;
  isStalemateFlag = false;
  specialKingSquare = MCUMAX_SQUARE_INVALID;
  drawReason = "";

  if (info.state == MCUMAX_GAME_ACTIVE)
    return;

  currentState = STATE_GAME_OVER;

  switch (info.state) {
  case MCUMAX_GAME_DRAW_ONLY_KINGS:
    gameOverMsg = "DRAW!";
    drawReason = "ONLY KINGS LEFT";
    break;
  case MCUMAX_GAME_DRAW_SAME_COLOR_BISHOPS:
    gameOverMsg = "DRAW!";
    drawReason = "SAME COLOR BISHOPS";
    break;
  case MCUMAX_GAME_DRAW_INSUFFICIENT_MATERIAL:
    gameOverMsg = "DRAW!";
    drawReason = "INSUFFICIENT MATERIAL";
    break;
  case MCUMAX_GAME_DRAW_FIFTY_MOVE:
    gameOverMsg = "DRAW!";
    drawReason = "50-MOVE RULE";
    Serial.println("[SYSTEM] Game Over: Draw by 50-move rule!");
    break;
  case MCUMAX_GAME_DRAW_THREEFOLD:
    gameOverMsg = "DRAW!";
    drawReason = "3-FOLD REPETITION";
    break;
  case MCUMAX_GAME_STALEMATE:
    gameOverMsg = "DRAW!";
    drawReason = "STALEMATE";
    isStalemateFlag = true;
    specialKingSquare = info.king_square;
    break;
  case MCUMAX_GAME_CHECKMATE: {
    bool whiteIsMated = (mcumax_get_current_side() == SIDE_WHITE);
    if (currentGameMode == MODE_PVP) {
      gameOverMsg = whiteIsMated ? "BLACK WINS!" : "WHITE WINS!";
    } else {
      gameOverMsg =
          (mcumax_get_current_side() == playerSide) ? "YOU LOSE!" : "YOU WIN!";
    }
    isCheckmateFlag = true;
    specialKingSquare = info.king_square;
    if (currentMoveCount > 0) {
      char *lastSan = moveSANHistory[currentMoveCount - 1];
      size_t len = strlen(lastSan);
      if (len > 0 && lastSan[len - 1] == '+') {
        lastSan[len - 1] = '#';
      } else if (len + 1 < sizeof(moveSANHistory[0])) {
        lastSan[len] = '#';
        lastSan[len + 1] = '\0';
      }
    }
    break;
  }
  default:
    break;
  }
}

static void checkAndFlashKing() {
  int displayFile, displayRank;
  if (!squareToDisplay(kingInCheckSquare, displayFile, displayRank))
    return;

  mcumax_square saved = kingInCheckSquare;
  for (int flash = 0; flash < 2; flash++) {
    kingInCheckSquare = MCUMAX_SQUARE_INVALID;
    drawSingleSquare(displayFile, displayRank);
    delay(150);
    kingInCheckSquare = saved;
    drawSingleSquare(displayFile, displayRank);
    delay(200);
  }
}

static void reviewShowMove(int moveIdx) {
  rebuildBoardToPly(moveIdx);
  drawBoard();
}

static void animateMove(mcumax_move m, mcumax_piece movingPiece) {
  isAnimating = true;
  bool isWhite = mcumax_is_piece_white(movingPiece);
  bool rot180 = shouldRotatePiece180(isWhite);
  const uint8_t *tex = PIECE_TEX[movingPiece & 0x07];

  if ((movingPiece & 0x07) == MCUMAX_KING &&
      abs((int)(m.to & 0x07) - (int)(m.from & 0x07)) == 2) {
    int rank = m.from >> 4;
    int kFromFile = m.from & 0x07;
    int kToFile = m.to & 0x07;
    mcumax_square rookFrom, rookTo;

    if (kToFile > kFromFile) {
      rookFrom = (mcumax_square)((rank << 4) | ((kFromFile == 4) ? 7 : 3));
      rookTo = (mcumax_square)((rank << 4) | ((kFromFile == 4) ? 5 : 0));
    } else {
      rookFrom = (mcumax_square)((rank << 4) | ((kFromFile == 4) ? 0 : 5));
      rookTo = (mcumax_square)((rank << 4) | ((kFromFile == 4) ? 3 : 7));
    }

    int kFromDF, kFromDR, kToDF, kToDR, rFromDF, rFromDR, rToDF, rToDR;
    if (!squareToDisplay(m.from, kFromDF, kFromDR) ||
        !squareToDisplay(m.to, kToDF, kToDR) ||
        !squareToDisplay(rookFrom, rFromDF, rFromDR) ||
        !squareToDisplay(rookTo, rToDF, rToDR)) {
      isAnimating = false;
      return;
    }

    float kStartX = kFromDF * SQUARE_SIZE, kStartY = kFromDR * SQUARE_SIZE;
    float kEndX = kToDF * SQUARE_SIZE, kEndY = kToDR * SQUARE_SIZE;
    float rStartX = rFromDF * SQUARE_SIZE, rStartY = rFromDR * SQUARE_SIZE;
    float rEndX = rToDF * SQUARE_SIZE, rEndY = rToDR * SQUARE_SIZE;

    int minF = min(min(kFromDF, kToDF), min(rFromDF, rToDF));
    int maxF = max(max(kFromDF, kToDF), max(rFromDF, rToDF));
    int animRank = kFromDR;

    uint32_t kRows[28], rRows[28];
    unpackBitmapRows(tex, kRows);
    unpackBitmapRows(tex_rook, rRows);
    uint16_t kFill = isWhite ? 0xFFFF : 0x0000, kOut = isWhite ? 0x0000 : 0xFFFF;
    uint16_t rFill = isWhite ? 0xFFFF : 0x0000, rOut = isWhite ? 0x0000 : 0xFFFF;

    unsigned long startTime = millis();
    const unsigned long duration = 200;
    unsigned long lastFrame = 0;
    const unsigned long frameInterval = 16;

    while (millis() - startTime < duration) {
      if (millis() - lastFrame >= frameInterval) {
        lastFrame = millis();
        float t = (float)(millis() - startTime) / duration;
        if (t > 1.0f)
          t = 1.0f;
        float easeT = (t < 0.5f) ? (2.0f * t * t) : (1.0f - powf(-2.0f * t + 2.0f, 2.0f) / 2.0f);

        int kCurrX = (int)(kStartX + (kEndX - kStartX) * easeT);
        int kCurrY = (int)(kStartY + (kEndY - kStartY) * easeT);
        int rCurrX = (int)(rStartX + (rEndX - rStartX) * easeT);
        int rCurrY = (int)(rStartY + (rEndY - rStartY) * easeT);

        tft.startWrite();
        for (int f = minF; f <= maxF; f++) {
          drawSingleSquareAnimated(f, animRank, m.from, rookFrom,
                                   kCurrX, kCurrY, kRows, kFill, kOut, rot180,
                                   rCurrX, rCurrY, rRows, rFill, rOut, rot180);
        }
        tft.endWrite();
      }
      yield();
    }

    tft.startWrite();
    for (int f = minF; f <= maxF; f++) {
      drawSingleSquare(f, animRank, false, m.from, rookFrom);
    }
    drawPieceAtPixel((int)kEndX, (int)kEndY, tex, isWhite, rot180);
    drawPieceAtPixel((int)rEndX, (int)rEndY, tex_rook, isWhite, rot180);
    tft.endWrite();
    isAnimating = false;
    return;
  }

  int fromDF, fromDR, toDF, toDR;
  if (!squareToDisplay(m.from, fromDF, fromDR) ||
      !squareToDisplay(m.to, toDF, toDR)) {
    isAnimating = false;
    return;
  }

  float startX = fromDF * SQUARE_SIZE, startY = fromDR * SQUARE_SIZE;
  float endX = toDF * SQUARE_SIZE, endY = toDR * SQUARE_SIZE;

  uint32_t pRows[28];
  unpackBitmapRows(tex, pRows);
  uint16_t fillColor = isWhite ? 0xFFFF : 0x0000;
  uint16_t outColor = isWhite ? 0x0000 : 0xFFFF;

  unsigned long startTime = millis();
  const unsigned long duration = 200;
  unsigned long lastFrame = 0;
  const unsigned long frameInterval = 16;
  int prevMinFile = -1, prevMinRank = -1;

  while (millis() - startTime < duration) {
    if (millis() - lastFrame >= frameInterval) {
      lastFrame = millis();
      float t = (float)(millis() - startTime) / duration;
      if (t > 1.0f)
        t = 1.0f;
      float easeT = (t < 0.5f) ? (2.0f * t * t) : (1.0f - powf(-2.0f * t + 2.0f, 2.0f) / 2.0f);

      int currentX = (int)(startX + (endX - startX) * easeT);
      int currentY = (int)(startY + (endY - startY) * easeT);

      int minFile = currentX / SQUARE_SIZE;
      int minRank = currentY / SQUARE_SIZE;

      tft.startWrite();
      // Xóa vùng 2x2 của khung hình trước nếu bước nhảy rời khỏi ô cũ
      if (prevMinFile != -1) {
        for (int dr = 0; dr <= 1; dr++) {
          for (int df = 0; df <= 1; df++) {
            int pf = prevMinFile + df;
            int pr = prevMinRank + dr;
            if ((unsigned)pf < 8U && (unsigned)pr < 8U &&
                (pf < minFile || pf > minFile + 1 || pr < minRank ||
                 pr > minRank + 1)) {
              drawSingleSquare(pf, pr, false, m.from);
            }
          }
        }
      }
      for (int dr = 0; dr <= 1; dr++) {
        for (int df = 0; df <= 1; df++) {
          int f = minFile + df;
          int r = minRank + dr;
          if ((unsigned)f < 8U && (unsigned)r < 8U) {
            drawSingleSquareAnimated(f, r, m.from, MCUMAX_SQUARE_INVALID,
                                     currentX, currentY, pRows, fillColor, outColor, rot180);
          }
        }
      }
      tft.endWrite();

      prevMinFile = minFile;
      prevMinRank = minRank;
    }
    yield();
  }

  tft.startWrite();
  if (prevMinFile != -1) {
    for (int dr = 0; dr <= 1; dr++) {
      for (int df = 0; df <= 1; df++) {
        int pf = prevMinFile + df;
        int pr = prevMinRank + dr;
        if ((unsigned)pf < 8U && (unsigned)pr < 8U &&
            (pf != toDF || pr != toDR)) {
          drawSingleSquare(pf, pr, false, m.from);
        }
      }
    }
  }
  drawSingleSquare(toDF, toDR, false, m.from);
  drawPieceAtPixel((int)endX, (int)endY, tex, isWhite, rot180);
  tft.endWrite();
  isAnimating = false;
}

static void moveToSAN(mcumax_move m, char *out, size_t outSize) {
  if (outSize == 0)
    return;
  size_t pos = 0;
  auto pushChar = [&](char c) {
    if (pos + 1 < outSize)
      out[pos++] = c;
  };

  if (mcumax_is_castling_move(m)) {
    const char *castle =
        ((m.to & 0x07) > (m.from & 0x07)) ? "O-O" : "O-O-O";
    while (*castle)
      pushChar(*castle++);
    out[pos] = '\0';
    return;
  }

  mcumax_piece pieceFrom = mcumax_get_piece(m.from);
  mcumax_piece pieceTo = mcumax_get_piece(m.to);
  uint8_t type = pieceFrom & 0x07;
  bool isPawnMove =
      (type == MCUMAX_PAWN_UPSTREAM) || (type == MCUMAX_PAWN_DOWNSTREAM);
  bool isCapture =
      mcumax_is_en_passant_move(m) || !mcumax_is_empty_square(pieceTo);

  char pChar = mcumax_get_piece_char(pieceFrom);
  if (pChar != '\0') {
    pushChar(pChar);
    if (!isPawnMove) {
      mcumax_move allMoves[100];
      uint32_t allCount = mcumax_get_legal_moves(allMoves, 100);
      bool needFile = false, needRank = false;
      for (uint32_t i = 0; i < allCount; i++) {
        if (allMoves[i].from != m.from && allMoves[i].to == m.to) {
          mcumax_piece otherPiece = mcumax_get_piece(allMoves[i].from);
          if ((otherPiece & 0x07) == type) {
            if ((allMoves[i].from & 0x07) == (m.from & 0x07))
              needRank = true;
            else
              needFile = true;
          }
        }
      }
      if (needFile)
        pushChar((char)('a' + (m.from & 0x07)));
      if (needRank)
        pushChar((char)('8' - (m.from >> 4)));
    }
  }

  if (isCapture) {
    if (isPawnMove)
      pushChar((char)('a' + (m.from & 0x07)));
    pushChar('x');
  }

  pushChar((char)('a' + (m.to & 0x07)));
  pushChar((char)('8' - (m.to >> 4)));

  if (isPawnMove) {
    int toRank = m.to >> 4;
    if (toRank == 0 || toRank == 7) {
      uint8_t promo = m.promotion_piece ? m.promotion_piece : MCUMAX_QUEEN;
      pushChar('=');
      char promoChar = 'Q';
      if (promo == MCUMAX_ROOK)
        promoChar = 'R';
      else if (promo == MCUMAX_BISHOP)
        promoChar = 'B';
      else if (promo == MCUMAX_KNIGHT)
        promoChar = 'N';
      pushChar(promoChar);
    }
  }

  out[pos] = '\0';
}

static uint8_t getPromotionSelection(bool isWhite) {
  static const uint8_t PROMO_OPTS[4] = {MCUMAX_QUEEN, MCUMAX_ROOK,
                                        MCUMAX_BISHOP, MCUMAX_KNIGHT};
  tft.fillRoundRect(20, 80, 200, 80, 8, CYBER_BG);
  tft.drawRoundRect(20, 80, 200, 80, 8, CYBER_CYAN);
  tft.drawRoundRect(21, 81, 198, 78, 7, CYBER_MAGENTA);

  tft.setTextSize(1);
  tft.setTextColor(CYBER_YELLOW);
  tft.drawString("SELECT PROMOTION", 75, 90);

  bool rot180 = shouldRotatePiece180(isWhite);
  for (int i = 0; i < 4; i++) {
    int ox = 30 + i * 45;
    tft.drawRoundRect(ox, 110, 36, 36, 4, CYBER_CYAN);
    drawPieceAtPixel(ox + 3, 113, PIECE_TEX[PROMO_OPTS[i]], isWhite, rot180);
  }

  while (ts.touched()) {
    yield();
  }
  delay(50);

  while (true) {
    int tx, ty;
    if (getStableTouchPoint(tx, ty) && ty >= 110 && ty <= 146) {
      for (int i = 0; i < 4; i++) {
        int ox = 30 + i * 45;
        if (tx >= ox && tx <= ox + 36) {
          tft.drawRoundRect(ox, 110, 36, 36, 4, CYBER_YELLOW);
          delay(150);
          while (ts.touched()) {
            yield();
          }
          return PROMO_OPTS[i];
        }
      }
    }
    yield();
  }
}

static void executeMove(mcumax_move m) {
  unsigned long animStart = millis();
  mcumax_piece pieceFrom = mcumax_get_piece(m.from);
  uint8_t type = pieceFrom & 0x07;
  bool isPawnMove =
      (type == MCUMAX_PAWN_UPSTREAM) || (type == MCUMAX_PAWN_DOWNSTREAM);
  mcumax_piece pieceTo = mcumax_get_piece(m.to);
  bool isCapture =
      mcumax_is_en_passant_move(m) || !mcumax_is_empty_square(pieceTo);
  uint8_t movingSide = mcumax_get_current_side();

  animateMove(m, pieceFrom);
  unsigned long animCost = millis() - animStart;

  bool isPromotion = isPawnMove && ((m.to >> 4) == 0 || (m.to >> 4) == 7);
  if (isPromotion && !(currentGameMode == MODE_PVB && movingSide == botSide)) {
    m.promotion_piece = getPromotionSelection(mcumax_is_piece_white(pieceFrom));
  }

  if (currentGameMode == MODE_PVP && currentTimeMode != TIME_UNTIMED) {
    unsigned long now = millis();
    unsigned long rawElapsed = now - lastTimerUpdate;
    uint32_t elapsed =
        (rawElapsed > animCost) ? (uint32_t)(rawElapsed - animCost) : 0;
    lastTimerUpdate = now;
    pvpTimerStarted = true;

    if (movingSide == SIDE_WHITE) {
      whiteTimeLeft = (whiteTimeLeft > elapsed) ? (whiteTimeLeft - elapsed) : 0;
      whiteTimeLeft += timeIncrementMs;
      whiteClockFlash = false;
      if (currentMoveCount < MAX_MOVES)
        moveTimeHistory[currentMoveCount] = whiteTimeLeft;
    } else if (movingSide == SIDE_BLACK) {
      blackTimeLeft = (blackTimeLeft > elapsed) ? (blackTimeLeft - elapsed) : 0;
      blackTimeLeft += timeIncrementMs;
      blackClockFlash = false;
      if (currentMoveCount < MAX_MOVES)
        moveTimeHistory[currentMoveCount] = blackTimeLeft;
    }
  }

  char san[8];
  moveToSAN(m, san, sizeof(san));

  Serial.printf(
      "[DEBUG] Move #%d: %s | PawnMove: %s | Capture: %s | 50-Move Clock: %d/100\n",
      currentMoveCount + 1, san, isPawnMove ? "YES" : "NO",
      isCapture ? "YES" : "NO", (int)mcumax_get_halfmove_clock());

  mcumax_play_move(m);

  kingInCheckSquare = mcumax_get_check_square(mcumax_get_current_side());
  if (kingInCheckSquare != MCUMAX_SQUARE_INVALID) {
    size_t len = strlen(san);
    if (len + 1 < sizeof(san)) {
      san[len] = '+';
      san[len + 1] = '\0';
    }
  }

  if (currentMoveCount < MAX_MOVES) {
    moveHistory[currentMoveCount] = {
        m.from, m.to,
        (uint8_t)((m.promotion_piece & 0x07) | ((pieceFrom & 0x1F) << 3))};
    memcpy(moveSANHistory[currentMoveCount], san, sizeof(san));
    currentMoveCount++;
  }
}

// ==================== SIDEBAR & ĐỒNG HỒ ====================
static void drawTextRotated180(const char *text, int x, int y,
                               uint8_t size = 2) {
  tft.setRotation(3);
  tft.setTextSize(size);
  int charW = (size == 1) ? 6 : 12;
  int charH = (size == 1) ? 8 : 16;
  int textWidth = (int)strlen(text) * charW;
  tft.drawString(text, 320 - x - textWidth, 240 - y - charH);
  tft.setRotation(1);
  tft.setTextSize(size);
}

static int getReviewDisplayPly() {
  if (reviewMoveIndex == 0) return -1;
  if (currentGameMode == MODE_PVB) {
    for (int i = reviewMoveIndex - 1; i >= 0; i--) {
      if (isPlyPlayerMove(i))
        return i;
    }
    return -1;
  }
  return reviewMoveIndex - 1;
}

static void drawReviewCalculatingDots(uint8_t dots) {
  if (currentState != STATE_REVIEW)
    return;
  int idx = getReviewDisplayPly();
  if (idx < 0 || idx >= currentMoveCount)
    return;
  if (moveDelta[idx] >= 0)
    return;

  static const char *const CALC_FRAMES[4] = {
      "Calc   ", "Calc.  ", "Calc.. ", "Calc..."};
  tft.setTextColor(CYBER_YELLOW, CYBER_BG);
  tft.setTextSize(1);
  tft.drawString(CALC_FRAMES[dots], 255, 54);
}

static void drawReviewEvalBadge() {
  if (currentState != STATE_REVIEW)
    return;

  tft.fillRect(245, 31, 70, 44, CYBER_BG);

  int idx = getReviewDisplayPly();
  if (idx < 0) {
    tft.drawRoundRect(244, 31, 72, 44, 5, CYBER_CYAN);
    tft.setTextColor(CYBER_GREY, CYBER_BG);
    tft.setTextSize(1);
    tft.drawString("START", 262, 40);
    char pBuf[16];
    snprintf(pBuf, sizeof(pBuf), "0/%d ply", currentMoveCount);
    tft.drawString(pBuf, 252, 54);
    return;
  }

  int16_t delta = moveDelta[idx];
  if (delta < 0) {
    tft.drawRoundRect(244, 31, 72, 44, 5, CYBER_YELLOW);
    tft.setTextColor(CYBER_YELLOW, CYBER_BG);
    tft.setTextSize(1);
    tft.drawString("CALCULATING", 246, 38);
    static const char *const CALC_FRAMES[4] = {
        "Calc   ", "Calc.  ", "Calc.. ", "Calc..."};
    tft.drawString(CALC_FRAMES[calcDots], 255, 54);
    return;
  }

  MoveEvalClass cls = (MoveEvalClass)moveEvalClass[idx];
  if (cls == EVAL_NONE)
    cls = getMoveEvalClass(delta);

  uint16_t color = CYBER_CYAN;
  const char *label = "GREAT";
  const char *sym = "!";
  bool isStar = false;

  switch (cls) {
  case EVAL_BRILLIANT:
    color = 0x03FF; // Xanh lam sáng
    label = "GENIUS";
    sym = "!!";
    break;
  case EVAL_GREAT:
    color = CYBER_CYAN; // Xanh dương cyan
    label = "GREAT";
    sym = "!";
    break;
  case EVAL_GOOD:
    color = 0x07E0; // Xanh lá cây
    label = "GOOD";
    isStar = true;
    break;
  case EVAL_INACCURACY:
    color = CYBER_ORANGE; // Cam
    label = "MISTAKE";
    sym = "?";
    break;
  case EVAL_BLUNDER:
  default:
    color = CYBER_RED; // Đỏ
    label = "BLUNDER";
    sym = "??";
    break;
  }

  tft.drawRoundRect(244, 31, 72, 44, 5, color);

  if (isStar) {
    drawStarIcon(280, 42, color);
  } else {
    tft.fillCircle(strlen(sym) == 1 ? 270 : 264, 43, 3, color);
    tft.setTextColor(color, CYBER_BG);
    tft.setTextSize(2);
    tft.drawString(sym, (strlen(sym) == 1 ? 280 : 274), 36);
  }

  tft.setTextSize(1);
  tft.setTextColor(color, CYBER_BG);
  int labelX = 280 - ((int)strlen(label) * 6) / 2;
  tft.drawString(label, labelX, 57);
}

// Tính độ chính xác của một nước đi dựa trên mô hình CAPS2 (Chess.com / Lichess Win Probability)
// delta: tổn thất centipawn (0 = Best/Brilliant, >= 0)
static float calculateSingleMoveAccuracy(int16_t delta) {
  if (delta <= 0) return 100.0f;

  // 1. Chuyển đổi centipawn loss thành độ sụt giảm xác suất thắng (Win% drop)
  // Công thức Sigmoid: Win% = 50 * (2 / (1 + exp(-0.00368208 * delta)) - 1)
  float deltaWin = 50.0f * (2.0f / (1.0f + expf(-0.00368208f * (float)delta)) - 1.0f);

  // 2. Chuyển đổi Win% drop thành Move Accuracy theo hàm mũ CAPS2
  // Accuracy = 103.1668 * exp(-0.04354 * deltaWin) - 3.1669
  float acc = 103.1668f * expf(-0.04354f * deltaWin) - 3.1669f;

  if (acc < 0.0f) acc = 0.0f;
  if (acc > 100.0f) acc = 100.0f;
  return acc;
}

static inline bool isPlyPlayerMove(int ply) {
  if (ply < 0) return false;
  if (currentGameMode != MODE_PVB) return true; // Trong PvP, tính tất cả các nước
  bool isWhite = ((ply & 1) == 0);
  return (playerSide == SIDE_WHITE) ? isWhite : !isWhite;
}

static float getPlayerAccuracy(int *outCount) {
  uint32_t count = 0;
  float sumAcc = 0.0f;

  for (int i = 0; i < currentMoveCount; i++) {
    if (!isPlyPlayerMove(i))
      continue; // Ở PvE: CHỈ tính nước của người chơi, KHÔNG tính bot!

    int16_t d = moveDelta[i];
    if (d < 0)
      continue; // Nước chưa được tính toán xong

    sumAcc += calculateSingleMoveAccuracy(d);
    count++;
  }

  if (outCount)
    *outCount = count;

  if (count == 0)
    return 0.0f;
  return sumAcc / (float)count;
}

static void drawQuitConfirmModal() {
  tft.fillRoundRect(25, 45, 270, 150, 8, CYBER_BG);
  tft.drawRoundRect(25, 45, 270, 150, 8, CYBER_CYAN);
  tft.drawRoundRect(26, 46, 268, 148, 7, CYBER_MAGENTA);

  tft.setTextSize(2);
  tft.setTextColor(CYBER_YELLOW);
  if (currentState == STATE_REVIEW) {
    tft.drawString("EXIT REVIEW?", 90, 58);
  } else {
    tft.drawString("QUIT GAME?", 98, 58);
  }

  tft.setTextSize(1);
  if (currentState == STATE_REVIEW) {
    char accBuf[48];
    int count = 0;
    float acc = getPlayerAccuracy(&count);
    if (count > 0) {
      if (currentGameMode == MODE_PVB) {
        snprintf(accBuf, sizeof(accBuf), "Player Accuracy: %.1f%% (%d eval)", acc, count);
      } else {
        snprintf(accBuf, sizeof(accBuf), "Game Accuracy: %.1f%% (%d eval)", acc, count);
      }
    } else {
      snprintf(accBuf, sizeof(accBuf), "Accuracy: Calculating...");
    }
    tft.setTextColor(CYBER_CYAN);
    int len = (int)strlen(accBuf) * 6;
    tft.drawString(accBuf, 160 - len / 2, 85);

    tft.setTextColor(CYBER_GREY);
    tft.drawString("Return to main menu?", 100, 105);
  } else {
    tft.setTextColor(CYBER_GREY);
    tft.drawString("Current match progress will be lost.", 55, 88);
    tft.drawString("Are you sure you want to quit?", 75, 104);
  }

  tft.drawRoundRect(45, 135, 100, 38, 6, CYBER_RED);
  tft.setTextSize(2);
  tft.setTextColor(CYBER_RED);
  tft.drawString("YES", 80, 145);

  tft.drawRoundRect(175, 135, 100, 38, 6, CYBER_CYAN);
  tft.setTextColor(CYBER_CYAN);
  tft.drawString("NO", 215, 145);
}

static void drawHistoryStatic() {
  int historyStartY = (currentState == STATE_REVIEW) ? 78 : 88;
  if (currentGameMode == MODE_PVP && currentState != STATE_REVIEW)
    return;

  bool isBotPlaying =
      (currentGameMode == MODE_PVB && currentState == STATE_PLAYING);
  int endY = isBotPlaying ? 210 : 240;
  tft.fillRect(241, historyStartY, 78, endY - historyStartY, CYBER_BG);

  int availableHeight = endY - historyStartY - 5;
  int totalPairs = (currentMoveCount + 1) / 2;

  static int startPair = 0;
  if (totalPairs == 0)
    startPair = 0;

  auto getPairLines = [](int p) -> int {
    if (p >= (currentMoveCount + 1) / 2)
      return 1;
    int wLen = (int)strlen(moveSANHistory[p * 2]);
    int bLen = (p * 2 + 1 < currentMoveCount)
                   ? (int)strlen(moveSANHistory[p * 2 + 1])
                   : 0;
    int wLines = (wLen > 0) ? (wLen + 3) / 4 : 1;
    int bLines = (bLen > 0) ? (bLen + 3) / 4 : 1;
    return (wLines > bLines) ? wLines : bLines;
  };

  if (currentState == STATE_REVIEW) {
    int activePair = (reviewMoveIndex > 0) ? (reviewMoveIndex - 1) / 2 : 0;
    if (activePair < startPair) {
      startPair = activePair;
    } else {
      int yHeight = 0;
      for (int p = startPair; p <= activePair; p++)
        yHeight += getPairLines(p) * 12;
      while (yHeight > availableHeight && startPair < activePair) {
        yHeight -= getPairLines(startPair) * 12;
        startPair++;
      }
    }

    int totalY = 0;
    for (int p = startPair; p < totalPairs; p++)
      totalY += getPairLines(p) * 12;
    while (totalY < availableHeight && startPair > 0) {
      int addedHeight = getPairLines(startPair - 1) * 12;
      if (totalY + addedHeight <= availableHeight) {
        startPair--;
        totalY += addedHeight;
      } else {
        break;
      }
    }
  } else {
    startPair = totalPairs - 1;
    if (startPair < 0) {
      startPair = 0;
    } else {
      int yHeight = getPairLines(startPair) * 12;
      while (startPair > 0) {
        int nextHeight = yHeight + getPairLines(startPair - 1) * 12;
        if (nextHeight <= availableHeight) {
          yHeight = nextHeight;
          startPair--;
        } else {
          break;
        }
      }
    }
  }

  if (startPair < 0)
    startPair = 0;

  int yPos = historyStartY + 5;
  char numBuf[8];
  char chunk[5];

  for (int p = startPair; p < totalPairs; p++) {
    int lines = getPairLines(p);
    if (yPos > historyStartY + availableHeight + 5)
      break;

    int numLen = snprintf(numBuf, sizeof(numBuf), "%d.", p + 1);
    tft.setTextColor(CYBER_HIST_NUM);
    tft.setTextSize(1);
    tft.setCursor(267 - numLen * 6, yPos);
    tft.print(numBuf);

    const char *wSan = moveSANHistory[p * 2];
    tft.setTextColor((currentState == STATE_REVIEW && reviewMoveIndex == p * 2 + 1)
                         ? CYBER_YELLOW
                         : 0xFFFF);
    int wLen = (int)strlen(wSan);
    int wChunks = (wLen > 0) ? (wLen + 3) / 4 : 1;
    for (int l = 0; l < wChunks; l++) {
      if (yPos + l * 12 >= historyStartY + availableHeight)
        break;
      snprintf(chunk, sizeof(chunk), "%.4s", wSan + l * 4);
      tft.setCursor(269, yPos + l * 12);
      tft.print(chunk);
    }

    if (p * 2 + 1 < currentMoveCount) {
      const char *bSan = moveSANHistory[p * 2 + 1];
      tft.setTextColor(
          (currentState == STATE_REVIEW && reviewMoveIndex == p * 2 + 2)
              ? CYBER_YELLOW
              : CYBER_HIST_BLK);
      int bLen = (int)strlen(bSan);
      int bChunks = (bLen > 0) ? (bLen + 3) / 4 : 1;
      for (int l = 0; l < bChunks; l++) {
        if (yPos + l * 12 >= historyStartY + availableHeight)
          break;
        snprintf(chunk, sizeof(chunk), "%.4s", bSan + l * 4);
        tft.setCursor(295, yPos + l * 12);
        tft.print(chunk);
      }
    }
    yPos += lines * 12;
  }
}

static void drawPvPClocks() {
  if (currentTimeMode == TIME_UNTIMED) {
    tft.setTextColor(CYBER_NO_TIME_TXT, CYBER_BG);
    tft.setTextSize(1);
    drawTextRotated180("NO TIME", 248, 35, 1);
    tft.drawString("NO TIME", 248, 195);
    return;
  }

  static int lastWLen = 0, lastBLen = 0;
  uint32_t wTot = whiteTimeLeft / 1000;
  uint32_t bTot = blackTimeLeft / 1000;
  char wBuf[12], bBuf[12];
  int wLen = snprintf(wBuf, sizeof(wBuf), "%02u:%02u", (unsigned)(wTot / 60),
                      (unsigned)(wTot % 60));
  int bLen = snprintf(bBuf, sizeof(bBuf), "%02u:%02u", (unsigned)(bTot / 60),
                      (unsigned)(bTot % 60));

  if (bLen < lastBLen)
    tft.fillRect(242, 34, 76, 18, CYBER_BG);
  if (wLen < lastWLen)
    tft.fillRect(242, 194, 76, 18, CYBER_BG);
  lastBLen = bLen;
  lastWLen = wLen;

  uint16_t bClkColor =
      (blackTimeLeft < 15000 && blackClockFlash) ? CYBER_RED : 0xFFFF;
  tft.setTextColor(bClkColor, CYBER_BG);
  tft.setTextSize(2);
  drawTextRotated180(bBuf, 280 - (bLen * 12) / 2, 35, 2);

  uint16_t wClkColor =
      (whiteTimeLeft < 15000 && whiteClockFlash) ? CYBER_RED : 0xFFFF;
  tft.setTextColor(wClkColor, CYBER_BG);
  tft.setTextSize(2);
  tft.drawString(wBuf, 280 - (wLen * 12) / 2, 195);
}

static void drawSidebar() {
  tft.fillRect(240, 0, 80, 240, CYBER_BG);
  tft.drawFastVLine(240, 0, 240, CYBER_CYAN);

  if (currentState == STATE_REVIEW) {
    tft.drawRoundRect(245, 5, 70, 24, 6, CYBER_CYAN);
    tft.setTextColor(CYBER_CYAN);
    tft.setTextSize(1);
    tft.drawString("MENU", 265, 12);
    drawReviewEvalBadge();
    tft.drawFastHLine(240, 76, 80, CYBER_GREY);
    drawHistoryStatic();
    return;
  }

  char sqStr[3] = {0, 0, 0};
  if (selectedSquare != MCUMAX_SQUARE_INVALID) {
    sqStr[0] = (char)('a' + (selectedSquare & 0x07));
    sqStr[1] = (char)('8' - (selectedSquare >> 4));
  }

  if (currentGameMode == MODE_PVB) {
    tft.drawRoundRect(245, 5, 70, 25, 6, CYBER_CYAN);
    tft.setTextColor(CYBER_CYAN);
    tft.setTextSize(1);
    tft.drawString("MENU", 265, 13);

    tft.drawRoundRect(245, 35, 70, 25, 6, CYBER_MAGENTA);
    tft.setTextColor(CYBER_MAGENTA);
    tft.drawString("UNDO", 265, 43);

    int32_t adv = mcumax_calculate_advantage(
        (playerSide == SIDE_BLACK) ? SIDE_BLACK : SIDE_WHITE);
    char advBuf[8];
    tft.setTextSize(2);
    if (adv > 0) {
      snprintf(advBuf, sizeof(advBuf), "+%d", (int)adv);
      tft.setTextColor(CYBER_CYAN);
    } else if (adv < 0) {
      snprintf(advBuf, sizeof(advBuf), "%d", (int)adv);
      tft.setTextColor(CYBER_MAGENTA);
    } else {
      strcpy(advBuf, "0");
      tft.setTextColor(0xFFFF);
    }
    tft.drawString(advBuf, 245, 65);

    if (sqStr[0]) {
      tft.setTextColor(CYBER_YELLOW);
      tft.drawString(sqStr, 290, 65);
    }

    tft.drawFastHLine(240, 85, 80, CYBER_GREY);
    drawHistoryStatic();
    return;
  }

  tft.drawRoundRect(245, 92, 70, 25, 6, CYBER_CYAN);
  tft.setTextColor(CYBER_CYAN);
  tft.setTextSize(1);
  tft.drawString("MENU", 265, 100);

  tft.drawRoundRect(245, 122, 70, 25, 6, CYBER_MAGENTA);
  tft.setTextColor(CYBER_MAGENTA);
  tft.drawString("UNDO", 265, 130);

  tft.drawFastHLine(240, 85, 80, CYBER_GREY);
  tft.drawFastHLine(240, 155, 80, CYBER_GREY);

  drawPvPClocks();

  if (sqStr[0]) {
    tft.setTextSize(2);
    tft.setTextColor(CYBER_YELLOW);
    if (mcumax_get_current_side() == SIDE_WHITE) {
      tft.drawString(sqStr, 270, 160);
    } else {
      drawTextRotated180(sqStr, 270, 60, 2);
    }
  }
}

// ==================== MENUS ====================
static void drawBackButtonAndTitle(const char *title, int titleX, int titleY) {
  tft.fillScreen(CYBER_BG);
  tft.drawRoundRect(10, 10, 45, 30, 6, CYBER_CYAN);
  tft.setTextSize(2);
  tft.setTextColor(CYBER_CYAN);
  tft.drawString("<--", 18, 17);
  tft.drawString(title, titleX, titleY);
}

static void drawModeMenu() {
  tft.fillScreen(CYBER_BG);
  tft.setTextSize(2);
  tft.setTextColor(CYBER_CYAN);
  tft.drawString("CYBER CHESS", 95, 15);

  tft.drawRoundRect(40, 55, 240, 45, 8, CYBER_CYAN);
  tft.drawRoundRect(39, 54, 242, 47, 9, 0x1802);
  tft.setTextColor(CYBER_CYAN);
  tft.drawString("PLAYER VS BOT", 85, 70);

  tft.drawRoundRect(40, 110, 240, 45, 8, CYBER_MAGENTA);
  tft.drawRoundRect(39, 109, 242, 47, 9, 0x0802);
  tft.setTextColor(CYBER_MAGENTA);
  tft.drawString("PLAYER VS PLAYER", 70, 125);

  tft.drawRoundRect(40, 165, 240, 45, 8, CYBER_ORANGE);
  tft.drawRoundRect(39, 164, 242, 47, 9, 0x0402);
  tft.setTextColor(CYBER_ORANGE);
  tft.drawString("CHESS TIMER", 95, 180);
}

static void drawLevelMenu() {
  drawBackButtonAndTitle("CHOOSE LEVEL", 85, 17);

  tft.drawRoundRect(40, 50, 240, 35, 6, CYBER_CYAN);
  tft.drawString("EASY", 135, 60);
  tft.drawRoundRect(40, 95, 240, 35, 6, CYBER_YELLOW);
  tft.setTextColor(CYBER_YELLOW);
  tft.drawString("MEDIUM", 125, 105);
  tft.drawRoundRect(40, 140, 240, 35, 6, CYBER_MAGENTA);
  tft.setTextColor(CYBER_MAGENTA);
  tft.drawString("HARD", 135, 150);
  tft.fillRoundRect(40, 185, 240, 35, 6, CYBER_MAGENTA);
  tft.setTextColor(CYBER_BG);
  tft.drawString("EXTREME", 115, 195);
}

static void drawColorMenu() {
  drawBackButtonAndTitle("SELECT FACTION", 75, 50);

  tft.drawRoundRect(40, 100, 100, 60, 8, CYBER_CYAN);
  tft.setTextColor(0xFFFF);
  tft.drawString("WHITE", 60, 122);
  tft.drawRoundRect(180, 100, 100, 60, 8, CYBER_MAGENTA);
  tft.setTextColor(CYBER_MAGENTA);
  tft.drawString("BLACK", 200, 122);
}

static void drawTimeMenu() {
  drawBackButtonAndTitle("CHOOSE TIMING", 80, 17);

  int yPos = 50;
  auto addItem = [&](const char *label, int textX, uint16_t borderCol,
                     uint16_t textCol) {
    tft.drawRoundRect(40, yPos, 240, 30, 6, borderCol);
    tft.setTextColor(textCol);
    tft.drawString(label, textX, yPos + 7);
    yPos += 35;
  };

  if (currentGameMode != MODE_TIMER)
    addItem("Unlimited", 100, CYBER_CYAN, 0xFFFF);
  addItem("Blitz 3 + 0s", 90, CYBER_CYAN, 0xFFFF);
  addItem("Blitz 3 + 2s", 90, CYBER_CYAN, 0xFFFF);
  addItem("Rapid 10 + 0s", 85, CYBER_CYAN, 0xFFFF);
  if (currentGameMode == MODE_TIMER)
    addItem("Rapid 15 + 10s", 85, CYBER_CYAN, 0xFFFF);
  addItem("Custom Time", 95, CYBER_YELLOW, CYBER_YELLOW);
}

static void drawCustomSpinnerValue(int boxX, int centerX, int value) {
  tft.fillRect(boxX + 2, 107, 76, 36, CYBER_BG);
  tft.setTextSize(2);
  tft.setTextColor(0xFFFF);
  char buf[8];
  int len = snprintf(buf, sizeof(buf), "%d", value);
  tft.drawString(buf, centerX - (len * 6), 117);
}

static void drawCustomTimeMenu() {
  drawBackButtonAndTitle("CUSTOM TIMER", 80, 17);
  tft.setTextSize(1);
  tft.setTextColor(CYBER_GREY);
  tft.drawString("START MINUTES", 50, 48);
  tft.drawString("INCREMENT SECS", 190, 48);
  tft.setTextSize(2);

  for (int col = 0; col < 2; col++) {
    int bx = (col == 0) ? 50 : 190;
    tft.drawRoundRect(bx, 65, 80, 30, 6, CYBER_CYAN);
    tft.setTextColor(CYBER_CYAN);
    tft.drawString("+", bx + 35, 72);
    tft.drawRoundRect(bx, 105, 80, 40, 6, CYBER_CYAN);
    tft.drawRoundRect(bx, 155, 80, 30, 6, CYBER_CYAN);
    tft.drawString("-", bx + 35, 162);
  }
  drawCustomSpinnerValue(50, 90, customMinutes);
  drawCustomSpinnerValue(190, 230, customIncrement);

  tft.drawRoundRect(50, 200, 220, 35, 6, CYBER_YELLOW);
  tft.setTextColor(CYBER_YELLOW);
  tft.drawString("CONFIRM", 120, 210);
}

static void showGameOverMenuAnimated(const char *msg) {
  const int targetW = 210;
  const int targetH = 180;
  const int centerX = 120;
  const int centerY = 130;
  unsigned long startTime = millis();
  const unsigned long duration = 250;
  int lastW = -1, lastH = -1;

  while (millis() - startTime < duration) {
    float t = (float)(millis() - startTime) / duration;
    if (t > 1.0f)
      t = 1.0f;
    int w = (int)(targetW * t);
    int h = (int)(targetH * t);
    if (w > 16 && h > 16 && (w != lastW || h != lastH)) {
      lastW = w;
      lastH = h;
      int x = centerX - w / 2;
      int y = centerY - h / 2;
      tft.fillRoundRect(x, y, w, h, 8, CYBER_BG);
      tft.drawRoundRect(x, y, w, h, 8, CYBER_CYAN);
      tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, CYBER_MAGENTA);
    }
    yield();
  }

  tft.fillRoundRect(15, 40, 210, 180, 8, CYBER_BG);
  tft.drawRoundRect(15, 40, 210, 180, 8, CYBER_CYAN);
  tft.drawRoundRect(16, 41, 208, 178, 7, CYBER_MAGENTA);

  bool isWin = (strcmp(msg, "YOU WIN!") == 0 ||
                strcmp(msg, "WHITE WINS!") == 0 ||
                strcmp(msg, "BLACK WINS!") == 0);
  tft.setTextSize(3);
  if (isWin) {
    tft.setTextColor(CYBER_CYAN);
  } else if (strcmp(msg, "YOU LOSE!") == 0) {
    tft.setTextColor(CYBER_RED);
  } else {
    tft.setTextColor(CYBER_YELLOW);
  }

  int textX = 40;
  if (strcmp(msg, "DRAW!") == 0)
    textX = 65;
  else if (strcmp(msg, "WHITE WINS!") == 0 || strcmp(msg, "BLACK WINS!") == 0)
    textX = 22;

  tft.drawString(msg, textX, 58);

  if (strcmp(msg, "DRAW!") == 0 && drawReason[0] != '\0') {
    tft.setTextSize(1);
    tft.setTextColor(CYBER_YELLOW);
    int reasonWidth = (int)strlen(drawReason) * 6;
    tft.drawString(drawReason, 120 - (reasonWidth / 2), 92);
  }

  char accBuf[40];
  int count = 0;
  float acc = getPlayerAccuracy(&count);
  if (count > 0) {
    if (currentGameMode == MODE_PVB) {
      snprintf(accBuf, sizeof(accBuf), "Player Acc: %.1f%%", acc);
    } else {
      snprintf(accBuf, sizeof(accBuf), "Game Acc: %.1f%%", acc);
    }
  } else {
    snprintf(accBuf, sizeof(accBuf), "Accuracy: Calc...");
  }
  tft.setTextSize(1);
  tft.setTextColor(CYBER_CYAN);
  int accWidth = (int)strlen(accBuf) * 6;
  tft.drawString(accBuf, 120 - (accWidth / 2), 115);

  tft.drawRoundRect(35, 145, 170, 25, 6, CYBER_CYAN);
  tft.setTextSize(1);
  tft.setTextColor(CYBER_CYAN);
  tft.drawString("MAIN MENU", 85, 153);
  tft.drawRoundRect(35, 180, 170, 25, 6, CYBER_YELLOW);
  tft.setTextColor(CYBER_YELLOW);
  tft.drawString("VIEW MATCH", 85, 188);
}

// ==================== CALLBACK BOT SUY NGHĨ ====================
static void engine_callback(void *userdata) {
  (void)userdata;
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck < 50)
    return;
  lastCheck = now;

  yield();
  if (isSerialDebugActive) {
    if (now - lastThinkingAnim >= 300) {
      lastThinkingAnim = now;
      Serial.print('.');
    }
    return;
  }

  if (!botThinking)
    return;

  if (now - lastThinkingAnim >= 300) {
    lastThinkingAnim = now;
    thinkingDots = (thinkingDots + 1) & 3;
    static const char *const THINKING_FRAMES[4] = {
        "Thinking   ", "Thinking.  ", "Thinking.. ", "Thinking..."};
    tft.setTextColor(CYBER_CYAN, CYBER_BG);
    tft.setTextSize(1);
    tft.drawString(THINKING_FRAMES[thinkingDots], 245, 215);
  }

  int x, y;
  if (getStableTouchPoint(x, y)) {
    touchActive = true;
    if (x >= BOARD_SIZE) {
      if (y > 5 && y < 30) {
        mcumax_stop_search();
        botStopped = true;
        pendingMenuAction = true;
        pendingUndoAction = false;
      } else if (y > 35 && y < 60) {
        mcumax_stop_search();
        botStopped = true;
        pendingUndoAction = true;
        pendingMenuAction = false;
      }
    } else {
      mcumax_square touchedSquare =
          displayToSquare(x / SQUARE_SIZE, y / SQUARE_SIZE);
      if (touchedSquare != selectedSquare) {
        mcumax_square oldSquare = selectedSquare;
        selectedSquare = touchedSquare;
        validMovesCount = 0;

        int df, dr;
        if (squareToDisplay(oldSquare, df, dr))
          drawSingleSquare(df, dr);
        if (squareToDisplay(selectedSquare, df, dr))
          drawSingleSquare(df, dr);

        tft.fillRect(290, 65, 30, 20, CYBER_BG);
        if (selectedSquare != MCUMAX_SQUARE_INVALID) {
          char sqStr[3] = {(char)('a' + (selectedSquare & 0x07)),
                           (char)('8' - (selectedSquare >> 4)), '\0'};
          tft.setTextSize(2);
          tft.setTextColor(CYBER_YELLOW);
          tft.drawString(sqStr, 290, 65);
        }
      }
    }
  }
}

// ==================== CHẾ ĐỘ ĐỒNG HỒ & KHỞI TẠO VÁN CỜ ====================
static uint32_t getInitialTimeMs(uint32_t *outIncrementMs = nullptr) {
  uint32_t initMs = 0, incMs = 0;
  switch (currentTimeMode) {
  case TIME_BLITZ_3_0:
    initMs = 180000;
    break;
  case TIME_BLITZ_3_2:
    initMs = 180000;
    incMs = 2000;
    break;
  case TIME_RAPID_10_0:
    initMs = 600000;
    break;
  case TIME_RAPID_15_10:
    initMs = 900000;
    incMs = 10000;
    break;
  case TIME_CUSTOM:
    initMs = (uint32_t)customMinutes * 60000UL;
    incMs = (uint32_t)customIncrement * 1000UL;
    break;
  default:
    if (currentGameMode == MODE_TIMER)
      initMs = 180000;
    break;
  }
  if (outIncrementMs)
    *outIncrementMs = incMs;
  return initMs;
}

static void formatTimerTime(uint32_t ms, char *out, size_t outSize) {
  uint32_t totalSecs = ms / 1000;
  if (ms < 10000) {
    snprintf(out, outSize, "%u.%u", (unsigned)(totalSecs % 60),
             (unsigned)((ms % 1000) / 100));
  } else {
    snprintf(out, outSize, "%02u:%02u", (unsigned)(totalSecs / 60),
             (unsigned)(totalSecs % 60));
  }
}

static void updateTimerDisplay(bool forceRedraw) {
  static char lastWhiteStr[12] = "";
  static char lastBlackStr[12] = "";
  static bool lastActiveWhite = false;
  static bool lastPaused = false;

  char whiteStr[12], blackStr[12];
  formatTimerTime(whiteTimeLeft, whiteStr, sizeof(whiteStr));
  formatTimerTime(blackTimeLeft, blackStr, sizeof(blackStr));

  bool stateChanged =
      forceRedraw || (timerActiveWhite != lastActiveWhite) ||
      (timerPaused != lastPaused);
  bool whiteChanged = stateChanged || (strcmp(whiteStr, lastWhiteStr) != 0);
  bool blackChanged = stateChanged || (strcmp(blackStr, lastBlackStr) != 0);

  if (!whiteChanged && !blackChanged)
    return;

  if (whiteChanged) {
    tft.setRotation(1);
    tft.fillRect(0, 120, 320, 120, CYBER_LIGHT_SQ);
    if (timerActiveWhite && !timerPaused) {
      tft.drawRect(2, 122, 316, 116, CYBER_MAGENTA);
      tft.drawRect(3, 123, 314, 114, CYBER_MAGENTA);
    }
    tft.setTextSize(5);
    int tw = (int)strlen(whiteStr) * 30;
    tft.setTextColor(CYBER_DARK_SQ);
    tft.drawString(whiteStr, 160 - tw / 2, 160);
    strcpy(lastWhiteStr, whiteStr);
  }

  if (blackChanged) {
    tft.setRotation(3);
    tft.fillRect(0, 120, 320, 120, CYBER_DARK_SQ);
    if (!timerActiveWhite && !timerPaused) {
      tft.drawRect(2, 122, 316, 116, CYBER_CYAN);
      tft.drawRect(3, 123, 314, 114, CYBER_CYAN);
    }
    tft.setTextSize(5);
    int tw = (int)strlen(blackStr) * 30;
    tft.setTextColor(CYBER_LIGHT_SQ);
    tft.drawString(blackStr, 160 - tw / 2, 160);
    strcpy(lastBlackStr, blackStr);
  }

  tft.setRotation(1);
  tft.drawFastHLine(0, 119, 320, CYBER_BG);
  tft.drawFastHLine(0, 120, 320, CYBER_BG);

  tft.fillRoundRect(272, 106, 46, 28, 5, CYBER_YELLOW);
  tft.setTextColor(CYBER_BG);
  tft.setTextSize(1);
  tft.drawString(timerPaused ? "PLAY" : "PAUS", 283, 116);

  if (timerPaused && timerOnlyStarted && whiteTimeLeft > 0 &&
      blackTimeLeft > 0) {
    tft.fillRoundRect(50, 40, 220, 160, 8, CYBER_BG);
    tft.drawRoundRect(50, 40, 220, 160, 8, CYBER_CYAN);
    tft.drawRoundRect(51, 41, 218, 158, 7, CYBER_MAGENTA);

    tft.setTextSize(2);
    tft.setTextColor(CYBER_YELLOW);
    tft.drawString("PAUSED", 120, 55);

    tft.drawRoundRect(70, 95, 180, 35, 6, CYBER_CYAN);
    tft.setTextColor(CYBER_CYAN);
    tft.drawString("RESUME", 125, 105);

    tft.drawRoundRect(70, 145, 180, 35, 6, CYBER_MAGENTA);
    tft.setTextColor(CYBER_MAGENTA);
    tft.drawString("QUIT TIMER", 105, 155);
  }

  lastActiveWhite = timerActiveWhite;
  lastPaused = timerPaused;
}

static void startTimerOnly() {
  whiteTimeLeft = blackTimeLeft = getInitialTimeMs(&timeIncrementMs);
  currentState = STATE_TIMER_RUNNING;
  timerActiveWhite = true;
  timerPaused = true;
  timerOnlyStarted = false;
  lastTimerUpdate = millis();
  updateTimerDisplay(true);
}

static void drawFlagFallOverlay(bool whiteFallen) {
  tft.setRotation(1);
  tft.fillRoundRect(40, 50, 240, 140, 8, CYBER_BG);
  tft.drawRoundRect(40, 50, 240, 140, 8, CYBER_CYAN);
  tft.drawRoundRect(41, 51, 238, 138, 7, CYBER_MAGENTA);

  tft.setTextSize(2);
  tft.setTextColor(CYBER_RED);
  tft.drawString("TIME OUT!", 110, 65);

  tft.setTextColor(0xFFFF);
  tft.drawString(whiteFallen ? "BLACK WINS!" : "WHITE WINS!", 100, 105);

  tft.drawRoundRect(60, 140, 200, 35, 6, CYBER_CYAN);
  tft.setTextColor(CYBER_CYAN);
  tft.drawString("MAIN MENU", 110, 150);
}

static void startGame() {
  mcumax_stop_search_core(0);
  mcumax_init();
  currentMoveCount = 0;
  for (int i = 0; i < MAX_MOVES; i++) {
    moveDelta[i] = -1;
    moveEvalClass[i] = EVAL_NONE;
  }
  selectedSquare = MCUMAX_SQUARE_INVALID;
  validMovesCount = 0;
  kingInCheckSquare = MCUMAX_SQUARE_INVALID;
  isCheckmateFlag = isStalemateFlag = false;
  specialKingSquare = MCUMAX_SQUARE_INVALID;
  drawReason = "";
  pvpTimerStarted = false;
  pendingMenuAction = false;
  pendingUndoAction = false;
  showQuitConfirm = false;

  whiteTimeLeft = blackTimeLeft = getInitialTimeMs(&timeIncrementMs);
  whiteClockFlash = blackClockFlash = false;
  lastTimerUpdate = millis();

  currentState = STATE_PLAYING;
  tft.fillScreen(CYBER_BG);
  drawBoard();
  drawSidebar();
  Serial.println("[SYSTEM] New Game Started! Board Initialized.");
}

static void undoMove() {
  mcumax_stop_search_core(0);
  // Trong PvB: nếu nhấn UNDO khi BOT đang nghĩ (BOT chưa kịp đi), chỉ lùi 1 ply
  // để trả lại đúng lượt của người chơi; nếu đang ở lượt người chơi thì lùi 2 ply.
  int movesToUndo = 1;
  if (currentGameMode == MODE_PVB && mcumax_get_current_side() != botSide)
    movesToUndo = 2;

  int minRemaining =
      (currentGameMode == MODE_PVB && playerSide == SIDE_BLACK) ? 1 : 0;
  if (currentMoveCount - movesToUndo < minRemaining)
    return;

  for (int i = 0; i < movesToUndo; i++) {
    int idx = currentMoveCount - 1 - i;
    if (i > 0) {
      rebuildBoardToPly(idx + 1);
      drawBoard();
    }
    mcumax_move m = getHistoryMove(idx);
    mcumax_move revMove = {m.to, m.from, 0};
    animateMove(revMove, getHistoryPiece(idx));
  }

  int targetIdx = currentMoveCount - movesToUndo;
  if (currentGameMode == MODE_PVP && currentTimeMode != TIME_UNTIMED) {
    uint32_t initMs = getInitialTimeMs();
    if (targetIdx == 0) {
      whiteTimeLeft = blackTimeLeft = initMs;
      pvpTimerStarted = false;
    } else if (targetIdx == 1) {
      whiteTimeLeft = moveTimeHistory[0];
      blackTimeLeft = initMs;
    } else if (((targetIdx - 1) & 1) == 0) {
      whiteTimeLeft = moveTimeHistory[targetIdx - 1];
      blackTimeLeft = moveTimeHistory[targetIdx - 2];
    } else {
      blackTimeLeft = moveTimeHistory[targetIdx - 1];
      whiteTimeLeft = moveTimeHistory[targetIdx - 2];
    }
    whiteClockFlash = blackClockFlash = false;
  }

  for (int i = targetIdx; i < currentMoveCount; i++) {
    moveDelta[i] = -1;
    moveEvalClass[i] = EVAL_NONE;
  }

  currentMoveCount = targetIdx;
  rebuildBoardToPly(currentMoveCount);
  selectedSquare = MCUMAX_SQUARE_INVALID;
  validMovesCount = 0;

  drawBoard();
  drawSidebar();
  Serial.printf("[SYSTEM] Undo Successful. HalfMove Clock rolled back to: %d\n",
                (int)mcumax_get_halfmove_clock());
}

static void goToRootMenu() {
  mcumax_stop_search_core(0);
  selectedSquare = MCUMAX_SQUARE_INVALID;
  validMovesCount = 0;
  showQuitConfirm = false;
  currentState = STATE_MENU_MODE;
  drawModeMenu();
}

static void finishPostMoveFlow() {
  drawBoard();
  checkGameState();
  drawSidebar();

  if (currentState == STATE_GAME_OVER) {
    if (isCheckmateFlag || isStalemateFlag) {
      animateKingRotation(specialKingSquare, 0.0f, 90.0f);
      animateCheckmateSymbol(specialKingSquare, isStalemateFlag ? '=' : '#');
    }
    delay(750);
    showGameOverMenuAnimated(gameOverMsg);
  } else {
    checkAndFlashKing();
  }
}

// ==================== SERIAL FEN DEBUG ====================
struct MoveStat {
  mcumax_move move;
  uint16_t count;
  char label[18];
};

static void trimInPlace(char *s) {
  size_t len = strlen(s);
  while (len > 0 && isspace((unsigned char)s[len - 1]))
    s[--len] = '\0';
  size_t start = 0;
  while (s[start] && isspace((unsigned char)s[start]))
    start++;
  if (start > 0)
    memmove(s, s + start, len - start + 1);
}

static void restoreGamePositionAfterDebug() {
  if (currentState == STATE_PLAYING || currentState == STATE_GAME_OVER) {
    rebuildBoardToPly(currentMoveCount);
  } else if (currentState == STATE_REVIEW) {
    rebuildBoardToPly(reviewMoveIndex);
  }
}

static void handleSerialDebug() {
  if (Serial.available() <= 0)
    return;

  char line[160];
  size_t n = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
  line[n] = '\0';
  trimInPlace(line);

  char *cmd = nullptr;
  if (strncmp(line, "FEN ", 4) == 0)
    cmd = line + 4;
  else if (strncmp(line, "DEBUG_FEN ", 10) == 0)
    cmd = line + 10;
  else
    return;

  trimInPlace(cmd);

  int repeatCount = 1;
  const BotLevelConfig *lvl = &BOT_LEVELS[1]; // mặc định: medium

  for (int step = 0; step < 2; step++) {
    trimInPlace(cmd);
    char *lastSpace = strrchr(cmd, ' ');
    if (!lastSpace)
      break;
    const char *token = lastSpace + 1;
    bool matchedLevel = false;
    for (int i = 0; i < 4; i++) {
      if (strcasecmp(token, BOT_LEVELS[i].name) == 0) {
        lvl = &BOT_LEVELS[i];
        *lastSpace = '\0';
        matchedLevel = true;
        break;
      }
    }
    if (matchedLevel)
      continue;

    bool isNumeric = (*token != '\0');
    for (const char *p = token; *p; p++) {
      if (!isdigit((unsigned char)*p)) {
        isNumeric = false;
        break;
      }
    }
    if (isNumeric) {
      repeatCount = atoi(token);
      if (repeatCount < 1)
        repeatCount = 1;
      *lastSpace = '\0';
    } else {
      break;
    }
  }
  trimInPlace(cmd);

  Serial.println("\n==================================");
  Serial.println("FEN DEBUG MODE ACTIVATED!");
  Serial.printf("FEN string: %s\n", cmd);
  Serial.printf("Level config: %s (Nodes: %u, Depth: %u)\n", lvl->name,
                (unsigned)lvl->nodes, (unsigned)lvl->depth);
  Serial.printf("Repeat count: %d\n", repeatCount);

  isSerialDebugActive = true;
  bool prevProb = mcumax_get_probabilistic_selection();
  mcumax_set_probabilistic_selection(lvl->probabilistic);

  MoveStat stats[100];
  int uniqueMovesCount = 0;
  uint32_t totalNodesUsed = 0;

  for (int run = 0; run < repeatCount; run++) {
    mcumax_set_fen_position(cmd);

    uint8_t botTurnSide = mcumax_get_current_side();
    if (run == 0) {
      Serial.printf("Current Side to Move: %s\n",
                    (botTurnSide == SIDE_WHITE) ? "WHITE" : "BLACK");

      mcumax_game_state_info_t info = mcumax_evaluate_game_state();
      if (info.state != MCUMAX_GAME_ACTIVE) {
        switch (info.state) {
        case MCUMAX_GAME_CHECKMATE:
          Serial.printf("Result: CHECKMATE. %s\n",
                        (botTurnSide == SIDE_WHITE) ? "BLACK WINS!"
                                                    : "WHITE WINS!");
          break;
        case MCUMAX_GAME_STALEMATE:
          Serial.println("Result: STALEMATE. DRAW!");
          break;
        case MCUMAX_GAME_DRAW_ONLY_KINGS:
          Serial.println("Result: DRAW! (ONLY KINGS LEFT)");
          break;
        case MCUMAX_GAME_DRAW_SAME_COLOR_BISHOPS:
          Serial.println("Result: DRAW! (SAME COLOR BISHOPS)");
          break;
        case MCUMAX_GAME_DRAW_FIFTY_MOVE:
          Serial.println("Result: DRAW! (50-MOVE RULE)");
          break;
        default:
          Serial.println("Result: DRAW! (INSUFFICIENT MATERIAL)");
          break;
        }
        Serial.println("==================================");
        mcumax_set_probabilistic_selection(prevProb);
        isSerialDebugActive = false;
        restoreGamePositionAfterDebug();
        return;
      }

      Serial.print((repeatCount > 1) ? "Processing searches" : "Thinking");
      lastThinkingAnim = millis();
    }

    mcumax_move botMove = mcumax_search_best_move(lvl->nodes, lvl->depth);
    totalNodesUsed += mcumax_get_last_node_count();

    if (repeatCount > 1 && run % (repeatCount / 10 + 1) == 0)
      Serial.print('.');

    if (botMove.from != MCUMAX_SQUARE_INVALID &&
        botMove.to != MCUMAX_SQUARE_INVALID) {
      char san[8];
      moveToSAN(botMove, san, sizeof(san));
      mcumax_play_move(botMove);
      if (mcumax_get_check_square(mcumax_get_current_side()) !=
          MCUMAX_SQUARE_INVALID) {
        size_t slen = strlen(san);
        if (slen + 1 < sizeof(san)) {
          san[slen] = '+';
          san[slen + 1] = '\0';
        }
      }

      bool found = false;
      for (int s = 0; s < uniqueMovesCount; s++) {
        if (stats[s].move.from == botMove.from &&
            stats[s].move.to == botMove.to &&
            stats[s].move.promotion_piece == botMove.promotion_piece) {
          stats[s].count++;
          found = true;
          break;
        }
      }
      if (!found && uniqueMovesCount < 100) {
        stats[uniqueMovesCount].move = botMove;
        stats[uniqueMovesCount].count = 1;
        snprintf(stats[uniqueMovesCount].label,
                 sizeof(stats[uniqueMovesCount].label), "%c%c%c%c (%s)",
                 (char)('a' + (botMove.from & 0x07)),
                 (char)('8' - (botMove.from >> 4)),
                 (char)('a' + (botMove.to & 0x07)),
                 (char)('8' - (botMove.to >> 4)), san);
        uniqueMovesCount++;
      }
    } else {
      bool found = false;
      for (int s = 0; s < uniqueMovesCount; s++) {
        if (stats[s].move.from == MCUMAX_SQUARE_INVALID) {
          stats[s].count++;
          found = true;
          break;
        }
      }
      if (!found && uniqueMovesCount < 100) {
        stats[uniqueMovesCount].move = MCUMAX_MOVE_INVALID;
        stats[uniqueMovesCount].count = 1;
        strcpy(stats[uniqueMovesCount].label, "No legal moves");
        uniqueMovesCount++;
      }
    }
    yield();
  }

  Serial.println("\n\n--- SEARCH STATISTICS ---");
  Serial.printf("Total nodes used across all runs: %u\n",
                (unsigned)totalNodesUsed);
  Serial.printf("Average nodes per run: %u\n",
                (unsigned)(totalNodesUsed / (uint32_t)repeatCount));

  if (repeatCount == 1) {
    mcumax_move evalMoves[100];
    int32_t evalScores[100];
    float evalPercentages[100];
    uint32_t evalCount = mcumax_get_last_move_evaluations(
        evalMoves, evalScores, evalPercentages, 100);
    if (evalCount > 0) {
      Serial.println(!lvl->probabilistic
                         ? "Evaluated best move (Extreme mode - deterministic):"
                         : "Candidate moves (scores & calculated probabilities):");
      for (uint32_t k = 0; k < evalCount; k++) {
        if (evalScores[k] >= 0 || evalPercentages[k] > 0.0f) {
          Serial.printf("  * %c%c%c%c: Score = %+d cp | Probability = %.2f%%\n",
                        (char)('a' + (evalMoves[k].from & 0x07)),
                        (char)('8' - (evalMoves[k].from >> 4)),
                        (char)('a' + (evalMoves[k].to & 0x07)),
                        (char)('8' - (evalMoves[k].to >> 4)),
                        (int)evalScores[k], evalPercentages[k]);
        }
      }
    }
  }

  Serial.println("Distribution of returned moves:");
  for (int s = 0; s < uniqueMovesCount; s++) {
    Serial.printf("  * %s: %u times (%.1f%%)\n", stats[s].label,
                  (unsigned)stats[s].count,
                  ((float)stats[s].count / repeatCount) * 100.0f);
  }
  Serial.println("==================================");
  mcumax_set_probabilistic_selection(prevProb);
  isSerialDebugActive = false;
  restoreGamePositionAfterDebug();
}

// ==================== TÍNH TOÁN ĐÁNH GIÁ CHẠY NGẦM TRÊN CORE 0 ====================
static void bgEvalCallback(void *userdata) {
  (void)userdata;
  static unsigned long lastYield = 0;
  unsigned long now = millis();
  if (now - lastYield >= 50) {
    lastYield = now;
    vTaskDelay(1); // Cho phép FreeRTOS scheduler & IDLE0 task chạy để reset watchdog
  }
}

static void bgEvalTask(void *pvParameters) {
  (void)pvParameters;
  mcumax_init();
  mcumax_set_opening_book((const mcumax_opening_entry_t *)opening_book, OPENING_BOOK_ENTRIES);
  mcumax_set_callback(bgEvalCallback, NULL);

  while (bgEvalCore0Running) {
    while (isAnimating) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (isSerialDebugActive || currentMoveCount == 0 ||
        (currentState != STATE_PLAYING && currentState != STATE_REVIEW && currentState != STATE_GAME_OVER)) {
      bgEvalCurrentPly = -1;
      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }

    if (currentGameMode == MODE_PVB && botThinking) {
      bgEvalCurrentPly = -1;
      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }

    int targetPly = -1;

    // Ưu tiên 1: Nước đang xem ở Review mode
    if (currentState == STATE_REVIEW) {
      int curPly = getReviewDisplayPly();
      if (curPly >= 0 && isPlyPlayerMove(curPly) && moveDelta[curPly] < 0) {
        targetPly = curPly;
      }
    }

    // Ưu tiên 2: Tìm nước chưa tính từ đầu ván
    if (targetPly == -1) {
      int count = currentMoveCount;
      for (int i = 0; i < count; i++) {
        if (moveDelta[i] >= 0)
          continue;
        if (!isPlyPlayerMove(i))
          continue; // Ở PvE: CHỈ tính nước của người chơi, KHÔNG tính bot!
        targetPly = i;
        break;
      }
    }

    if (targetPly == -1) {
      bgEvalCurrentPly = -1;
      vTaskDelay(pdMS_TO_TICKS(40));
      continue;
    }

    bgEvalCurrentPly = targetPly;

    // Rebuild thế cờ trên instance Core 0 độc lập hoàn toàn với Core 1
    mcumax_init();
    for (int i = 0; i < targetPly; i++) {
      mcumax_play_move(getHistoryMove(i));
    }

    mcumax_move playedMove = getHistoryMove(targetPly);

    // Kiểm tra sách khai cuộc trong 0 ms
    if (mcumax_is_opening_move(playedMove)) {
      moveDelta[targetPly] = 0;
      moveEvalClass[targetPly] = EVAL_GOOD;
      bgEvalCurrentPly = -1;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    bool prevProb = mcumax_get_probabilistic_selection();
    mcumax_set_probabilistic_selection(false);
    mcumax_set_opening_book(NULL, 0);

    // Chạy tìm kiếm ở cấp độ đánh giá (nodes = 100000, depth = 10)
    mcumax_move bestMove = mcumax_search_best_move(100000, 10);

    mcumax_set_opening_book((const mcumax_opening_entry_t *)opening_book, OPENING_BOOK_ENTRIES);
    mcumax_set_probabilistic_selection(prevProb);

    mcumax_move currentTargetMove = (targetPly < currentMoveCount) ? getHistoryMove(targetPly) : MCUMAX_MOVE_INVALID;
    if (targetPly >= currentMoveCount ||
        currentTargetMove.from != playedMove.from ||
        currentTargetMove.to != playedMove.to) {
      bgEvalCurrentPly = -1;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    int32_t delta = 0;
    mcumax_move rootMoves[100];
    int32_t rootScores[100];
    uint32_t rootCount = mcumax_get_last_move_evaluations(rootMoves, rootScores, nullptr, 100);

    int32_t maxScore = -999999;
    int32_t playedScore = -999999;
    int32_t secondBestScore = -999999;

    for (uint32_t i = 0; i < rootCount; i++) {
      if (rootScores[i] > maxScore) {
        secondBestScore = maxScore;
        maxScore = rootScores[i];
      } else if (rootScores[i] > secondBestScore) {
        secondBestScore = rootScores[i];
      }
      if (rootMoves[i].from == playedMove.from && rootMoves[i].to == playedMove.to) {
        playedScore = rootScores[i];
      }
    }

    uint8_t side = (targetPly & 1) ? SIDE_BLACK : SIDE_WHITE;
    MoveEvalClass evalClass = EVAL_GOOD;

    if (playedMove.from == bestMove.from && playedMove.to == bestMove.to) {
      delta = 0;
      bool isSac = isSacrificeMove(playedMove, side, maxScore);
      if (isSac) {
        evalClass = EVAL_BRILLIANT;
      } else if (rootCount > 1 && secondBestScore > -5000 && (secondBestScore <= maxScore - 120)) {
        evalClass = EVAL_GREAT;
      } else {
        evalClass = EVAL_GOOD;
      }
    } else {
      if (playedScore > -5000) {
        delta = maxScore - playedScore;
      } else {
        // Nước cờ bị cắt tỉa ở root: đánh giá thế cờ sau khi người chơi đi
        mcumax_play_move(playedMove);
        mcumax_set_probabilistic_selection(false);
        mcumax_set_opening_book(NULL, 0);
        mcumax_search_best_move(10000, 4);
        mcumax_set_opening_book((const mcumax_opening_entry_t *)opening_book, OPENING_BOOK_ENTRIES);
        mcumax_set_probabilistic_selection(prevProb);

        mcumax_move oppRootMoves[100];
        int32_t oppScores[100];
        uint32_t oppCount = mcumax_get_last_move_evaluations(oppRootMoves, oppScores, nullptr, 100);
        int32_t oppMax = 0;
        for (uint32_t j = 0; j < oppCount; j++) {
          if (oppScores[j] > oppMax) oppMax = oppScores[j];
        }
        int32_t playerEvalAfter = -oppMax;
        delta = maxScore - playerEvalAfter;
      }

      if (delta < 0) delta = 0;
      if (delta > 2000) delta = 2000;

      if (delta <= 50) {
        evalClass = EVAL_GOOD;
      } else if (delta < 200) {
        evalClass = EVAL_INACCURACY;
      } else {
        evalClass = EVAL_BLUNDER;
      }
    }

    moveDelta[targetPly] = (int16_t)delta;
    moveEvalClass[targetPly] = (uint8_t)evalClass;
    bgEvalCurrentPly = -1;

    vTaskDelay(pdMS_TO_TICKS(10));
  }

  vTaskDelete(NULL);
}

// ==================== XỬ LÝ CẢM ỨNG CHÍNH ====================
static void selectSquareForPlayer(mcumax_square sq) {
  selectedSquare = sq;
  mcumax_piece p = mcumax_get_piece(sq);
  if (mcumax_is_piece_side(p, getInteractiveSide())) {
    validMovesCount =
        mcumax_get_legal_moves_for_square(sq, validMoves, 32);
    if (currentGameMode == MODE_PVP && currentTimeMode != TIME_UNTIMED &&
        !pvpTimerStarted) {
      pvpTimerStarted = true;
      lastTimerUpdate = millis();
    }
  } else {
    validMovesCount = 0;
  }
  drawBoard();
  drawSidebar();
}

static void handleTouch(int x, int y) {
  if (showQuitConfirm) {
    if (x >= 45 && x <= 145 && y >= 135 && y <= 175) {
      showQuitConfirm = false;
      goToRootMenu();
      return;
    }
    if (x >= 175 && x <= 275 && y >= 135 && y <= 175) {
      showQuitConfirm = false;
      drawBoard();
      drawSidebar();
      if (currentState == STATE_REVIEW) {
        drawReviewEvalBadge();
      }
      return;
    }
    return;
  }

  if (currentState == STATE_TIMER_RUNNING) {
    if (whiteTimeLeft == 0 || blackTimeLeft == 0) {
      if (x > 60 && x < 260 && y > 140 && y < 175)
        goToRootMenu();
      return;
    }

    if (!timerOnlyStarted) {
      timerOnlyStarted = true;
      timerPaused = false;
      lastTimerUpdate = millis();
      updateTimerDisplay(true);
      return;
    }

    if (x >= 270 && x <= 320 && y >= 104 && y <= 136) {
      timerPaused = !timerPaused;
      lastTimerUpdate = millis();
      updateTimerDisplay(true);
      return;
    }

    if (timerPaused) {
      if (x > 70 && x < 250 && y > 145 && y < 180) {
        goToRootMenu();
        return;
      }
      timerPaused = false;
      lastTimerUpdate = millis();
      updateTimerDisplay(true);
      return;
    }

    if (timerActiveWhite) {
      whiteTimeLeft += timeIncrementMs;
      timerActiveWhite = false;
    } else {
      blackTimeLeft += timeIncrementMs;
      timerActiveWhite = true;
    }
    lastTimerUpdate = millis();
    updateTimerDisplay(true);
    return;
  }

  if (currentState == STATE_GAME_OVER) {
    if (x > 35 && x < 205 && y > 145 && y < 170) {
      goToRootMenu();
    } else if (x > 35 && x < 205 && y > 180 && y < 205) {
      currentState = STATE_REVIEW;
      reviewMoveIndex = currentMoveCount;
      selectedSquare = MCUMAX_SQUARE_INVALID;
      validMovesCount = 0;
      reviewShowMove(reviewMoveIndex);
      drawSidebar();
      drawReviewEvalBadge();
    }
    return;
  }

  if (currentState == STATE_REVIEW) {
    if (x >= BOARD_SIZE) {
      if (y > 5 && y < 30) {
        showQuitConfirm = true;
        drawQuitConfirmModal();
      }
      return;
    }

    if (x < 120) {
      if (currentGameMode == MODE_PVB) {
        if (reviewMoveIndex > 0) {
          bool wasMated = (reviewMoveIndex == currentMoveCount) &&
                          (isCheckmateFlag || isStalemateFlag);
          mcumax_square mateKingSq = mcumax_find_king(mcumax_get_current_side());
          if (wasMated) {
            animateKingRotation(mateKingSq, 90.0f, 0.0f);
            specialKingSquare = MCUMAX_SQUARE_INVALID;
          }

          int stepsDone = 0;
          int maxSteps = (reviewMoveIndex % 2 == 0) ? 2 : 1;
          while (stepsDone < maxSteps && reviewMoveIndex > 0) {
            int prevIdx = reviewMoveIndex - 1;
            mcumax_move m = getHistoryMove(prevIdx);
            mcumax_move revMove = {m.to, m.from, 0};
            animateMove(revMove, getHistoryPiece(prevIdx));
            reviewMoveIndex--;
            rebuildBoardToPly(reviewMoveIndex);
            drawBoard();
            stepsDone++;
            if (stepsDone < maxSteps && reviewMoveIndex > 0) {
              delay(50);
            }
          }
          drawSidebar();
          drawReviewEvalBadge();
        }
      } else {
        if (reviewMoveIndex > 0) {
          bool wasMated = (reviewMoveIndex == currentMoveCount) &&
                          (isCheckmateFlag || isStalemateFlag);
          mcumax_square mateKingSq = mcumax_find_king(mcumax_get_current_side());
          if (wasMated) {
            animateKingRotation(mateKingSq, 90.0f, 0.0f);
            specialKingSquare = MCUMAX_SQUARE_INVALID;
          }

          int prevIdx = reviewMoveIndex - 1;
          mcumax_move m = getHistoryMove(prevIdx);
          mcumax_move revMove = {m.to, m.from, 0};
          animateMove(revMove, getHistoryPiece(prevIdx));
          reviewMoveIndex--;
          rebuildBoardToPly(reviewMoveIndex);
          drawBoard();
          drawSidebar();
          drawReviewEvalBadge();
        }
      }
    } else {
      if (currentGameMode == MODE_PVB) {
        int stepsDone = 0;
        int maxSteps = (reviewMoveIndex % 2 == 0) ? 2 : 1;
        while (stepsDone < maxSteps && reviewMoveIndex < currentMoveCount) {
          int nextIdx = reviewMoveIndex;
          animateMove(getHistoryMove(nextIdx), getHistoryPiece(nextIdx));
          reviewMoveIndex++;
          rebuildBoardToPly(reviewMoveIndex);
          drawBoard();
          stepsDone++;
          if (stepsDone < maxSteps && reviewMoveIndex < currentMoveCount) {
            delay(50);
          }
        }
        drawSidebar();
        drawReviewEvalBadge();

        if (reviewMoveIndex == currentMoveCount &&
            (isCheckmateFlag || isStalemateFlag)) {
          mcumax_square mateKingSq = mcumax_find_king(mcumax_get_current_side());
          animateKingRotation(mateKingSq, 0.0f, 90.0f);
          animateCheckmateSymbol(mateKingSq, isStalemateFlag ? '=' : '#');
        }
      } else {
        if (reviewMoveIndex < currentMoveCount) {
          int nextIdx = reviewMoveIndex++;
          animateMove(getHistoryMove(nextIdx), getHistoryPiece(nextIdx));
          rebuildBoardToPly(reviewMoveIndex);
          drawBoard();
          drawSidebar();
          drawReviewEvalBadge();

          if (reviewMoveIndex == currentMoveCount &&
              (isCheckmateFlag || isStalemateFlag)) {
            mcumax_square mateKingSq = mcumax_find_king(mcumax_get_current_side());
            animateKingRotation(mateKingSq, 0.0f, 90.0f);
            animateCheckmateSymbol(mateKingSq, isStalemateFlag ? '=' : '#');
          }
        }
      }
    }
    return;
  }

  if (currentState == STATE_MENU_MODE) {
    if (x > 40 && x < 280) {
      if (y > 55 && y < 100) {
        currentGameMode = MODE_PVB;
        currentState = STATE_MENU_LEVEL;
        drawLevelMenu();
      } else if (y > 110 && y < 155) {
        currentGameMode = MODE_PVP;
        currentState = STATE_MENU_TIME;
        drawTimeMenu();
      } else if (y > 165 && y < 210) {
        currentGameMode = MODE_TIMER;
        currentState = STATE_MENU_TIME;
        drawTimeMenu();
      }
    }
    return;
  }

  if (currentState == STATE_MENU_TIME) {
    if (x > 10 && x < 55 && y > 10 && y < 40) {
      goToRootMenu();
      return;
    }
    if (x <= 40 || x >= 280)
      return;

    int yPos = 50;
    auto hitRow = [&]() -> bool {
      bool hit = (y > yPos && y < yPos + 30);
      yPos += 35;
      return hit;
    };

    bool selected = false;
    if (currentGameMode != MODE_TIMER && hitRow()) {
      currentTimeMode = TIME_UNTIMED;
      selected = true;
    } else if (hitRow()) {
      currentTimeMode = TIME_BLITZ_3_0;
      selected = true;
    } else if (hitRow()) {
      currentTimeMode = TIME_BLITZ_3_2;
      selected = true;
    } else if (hitRow()) {
      currentTimeMode = TIME_RAPID_10_0;
      selected = true;
    } else if (currentGameMode == MODE_TIMER && hitRow()) {
      currentTimeMode = TIME_RAPID_15_10;
      selected = true;
    } else if (hitRow()) {
      currentState = STATE_MENU_CUSTOM_TIME;
      drawCustomTimeMenu();
      return;
    }

    if (selected) {
      if (currentGameMode == MODE_TIMER) {
        startTimerOnly();
      } else {
        displaySide = SIDE_WHITE;
        playerSide = SIDE_WHITE;
        botSide = MCUMAX_SQUARE_INVALID;
        startGame();
      }
    }
    return;
  }

  if (currentState == STATE_MENU_CUSTOM_TIME) {
    if (x > 10 && x < 55 && y > 10 && y < 40) {
      currentState = STATE_MENU_TIME;
      drawTimeMenu();
      return;
    }
    if (x > 50 && x < 130 && y > 65 && y < 95) {
      if (customMinutes < 99) {
        customMinutes++;
        drawCustomSpinnerValue(50, 90, customMinutes);
      }
    } else if (x > 50 && x < 130 && y > 155 && y < 185) {
      if (customMinutes > 1) {
        customMinutes--;
        drawCustomSpinnerValue(50, 90, customMinutes);
      }
    } else if (x > 190 && x < 270 && y > 65 && y < 95) {
      if (customIncrement < 60) {
        customIncrement++;
        drawCustomSpinnerValue(190, 230, customIncrement);
      }
    } else if (x > 190 && x < 270 && y > 155 && y < 185) {
      if (customIncrement > 0) {
        customIncrement--;
        drawCustomSpinnerValue(190, 230, customIncrement);
      }
    } else if (x > 50 && x < 270 && y > 200 && y < 235) {
      currentTimeMode = TIME_CUSTOM;
      if (currentGameMode == MODE_TIMER) {
        startTimerOnly();
      } else {
        displaySide = SIDE_WHITE;
        playerSide = SIDE_WHITE;
        botSide = MCUMAX_SQUARE_INVALID;
        startGame();
      }
    }
    return;
  }

  if (currentState == STATE_MENU_LEVEL) {
    if (x > 10 && x < 55 && y > 10 && y < 40) {
      goToRootMenu();
      return;
    }
    if (x > 40 && x < 280) {
      int idx = -1;
      if (y > 50 && y < 85)
        idx = 0;
      else if (y > 95 && y < 130)
        idx = 1;
      else if (y > 140 && y < 175)
        idx = 2;
      else if (y > 185 && y < 220)
        idx = 3;
      if (idx >= 0) {
        engineNodes = BOT_LEVELS[idx].nodes;
        engineDepth = BOT_LEVELS[idx].depth;
        mcumax_set_probabilistic_selection(BOT_LEVELS[idx].probabilistic);
        currentState = STATE_MENU_COLOR;
        drawColorMenu();
      }
    }
    return;
  }

  if (currentState == STATE_MENU_COLOR) {
    if (x > 10 && x < 55 && y > 10 && y < 40) {
      currentState = STATE_MENU_LEVEL;
      drawLevelMenu();
      return;
    }
    if (y > 100 && y < 160) {
      if (x > 40 && x < 140) {
        displaySide = playerSide = SIDE_WHITE;
        botSide = SIDE_BLACK;
        startGame();
      } else if (x > 180 && x < 280) {
        displaySide = playerSide = SIDE_BLACK;
        botSide = SIDE_WHITE;
        startGame();
      }
    }
    return;
  }

  if (currentState == STATE_PLAYING) {
    if (x >= BOARD_SIZE) {
      bool pressMenu = (currentGameMode == MODE_PVB) ? (y > 5 && y < 30)
                                                     : (y > 92 && y < 117);
      bool pressUndo = (currentGameMode == MODE_PVB) ? (y > 35 && y < 60)
                                                     : (y > 122 && y < 147);

      if (pressMenu || pressUndo) {
        if (botThinking) {
          mcumax_stop_search();
          botThinking = false;
          botStopped = true;
          tft.fillRect(245, 210, 70, 20, CYBER_BG);
        }
        if (pressMenu) {
          showQuitConfirm = true;
          drawQuitConfirmModal();
        } else {
          undoMove();
          lastTimerUpdate = millis();
        }
      }
      return;
    }

    if (botThinking)
      return;

    if (currentGameMode == MODE_PVP ||
        mcumax_get_current_side() == playerSide) {
      mcumax_square touchedSquare =
          displayToSquare(x / SQUARE_SIZE, y / SQUARE_SIZE);

      if (selectedSquare != MCUMAX_SQUARE_INVALID) {
        if (touchedSquare == selectedSquare) {
          selectedSquare = MCUMAX_SQUARE_INVALID;
          validMovesCount = 0;
          drawBoard();
          drawSidebar();
          return;
        }
        for (uint32_t i = 0; i < validMovesCount; i++) {
          if (validMoves[i].to == touchedSquare) {
            mcumax_move chosenMove = validMoves[i];
            selectedSquare = MCUMAX_SQUARE_INVALID;
            validMovesCount = 0;

            executeMove(chosenMove);
            if (currentGameMode == MODE_PVP)
              lastTimerUpdate = millis();
            finishPostMoveFlow();
            return;
          }
        }
      }
      selectSquareForPlayer(touchedSquare);
    }
  }
}

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  tft.init();
  tft.invertDisplay(false);
  tft.setRotation(1);
  tft.fillScreen(CYBER_BG);

  ts.begin();
  ts.setRotation(1);

  mcumax_set_opening_book((const mcumax_opening_entry_t *)opening_book,
                          OPENING_BOOK_ENTRIES);
  mcumax_set_callback(engine_callback, nullptr);

  xTaskCreatePinnedToCore(bgEvalTask, "BgEvalCore0", 16384, NULL, 1,
                          &bgEvalTaskHandle, 0);

  drawModeMenu();
}

void loop() {
  unsigned long currentMillis = millis();

  handleSerialDebug();

  if (currentState == STATE_TIMER_RUNNING && !timerPaused &&
      whiteTimeLeft > 0 && blackTimeLeft > 0) {
    uint32_t elapsed = (uint32_t)(currentMillis - lastTimerUpdate);
    lastTimerUpdate = currentMillis;

    uint32_t &activeClock = timerActiveWhite ? whiteTimeLeft : blackTimeLeft;
    if (activeClock > elapsed) {
      activeClock -= elapsed;
      static unsigned long lastTimerDraw = 0;
      if (currentMillis - lastTimerDraw >= 100) {
        lastTimerDraw = currentMillis;
        updateTimerDisplay(false);
      }
    } else {
      activeClock = 0;
      timerPaused = true;
      updateTimerDisplay(true);
      drawFlagFallOverlay(timerActiveWhite);
    }
  }

  if (currentState == STATE_PLAYING && currentGameMode == MODE_PVP &&
      currentTimeMode != TIME_UNTIMED) {
    if (pvpTimerStarted) {
      uint32_t elapsed = (uint32_t)(currentMillis - lastTimerUpdate);
      lastTimerUpdate = currentMillis;

      bool whiteTurn = (mcumax_get_current_side() == SIDE_WHITE);
      uint32_t &activeClock = whiteTurn ? whiteTimeLeft : blackTimeLeft;
      bool &activeFlash = whiteTurn ? whiteClockFlash : blackClockFlash;

      if (activeClock > elapsed) {
        activeClock -= elapsed;
        if (activeClock < 15000 && currentMillis - lastFlashToggle >= 250) {
          activeFlash = !activeFlash;
          lastFlashToggle = currentMillis;
          drawPvPClocks();
        }
        static unsigned long lastClockDraw = 0;
        if (currentMillis - lastClockDraw >= 1000) {
          lastClockDraw = currentMillis;
          drawPvPClocks();
        }
      } else {
        activeClock = 0;
        activeFlash = false;
        drawPvPClocks();
        currentState = STATE_GAME_OVER;
        isCheckmateFlag = isStalemateFlag = false;
        specialKingSquare = MCUMAX_SQUARE_INVALID;
        gameOverMsg = whiteTurn ? "BLACK WINS!" : "WHITE WINS!";
        drawReason = "";
        delay(750);
        showGameOverMenuAnimated(gameOverMsg);
      }
    } else {
      lastTimerUpdate = currentMillis;
    }
  }

  if (!isSerialDebugActive) {
    int tx, ty;
    if (getStableTouchPoint(tx, ty)) {
      if (!touchActive && (millis() - lastTouchRelease > 80)) {
        touchActive = true;
        handleTouch(tx, ty);
      }
    } else if (touchActive) {
      touchActive = false;
      lastTouchRelease = millis();
    }
  }

  if (!isSerialDebugActive && currentGameMode == MODE_PVB &&
      currentState == STATE_PLAYING && mcumax_get_current_side() == botSide &&
      !botThinking) {
    botThinking = true;
    botStopped = false;
    pendingMenuAction = false;
    pendingUndoAction = false;
    thinkingDots = 0;
    lastThinkingAnim = millis();
    tft.setTextColor(CYBER_CYAN, CYBER_BG);
    tft.setTextSize(1);
    tft.drawString("Thinking   ", 245, 215);

    mcumax_move botMove = mcumax_search_best_move(engineNodes, engineDepth);

    botThinking = false;
    tft.fillRect(245, 210, 70, 20, CYBER_BG);

    if (botStopped) {
      botStopped = false;
      if (pendingMenuAction) {
        pendingMenuAction = false;
        showQuitConfirm = true;
        drawQuitConfirmModal();
      } else if (pendingUndoAction) {
        pendingUndoAction = false;
        undoMove();
        lastTimerUpdate = millis();
      }
      return;
    }

    if (botMove.from != MCUMAX_SQUARE_INVALID) {
      executeMove(botMove);
      finishPostMoveFlow();
    } else {
      checkGameState();
      if (currentState != STATE_GAME_OVER) {
        currentState = STATE_GAME_OVER;
        gameOverMsg = "YOU WIN!";
      }
      delay(750);
      showGameOverMenuAnimated(gameOverMsg);
    }
  }

  if (currentState == STATE_REVIEW && !showQuitConfirm) {
    static int lastBadgePly = -2;
    static int16_t lastBadgeDelta = -2;
    int curPly = getReviewDisplayPly();
    int16_t curDelta = (curPly >= 0 && curPly < currentMoveCount) ? moveDelta[curPly] : -1;

    if (curPly >= 0 && curDelta >= 0 && (lastBadgePly != curPly || lastBadgeDelta < 0)) {
      lastBadgePly = curPly;
      lastBadgeDelta = curDelta;
      drawReviewEvalBadge();
    } else if (curPly >= 0 && curDelta < 0) {
      lastBadgePly = curPly;
      lastBadgeDelta = -1;
      if (currentMillis - lastCalcAnim >= 300) {
        lastCalcAnim = currentMillis;
        calcDots = (calcDots + 1) & 3;
        drawReviewCalculatingDots(calcDots);
      }
    }
  }
}

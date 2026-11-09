# ESP32-S3 Cyber Chess

A standalone, touchscreen chess computer for the **ESP32-S3**, built on the [mcu-max](https://github.com/gissio/mcu-max) engine. Play against the bot, play a friend on the same screen, or use it as a chess clock, all without a PC or a network connection.

> **This project is built for [PlatformIO](https://platformio.org/)** (Arduino framework, `espressif32` platform). It is not an Arduino IDE sketch. Open the project folder in VS Code with the PlatformIO extension, or use the PlatformIO CLI. All libraries (`TFT_eSPI`, `XPT2046_Touchscreen`, `mcu-max`) are bundled in `lib/`, so no extra dependencies need to be installed.

## Key Features

- **Three game modes:** Player vs Bot, Player vs Player (board flips for the opponent), and a standalone Chess Timer.
- **Four bot levels:** Easy, Medium, Hard and Extreme, each with its own node and search-depth limits.
- **Flexible time controls:** untimed, 3+0, 3+2, 10+0, 15+10, or a custom start time and increment.
- **Large built-in opening book:** about 1.96 million entries stored in flash, generated from PGN files with the included parser.
- **Full touch GUI** (landscape, 320×240): side selection, move highlighting, promotion picker, undo, pause/resume, and a quit confirmation dialog.
- **Rules handling:** check and checkmate detection, castling, promotion, and draws by stalemate, insufficient material (including same-colour bishops) and the 50-move rule.
- **Game review mode:** after the game, step through the move list in SAN. Moves are evaluated in the background on Core 0 and classified as brilliant, great, good, inaccuracy or blunder.
- **Dual-core design:** the engine and UI run on one core while analysis runs on the other, so the interface stays responsive.
- **Serial FEN debug mode:** send a FEN string over the serial monitor (115200 baud) to analyse a position with a chosen bot level.
- **Optimised build:** `-O3`, 240 MHz CPU, 80 MHz QIO flash, and a custom 16 MB partition table with a single large app partition.

## Hardware Requirements

| Component | Requirement |
|---|---|
| **Microcontroller** | ESP32-S3 board with **16 MB flash and 8 MB octal PSRAM** (N16R8), e.g. ESP32-S3-DevKitC-1 |
| **Display** | SPI TFT, **320×240**, **ST7789** driver |
| **Touch controller** | **XPT2046** resistive touch (usually on the same display module) |
| **Wiring** | Jumper wires or a breadboard |
| **Power / upload** | USB cable (the project uploads over the board's USB port) |

The PlatformIO environment is `esp32-s3-devkitc-1` with `qio_opi` memory type. If your board has a different flash or PSRAM configuration, adjust `platformio.ini` accordingly. The default `upload_port` is `/dev/ttyACM0`, so change it to match your system (e.g. `COM3` on Windows).

## Pin Configuration

The display and touch controller share one SPI bus. The pins are defined in:

```
lib/TFT_eSPI-master/User_Setups/Setup42_ILI9341_ESP32.h
```

| Signal | Display / Touch Pin | ESP32-S3 GPIO |
|---|---|---|
| `TFT_MISO` | SDO / T_DO | **42** |
| `TFT_MOSI` | SDI / T_DIN | **47** |
| `TFT_SCLK` | SCK / T_CLK | **21** |
| `TFT_CS` | CS | **41** |
| `TFT_DC` | DC / RS | **40** |
| `TFT_RST` | RESET | **45** |
| `TOUCH_CS` | T_CS | **39** |
| Power | VCC / GND | 3.3 V / GND |

Notes:

- Connect the display's backlight pin (LED/BL) to 3.3 V; the firmware does not control it.
- The SPI clock is set to 40 MHz for the display and 2.5 MHz for touch.
- To use different pins, edit the values in `Setup42_ILI9341_ESP32.h`. This setup is already selected in `lib/TFT_eSPI-master/User_Setup_Select.h`.

## PGN Parser Usage

`pgn_parser.py` converts PGN game collections into the opening book used by the engine. It processes multiple PGN files in parallel (one per CPU core) and merges the results into a single book.

### Requirements

- Python 3.8+
- [python-chess](https://pypi.org/project/chess/):

```bash
pip install chess
```

### Steps

1. Put one or more `.pgn` files in the **same folder as `pgn_parser.py`**. Files whose names start with `opening_book` or `all_` are ignored.
2. Run the script from that folder:

   ```bash
   python pgn_parser.py
   ```

3. The script will:
   - convert each `PGN` file to a matching `.bin` file (largest files first);
   - ask whether to skip files that already have a `.bin` (`y` = resume, `n` = reprocess everything);
   - merge all `.bin` files into **`opening_book.bin`** and generate **`opening_book.h`**;
   - write a log to `convert_log_<timestamp>.txt`.
4. Copy the generated `opening_book.h` into the `src/` folder, replacing the existing one, then rebuild and upload with PlatformIO.

### How it works

- For every game, the first **16 plies** (half-moves) are recorded.
- For each position, the most frequently played move is kept. Positions seen in fewer than **2 games** are dropped.
- Each entry is 7 bytes: a 32-bit position hash, the from-square, the to-square and a weight (the game count, capped at 255).
- Squares are numbered 0 to 63 starting at a8, going along each rank to h1.

### Tuning

The depth and the minimum game count are set at the top of `main()` in `pgn_parser.py`:

```python
max_depth = 16   # number of plies read from each game
min_games = 2    # minimum number of games for a position to be kept
```

A deeper or less strict book makes the generated header larger. Check that your firmware still fits in flash after regenerating it.

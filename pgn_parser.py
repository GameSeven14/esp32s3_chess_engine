#!/usr/bin/env python3
"""
Parallel PGN to Opening Book Converter for mcu-max
- Tận dụng tối đa CPU cores
- Auto merge thành 1 file khi hoàn thành
- Hỗ trợ resume (bỏ qua file đã có .bin)
- Log đầy đủ tiến trình
"""

import chess
import chess.pgn
import struct
import sys
import os
import glob
import hashlib
import time
import logging
from collections import defaultdict
from multiprocessing import Pool, cpu_count
from functools import partial
from datetime import datetime

# Cấu hình logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%H:%M:%S'
)
logger = logging.getLogger(__name__)

# Tạo file log riêng
log_file = f"convert_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.txt"
file_handler = logging.FileHandler(log_file)
file_handler.setFormatter(logging.Formatter('%(asctime)s [%(levelname)s] %(message)s'))
logger.addHandler(file_handler)

def fen_to_zobrist(fen):
    """Tính Zobrist hash từ FEN"""
    board_part = fen.split(' ')[0]
    return int(hashlib.md5(board_part.encode()).hexdigest()[:8], 16)

def uci_to_squares(uci_move):
    """Chuyển UCI move sang (from_square, to_square)"""
    file_map = {'a': 0, 'b': 1, 'c': 2, 'd': 3, 'e': 4, 'f': 5, 'g': 6, 'h': 7}
    from_sq = (8 - int(uci_move[1])) * 8 + file_map[uci_move[0]]
    to_sq = (8 - int(uci_move[3])) * 8 + file_map[uci_move[2]]
    return from_sq, to_sq

def extract_openings_from_pgn(pgn_file, max_depth=16, min_games=2):
    """Trích xuất từ 1 file PGN (dùng cho parallel)"""
    book = defaultdict(lambda: defaultdict(int))
    total_games = 0
    errors = 0
    
    logger.info(f"   📖 Bắt đầu đọc: {pgn_file}")
    
    try:
        with open(pgn_file, 'r', encoding='utf-8', errors='ignore') as f:
            while True:
                try:
                    game = chess.pgn.read_game(f)
                    if game is None:
                        break
                except Exception:
                    errors += 1
                    continue
                
                total_games += 1
                board = chess.Board()
                ply_count = 0
                
                for move in game.mainline_moves():
                    board.push(move)
                    ply_count += 1
                    
                    if ply_count <= max_depth:
                        board.pop()
                        fen = board.fen()
                        board.push(move)
                        book[fen][move.uci()] += 1
                    else:
                        break
                
                # Log mỗi 1000 game
                if total_games % 1000 == 0:
                    logger.info(f"   📊 {pgn_file}: đã xử lý {total_games} ván, {len(book)} thế cờ")
                    
    except Exception as e:
        logger.error(f"   ❌ Lỗi đọc {pgn_file}: {e}")
        return None, 0, 0, str(e)
    
    logger.info(f"   ✅ Đọc xong {pgn_file}: {total_games} ván, {errors} lỗi")
    
    # Chuyển thành entries
    entries = []
    for fen, moves in book.items():
        best_move = max(moves.items(), key=lambda x: x[1])
        move_uci = best_move[0]
        count = best_move[1]
        
        if count >= min_games:
            from_sq, to_sq = uci_to_squares(move_uci)
            entries.append({
                'hash': fen_to_zobrist(fen),
                'from': from_sq,
                'to': to_sq,
                'weight': min(count, 255)
            })
    
    entries.sort(key=lambda x: x['hash'])
    logger.info(f"   📊 {pgn_file}: {len(entries)} entries sau khi lọc (>= {min_games} ván)")
    
    return entries, total_games, errors, None

def process_single_file(pgn_file, max_depth=16, min_games=2):
    """Xử lý 1 file PGN, lưu thành .bin riêng"""
    output_bin = pgn_file.replace('.pgn', '.bin')
    
    logger.info(f"🔄 Bắt đầu xử lý: {pgn_file}")
    
    # Resume: bỏ qua nếu đã có file .bin
    if os.path.exists(output_bin):
        size_kb = os.path.getsize(output_bin) / 1024
        logger.info(f"⏭️  Bỏ qua {pgn_file} (đã có .bin, {size_kb:.1f} KB)")
        return {
            'pgn': pgn_file,
            'status': 'skipped',
            'entries': 0,
            'games': 0,
            'size_kb': size_kb,
            'time': 0
        }
    
    start_time = time.time()
    entries, games, errors, error_msg = extract_openings_from_pgn(
        pgn_file, max_depth, min_games
    )
    elapsed = time.time() - start_time
    
    if entries is None or len(entries) == 0:
        logger.warning(f"⚠️  {pgn_file}: không có entries (games={games}, error={error_msg})")
        return {
            'pgn': pgn_file,
            'status': 'error' if error_msg else 'empty',
            'entries': 0,
            'games': games,
            'error': error_msg,
            'time': elapsed
        }
    
    # Lưu binary
    try:
        with open(output_bin, 'wb') as f:
            f.write(struct.pack('<I', 0x4D43554D))  # MAGIC
            f.write(struct.pack('<H', 1))           # VERSION
            f.write(struct.pack('<I', len(entries)))
            for e in entries:
                f.write(struct.pack('<IBBB', e['hash'], e['from'], e['to'], e['weight']))
        
        size_kb = os.path.getsize(output_bin) / 1024
        logger.info(f"✅ Hoàn thành: {pgn_file} -> {output_bin} ({len(entries)} entries, {size_kb:.1f} KB, {elapsed:.1f}s)")
        
        return {
            'pgn': pgn_file,
            'status': 'success',
            'entries': len(entries),
            'games': games,
            'size_kb': size_kb,
            'time': elapsed
        }
    except Exception as e:
        logger.error(f"❌ Lỗi lưu {pgn_file}: {e}")
        return {
            'pgn': pgn_file,
            'status': 'error',
            'entries': 0,
            'games': games,
            'error': str(e),
            'time': elapsed
        }

def find_pgn_files():
    """Tìm tất cả file .pgn trong thư mục hiện tại"""
    pgn_files = glob.glob("*.pgn") + glob.glob("*.PGN")
    # Bỏ qua file đã merge
    pgn_files = [f for f in pgn_files if not f.startswith("opening_book") and not f.startswith("all_")]
    pgn_files.sort(key=lambda x: os.path.getsize(x), reverse=True)
    return pgn_files

def merge_all_bin():
    """Merge tất cả file .bin thành opening_book.bin"""
    bin_files = glob.glob("*.bin")
    bin_files = [f for f in bin_files if f != "opening_book.bin"]
    
    if not bin_files:
        logger.warning("❌ Không tìm thấy file .bin để merge")
        return None
    
    logger.info(f"📦 Đang merge {len(bin_files)} file .bin...")
    
    all_entries = []
    total_entries = 0
    merged_files = 0
    
    for bin_file in bin_files:
        try:
            with open(bin_file, 'rb') as f:
                magic = struct.unpack('<I', f.read(4))[0]
                version = struct.unpack('<H', f.read(2))[0]
                count = struct.unpack('<I', f.read(4))[0]
                
                logger.info(f"   📄 {bin_file}: {count} entries")
                
                for _ in range(count):
                    data = f.read(7)
                    if len(data) == 7:
                        hash_val, from_sq, to_sq, weight = struct.unpack('<IBBB', data)
                        all_entries.append((hash_val, from_sq, to_sq, weight))
                        total_entries += 1
                merged_files += 1
        except Exception as e:
            logger.error(f"   ❌ Lỗi đọc {bin_file}: {e}")
    
    if not all_entries:
        logger.error("❌ Không có entry nào để merge")
        return None
    
    # Sắp xếp theo hash
    all_entries.sort(key=lambda x: x[0])
    
    # Lưu thành 1 file
    output_file = "opening_book.bin"
    with open(output_file, 'wb') as f:
        f.write(struct.pack('<I', 0x4D43554D))
        f.write(struct.pack('<H', 1))
        f.write(struct.pack('<I', total_entries))
        for e in all_entries:
            f.write(struct.pack('<IBBB', e[0], e[1], e[2], e[3]))
    
    size_kb = os.path.getsize(output_file) / 1024
    logger.info(f"✅ Đã merge {merged_files} files, {total_entries} entries vào {output_file} ({size_kb:.1f} KB)")
    
    # Tạo header
    create_header(output_file, all_entries)
    
    return output_file

def create_header(bin_file, entries):
    """Tạo header C từ entries"""
    header_file = bin_file.replace('.bin', '.h')
    
    with open(header_file, 'w') as f:
        f.write("// Auto-generated opening book for mcu-max\n")
        f.write("// Merged from all openings\n\n")
        f.write("#ifndef OPENING_BOOK_H\n")
        f.write("#define OPENING_BOOK_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define OPENING_BOOK_ENTRIES {len(entries)}\n\n")
        f.write("typedef struct {\n")
        f.write("    uint32_t hash;\n")
        f.write("    uint8_t from;\n")
        f.write("    uint8_t to;\n")
        f.write("    uint8_t weight;\n")
        f.write("} opening_entry_t;\n\n")
        f.write("static const opening_entry_t opening_book[] = {\n")
        
        for i, e in enumerate(entries):
            f.write(f"    {{0x{e[0]:08x}, {e[1]:2d}, {e[2]:2d}, {e[3]:3d}}}")
            if i < len(entries) - 1:
                f.write(",\n")
            else:
                f.write("\n")
        
        f.write("};\n\n")
        f.write("#endif // OPENING_BOOK_H\n")
    
    logger.info(f"📄 Đã tạo header: {header_file}")

def main():
    logger.info("🚀 Parallel PGN to Opening Book Converter")
    logger.info("=" * 70)
    logger.info(f"📝 Log file: {log_file}")
    logger.info("")
    
    # Tham số
    max_depth = 16
    min_games = 2
    num_cores = cpu_count()
    
    # Tìm file PGN
    pgn_files = find_pgn_files()
    
    if not pgn_files:
        logger.error("❌ Không tìm thấy file .pgn nào!")
        sys.exit(1)
    
    logger.info(f"📁 Tìm thấy {len(pgn_files)} file PGN:")
    total_size = 0
    for f in pgn_files:
        size_mb = os.path.getsize(f) / (1024 * 1024)
        total_size += size_mb
        logger.info(f"   - {f} ({size_mb:.2f} MB)")
    
    logger.info(f"💻 Số core CPU: {num_cores}")
    logger.info(f"📏 Depth: {max_depth}, Min games: {min_games}")
    logger.info(f"📦 Tổng dung lượng: {total_size:.2f} MB")
    logger.info("")
    
    # Hỏi có muốn skip file đã có .bin không
    existing_bin = sum(1 for f in pgn_files if os.path.exists(f.replace('.pgn', '.bin')))
    if existing_bin > 0:
        logger.info(f"⚠️  Phát hiện {existing_bin} file đã có .bin")
        logger.info("   - 'y': Bỏ qua các file đã có (resume)")
        logger.info("   - 'n': Xử lý lại tất cả (overwrite)")
        choice = input("   Lựa chọn (y/n): ").strip().lower()
        if choice == 'y':
            # Lọc bỏ file đã có .bin
            pgn_files = [f for f in pgn_files if not os.path.exists(f.replace('.pgn', '.bin'))]
            logger.info(f"   ✅ Sẽ xử lý {len(pgn_files)} file chưa có .bin")
        logger.info("")
    
    if not pgn_files:
        logger.info("✅ Tất cả file đã được xử lý!")
        merge_all_bin()
        sys.exit(0)
    
    # Chạy song song
    cores_to_use = min(num_cores, len(pgn_files))
    logger.info(f"🔄 Đang xử lý với {cores_to_use} core...")
    logger.info(f"📝 Số file cần xử lý: {len(pgn_files)}")
    logger.info("=" * 70)
    logger.info("")
    
    start_total = time.time()
    
    # Tạo partial function với tham số cố định
    process_func = partial(process_single_file, max_depth=max_depth, min_games=min_games)
    
    # Chạy parallel với progress bar đơn giản
    with Pool(processes=cores_to_use) as pool:
        # Dùng imap để có thể log progress
        results = []
        completed = 0
        for result in pool.imap_unordered(process_func, pgn_files):
            results.append(result)
            completed += 1
            logger.info(f"📊 Tiến độ: {completed}/{len(pgn_files)} file đã hoàn thành")
    
    elapsed_total = time.time() - start_total
    
    # Thống kê kết quả
    logger.info("")
    logger.info("=" * 70)
    logger.info("📊 KẾT QUẢ:")
    logger.info("=" * 70)
    
    success = 0
    skipped = 0
    errors = 0
    total_entries = 0
    total_games = 0
    total_size = 0
    
    for r in results:
        if r['status'] == 'success':
            success += 1
            total_entries += r['entries']
            total_games += r['games']
            total_size += r['size_kb']
            logger.info(f"   ✅ {r['pgn']:<35} {r['entries']:>4} entries, {r['games']:>5} games, {r['size_kb']:>6.1f} KB, {r['time']:>5.1f}s")
        elif r['status'] == 'skipped':
            skipped += 1
            logger.info(f"   ⏭️  {r['pgn']:<35} (đã có .bin, {r['size_kb']:.1f} KB)")
        else:
            errors += 1
            logger.error(f"   ❌ {r['pgn']:<35} Lỗi: {r.get('error', 'unknown')}")
    
    logger.info("")
    logger.info("=" * 70)
    logger.info(f"✅ Thành công: {success} file")
    logger.info(f"⏭️  Bỏ qua: {skipped} file")
    logger.info(f"❌ Lỗi: {errors} file")
    logger.info(f"📊 Tổng entries: {total_entries}")
    logger.info(f"📊 Tổng games: {total_games}")
    logger.info(f"📦 Tổng dung lượng: {total_size:.1f} KB")
    logger.info(f"⏱️  Thời gian: {elapsed_total:.1f} giây ({elapsed_total/60:.1f} phút)")
    
    # Auto merge nếu có nhiều file
    if success > 1 or (success + skipped) > 1:
        logger.info("")
        logger.info("=" * 70)
        merge_all_bin()
    elif success == 1:
        # Chỉ có 1 file, đổi tên thành opening_book.bin
        bin_files = glob.glob("*.bin")
        bin_files = [f for f in bin_files if f != "opening_book.bin"]
        if bin_files:
            import shutil
            shutil.copy(bin_files[0], "opening_book.bin")
            logger.info(f"📦 Đã copy {bin_files[0]} -> opening_book.bin")
            # Tạo header
            with open(bin_files[0], 'rb') as f:
                f.read(4)  # Magic
                f.read(2)  # Version
                count = struct.unpack('<I', f.read(4))[0]
                entries = []
                for _ in range(count):
                    hash_val, from_sq, to_sq, weight = struct.unpack('<IBBB', f.read(7))
                    entries.append((hash_val, from_sq, to_sq, weight))
                create_header("opening_book.bin", entries)
    
    logger.info("")
    logger.info("🎉 Hoàn thành!")
    logger.info(f"📝 Log đã lưu tại: {log_file}")

if __name__ == "__main__":
    main()
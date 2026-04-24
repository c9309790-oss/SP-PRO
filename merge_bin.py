import os
import re
import json
from datetime import datetime
import shutil
import zlib
import zipfile
import subprocess

def calculate_crc32(file_path):
    """计算文件的CRC32校验值"""
    buf_size = 65536
    crc = 0
    with open(file_path, 'rb') as f:
        while chunk := f.read(buf_size):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF

def parse_json(json_file):
    """解析JSON文件"""
    with open(json_file, 'r') as f:
        return json.load(f)

def extract_module_version(header_file):
    """从sysdef.h文件中提取MODULE_VER"""
    if not os.path.exists(header_file):
        print(f"错误: 文件 {header_file} 不存在")
        return None

    with open(header_file, 'r', encoding='utf-8') as f:
        content = f.read()

    match = re.search(r'#define\s+MODULE_VER\s+"([^"]+)"', content)
    if match:
        return match.group(1)
    else:
        print(f"错误: 未在 {header_file} 中找到 MODULE_VER 定义")
        return None

def copy_and_rename_app_bin(config, module_ver, current_date):
    """复制并重命名app bin文件"""
    app_info = config.get("app", {})
    app_file = app_info.get("file")
    if not app_file or not os.path.exists(app_file):
        print("错误: app bin 文件不存在或未定义")
        return None

    target_file = f"../KLM_IOT_{module_ver.replace('-', '_')}_{current_date}_app.bin"

    try:
        shutil.copy(app_file, target_file)
        print(f"已复制并重命名app bin文件到: {target_file}")
        return target_file
    except Exception as e:
        print(f"错误: 无法复制文件 {app_file} 到 {target_file}: {e}")
        return None

def generate_combined_bin(config, output_file):
    """生成合并后的BIN文件"""
    flash_files = config.get("flash_files", {})
    flash_files = {int(k, 16): v for k, v in flash_files.items()}
    sorted_flash_files = sorted(flash_files.items())

    with open(output_file, 'wb') as combined_file:
        for offset, file_path in sorted_flash_files:
            if not os.path.exists(file_path):
                print(f"错误: 文件 {file_path} 不存在")
                continue

            with open(file_path, 'rb') as bin_file:
                bin_data = bin_file.read()

            current_pos = combined_file.tell()
            if current_pos < offset:
                combined_file.write(b'\xFF' * (offset - current_pos))

            combined_file.write(bin_data)
            print(f"已写入文件 {file_path} 到偏移 {hex(offset)}")

    print(f"合并完成，生成文件：{output_file}")

def append_crc_renamed_copy(file_path, prefix):
    """生成带CRC32的副本文件名"""
    crc = calculate_crc32(file_path)
    crc_hex = f"{crc:08X}"
    base, ext = os.path.splitext(file_path)
    renamed = f"{base}_{crc_hex}{ext}"
    try:
        shutil.copy(file_path, renamed)
        print(f"已生成 {prefix} 文件副本: {renamed}")
        return renamed
    except Exception as e:
        print(f"错误: 无法生成 {prefix} CRC32 文件副本: {e}")
        return None

def save_git_info():
    """保存当前git log和diff到文件"""
    git_log_file = "./git_log.txt"
    git_diff_file = "./git_diff.txt"
    try:
        with open(git_log_file, 'w', encoding='utf-8') as f:
            subprocess.run(["git", "log", "--oneline", "-n", "20"], stdout=f, check=True, text=True)
        with open(git_diff_file, 'w', encoding='utf-8') as f:
            subprocess.run(["git", "diff"], stdout=f, check=True, text=True)
        print("已保存 git log 和 git diff")
        return git_log_file, git_diff_file
    except Exception as e:
        print(f"⚠ 获取 git 信息失败: {e}")
        return None, None

def generate_release_zip(module_ver, current_date, app_bin_crc, factory_bin_crc, elf_file, git_log=None, git_diff=None):
    """生成最终发布压缩包，文件名包含两个 CRC32"""
    def extract_crc(filename):
        m = re.search(r'_([0-9A-Fa-f]{8})\.bin$', os.path.basename(filename))
        return m.group(1) if m else "00000000"

    app_crc_val = extract_crc(app_bin_crc) if app_bin_crc else "00000000"
    factory_crc_val = extract_crc(factory_bin_crc) if factory_bin_crc else "00000000"

    zip_name = f"../KLM_IOT_{module_ver.replace('-', '_')}_{current_date}_A{app_crc_val}_F{factory_crc_val}_release.zip"

    try:
        with zipfile.ZipFile(zip_name, 'w', zipfile.ZIP_DEFLATED) as zipf:
            if app_bin_crc and os.path.exists(app_bin_crc):
                zipf.write(app_bin_crc, f"bin/{os.path.basename(app_bin_crc)}")
            if factory_bin_crc and os.path.exists(factory_bin_crc):
                zipf.write(factory_bin_crc, f"bin/{os.path.basename(factory_bin_crc)}")
            if elf_file and os.path.exists(elf_file):
                zipf.write(elf_file, f"debug/{os.path.basename(elf_file)}")
            if git_log and os.path.exists(git_log):
                zipf.write(git_log, f"debug/{os.path.basename(git_log)}")
            if git_diff and os.path.exists(git_diff):
                zipf.write(git_diff, f"debug/{os.path.basename(git_diff)}")
        print(f"✅ 已生成发布压缩包: {zip_name}")
    except Exception as e:
        print(f"错误: 生成压缩包失败: {e}")

def main():
    json_file = "flasher_args.json"
    header_file = "../main/sysdef.h"

    if not os.path.exists(json_file):
        print(f"错误: JSON文件 {json_file} 不存在")
        return
    config = parse_json(json_file)

    module_ver = extract_module_version(header_file)
    if not module_ver:
        return

    current_date = datetime.now().strftime("%Y_%m_%d")

    new_app_file = copy_and_rename_app_bin(config, module_ver, current_date)
    if not new_app_file:
        return
    app_bin_crc = append_crc_renamed_copy(new_app_file, "APP")

    output_file = f"../KLM_IOT_{module_ver.replace('-', '_')}_{current_date}_factory.bin"
    generate_combined_bin(config, output_file)
    factory_bin_crc = append_crc_renamed_copy(output_file, "Factory")

    # 固定 ELF 文件路径
    elf_file = "./simple_ota.elf"
    if not os.path.exists(elf_file):
        print(f"⚠ ELF 文件 {elf_file} 不存在，压缩包将不包含 ELF")
        elf_file = None

    # 获取 git 信息
    git_log, git_diff = save_git_info()

    # 生成压缩包
    generate_release_zip(module_ver, current_date, app_bin_crc, factory_bin_crc, elf_file, git_log, git_diff)

if __name__ == "__main__":
    main()

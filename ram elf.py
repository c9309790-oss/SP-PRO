import matplotlib.pyplot as plt
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

def parse_elf_and_get_ram_variables(elf_file_path, ram_start, ram_end):
    with open(elf_file_path, 'rb') as f:
        elffile = ELFFile(f)
        
        # 查找符号表
        symtab = None
        for section in elffile.iter_sections():
            if isinstance(section, SymbolTableSection):
                symtab = section
                break
        
        if not symtab:
            raise ValueError("ELF 文件中没有符号表。")
        
        ram_variables = []
        for symbol in symtab.iter_symbols():
            sym_addr = symbol['st_value']
            sym_size = symbol['st_size']
            sym_type = symbol['st_info']['type']
            sym_bind = symbol['st_info']['bind']
            
            if ram_start <= sym_addr < ram_end:
                ram_variables.append({
                    'name': symbol.name,
                    'address': sym_addr,
                    'size': sym_size,
                    'type': sym_type,
                    'bind': sym_bind
                })
        
        ram_variables.sort(key=lambda x: x['size'], reverse=True)
        return ram_variables

def save_ram_variables_to_csv(filename, title, variables):
    with open(filename, mode='w') as file:
        total_size = 0
        file.write(f"{title}（按占用空间从大到小排序）\n")
        file.write('变量名,地址,大小(字节),类型,绑定\n')

        for var in variables:
            total_size += var['size']
            sym_type_str = {
                'STT_OBJECT': 'OBJECT',
                'STT_FUNC': 'FUNC',
                'STT_NOTYPE': 'NOTYPE',
                'STT_SECTION': 'SECTION',
                'STT_FILE': 'FILE',
            }.get(var['type'], str(var['type']))
            sym_bind_str = {
                'STB_LOCAL': 'LOCAL',
                'STB_GLOBAL': 'GLOBAL',
                'STB_WEAK': 'WEAK'
            }.get(var['bind'], str(var['bind']))

            file.write(f"{var['name']},0x{var['address']:08X},{var['size']},{sym_type_str},{sym_bind_str}\n")
        
        file.write(f"总占用,,{total_size},,\n")

def plot_ram_variable_pie_chart(title, variables, threshold=2000):
    if not variables:
        print(f"没有数据可绘制饼图。")
        return
    
    other_size = 0
    other_name = "less than "+str(threshold)
    filtered_vars = []

    for var in variables:
        if var['size'] >= threshold:
            filtered_vars.append(var)
        else:
            other_size += var['size']
    
    if other_size > 0:
        filtered_vars.append({'name': other_name, 'size': other_size})

    labels = [var['name'] for var in filtered_vars]
    sizes = [var['size'] for var in filtered_vars]
    
    plt.figure(figsize=(8, 8))
    plt.pie(sizes, labels=labels, autopct='%1.1f%%', startangle=90, colors=plt.cm.Paired.colors)
    plt.title(f"{title}")
    plt.axis('equal')
    plt.show()

# ===== 这里改成你的 ELF 文件路径 =====
elf_path = "simple_ota.elf"

# RAM 区域（合并 SRAM0/1/2）
ram_start = 0x3FC80000
ram_end   = 0x3FD00000
ram_vars = parse_elf_and_get_ram_variables(elf_path, ram_start, ram_end)

# Flash XIP 区域
flash_start = 0x42000000
flash_end   = 0x43000000  # 可按固件大小调节
flash_vars = parse_elf_and_get_ram_variables(elf_path, flash_start, flash_end)

# 保存 CSV
save_ram_variables_to_csv('esp32s3_ram.csv', "ESP32-S3 RAM", ram_vars)
save_ram_variables_to_csv('esp32s3_flash.csv', "ESP32-S3 FLASH", flash_vars)

# 绘制饼图
plot_ram_variable_pie_chart("ESP32-S3 RAM", ram_vars, 512)
plot_ram_variable_pie_chart("ESP32-S3 FLASH", flash_vars, 512)

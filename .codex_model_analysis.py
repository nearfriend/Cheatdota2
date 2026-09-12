import sys
import struct
import bisect
import re
sys.path.insert(0, '.codex_analysis_packages')
import pefile
import capstone

pe = pefile.PE(r'D:\SteamLibrary\steamapps\common\dota 2 beta\game\dota\bin\win64\client.dll', fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
section = next(section for section in pe.sections if section.Name.startswith(b'.text'))
code = section.get_data()
pdata = next(section for section in pe.sections if section.Name.startswith(b'.pdata')).get_data()
functions = [struct.unpack_from('<III', pdata, index)[:2] for index in range(0, len(pdata)-11, 12)]
functions = sorted(pair for pair in functions if pair[0])
starts = [pair[0] for pair in functions]
decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

def dump(address):
    index = bisect.bisect_right(starts, address) - 1
    start, end = functions[index]
    print('FUNCTION', hex(start), hex(end), 'target', hex(address))
    for instruction in decoder.disasm(pe.get_data(start, min(end-start, 2400)), start):
        print(hex(instruction.address), instruction.bytes.hex(' '), instruction.mnemonic, instruction.op_str)

if len(sys.argv) > 1:
    for address in sys.argv[1:]:
        dump(int(address, 16))
else:
    for match in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code):
        offset = match.start()
        target = section.VirtualAddress + offset + 7 + struct.unpack_from('<i', code, offset+3)[0]
        if target in [0x481d593, 0x481d5e3]:
            dump(section.VirtualAddress + offset)

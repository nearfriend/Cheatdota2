"""Offline ABI checks for the installed Dota build; does not execute game code."""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '.codex_analysis_packages'))
import pefile

parser = argparse.ArgumentParser()
parser.add_argument('game', type=Path, help='Dota game directory')
args = parser.parse_args()
client = pefile.PE(str(args.game / 'dota/bin/win64/client.dll'), fast_load=True)
resources = pefile.PE(str(args.game / 'bin/win64/resourcesystem.dll'), fast_load=True)

init = bytes.fromhex('48 89 5C 24 10 57 48 83 EC 30 8B 41 04 48 8D 79 08 48 8B D9 A9 FF FF FF 3F')
assert client.__data__[:].count(init) == 1, 'ResourcePath initializer is not unique'
assert client.get_data(0x3A1DC80, len(init)) == init, 'Initializer moved; review ABI'

# ResourceSystem013's factory returns the object initialized with this vtable.
factory = resources.get_data(0x138C0, 8)
assert factory[:3] == bytes.fromhex('48 8D 05') and factory[7] == 0xC3
instance = 0x138C7 + struct.unpack_from('<i', factory, 3)[0]
assert instance == 0x81670
assert resources.get_data(0x138F5, 7) == bytes.fromhex('48 8D 05 CC C1 04 00')
assert resources.get_data(0x13903, 7) == bytes.fromhex('48 89 05 66 DD 06 00')

def method(offset):
    return struct.unpack('<Q', resources.get_data(0x5FAC8 + offset, 8))[0] - resources.OPTIONAL_HEADER.ImageBase

assert method(0x140) == 0x16720, 'Blocking loader slot changed'
assert method(0x188) == 0x170D0, 'Binding residency query changed'
assert method(0x278) == 0x162E0, 'Binding lookup changed'
loader = bytes.fromhex('48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 60')
assert resources.get_data(method(0x140), len(loader)) == loader
assert client.get_data(0x22E7AFC, 6) == bytes.fromhex('FF 90 40 01 00 00')
assert client.get_data(0x381C010, 4) == bytes.fromhex('48 8B 51 08'), 'CModel path offset changed'
print('PASS: unique initializer, interface factory, loader ABI, residency slot, and client call site')

for address, signature in [
    (0x1ACD250, '48 89 54 24 10 48 89 4C 24 08 55 56 41 56 48 8D AC 24 60 FE FF FF 48 81 EC A0 02 00 00'),
    (0x33289B0, '48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 20 8B EA 48 8B F1 E8'),
]:
    pattern = bytes.fromhex(signature)
    assert client.__data__[:].count(pattern) == 1, 'Combiner hook pattern is not unique'
    assert client.get_data(address, len(pattern)) == pattern
assert client.get_data(0x1ACD414, 5) == bytes.fromhex('E8 97 B5 85 01'), 'Combiner item-model call changed'
print('PASS: unique combiner and item-model hooks, and verified call between them')

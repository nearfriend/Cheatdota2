"""Compile the real hook code against a fake engine; run from an MSVC dev shell."""
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
hook = root / 'Andromeda-Dota2/Andromeda/Dota2/Hook/Hook_SetModel.cpp'
with tempfile.TemporaryDirectory(prefix='cosmetic-hook-test-') as directory:
    temp = Path(directory)
    (temp / 'Common').mkdir()
    (temp / 'AndromedaClient').mkdir()
    (temp / 'Common/Common.hpp').write_text('#pragma once\n')
    (temp / 'AndromedaClient/CAndromedaClient.hpp').write_text('''#pragma once
extern void* localHero;
struct FakeChanger {
 const char* GetCombinedModelOverride(void* hero, void*) { return hero == localHero ? "selected" : nullptr; }
 void* OnSkeletonSetModel(void*, void* binding) { return binding; }
};
struct FakeClient { FakeChanger changer; FakeChanger& GetCosmeticChanger() { return changer; } };
extern FakeClient* activeClient;
inline FakeClient* GetAndromedaClient() { return activeClient; }
''')
    source = '#include "' + hook.as_posix() + '"\n' + '''
#include <cassert>
#include <cstring>
#include <stdexcept>
void* localHero = reinterpret_cast<void*>(1);
FakeClient instance;
FakeClient* activeClient = &instance;
static int lastVariant = -1;
static bool throwFromBuilder = false;
const char* originalModel(void*, int variant) { lastVariant = variant; return "original"; }
bool originalBuild(void* hero, const void* items) {
 if (throwFromBuilder) throw std::runtime_error("test");
 const bool local = hero == localHero;
 assert(std::strcmp(Hook_GetItemModel(nullptr, 7), local && activeClient ? "selected" : "original") == 0);
 if(local) {
  assert(Hook_BuildCombinedModel(reinterpret_cast<void*>(2), items));
  assert(std::strcmp(Hook_GetItemModel(nullptr, 8), activeClient ? "selected" : "original") == 0);
 }
 return true;
}
int main() {
 GetItemModel_o = originalModel;
 BuildCombinedModel_o = originalBuild;
 assert(std::strcmp(Hook_GetItemModel(nullptr, 9), "original") == 0 && lastVariant == 9);
 assert(Hook_BuildCombinedModel(localHero, nullptr));
 assert(std::strcmp(Hook_GetItemModel(nullptr, 10), "original") == 0 && lastVariant == 10);
 throwFromBuilder = true;
 try { Hook_BuildCombinedModel(localHero, nullptr); assert(false); } catch(const std::runtime_error&) {}
 assert(std::strcmp(Hook_GetItemModel(nullptr, 11), "original") == 0);
 throwFromBuilder = false;
 activeClient = nullptr;
 assert(Hook_BuildCombinedModel(localHero, nullptr));
 BuildCombinedModel_o = nullptr;
 assert(!Hook_BuildCombinedModel(localHero, nullptr));
 GetItemModel_o = nullptr;
 assert(Hook_GetItemModel(nullptr, 0) == nullptr);
}
'''
    (temp / 'test.cpp').write_text(source)
    subprocess.run(['cl', '/nologo', '/std:c++17', '/EHsc', '/I' + str(temp),
                    'test.cpp', '/Fe:test.exe'], cwd=temp, check=True, capture_output=True)
    subprocess.run([str(temp / 'test.exe')], cwd=temp, check=True)
print('PASS: ordinary lookups, other heroes, nested scopes, exception cleanup, and missing hooks/client')

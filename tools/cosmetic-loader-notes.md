# Cosmetic resource loading

Verified offline against the installed Windows binaries on 2026-09-13.
Runtime visual verification is still required.

* `ResourceSystem013` is returned by resourcesystem.dll's CreateInterface factory.
  Factory RVA 0x138C0 returns the singleton at 0x81670, whose primary vtable is
  initialized to RVA 0x5FAC8.
* Vtable offset 0x140 calls RVA 0x16720 with `(system, ResourcePath*, label)`.
  This builds a loading manifest, obtains a binding, releases the temporary
  manifest, and returns the binding. The existing client call at 0x22E7AFC uses
  the same ABI with the label `review_load`.
* Vtable offset 0x188 queries binding state; 3 is resident, as checked by the
  engine's skeleton setter. Do not install a nonresident binding.
* Client RVA 0x3A1DC80 initializes a ResourcePath from a UTF-8 model path. Its
  layout is a string builder at 0, 200 inline bytes at 8, and hashes at 0xD0 and
  0xD8. Inputs are limited to 199 bytes to stay in inline storage.
* The new code checks the loader prologue at runtime and uses a unique pattern
  for the initializer. An unsupported build returns no binding.
* Each apply pass loads at most one missing model. Failed requests have a
  ten-second cooldown. A load can block briefly while the engine reads assets.
* `setModel=1` now requires read-back of the wearable's actual binding.

Run `verify_cosmetic_loader.py <Dota game directory>` for offline ABI checks.
This does not verify engine thread behavior, rendered appearance, or teardown.

The logged crash at client RVA 0x1833AAF is an indexed entity-handle-array read,
not a string-pool free. Its cause remains undetermined; the new loader does not
claim to fix it.

## Combined-model integration

The later Doom log shows the selected wearable handle changing while the hero
still renders `models/heroes/doom/doom_c_1.vmdl`. The combined-model builder at
client RVA 0x1ACD250 reads an item-view pointer list (count at 0, data at 8) and
calls the econ model-path getter at 0x33289B0, using the item definition at +8.
The getter result feeds the ResourcePath initializer at builder offset 0x2E6.

The implementation hooks both entries. A thread-local scope restricts item-path
replacement to synchronous calls from the builder, and a published hero pointer
and definition-to-path map restrict it to the local hero. Returned path strings
belong to the immutable catalog. No inventory definitions are rewritten.

After all selected resources are available, Apply publishes the replacements and
calls the builder with the wearable item views. Changed selections trigger a new
request. Rejected builds retry after ten seconds. Clearing selections or turning
Apply off requests a rebuild without substitutions. The log records substitutions
and builder acceptance separately from the later observed hero-binding change.
Neither a successful build nor a changed binding proves correct rendered pixels;
an in-game visual check is still required.

`m_CombinedModels` is not treated as a vector count. The earlier negative count
diagnostic was an invalid interpretation of this field and has been removed.

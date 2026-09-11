// Extract the FULL hero-cosmetic catalog from items_game.txt.
//
// Line-oriented on purpose: the file is ~1.7M lines and each item block carries
// big nested sub-blocks (portraits/visuals/styles) we do not care about, so we
// only capture scalar pairs at the item's own depth plus the used_by_heroes list.
const fs = require('fs');

const SRC = String.raw`C:\CheatDota2-nearfriend\Cheatdota2\.codex_tmp_items_game.txt`;
const OUT = process.argv[2] || 'cosmetic_catalog.tsv';

const lines = fs.readFileSync(SRC, 'utf8').split(/\r?\n/);

const KV = /^"([^"]+)"\s+"([^"]*)"\s*$/;   // "key"  "value"
const KEY_ONLY = /^"([^"]+)"\s*$/;          // "key"   (block follows)

// Scalars we want off each item / prefab.
const WANT = new Set([
  'name', 'prefab', 'item_slot', 'model_player', 'item_rarity',
  'image_inventory', 'item_name', 'item_description', 'hero',
]);

function parseSection(sectionName) {
  // Returns Map<blockKey, {scalars, heroes[]}> for a top-level section.
  const out = new Map();
  let depth = 0, inSection = false, sectionDepth = -1, sectionEntered = false;
  let cur = null, curKey = null, curDepth = -1, itemEntered = false;
  let inHeroes = false, heroesDepth = -1, heroesEntered = false;

  for (let i = 0; i < lines.length; i++) {
    const s = lines[i].trim();
    const opens = s.split('{').length - 1;
    const closes = s.split('}').length - 1;

    if (!inSection) {
      if (depth === 1 && KEY_ONLY.test(s) && s.replace(/"/g, '') === sectionName) {
        inSection = true;
        sectionDepth = depth;          // keys of this section sit at depth+1
      }
    } else {
      // An item key sits one level inside the section block.
      if (cur === null && depth === sectionDepth + 1 && KEY_ONLY.test(s)) {
        curKey = s.replace(/^"|"$/g, '');
        cur = { scalars: {}, heroes: [] };
        curDepth = depth;
        itemEntered = false;
      } else if (cur !== null) {
        if (inHeroes && depth === heroesDepth + 1) {
          const m = KV.exec(s) || KEY_ONLY.exec(s);
          // Only real hero keys. Without this guard a mis-tracked exit lets keys
          // from sibling blocks (visuals/asset_modifier/styles) land in the hero
          // list and they end up masquerading as heroes in the catalog.
          if (m && /^npc_dota_hero_/.test(m[1])) cur.heroes.push(m[1]);
        }
        // Only immediate children of the item block, so nested portraits/visuals
        // (which reuse names like "name") cannot pollute the item's own fields.
        if (!inHeroes && depth === curDepth + 1) {
          const kv = KV.exec(s);
          if (kv && WANT.has(kv[1])) {
            if (cur.scalars[kv[1]] === undefined) cur.scalars[kv[1]] = kv[2];
          } else {
            const ko = KEY_ONLY.exec(s);
            if (ko && ko[1] === 'used_by_heroes') { inHeroes = true; heroesDepth = depth; heroesEntered = false; }
          }
        }
      }
    }

    depth += opens - closes;

    if (inSection && depth > sectionDepth) sectionEntered = true;
    if (cur !== null && depth > curDepth) itemEntered = true;
    // Leave the hero list as soon as its block closes. This must use the depth
    // AFTER this line's braces, otherwise the list stays "open" past its closing
    // brace and swallows every following sibling block's keys.
    if (inHeroes && depth > heroesDepth) heroesEntered = true;
    if (inHeroes && heroesEntered && depth <= heroesDepth) inHeroes = false;

    // Item block finished when we fall back to the section's key level. Only
    // valid once we actually descended into it, otherwise the opening key line
    // (which has no braces of its own) would close the item immediately.
    if (cur !== null && itemEntered && depth <= curDepth) {
      out.set(curKey, cur); cur = null; curKey = null; inHeroes = false; itemEntered = false;
    }
    // Section finished - same guard.
    if (inSection && sectionEntered && depth <= sectionDepth) break;
  }
  return out;
}

const prefabs = parseSection('prefabs');
const items = parseSection('items');
console.log('prefabs parsed:', prefabs.size);
console.log('items parsed  :', items.size);

// Resolve a field through the prefab chain (prefab can list several, space-sep).
function resolve(entry, field, seen = 0) {
  if (!entry) return '';
  if (entry.scalars[field]) return entry.scalars[field];
  if (seen > 6) return '';
  const pf = entry.scalars['prefab'];
  if (!pf) return '';
  for (const name of pf.split(/\s+/)) {
    const v = resolve(prefabs.get(name), field, seen + 1);
    if (v) return v;
  }
  return '';
}
function resolveHeroes(entry, seen = 0) {
  if (!entry) return [];
  if (entry.heroes.length) return entry.heroes;
  if (seen > 6) return [];
  const pf = entry.scalars['prefab'];
  if (!pf) return [];
  for (const name of pf.split(/\s+/)) {
    const v = resolveHeroes(prefabs.get(name), seen + 1);
    if (v.length) return v;
  }
  return [];
}

let withModel = 0, withHero = 0, withSlot = 0, emitted = 0;
const heroTally = new Map();
const slotTally = new Map();
const rows = [];
const rowSeen = new Set();

for (const [defindex, entry] of items) {
  if (!/^\d+$/.test(defindex)) continue;
  const model = resolve(entry, 'model_player');
  if (!model) continue;
  withModel++;

  const heroes = resolveHeroes(entry);
  if (heroes.length) withHero++;
  const slot = resolve(entry, 'item_slot');
  if (slot) withSlot++;

  const display = entry.scalars['name'] || '';
  const rarity = resolve(entry, 'item_rarity') || '';
  const image = resolve(entry, 'image_inventory') || '';

  // One row per hero the item can be worn by; items with no hero are kept under
  // an empty hero so nothing is silently dropped from the catalog.
  const list = heroes.length ? [...new Set(heroes)] : [''];
  for (const h of list) {
    const hero = h.replace(/^npc_dota_hero_/, '');
    const key = hero + '|' + defindex;
    if (rowSeen.has(key)) continue;
    rowSeen.add(key);
    rows.push([hero, slot, defindex, display, display, rarity, model, image].join('\t'));
    emitted++;
    heroTally.set(hero, (heroTally.get(hero) || 0) + 1);
    slotTally.set(slot || '(none)', (slotTally.get(slot || '(none)') || 0) + 1);
  }
}

console.log('items with model_player :', withModel);
console.log('  of those with hero    :', withHero);
console.log('  of those with slot    :', withSlot);
console.log('rows emitted (hero x item):', emitted);
console.log('distinct heroes:', heroTally.size);

const topSlots = [...slotTally.entries()].sort((a, b) => b[1] - a[1]);
console.log('\nslots:');
for (const [s, n] of topSlots) console.log(`  ${s.padEnd(18)} ${n}`);

fs.writeFileSync(OUT, 'hero\tslot\tdefindex\tname\tdisplay\trarity\tmodel\timage\n' + rows.join('\n') + '\n', 'utf8');
console.log('\nwrote', OUT);

const fv = [...heroTally.entries()].filter(([h]) => h === 'faceless_void');
console.log('faceless_void rows:', fv.length ? fv[0][1] : 0);

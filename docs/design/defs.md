# Def database

Code: `ColonySim/Defs.h`, `Private/Defs.cpp`. Tests: `Tests/DefsTests.cpp`. Task D1.

## What it does

`DefDatabase::loadDirectory("data", db, errors)` reads every `data/defs/<Kind>/*.json` (the folder
is the kind) and `data/tuning.json`, and builds one read-only database. It follows the same rules
as `tools/check_defs.py` (valid JSON array, unique `defName` per kind, parent exists in the same
kind, no cycles), but reports every problem in `errors` and builds nothing if there is one.

- **Inheritance.** `parent` merges the parent's fields under the child's. Objects merge key by
  key, arrays and plain values are replaced. `abstract: true` defs are inherited from but get no id
  and cannot be looked up. `abstract` is not inherited.
- **Ids.** Every concrete def gets a dense `DefId` (0, 1, 2...). Order: kinds by name, files by
  name, then position in the file, so the same files always give the same ids, whatever order they
  are listed in. Ids are for fast lookups inside one running game. Saves store `defName`.
- **Reading fields.** `Def::number/integer/flag/text("stats.maxHitPoints")` use dotted paths (a
  number picks an array element: `degreeDatas.0.label`) and
  return a fallback when the field is absent. `count`, `keys`, `textAt`, `numberAt` cover arrays
  and objects. The JSON library stays inside `Defs.cpp`, so no other file needs it.
- **No exceptions.** The JSON library is built with `JSON_NOEXCEPTION`; bad files give error
  lines, never a crash.

## tuning.json

```json
{
  "defs":   { "Things": { "stats.maxHitPoints": 1.5 } },
  "values": { "work.speed": 0.8 }
}
```

- `defs`: per kind, a dotted field path and a multiplier. Applied to every concrete def of that
  kind that has the field, after inheritance. Whole numbers stay whole numbers (rounded).
- `values`: named global multipliers, read with `db.tuning("work.speed")` (1.0 if not set).
- An empty `{}` is valid.

## Not done here

- Hot reload (needs the database to be swappable while the sim runs; later).
- The sim holds the `DefDatabase` by `shared_ptr` and makes things from it (S1, `design/things.md`).
- Checking field names and cross-references between kinds stays in `check_defs.py` until the
  importer (D2/D3) fixes the field list.

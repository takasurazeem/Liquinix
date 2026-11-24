---
name: liquinix-translations
description: How user-visible text and translations work in Liquinix (the Tr() system). Use whenever adding or changing any text the user can see (labels, menus, tooltips, alerts, notifications, settings pages, installer text), format strings with %d or %ls, dates, month or day names, or any file named src/comum/idioma*.h or idioma_*.inc. Also use when a string shows up untranslated or in Portuguese for English users.
---

# Translations in Liquinix

The interface comes in 12 languages: pt, en (the default), es, fr, de, it, ja, zh, ko, ru, pl, tr. They are
chosen with `idioma=<code>` in `%APPDATA%\Shell\ajustes.txt` (System Settings > General > Language). The system
lives in `src/comum/idioma.h`, which explains itself at the top of the file.

## The rule: write Portuguese, wrap it in Tr()

Source strings are written **in Brazilian Portuguese** and passed through `Tr()`. The Portuguese text is the key.

```cpp
Tr(L"Abrir")                          // "Open" in en, "Ouvrir" in fr, "Abrir" in pt
swprintf(buf, n, Tr(L"%d itens"), count);
```

This feels backwards if you think in English, but it is how every existing string works. Mixing in English keys
would leave those strings untranslated in every language. So even if you think in English, write the Portuguese
string, then add the English translation.

## Adding a string

1. Write it in Portuguese inside `Tr(L"...")`.
2. Add the pair to `src/comum/idioma_en.inc`, in the same format as its neighbours:
   `{ L"texto em português", L"English text" },`. English is required. A missing key falls back to English, and
   then to the Portuguese text itself.
3. Add it to the other `idioma_<code>.inc` files when you can translate it well. A missing entry shows in
   English, which is acceptable; a bad translation is worse than none.
4. **Keep format specifiers identical.** Every translation has the same `%d`, `%ls`, `%.1f` in the **same
   order** as the key. A mismatch is a crash or garbage text, not a cosmetic bug.
5. Check with `python3 -I tools/idioma-conferir.py`, which gives a summary per language: missing keys and format
   mismatches. Add a code (`python3 -I tools/idioma-conferir.py fr`) to list the missing keys for one language.

## Dates and names

Use `idioma::Date()`, `DateTime()`, `MonthName()` and `DayName()`. They format through Windows in the chosen
locale. Never build dates from fixed month tables or patterns like `"%d de %ls"`; they're wrong in most of the 12
languages.

`idioma::Code()` and `idioma::Locale()` give the language code and locale for APIs and DirectWrite.
`idioma::En()` / `Pt()` still exist for old code paths.

## Changing language at runtime

Apps read the language once. The shell calls `idioma::Reload()` when System Settings broadcasts
`ShellAjustesMudaram`, then redraws. Don't cache translated strings across that message; translate at draw time.

## Not translated

- Log lines (`Log(L"...")`): keep them in plain Portuguese or English, no `Tr()`.
- Settings keys, file names and registry values.
- Apple product names that are the same in every language (Finder, Spotlight, Launchpad), unless an existing
  translation differs.

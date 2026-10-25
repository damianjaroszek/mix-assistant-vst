# Mix Assistant

Asystent miksu dla FL Studio (VST3): auto gain staging i balans każdego kanału
w kontekście całego miksu, z porównaniem do referencji (również po stemach).
Gatunki docelowe: house, tech house, garage, trance.

Koncepcja, architektura i plan faz: [rewizja koncepcji](https://claude.ai/artifact/5erMybFtPRsZ84QrMGyBFY).

## Co jest w środku

| Część | Gdzie | Co robi |
| --- | --- | --- |
| **Agent** | `plugins/agent` | Wtyczka na końcu każdego kanału. Ma rolę (stopa, bas, wokal...) i priorytet. „Naucz poziom” mierzy fragment utworu (RMS albo LUFS, true peak, widmo w pasmach ERB), kończy sam po ustawionym czasie i ustawia trim do poziomu docelowego (domyślnie -18 dBFS RMS), nigdy nie przekraczając limitu szczytu (domyślnie -1 dBTP). Pokazuje widmo ważone krzywą jednakowej głośności (ISO 226), czyli tak, jak słyszy je ucho; nauczony profil zapisuje się w projekcie. |
| **Mózg** | `plugins/brain` | Wtyczka na końcu mastera. Mierzy sumę: LUFS (integrated, short-term), true peak, PLR, widmo w 31 pasmach 1/3 oktawy i sprawdza zapas na mastering. |
| **Rdzeń DSP** | `core` | Pomiary wspólne dla wtyczek i laboratorium, bez zależności od JUCE: LUFS wg ITU-R BS.1770-4, true peak (4x oversampling), RMS z bramką, widmo w pasmach 1/3 oktawy albo ERB, krzywe jednakowej głośności ISO 226:2003. |
| **Laboratorium** | `lab` | `ma-lab`: ta sama analiza na stemach WAV wyeksportowanych z FL, plus analiza ich sumy. |
| **Testy** | `tests` | Testy rdzenia, m.in. przypadki z EBU Tech 3341. |

Komunikacja Agentów z Mózgiem przychodzi w fazie 2.

## Pobranie gotowych wtyczek

Każdy push i pull request buduje wtyczki na Windows w GitHub Actions.
W zakładce **Actions** wybierz ostatni udany przebieg „Build” i pobierz artefakt
`mix-assistant-windows`. W środku są:

- `Mix Assistant Agent.vst3` i `Mix Assistant Brain.vst3`: skopiuj do
  `C:\Program Files\Common Files\VST3`, potem w FL Studio *Options → Manage plugins → Find more plugins*.
- `ma-lab.exe`: laboratorium offline.

## Laboratorium offline

```
ma-lab [--target-rms -18] [--json] kick.wav bass.wav lead.wav
```

Dla każdego pliku: LUFS integrated, najgłośniejsze 3 s (LUFS-S), true peak, RMS z bramką,
crest factor i trim potrzebny do poziomu docelowego. Przy kilku plikach dochodzi wiersz
`SUM (mix)`, czyli analiza sumy stemów. `--json` dodaje widmo w 31 pasmach.

## Budowanie lokalnie

Wymagane: CMake 3.22+, kompilator C++17 (na Windows Visual Studio 2022). JUCE pobiera się automatycznie.

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Sam rdzeń, laboratorium i testy (szybko, bez JUCE): `cmake -S . -B build -DMA_BUILD_PLUGINS=OFF`.

## Licencja

Na razie projekt prywatny. JUCE jest używany na licencji JUCE (darmowa przy małych przychodach)
albo AGPLv3; przed ewentualną sprzedażą trzeba sprawdzić aktualne warunki.

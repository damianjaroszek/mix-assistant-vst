# Mix Assistant

Asystent miksu dla FL Studio (VST3): auto gain staging i balans każdego kanału
w kontekście całego miksu, z porównaniem do referencji (również po stemach).
Gatunki docelowe: house, tech house, garage, trance.

Koncepcja, architektura i plan faz: [rewizja koncepcji](https://claude.ai/artifact/5erMybFtPRsZ84QrMGyBFY).

## Co jest w środku

| Część | Gdzie | Co robi |
| --- | --- | --- |
| **Agent** | `plugins/agent` | Wtyczka na końcu każdego kanału. Ma rolę (stopa, bas, wokal...) i priorytet. „Naucz poziom” mierzy fragment utworu (RMS albo LUFS, true peak, widmo w pasmach ERB), kończy sam po ustawionym czasie i ustawia trim do poziomu docelowego (domyślnie -18 dBFS RMS), nigdy nie przekraczając limitu szczytu (domyślnie -1 dBTP). Pokazuje widmo ważone krzywą jednakowej głośności (ISO 226), czyli tak, jak słyszy je ucho; nauczony profil zapisuje się w projekcie. Wysyła swoje pomiary do Mózgu; nazwa kanału to nazwa ścieżki z FL albo własna, wpisana w oknie. Ma 4-pasmowy EQ „Korekcja od Mózgu”, który ustawia „Zastosuj” w Mózgu (można go wyłączyć albo wyzerować). Ma wejście sidechain i sekcję „Ustępowanie”: kanał ścisza się tylko wtedy, gdy gra sygnał z sidechaina (np. bas w paśmie stopy na każdym uderzeniu, albo cały kanał), z czasem powrotu dopasowanym do tempa (1/32 do 1/4). Mierzy też fazę w basie względem sidechaina i ma przełącznik odwrócenia polaryzacji. Sekcja „Atak” podkreśla tylko początki dźwięków (klik stopy) w paśmie, które słychać na małych głośnikach. Sekcja „Harmoniczne” dodaje alikwoty treści poniżej ustawionej częstotliwości (np. sub basu), żeby była słyszalna na telefonie i laptopie; ilość to poziom alikwotów względem źródła, niezależnie od głośności kanału. Sekcja „Przester” to saturacja z 4x nadpróbkowaniem (bez aliasingu) w pięciu trybach: Taśma, Lampa, Miękki clip, Twardy clip, Fałdowanie; drive liczy się od poziomu po gain stagingu, więc znaczy to samo na każdym kanale, a głośność zostaje wyrównana, zmienia się barwa. „Celowy przester” mówi, że kanał ma brzmieć brudno: nauka poziomu nie pilnuje wtedy szczytów, a Mózg nie ostrzega o przesterowaniu. Przy „Naucz poziom” Agent rozpoznaje też instrument (stopa, bas, perkusja, wokal, lead, pad) i jeśli kanał nie miał roli, ustawia ją razem z typowym priorytetem; rola wybrana ręcznie zawsze wygrywa. Ta sama nauka zapisuje dynamikę kanału (poziomy cichych, typowych i głośnych fragmentów) i typową nutę basu. Kolumna „Inżynier miksu” ma narzędzia, które ustawia Mózg („Ustaw cały miks”) albo Ty ręcznie: „Porządki” (HPF 24 dB/okt, mono poniżej podanej częstotliwości bez przesunięcia fazy, jedno szerokie cięcie mułu), „Kompresor” (RMS, miękkie kolano, wskaźnik redukcji), „Clipper” (4x nadpróbkowanie, ścina tylko szczyty ponad sufitem) i „Wyrównanie nut basu” (śledzi nutę basu i podnosi te, które ucho słyszy ciszej, a obniża wyższe, względem typowej nuty linii). |
| **Mózg** | `plugins/brain` | Wtyczka na końcu mastera. Mierzy sumę: LUFS (integrated, short-term), true peak, PLR, widmo w 31 pasmach 1/3 oktawy, korelację fazy (całość i dół poniżej 120 Hz). Widzi wszystkich Agentów: tabela kanałów, nałożone widma (tak, jak słyszy ucho), pasek „tłoku” w pasmach, lista kolizji („Pad zasłania Stopę w 200–600 Hz”) i raport poprawności. Kolizje liczy z tego, co gra teraz (uśrednione z ok. 3 s) albo z nauczonych profili. „Zaproponuj” liczy poprawki dla całego miksu naraz (cięcia EQ na kanale mniej ważnym, gain wszystkich kanałów, gdy brakuje zapasu na mastering), każdą z wyjaśnieniem. „Zastosuj zaznaczone” wysyła je do Agentów, „Cofnij” przywraca poprzednie ustawienia, „Przed / po” porównuje obie wersje z wyrównaną głośnością. Widok „Balans tonalny” porównuje uśrednione widmo miksu z korytarzem celu zbudowanym z referencji (nagrywanych w Mózgu jak „Capture” w Tonal Balance) i pokazuje dziury oraz nadmiary; „Zaproponuj” wypełnia dziury podbiciem kanału, który ma tam treść (bez tworzenia nowych kolizji), tnie nadmiar na kanale najmniej ważnym, a gdy nic w miksie nie gra w danym paśmie, podpowiada saturację albo brakujący element aranżu. Dla stopy i basu proponuje ustępowanie dynamiczne zamiast stałego cięcia (pasmo, głębokość i próg z pomiaru, powrót 1/16 z tempa projektu), a gdy bas znosi się ze stopą w przeciwfazie, odwrócenie polaryzacji; raport „Stopa i bas w czasie” pokazuje, czy sidechain jest podłączony i ile kanał ustępuje. Widok „Translacja” pokazuje, ile każdy kanał traci albo zyskuje względem reszty miksu na telefonie, laptopie, słuchawkach dousznych, w samochodzie i w klubie (mono); „Odsłuch” u góry pozwala posłuchać miksu przez taką symulację (pomiary dalej widzą prawdziwy miks, a ustawienie nie zapisuje się w projekcie). Dla kanału, który na małych głośnikach słabnie (od ok. -6 dB na telefonie), „Zaproponuj” analizuje cały miks: najpierw robi mu miejsce cięciem u mniej ważnego kanału w paśmie, które gra telefon, potem dobiera własny krok według rodzaju dźwięku (atak dla perkusji, harmoniczne dla niskich, obecność dla reszty), każdy w najmniejszej ilości, która wystarcza, i bez spychania innych kanałów, a dziurę w balansie tonalnym, w której nic nie gra, wypełnia harmonicznymi kanału grającego oktawę niżej. „Tryb adaptacyjny” (domyślnie wyłączony) zamienia propozycję w autopilota: Mózg dalej liczy solver na tym, co gra, i przesuwa korekcje EQ w Agentach najwyżej o 0,25 dB na sekundę w jego limitach; nowe pasmo zaczyna dopiero, gdy solver prosi o nie kilka razy z rzędu, stare puszcza po kilku zgodnych odczytach, w przerwach i przez 4 s po nich nic nie rusza, a kolizję, którą kanał już rozwiązuje ustępowaniem, zostawia ustępowaniu. Wyłączenie zamraża to, co jest ustawione. „Ustaw cały miks” to inżynier miksu: ustawia każdy kanał, zawsze w kontekście całej reszty. HPF idzie pod treść, która się liczy (wyżej, gdy dół należy do stopy i basu; stopa i bas dostają tylko subsonic), cały dół jest w mono poniżej 120 Hz, a gdy trzy kanały lub więcej tłoczą się w 200–500 Hz, mniej ważne oddają trochę mułu. Kompresja zależy od roli i od miksu: perkusja zostaje bez kompresora, kanał już ściśnięty u źródła nie dostaje więcej, kanał, którego ciche fragmenty giną pod resztą, jest trzymany ciaśniej, a kompresor nigdy nie schodzi głębiej, niż rola znosi (cichszy takt to aranżacja, nie nierówne granie). Clipper zdejmuje tylko nadmiar szczytów ponad to, co rola potrzebuje, a bas dostaje wyrównanie nut. Na tym Mózg liczy zwykłą propozycję (kolizje, stopa i bas, translacja) i w widoku „Werdykt” pisze dla każdego kanału, co ustawił, dlaczego i co jeszcze wymaga uwagi. Limiterów nie ma: mastering robi się osobno (np. w Ozone). |
| **Rdzeń DSP** | `core` | Pomiary wspólne dla wtyczek i laboratorium, bez zależności od JUCE: LUFS wg ITU-R BS.1770-4, true peak (4x oversampling), RMS z bramką, widmo w pasmach 1/3 oktawy albo ERB, krzywe jednakowej głośności ISO 226:2003, model maskowania między kanałami, korelacja fazy, kanał danych Agent ↔ Mózg, solver poprawek, EQ kanału, dynamiczne ustępowanie sterowane sidechainem, generator harmonicznych, modele systemów odsłuchu, saturacja z nadpróbkowaniem (filtry półpasmowe IIR), rozpoznawanie ról kanałów, łańcuch kanału (porządki, kompresor, clipper, wyrównanie nut basu) i planer, który ustawia go dla wszystkich kanałów naraz. |
| **Laboratorium** | `lab` | `ma-lab`: ta sama analiza na stemach WAV wyeksportowanych z FL, plus analiza ich sumy; buduje też cele balansu tonalnego z plików WAV. |
| **Testy** | `tests` | Testy rdzenia, m.in. przypadki z EBU Tech 3341. |

Agenci i Mózg rozmawiają przez mały plik w pamięci współdzielonej
(`%TEMP%\MixAssistant\session-v8.bin`), więc działa to także, gdy FL uruchamia wtyczki
w osobnych procesach (tryb bridged). Idą nim tylko wolne dane, kilka razy na sekundę.
Kolizja liczy się tylko wtedy, gdy mniej ważny kanał (wyższa liczba priorytetu) zasłania
ważniejszy albo równie ważny. Agent powinien stać na końcu łańcucha kanału, a suwak
kanału w mikserze FL najlepiej zostawić na 0 dB: Mózg nie widzi suwaków.

### Cele balansu tonalnego

Cel to zestaw referencji (utworów, które brzmią tak, jak chcesz). W Mózgu: „Balans tonalny” →
„Nowy cel” → puść referencję na masterze (np. na osobnym kanale bez Agenta, suwak miksu bez
efektów) → „Nagraj referencję” → po co najmniej 10 s „Zakończ nagrywanie”. Im więcej referencji,
tym trafniejszy korytarz (mediana i rozrzut między nimi). Cele zapisują się w
`%APPDATA%\MixAssistant\targets` i są wspólne dla wszystkich projektów. Ten sam plik zbuduje
`ma-lab --make-target "House" --out House.matarget ref1.wav ref2.wav ...`.

### Stopa i bas: ustępowanie przez sidechain

Stałe cięcie basu pod stopą zabiera dół także między uderzeniami. Ustępowanie ścisza bas
w paśmie stopy tylko na czas uderzenia. Żeby Agent basu słyszał stopę:

1. W mikserze FL zaznacz kanał stopy.
2. Na kanale basu kliknij prawym przyciskiem strzałkę wysyłki (na dole paska) i wybierz
   „Sidechain to this track”.
3. W oknie Agenta basu otwórz ustawienia wtyczki FL (ikona koła zębatego, karta „Processing”)
   i ustaw wejście sidechain na kanał stopy.

Status w sekcji „Ustępowanie” pokaże poziom sidechaina, ile kanał ustępuje i fazę w basie.
Potem „Zaproponuj” w Mózgu dobierze pasmo, głębokość, próg i czas powrotu.

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

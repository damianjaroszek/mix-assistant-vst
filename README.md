# Mix Assistant

Asystent miksu dla FL Studio (VST3): auto gain staging i balans każdego kanału
w kontekście całego miksu, z porównaniem do referencji (również po stemach).
Gatunki docelowe: house, tech house, garage, trance.

Koncepcja, architektura i plan faz: [rewizja koncepcji](https://claude.ai/artifact/5erMybFtPRsZ84QrMGyBFY).

## Co jest w środku

| Część | Gdzie | Co robi |
| --- | --- | --- |
| **Agent** | `plugins/agent` | Wtyczka na końcu każdego kanału. Ma rolę (stopa, bas, wokal...) i priorytet. „Naucz poziom” mierzy fragment utworu (RMS albo LUFS, true peak, widmo w pasmach ERB), kończy sam po ustawionym czasie i ustawia trim do poziomu docelowego (domyślnie -18 dBFS RMS), nigdy nie przekraczając limitu szczytu (domyślnie -1 dBTP). Pokazuje widmo ważone krzywą jednakowej głośności (ISO 226), czyli tak, jak słyszy je ucho; nauczony profil zapisuje się w projekcie. Wysyła swoje pomiary do Mózgu; nazwa kanału to nazwa ścieżki z FL albo własna, wpisana w oknie. Ma 4-pasmowy EQ „Korekcja od Mózgu”, który ustawia „Zastosuj” w Mózgu (można go wyłączyć albo wyzerować). Ma wejście sidechain i sekcję „Ustępowanie”: kanał ścisza się tylko wtedy, gdy gra sygnał z sidechaina (np. bas w paśmie stopy na każdym uderzeniu, albo cały kanał), z czasem powrotu dopasowanym do tempa (1/32 do 1/4). Mierzy też fazę w basie względem sidechaina i ma przełącznik odwrócenia polaryzacji. Sekcja „Atak” podkreśla tylko początki dźwięków (klik stopy) w paśmie, które słychać na małych głośnikach. Sekcja „Harmoniczne” dodaje alikwoty treści poniżej ustawionej częstotliwości (np. sub basu), żeby była słyszalna na telefonie i laptopie; ilość to poziom alikwotów względem źródła, niezależnie od głośności kanału. Sekcja „Przester” to saturacja z 4x nadpróbkowaniem (bez aliasingu) w pięciu trybach: Taśma, Lampa, Miękki clip, Twardy clip, Fałdowanie; drive liczy się od poziomu po gain stagingu, więc znaczy to samo na każdym kanale, a głośność zostaje wyrównana, zmienia się barwa. „Celowy przester” mówi, że kanał ma brzmieć brudno: nauka poziomu nie pilnuje wtedy szczytów, Mózg nie ostrzega o przesterowaniu i nie zmienia ustawień przesteru. Przy „Naucz poziom” Agent rozpoznaje też instrument (stopa, bas, perkusja, wokal, lead, pad) i jeśli kanał nie miał roli, ustawia ją razem z typowym priorytetem; rola wybrana ręcznie zawsze wygrywa. Ta sama nauka zapisuje dynamikę kanału (poziomy cichych, typowych i głośnych fragmentów) i typową nutę basu. Kolumna „Inżynier miksu” ma narzędzia, które ustawia Mózg („Ustaw cały miks”) albo Ty ręcznie: „Porządki” (HPF 24 dB/okt, mono poniżej podanej częstotliwości bez przesunięcia fazy, jedno szerokie cięcie mułu i do trzech wąskich cięć rezonansów), „Kompresor” (RMS, miękkie kolano, wskaźnik redukcji), „Clipper” (4x nadpróbkowanie, ścina tylko szczyty ponad sufitem) „Faza w pasmach (mid-side)” (podgląd fazy kanału w pasmach, jak Correlometer, i do czterech dzwonów EQ tylko na sygnale side) i „Wyrównanie nut basu” (śledzi nutę basu i podnosi te, które ucho słyszy ciszej, a obniża wyższe, względem typowej nuty linii). |
| **Mózg** | `plugins/brain` | Wtyczka na końcu mastera. Mierzy sumę: LUFS (integrated, short-term), true peak, PLR, widmo w 31 pasmach 1/3 oktawy, korelację fazy (całość i dół poniżej 120 Hz). Widzi wszystkich Agentów: tabela kanałów, nałożone widma (tak, jak słyszy ucho), pasek „tłoku” w pasmach, lista kolizji („Pad zasłania Stopę w 200–600 Hz”) i raport poprawności. Kolizje liczy z tego, co gra teraz (uśrednione z ok. 3 s) albo z nauczonych profili. „Zaproponuj” liczy poprawki dla całego miksu naraz (cięcia EQ na kanale mniej ważnym, gain wszystkich kanałów, gdy brakuje zapasu na mastering), każdą z wyjaśnieniem. „Zastosuj zaznaczone” wysyła je do Agentów, „Cofnij” przywraca poprzednie ustawienia, „Przed / po” porównuje obie wersje z wyrównaną głośnością. Widok „Balans tonalny” porównuje uśrednione widmo miksu z korytarzem celu zbudowanym z referencji (nagrywanych w Mózgu jak „Capture” w Tonal Balance) i pokazuje dziury oraz nadmiary; „Zaproponuj” wypełnia dziury podbiciem kanału, który ma tam treść (bez tworzenia nowych kolizji), tnie nadmiar na kanale najmniej ważnym, a gdy nic w miksie nie gra w danym paśmie, podpowiada saturację albo brakujący element aranżu. Dla stopy i basu proponuje ustępowanie dynamiczne zamiast stałego cięcia (pasmo, głębokość i próg z pomiaru, powrót 1/16 z tempa projektu), a gdy bas znosi się ze stopą w przeciwfazie, odwrócenie polaryzacji; raport „Stopa i bas w czasie” pokazuje, czy sidechain jest podłączony i ile kanał ustępuje. Gdy kanał zasłania stopę wyżej (np. lead w paśmie kliku), też ustępuje jej dynamicznie zamiast stałego cięcia. Lista kolizji liczy się z widma uśrednionego z kilku sekund, więc kolizji obsłużonej przez ustępowanie dalej nie znika; jest wtedy zielona z dopiskiem „ustępuje na każde uderzenie stopy”, a kolizja, przy której cięcie EQ doszło do limitu, ma dopisek, że reszta to sprawa poziomu kanału. Widok „Translacja” pokazuje, ile każdy kanał traci albo zyskuje względem reszty miksu na telefonie, laptopie, słuchawkach dousznych, w samochodzie i w klubie (mono); „Odsłuch” u góry pozwala posłuchać miksu przez taką symulację (pomiary dalej widzą prawdziwy miks, a ustawienie nie zapisuje się w projekcie). Dla kanału, który na małych głośnikach słabnie (od ok. -6 dB na telefonie), „Zaproponuj” analizuje cały miks: najpierw robi mu miejsce cięciem u mniej ważnego kanału w paśmie, które gra telefon, potem dobiera własny krok według rodzaju dźwięku (atak dla perkusji, harmoniczne dla niskich, obecność dla reszty), każdy w najmniejszej ilości, która wystarcza, i bez spychania innych kanałów, a dziurę w balansie tonalnym, w której nic nie gra, wypełnia harmonicznymi kanału grającego oktawę niżej. „Tryb adaptacyjny” (domyślnie wyłączony) zamienia propozycję w autopilota: Mózg dalej liczy solver na tym, co gra, i przesuwa korekcje EQ w Agentach najwyżej o 0,25 dB na sekundę w jego limitach; nowe pasmo zaczyna dopiero, gdy solver prosi o nie kilka razy z rzędu, stare puszcza po kilku zgodnych odczytach, w przerwach i przez 4 s po nich nic nie rusza, a kolizję, którą kanał już rozwiązuje ustępowaniem, zostawia ustępowaniu. Wyłączenie zamraża to, co jest ustawione. „Ustaw cały miks” to inżynier miksu: ustawia każdy kanał, zawsze w kontekście całej reszty. HPF idzie pod treść, która się liczy (wyżej, gdy dół należy do stopy i basu; stopa i bas dostają tylko subsonic), cały dół jest w mono poniżej 120 Hz, a gdy trzy kanały lub więcej tłoczą się w 200–500 Hz, mniej ważne oddają trochę mułu. Rezonanse (wąskie szczyty, które stoją w miejscu bez względu na to, co gra, np. dzwoniący werbel albo pudełkowy pokój) Agent znajduje przy nauce; harmoniczne melodii przesuwają się z nutami, więc się nie liczą. Mózg tnie rezonans tylko wtedy, gdy miks go słyszy, głębiej, gdy zasłania ważniejszy kanał, a na kanałach tonalnych tylko szersze garby (ostrość, pudełkowatość), nigdy pojedyncze harmoniczne. Kompresja zależy od roli i od miksu: perkusja zostaje bez kompresora, kanał już ściśnięty u źródła nie dostaje więcej, kanał, którego ciche fragmenty giną pod resztą, jest trzymany ciaśniej, a kompresor nigdy nie schodzi głębiej, niż rola znosi (cichszy takt to aranżacja, nie nierówne granie). Clipper zdejmuje tylko nadmiar szczytów ponad to, co rola potrzebuje, a bas dostaje wyrównanie nut. Saturację (faza 13) dostaje tylko partia za uboga w alikwoty: bas, którego prawie nie ma powyżej 150 Hz (sinusowy sub), wokal, lead albo pad o zbyt gładkim brzmieniu. Tryb zależy od roli (bas i lead: lampa, wokal i pad: taśma), a drive i miks Mózg dobiera, przepuszczając ton kanału przez prawdziwy saturator: bierze najłagodniejsze ustawienie, które wystarcza, i nigdy takie, którego nowe alikwoty zasłoniłyby ważniejszy kanał. Stopa i perkusja zostają przy clipperze i ataku. Gdy kanał ma zaznaczony „Celowy przester”, jego saturacja jest Twoja i Mózg jej nie rusza. „Ustaw cały miks” wyłącza też to, czego plan nie potrzebuje (np. kompresor zaznaczony ręcznie na perkusji). Na tym Mózg liczy zwykłą propozycję (kolizje, stopa i bas, translacja) i w widoku „Werdykt” pisze dla każdego kanału, co ustawił, dlaczego i co jeszcze wymaga uwagi. Stereo (faza 8) działa jak praca z Correlometerem, tylko na kanałach zamiast na sumie: nie poszerzamy na ślepo, pilnujemy fazy w każdym paśmie. Widok „Stereo” pokazuje fazę sumy w pasmach (na dole przeciwfaza, kropki to zaznaczony kanał), a raport „Faza w pasmach” mówi, które pasma wpadają w przeciwfazę i który kanał to robi. „Ustaw cały miks” zdejmuje side dokładnie w tym paśmie i tylko na tym kanale, a tam, gdzie cały miks jest prawie mono, daje szerokim partiom (pad, lead, efekty) najwyżej +1 dB side, jeśli kanał zostaje w fazie. Dół i tak jest w mono, a środek (mid), czyli suma mono, zostaje nietknięty. Referencje ze stemów (faza 9): widok „Referencje” porównuje cztery grupy miksu (perkusja, bas, wokal, reszta, z ról kanałów) ze stemami utworów referencyjnych, drop do dropu: poziom grupy względem całości, jej własną barwę w sześciu szerokich pasmach (sub, bas, dolny środek, środek, prezencja, powietrze) i szerokość. „Ustaw cały miks” przesuwa poziomy grup do korytarza referencji (najwyżej 3 dB na raz, głośność całości zostaje) i dodaje szerokie korekcje barwy na kanale grupy, który niesie dane pasmo. Referencje w innej tonacji niż Twój utwór są do niej przesuwane (faza 10), tonację Mózg rozpoznaje sam albo wybierasz ją ręcznie. Break porównuje z breakami referencji osobno (faza 12). Przed masteringiem (faza 11) podgląd mastera sprawdza, ile limiter musiałby ściąć przy docelowej głośności, a „Raport dla Ozone...” zapisuje wszystko w jednym pliku HTML. Limiterów nie ma: mastering robi się osobno (np. w Ozone). |
| **Rdzeń DSP** | `core` | Pomiary wspólne dla wtyczek i laboratorium, bez zależności od JUCE: LUFS wg ITU-R BS.1770-4, true peak (4x oversampling), RMS z bramką, widmo w pasmach 1/3 oktawy albo ERB, krzywe jednakowej głośności ISO 226:2003, model maskowania między kanałami, korelacja fazy, kanał danych Agent ↔ Mózg, solver poprawek, EQ kanału, dynamiczne ustępowanie sterowane sidechainem, generator harmonicznych, modele systemów odsłuchu, saturacja z nadpróbkowaniem (filtry półpasmowe IIR), rozpoznawanie ról kanałów, łańcuch kanału (porządki, kompresor, clipper, wyrównanie nut basu) i planer, który ustawia go dla wszystkich kanałów naraz, profile stemów referencji i rozpoznawanie tonacji. |
| **Laboratorium** | `lab` | `ma-lab`: ta sama analiza na stemach WAV wyeksportowanych z FL, plus analiza ich sumy; buduje też cele balansu tonalnego z plików WAV. |
| **Testy** | `tests` | Testy rdzenia, m.in. przypadki z EBU Tech 3341. |

Agenci i Mózg rozmawiają przez mały plik w pamięci współdzielonej
(`%TEMP%\MixAssistant\session-v9.bin`), więc działa to także, gdy FL uruchamia wtyczki
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

### Referencje ze stemów

Zamiast całego utworu referencyjnego wczytujesz jego stemy: perkusję, bas, wokal i resztę. Daje je
separator stemów w FL Studio (od 21.2, na klipie audio w playliście), Demucs albo serwis online. W Mózgu:
„Referencje” → „Nowy zestaw” (np. „Tech house”) → „Wczytaj stemy...” i zaznacz pliki jednego utworu.
Nazwy plików mówią, czym są: drums, bass, vocals, other (albo perkusja, bas, wokal, reszta); tytuł
utworu wspólny dla wszystkich plików jest pomijany. Dołóż też cały utwór (w nazwie „mix”), a Mózg sprawdzi,
czy stemy składają się na niego: słaba separacja dostaje ostrzeżenie, a stemy, które się nie
składają (brakuje któregoś albo są z innej wersji), nie są dodawane. Liczy się tylko najgłośniejsza
część (drop), więc intro i przejścia nie psują proporcji, a Twój miks Mózg mierzy tak samo: puść
jego drop przez co najmniej 8 s. Wszystko jest względem całości, więc głośność referencji nie ma
znaczenia. Im więcej utworów w zestawie, tym trafniejszy korytarz. Zestawy zapisują się obok celów
balansu tonalnego (`.mastems`); ten sam plik zbuduje
`ma-lab --add-stems "Tech house" --out "Tech house.mastems" drums.wav bass.wav vocals.wav other.wav mix.wav`.

Tonacja (faza 10): Mózg rozpoznaje tonację każdej referencji (z basu, wokalu i reszty, bez perkusji)
i Twojego utworu (z mastera, po ok. 20 s muzyki; zapamiętuje ją w projekcie) i pokazuje je w notacji
Camelot, np. „f-moll (4A)”. Profile basu, wokalu i reszty przesuwa do tonacji Twojego utworu (najkrótszą
drogą, najwyżej 6 półtonów; tonacje równoległe, np. a-moll i C-dur, nie są przesuwane), więc bas
referencji w a-moll nie udaje „za mało suba” przy Twoim f-moll. Przesuwa się tylko profil, nie dźwięk.
Jeśli znasz tonację, wybierz ją ręcznie pod listą zestawów; „Wykryj od nowa” zapomina to, co Mózg
usłyszał do tej pory. Zestawy zapisane przed fazą 10 nie mają widma do przesuwania: wczytaj ich stemy
jeszcze raz, żeby to działało.

Break (faza 12): poza dropem Mózg mierzy też break referencji, czyli część, w której perkusja jest co
najmniej 15 dB ciszej niż w dropie (potrzeba stemu perkusji i co najmniej 8 s takiej części).
Przyciski „Drop” i „Break” w widoku „Referencje” przełączają porównanie. Break Twojego miksu Mózg
zbiera sam, gdy po dropie stopa i perkusja milkną. Raport „Break jak w referencjach” mówi, które grupy
są w breaku za głośno albo za cicho, która gra, choć w referencjach milknie (np. bas), i czy break
jest tak samo cichszy od dropu jak w referencjach (za mała różnica: drop się nie wybija). To rada do
aranżu i automatyki głośności: „Ustaw cały miks” niczego w breaku nie zmienia. Zestawy zapisane przed
fazą 12 nie mają breaku: wczytaj ich stemy jeszcze raz.

### Przed masteringiem (Ozone)

„Cel mastera” w Mózgu to głośność, do której Ozone podciągnie master (domyślnie -8 LUFS). Mózg w tle robi
z miksu szybki podgląd mastera: podnosi go do tego celu przez limiter (sufit -1 dBFS, 5 ms wyprzedzenia) i mierzy,
ile limiter musi ściąć. Podgląd służy tylko do analizy i nie wychodzi na wyjście. Raport „Gotowość do głośnego
mastera” mówi, czy uderzenie przetrwa (do ok. 3 dB ścinania w głośnej części w porządku, ponad 6 dB master zgniecie
stopę), czy limiter w ogóle dobije do celu i który kanał daje najwyższe szczyty. „Raport dla Ozone...” zapisuje
plik HTML do wglądu przy masteringu. Na górze jest lista „Dla Ozone”: co najpierw poprawić w miksie, cel i podbicie
Maximizera, szerokie korekty EQ względem celu balansu tonalnego, uwagi do Imagera i referencje. Niżej są głośność
i zapas, podgląd mastera, cały raport poprawności, stereo, proporcje grup względem referencji (z tonacją) i łańcuch
każdego kanału.

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

### Stopa a tonacja

Przy „Naucz poziom” Agent stopy mierzy, na jakiej nucie ustala się ogon uderzenia (150–300 ms po
ataku). Raport „Stopa a tonacja” w Mózgu porównuje tę nutę z tonacją utworu (rozpoznaną albo ustawioną
ręcznie w widoku „Referencje”) i z typową nutą basu: najlepiej, gdy stopa siedzi na tonice albo kwincie,
źle, gdy jest poza skalą albo o sekundę, trytom lub septymę od basu (wtedy dół dudni na każdym uderzeniu).
Mózg podaje, o ile półtonów i centów przestroić sampel; przestrajasz go sam w FL („Pitch” albo
„Fine tune” w kanale samplera stopy) i uczysz Agenta stopy ponownie.

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

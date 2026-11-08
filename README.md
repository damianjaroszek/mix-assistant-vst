# Mix Assistant

Asystent miksu dla FL Studio (VST3): auto gain staging i balans każdego kanału
w kontekście całego miksu, z porównaniem do referencji (również po stemach).
Gatunki docelowe: house, tech house, garage, trance.

Koncepcja, architektura i plan faz: [rewizja koncepcji](https://claude.ai/artifact/5erMybFtPRsZ84QrMGyBFY).

## Jak używać, krok po kroku

To samo, z krokami odhaczanymi na żywo, jest w Mózgu na pierwszej zakładce „Jak zacząć”.

**Przygotowanie (raz na projekt)**

1. **Agent na każdym kanale miksera** (stopa, bas, wokal, synthy, busy), najlepiej jako pierwszy efekt. Kanał bez
   Agenta jest dla Mózgu niewidoczny. **Mózg** na masterze, jako ostatni efekt.
2. **„Naucz poziom” na dropie.** Zapętl najgęstszy fragment i kliknij „Naucz poziom” w Agentach (można w kilku naraz,
   w trakcie odtwarzania). Ustawia trim, rozpoznaje rolę, zapamiętuje dynamikę, szczyty i zwykłą nutę basu.
3. **Sprawdź rolę i priorytet** (Agent, zakładka „Poziom”). Od roli zależy prawie wszystko: kompresja, saturacja,
   kto komu ustępuje. Priorytet 1 dla najważniejszych kanałów.
4. **Cel brzmienia (zalecane).** Raz na gatunek: „Balans tonalny” → „Nowy cel” → nagraj 3–10 utworów referencyjnych
   (jak Capture w Tonal Balance Control). Jeszcze lepiej stemy referencji (zakładka „Referencje”). Bez tego Mózg
   trzyma się typowych proporcji house'u, a balansu tonalnego nie ocenia.

**Miks (powtarzaj po każdej większej zmianie)**

5. **Zapętl drop i graj.** Mózg porównuje tylko głośny fragment; potrzebuje 8 s dropu.
6. **„Analizuj”** w zakresie „Cały łańcuch”. Niczego nie zmienia: daje propozycję, werdykt na każdy kanał, a w Agentach
   zakładkę „Decyzje Mózgu” (co postanowił przy każdym narzędziu i dlaczego).
7. **Przejrzyj propozycję i „Zastosuj”.** Odznacz, czego nie chcesz. „Przed / po” porównuje z wyrównaną głośnością,
   „Cofnij” zdejmuje wszystko naraz.
8. **Graj dalej.** Z „Dopracuj sam” (domyślnie) Mózg sam mierzy drop na nowo i poprawia, co zostało (do 2 rund). Bez
   niego: puść drop jeszcze raz i znowu „Analizuj”, aż propozycja będzie pusta albo zostaną drobiazgi.
9. **Sprawdź „Raport”.** Każda czerwona pozycja mówi, co dalej; część to aranż albo brzmienie (do zrobienia w FL).

**Dodatkowo:** raz cały utwór od początku w trybie Song (mapa energii, intro, build, break), przed masteringiem
„Raport dla Ozone”, a po dodaniu kanału albo zmianie barwy „Naucz poziom” na nim i znowu od kroku 5.

## Co jest w środku

| Część | Gdzie | Co robi |
| --- | --- | --- |
| **Agent** | `plugins/agent` | Wtyczka na końcu każdego kanału. Ma rolę (stopa, bas, wokal...) i priorytet. „Naucz poziom” mierzy fragment utworu (RMS albo LUFS, true peak, widmo w pasmach ERB), kończy sam po ustawionym czasie i ustawia trim do poziomu docelowego (domyślnie -18 dBFS RMS), nigdy nie przekraczając limitu szczytu (domyślnie -1 dBTP). Pokazuje widmo ważone krzywą jednakowej głośności (ISO 226), czyli tak, jak słyszy je ucho; nauczony profil zapisuje się w projekcie. Wysyła swoje pomiary do Mózgu; nazwa kanału to nazwa ścieżki z FL albo własna, wpisana w oknie. Ma 4-pasmowy EQ „Korekcja od Mózgu”, który ustawia „Zastosuj” w Mózgu (można go wyłączyć albo wyzerować). Ma wejście sidechain i sekcję „Ustępowanie”: kanał ścisza się tylko wtedy, gdy gra sygnał z sidechaina (np. bas w paśmie stopy na każdym uderzeniu, albo cały kanał), z czasem powrotu dopasowanym do tempa (1/32 do 1/4). Mierzy też fazę w basie względem sidechaina i ma przełącznik odwrócenia polaryzacji. Sekcja „Atak” podkreśla tylko początki dźwięków (klik stopy) w paśmie, które słychać na małych głośnikach. Sekcja „Harmoniczne” dodaje alikwoty treści poniżej ustawionej częstotliwości (np. sub basu), żeby była słyszalna na telefonie i laptopie; ilość to poziom alikwotów względem źródła, niezależnie od głośności kanału. Sekcja „Przester” to saturacja z 4x nadpróbkowaniem (bez aliasingu) w pięciu trybach: Taśma, Lampa, Miękki clip, Twardy clip, Fałdowanie; drive liczy się od poziomu po gain stagingu, więc znaczy to samo na każdym kanale, a głośność zostaje wyrównana, zmienia się barwa. „Celowy przester” mówi, że kanał ma brzmieć brudno: nauka poziomu nie pilnuje wtedy szczytów, Mózg nie ostrzega o przesterowaniu i nie zmienia ustawień przesteru. Przy „Naucz poziom” Agent rozpoznaje też instrument (stopa, bas, perkusja, wokal, lead, pad) i jeśli kanał nie miał roli, ustawia ją razem z typowym priorytetem; rola wybrana ręcznie zawsze wygrywa. Ta sama nauka zapisuje dynamikę kanału (poziomy cichych, typowych i głośnych fragmentów) i typową nutę basu. Kolumna „Inżynier miksu” ma narzędzia, które ustawia Mózg („Analizuj” z zakresem „Cały łańcuch”) albo Ty ręcznie: „Porządki” (HPF 24 dB/okt, mono poniżej podanej częstotliwości bez przesunięcia fazy, jedno szerokie cięcie mułu i do trzech wąskich cięć rezonansów), „Kompresor” (RMS, miękkie kolano, wskaźnik redukcji), „Clipper” (4x nadpróbkowanie, ścina tylko szczyty ponad sufitem) „Faza w pasmach (mid-side)” (podgląd fazy kanału w pasmach, jak Correlometer, i do czterech dzwonów EQ tylko na sygnale side) i „Wyrównanie nut basu” (śledzi nutę basu i podnosi te, które ucho słyszy ciszej, a obniża wyższe, względem typowej nuty linii). |
| **Mózg** | `plugins/brain` | Wtyczka na końcu mastera. Mierzy sumę: LUFS (integrated, short-term), true peak, PLR, widmo w 31 pasmach 1/3 oktawy, korelację fazy (całość i dół poniżej 120 Hz). Widzi wszystkich Agentów: tabela kanałów, nałożone widma (tak, jak słyszy ucho), pasek „tłoku” w pasmach, lista kolizji („Pad zasłania Stopę w 200–600 Hz”) i raport poprawności. Kolizje liczy z tego, co gra teraz (uśrednione z ok. 3 s) albo z nauczonych profili. „Analizuj” liczy poprawki dla całego miksu naraz (cięcia EQ na kanale mniej ważnym, gain wszystkich kanałów, gdy brakuje zapasu na mastering), każdą z wyjaśnieniem. „Zastosuj” wysyła zaznaczone do Agentów, „Cofnij” przywraca poprzednie ustawienia, „Przed / po” porównuje obie wersje z wyrównaną głośnością. Widok „Balans tonalny” porównuje uśrednione widmo miksu z korytarzem celu zbudowanym z referencji (nagrywanych w Mózgu jak „Capture” w Tonal Balance) i pokazuje dziury oraz nadmiary; „Analizuj” wypełnia dziury podbiciem kanału, który ma tam treść (bez tworzenia nowych kolizji), tnie nadmiar na kanale najmniej ważnym, a gdy nic w miksie nie gra w danym paśmie, podpowiada saturację albo brakujący element aranżu. Dla stopy i basu proponuje ustępowanie dynamiczne zamiast stałego cięcia (pasmo, głębokość i próg z pomiaru, powrót 1/16 z tempa projektu), a gdy bas znosi się ze stopą w przeciwfazie, odwrócenie polaryzacji; raport „Stopa i bas w czasie” pokazuje, czy sidechain jest podłączony i ile kanał ustępuje. Gdy kanał zasłania stopę wyżej (np. lead w paśmie kliku), też ustępuje jej dynamicznie zamiast stałego cięcia. Lista kolizji liczy się z widma uśrednionego z kilku sekund, więc kolizji obsłużonej przez ustępowanie dalej nie znika; jest wtedy zielona z dopiskiem „ustępuje na każde uderzenie stopy”, a kolizja, przy której cięcie EQ doszło do limitu, ma dopisek, że reszta to sprawa poziomu kanału. Widok „Translacja” pokazuje, ile każdy kanał traci albo zyskuje względem reszty miksu na telefonie, laptopie, słuchawkach dousznych, w samochodzie i w klubie (mono); „Odsłuch” u góry pozwala posłuchać miksu przez taką symulację (pomiary dalej widzą prawdziwy miks, a ustawienie nie zapisuje się w projekcie). Dla kanału, który na małych głośnikach słabnie (od ok. -6 dB na telefonie), „Analizuj” analizuje cały miks: najpierw robi mu miejsce cięciem u mniej ważnego kanału w paśmie, które gra telefon, potem dobiera własny krok według rodzaju dźwięku (atak dla perkusji, harmoniczne dla niskich, obecność dla reszty), każdy w najmniejszej ilości, która wystarcza, i bez spychania innych kanałów, a dziurę w balansie tonalnym, w której nic nie gra, wypełnia harmonicznymi kanału grającego oktawę niżej. „Tryb adaptacyjny” (domyślnie wyłączony) zamienia propozycję w autopilota: Mózg dalej liczy solver na tym, co gra, i przesuwa korekcje EQ w Agentach najwyżej o 0,25 dB na sekundę w jego limitach; nowe pasmo zaczyna dopiero, gdy solver prosi o nie kilka razy z rzędu, stare puszcza po kilku zgodnych odczytach, w przerwach i przez 4 s po nich nic nie rusza, a kolizję, którą kanał już rozwiązuje ustępowaniem, zostawia ustępowaniu. Wyłączenie zamraża to, co jest ustawione. „Analizuj” z zakresem „Cały łańcuch” to inżynier miksu: ustawia każdy kanał, zawsze w kontekście całej reszty. HPF idzie pod treść, która się liczy (wyżej, gdy dół należy do stopy i basu; stopa i bas dostają tylko subsonic), cały dół jest w mono poniżej 120 Hz, a gdy trzy kanały lub więcej tłoczą się w 200–500 Hz, mniej ważne oddają trochę mułu. Rezonanse (wąskie szczyty, które stoją w miejscu bez względu na to, co gra, np. dzwoniący werbel albo pudełkowy pokój) Agent znajduje przy nauce; harmoniczne melodii przesuwają się z nutami, więc się nie liczą. Mózg tnie rezonans tylko wtedy, gdy miks go słyszy, głębiej, gdy zasłania ważniejszy kanał, a na kanałach tonalnych tylko szersze garby (ostrość, pudełkowatość), nigdy pojedyncze harmoniczne. Kompresja zależy od roli i od miksu: perkusja zostaje bez kompresora, kanał już ściśnięty u źródła nie dostaje więcej, kanał, którego ciche fragmenty giną pod resztą, jest trzymany ciaśniej, a kompresor nigdy nie schodzi głębiej, niż rola znosi (cichszy takt to aranżacja, nie nierówne granie). Clipper zdejmuje tylko nadmiar szczytów ponad to, co rola potrzebuje, a bas dostaje wyrównanie nut. Saturację (faza 13) dostaje tylko partia za uboga w alikwoty: bas, którego prawie nie ma powyżej 150 Hz (sinusowy sub), wokal, lead albo pad o zbyt gładkim brzmieniu. Tryb zależy od roli (bas i lead: lampa, wokal i pad: taśma), a drive i miks Mózg dobiera, przepuszczając ton kanału przez prawdziwy saturator: bierze najłagodniejsze ustawienie, które wystarcza, i nigdy takie, którego nowe alikwoty zasłoniłyby ważniejszy kanał. Stopa i perkusja zostają przy clipperze i ataku. Gdy kanał ma zaznaczony „Celowy przester”, jego saturacja jest Twoja i Mózg jej nie rusza. „Analizuj” z zakresem „Cały łańcuch” wyłącza też to, czego plan nie potrzebuje (np. kompresor zaznaczony ręcznie na perkusji). Na tym Mózg liczy zwykłą propozycję (kolizje, stopa i bas, translacja) i w widoku „Werdykt” pisze dla każdego kanału, co ustawił, dlaczego i co jeszcze wymaga uwagi. Stereo (faza 8) działa jak praca z Correlometerem, tylko na kanałach zamiast na sumie: nie poszerzamy na ślepo, pilnujemy fazy w każdym paśmie. Widok „Stereo” pokazuje fazę sumy w pasmach (na dole przeciwfaza, kropki to zaznaczony kanał), a raport „Faza w pasmach” mówi, które pasma wpadają w przeciwfazę i który kanał to robi. „Analizuj” z zakresem „Cały łańcuch” zdejmuje side dokładnie w tym paśmie i tylko na tym kanale, a tam, gdzie cały miks jest prawie mono, daje szerokim partiom (pad, lead, efekty) najwyżej +1 dB side, jeśli kanał zostaje w fazie. Dół i tak jest w mono, a środek (mid), czyli suma mono, zostaje nietknięty. Referencje ze stemów (faza 9): widok „Referencje” porównuje cztery grupy miksu (perkusja, bas, wokal, reszta, z ról kanałów) ze stemami utworów referencyjnych, drop do dropu: poziom grupy względem całości, jej własną barwę w sześciu szerokich pasmach (sub, bas, dolny środek, środek, prezencja, powietrze) i szerokość. „Analizuj” z zakresem „Cały łańcuch” przesuwa poziomy grup do korytarza referencji (najwyżej 3 dB na raz, głośność całości zostaje) i dodaje szerokie korekcje barwy na kanale grupy, który niesie dane pasmo. Referencje w innej tonacji niż Twój utwór są do niej przesuwane (faza 10), tonację Mózg rozpoznaje sam albo wybierasz ją ręcznie. Break porównuje z breakami referencji osobno (faza 12). Przed masteringiem (faza 11) podgląd mastera sprawdza, ile limiter musiałby ściąć przy docelowej głośności, a „Raport dla Ozone...” zapisuje wszystko w jednym pliku HTML. Limiterów nie ma: mastering robi się osobno (np. w Ozone). |
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
aranżu i automatyki głośności: „Analizuj” z zakresem „Cały łańcuch” niczego w breaku nie zmienia. Zestawy zapisane przed
fazą 12 nie mają breaku: wczytaj ich stemy jeszcze raz.

Intro i build (faza 16): w referencjach intro to to, co gra przed pierwszym dropem, a build to
ostatnie 8 s przed każdym dropem. Przyciski „Intro” i „Build” w widoku „Referencje” porównują je tak
samo jak break: proporcje grup (np. w intrach referencji grają same bębny, a u Ciebie już pady)
i o ile sekcja jest cichsza od dropu (intro albo build za głośne: drop nie robi wrażenia). Swoje intro
i build Mózg zna z pozycji w utworze, więc puść utwór w FL w trybie utworu (Song) od początku przez
pierwszy drop. Break może zawierać build (koniec breaku, który narasta do dropu). Zestawy zapisane
przed fazą 16 nie mają intro ani buildu: wczytaj ich stemy jeszcze raz.

Tryb szkicu (faza 17, zakładka „Aranż”, domyślnie włączony): gdy dropowi brakuje partii, które mają
referencje (albo, bez wczytanych stemów, typowy drop house'u: perkusja, bas i reszta), Mózg wstawia za
nie partie-duchy. Duch ma widmo i poziom tej grupy z referencji, ustawione względem tego, co już gra.
Duchem jest cała brakująca grupa (np. wokal) albo pasmo grupy, w którym żaden jej kanał nie gra (np. pad
pod leadem w 90–250 Hz). Duchy liczą się w balansie tonalnym (przerywana krzywa; szare pasma to dziury
aranżu, nie EQ), w proporcjach grup i w kolizjach. „Analizuj” z zakresem „Cały łańcuch” zostawia im miejsce (cięcie do 3 dB
na kanale, który je zasłoni), nie łata EQ-em dziur, które wypełni aranż, a samym duchom niczego nie
zmienia. Raport „Aranż” i propozycja podają dla każdej brakującej partii, co dograć: poziom względem całego
dropu, główne pasmo, szerokość i tonację z referencji oraz sekcje, w których gra w referencjach. O
kompletności decyduje sam drop, więc rzadkie intro to nie szkic. Gdy po „Analizuj” z zakresem „Cały łańcuch” dojdzie nowy
kanał, raport przypomni, żeby uruchomić je jeszcze raz. Wyłączony tryb szkicu ocenia miks tak, jak gra.

Jedna korekta na problem (faza 18): balans tonalny w propozycji liczony jest na sumie po całej
propozycji, a nie na tym, co zmierzono. Uwzględnia EQ, które kanały już mają, cięcia z kolizji i
referencji ze stemów oraz zmiany poziomów grup z „Analizuj” z zakresem „Cały łańcuch” (te Mózg planuje teraz przed EQ).
Dlatego EQ nie dokłada drugi raz tego, co robi już ruch poziomu. Drugie „Analizuj” po „Zastosuj” daje
ten sam EQ, zamiast zdejmować podbicia, które właśnie wypełniły dziurę. Podbicie, które przestrzeliłoby
na drugą stronę korytarza, jest zmniejszane.

Automatyka sekcji (faza 19): dla intro, buildu i breaku Mózg podaje gotową automatykę głośności każdej
grupy względem dropu, np. „Bas zgaś (BASS), Reszta -7 dB (LEAD, PAD), wtedy break będzie 7,4 dB ciszej niż
drop. Gdzie: takty 25–31”. Liczy ją tak, żeby proporcje grup w sekcji i jej głośność względem dropu weszły
w korytarz referencji; grupę, której referencje w tej sekcji nie mają, każe zgasić, a żadnej nie podnosi
głośniej, niż gra w dropie. Pojawia się w raporcie, w raporcie HTML i w kolumnie „automatyka” na zakładce
„Referencje” (przyciski Intro / Build / Break). Wpisz ją w głośność kanału w Channel Rack (albo w gain
przed Agentem), nie w suwak miksera: wtedy Agent ją słyszy i po ponownym odsłuchu sekcja nie potrzebuje
już zmian. W buildzie to wartość średnia (build zwykle narasta do dropu). Takty liczone są w 4/4 z tempa FL.

Mapa energii (faza 20, zakładka „Energia”): głośność całego utworu sekunda po sekundzie względem dropu
(biała linia) na tle referencji (niebieski korytarz i przerywana mediana), wyrównanych do Twojego pierwszego
dropu, z zaznaczonym intro, buildem i breakiem. Pod spodem energia każdej grupy (perkusja, bas, wokal,
reszta) i mediana referencji. Raport „Przebieg utworu” podaje, kiedy wchodzi pierwszy drop (takt i czas),
ile jest dropów, jak głęboko utwór siada między dropami (wszystko na tle referencji) oraz dłuższe odcinki,
w których miks gra o 3 dB lub więcej głośniej albo ciszej niż referencje w tym samym miejscu (na mapie na
czerwono). Mózg zna przebieg z pozycji w utworze, więc puść utwór w FL w trybie Song od początku do końca.
Zestawy referencji zapisane przed fazą 20 nie mają przebiegu: wczytaj ich stemy jeszcze raz.

Odsłuch z duchem (faza 21, zakładka „Aranż”, przełącznik „Słuchaj duchów”): w trybie szkicu Mózg dogrywa w
odsłuchu brakujące partie jako szum o ich widmie, poziomie i szerokości (perkusja pulsuje na bit z tempa FL).
Słychać, jak drop zabrzmi, gdy partie dojdą, i czy to, co gra, zostawia im miejsce. Duchy grają tylko w
dropie (gdy miks jest blisko poziomu dropu) i tylko przy grającym transporcie. Pomiary Mózgu ich nie słyszą,
ale wychodzą z wtyczki, więc wyłącz przełącznik przed eksportem (raport przypomina). Po każdym otwarciu
projektu jest wyłączony.

### Praca z Mózgiem: analiza, przegląd, zastosowanie (faza 22)

Na dole okna są cztery kroki, a podświetlony przycisk to zawsze następny. „Analizuj” liczy propozycję na tym,
co gra (najlepiej drop) i niczego nie zmienia; zakres obok wybiera „Cały łańcuch” (inżynier miksu: porządki,
kompresja, clipper, bas, stereo, poziomy grup, plus korekty i werdykt) albo „Tylko korekty” (cięcia EQ,
ustępowanie, polaryzacja, translacja). Na zakładce „Propozycja” odznaczasz, czego nie chcesz, i klikasz
„Zastosuj (N zmian)”. Potem puść drop jeszcze raz: pasek pokazuje „mierzę efekt 0 / 8 s” i mówi, kiedy pomiar
po zmianie jest gotowy.

Raport zaczyna od tego, co trzeba poprawić, w trzech grupach: do poprawy w miksie, aranż i przebieg (rzeczy
poza miksem: dograć, zautomatyzować, podmienić brzmienie) i ustawienia wtyczek. To, co jest w porządku, jest
zwinięte w jedną linię, która rozwija się po kliknięciu. Balans tonalny jest czerwony tylko za to, co da się
poprawić EQ albo poziomem: dziura, w której żaden kanał nie gra (do 24 dB pod swoim najgłośniejszym pasmem),
idzie do „Poza zasięgiem EQ”. Gdy słychać tylko drop w pętli, „Przebieg utworu” mówi, że nie ma czego oceniać,
zamiast „pierwszy drop: takt 1”. W „Aranżu” pasmo 90–250 Hz basu to „dół basu (alikwoty nad subem)”, a przy
sinusowym basie Mózg najpierw radzi saturację albo „Harmoniczne” na kanale basu. Szkic liczy już to, co dodaje
łańcuch kanału (saturacja, harmoniczne), więc po zastosowaniu saturacji duch znika, jeśli pasmo się wypełniło.

### Co działa w auto, a co nie (faza 23)

- „Propozycja” oznacza na zielono jako „Już działa” to, co Agent już ma tak, jak Mózg chce. Licznik „Zastosuj (N)”
  liczy tylko nowe zmiany, a odznaczenie czegoś, co działa, usuwa to z Agenta. Cięcia z poprzednich rund zostają,
  zamiast znikać i wracać w innym paśmie, więc kolejne „Analizuj” po „Zastosuj” dają coraz mniej nowych zmian.
- „Kolizje”: zielone są rozwiązane, niebieskie to te, przy których miks zrobił, ile mógł (EQ na limicie albo żadne
  cięcie nie pomaga). Gdy wystarczy ściszyć kanał o najwyżej 3 dB, propozycja ma taką zmianę (do 2 dB zaznaczoną).
  Gdy trzeba więcej, Mózg pisze, że to rejestr albo brzmienie. Pomarańczowe i czerwone są tylko wtedy, gdy jest coś do zrobienia.
- „Werdykt” zostaje po „Zastosuj”, a każda linia o narzędziu ma stan: [działa], [do zastosowania], [odznaczone],
  [zmienione ręcznie] albo [nie działa: nie zastosowano]. W Agencie przy każdej sekcji jest „od Mózgu”,
  „zmienione ręcznie” albo „ręcznie”.
- Raport ma pozycję „Z czego korzysta Mózg”: nauka poziomu, cel tonalny, referencje, sidechain stopy, przebieg utworu
  i pomiar dropu, przy każdej, do czego służy.
- Poprawki błędów: „Porządki” (HPF, mono w dole, muł, rezonanse) od fazy 9 nie trafiały do propozycji, choć werdykt
  je opisywał, więc nigdy się nie włączały. Agent nie zapisywał w projekcie zmierzonego szczytu, więc po ponownym
  otwarciu Mózg widział „szczyt -182 dB” i uznawał kanały za przekompresowane. Projekty zapisane wcześniej potrzebują
  jednego „Naucz poziom” na kanale, do tego czasu werdykt pisze „szczyty nieznane”.

### Środek korytarza (faza 24)

Miks może mieścić się w korytarzu celu i nadal leżeć przy jego krawędzi w całym zakresie: tak wyglądał w Tonal Balance
low-mid i high-mid przy dolnej krawędzi, który Ozone potem dociągał do środka. Widok „Balans tonalny” dzieli pasmo na
cztery zakresy jak TBC (Low 40–250 Hz, Low-Mid 250 Hz–2 kHz, High-Mid 2–9 kHz, High 9–16 kHz) i przy każdym pisze, ile
dB miks leży nad albo pod środkiem korytarza (medianą referencji). Na zielono jest to, co mieści się w 0,75 dB.
„Analizuj” dociąga zakresy dalej niż 0,75 dB od środka: jeden szeroki bell na zakres (albo większe istniejące cięcie
lub podbicie w tym zakresie), nadmiar zdejmuje z mniej ważnego kanału, który niesie co najmniej 1/4 zakresu, brak
podbija na ważniejszym (co najmniej 15% zakresu, bez nowej kolizji). Rusza zakres najwyżej o 1,5 dB i kanał najwyżej
o 3 dB, nie przechodzi na drugą stronę środka i nie wypycha żadnego pasma poza korytarz. Zakres, który jest jeszcze
poza korytarzem, zostaje zwykłej poprawce tonalnej. Kolejne „Analizuj” daje te same bele jako „Już działa”.

Referencje to gotowe mastery, a miks jeszcze nie przeszedł przez limiter. Dlatego Mózg porównuje go tak, jak zostawiłby
go limiter z podglądu mastera do „Celu mastera” (przełącznik „Jak po limiterze (master)”, domyślnie włączony). Gruba
linia to ten szacunek, cienka to miks tak, jak gra (to, co pokazuje Tonal Balance przed Ozone). Podgląd mierzy widmo
przed limiterem i za nim; zostaje tylko kształt różnicy, najwyżej ±2 dB na pasmo, bo prosty limiter zjada dół mocniej
niż maximizer w Ozone. „Raport dla Ozone” wypisuje to, co z zakresów zostanie po „Zastosuj”, jako szerokie ruchy
(EQ albo Band Gain w Dynamics).

### Szerokość jak w referencjach (faza 25)

Z wczytanymi stemami referencji Mózg porównuje szerokość każdej grupy (perkusja, wokal, reszta) od 300 Hz w górę z tym,
jak gra ta grupa w referencjach (mediana korelacji w każdym zakresie), i dociąga kanały do tej szerokości w „Analizuj”
(cały łańcuch). Kanał, który już ma side, dostaje podbicie side'u w tym zakresie. Kanał prawie mono dostaje poszerzenie:
do side'u idzie kopia środka opóźniona o kilka ms i przepuszczona przez filtr od 300 Hz (albo od miejsca, gdzie
referencje są szersze). Środek się nie zmienia, więc w mono kanał gra dokładnie tak samo. Każdy kanał ma inne
opóźnienie, żeby poszerzenia nie składały się w jedno.

Granica: korelacja żadnego pasma, ani kanału, ani sumy, nie spada poniżej 0,2. Gdy suma by spadła, Mózg cofa
krokami wszystkie poszerzenia grające w tym paśmie, aż suma wróci do 0,2. Stopa, bas, bus i send nigdy nie są poszerzane, a wokal dostaje
tylko podbicie istniejącego side'u (bez sztucznego poszerzenia). Bez stemów referencji zostaje to, co było: cięcie side'u
tam, gdzie pasmo wpada w przeciwfazę, i najwyżej +1 dB side'u na szerokich rolach. W Agencie na zakładce „Stereo” są trzy
pokrętła poszerzenia (ile dB pod środkiem, od ilu Hz, opóźnienie); -40 dB znaczy wyłączone. W widoku „Stereo” Mózgu
pomarańczowe są dopiero pasma poniżej 0,15, bo 0,2 to zamierzona granica.

### Suwak szerokości (faza 26)

Każdy Agent ma na zakładce „Stereo” suwak „Szerokość”, działający także bez stemów referencji: 0% to mono, 100% to bez
zmian (domyślnie, nic nie jest przetwarzane), 200% to „max”, czyli tak szeroko, jak się da bez przeciwfazy. Gdy suwak
jest w użyciu, poniżej „Mono poniżej” (domyślnie 150 Hz) kanał gra w mono. W lewo side nad tą częstotliwością jest
ściszany. W prawo side jest podbijany (do +6 dB) i dochodzi poszerzenie od 300 Hz (kopia środka opóźniona o kilka ms,
inne opóźnienie na każdym kanale). Środek się nie zmienia, więc w mono kanał gra tak samo.

Agent pilnuje każdego pasma: korelacja nie spada poniżej 0,2. Pasmo, które by spadło (albo już jest w przeciwfazie),
dostaje cięcie side'u tylko w tym miejscu, najwyżej w dwóch miejscach. „Max” to największe poszerzenie, przy którym to
wystarcza, więc kanał już szeroki poszerzy się mniej niż mono. Kanał w przeciwfazie w zbyt wielu pasmach (np. krótki
delay stereo) zostaje zwężony jako całość. Agenty pilnują też sumy: każdy liczy ten sam plan z tego, co wszystkie
publikują, i gdy pasmo sumy zeszłoby poniżej 0,2 przez suwaki (z zapasem tam, gdzie spotykają się side'y kilku kanałów),
poszerzenia grające w tym paśmie ustępują razem. Działa to także bez Mózgu. Mózg liczy kanał z suwakiem w sumie, nie
proponuje mu zmian stereo i pisze w werdykcie, że szerokość jest ustawiona ręcznie.

### Stabilność (faza 27)

- Kroki translacji (miejsce u mniej ważnego kanału, obecność, harmoniczne, atak) nie wypychają balansu tonalnego dalej
  poza korytarz celu, a harmoniczne dostają tylko tyle, ile balans zniesie. Gdy kroki translacji i tak ruszają balans
  o więcej niż 0,5 dB, Mózg liczy propozycję drugi raz i wtedy korekcja tonalna już je uwzględnia. Jedno „Zastosuj”
  trafia więc tam, gdzie celowało, zamiast podbić Low i zostawić poprawkę na drugą rundę.
- Cięcie side'u w paśmie, które wpadło w przeciwfazę, zostaje (tak głębokie, jak jest), dopóki pasmo nie wróci wyraźnie
  do fazy (korelacja 0,15). Pasmo, które krąży wokół zera, nie dostaje już raz cięcia, a raz nie.
- Plan szerokości (faza 26) liczy się 3–4 razy szybciej i co pół sekundy (od razu po ruchu suwaka), żeby duży projekt z
  wieloma suwakami nie obciążał FL.

### Domknięcie po analizie (faza 28)

- Kolizje: jedno „Analizuj” liczy kolizje na miksie takim, jaki będzie po całej propozycji. Gdzie cięcia EQ z
  propozycji nie wystarczą, od razu dokłada ściszenie kanału (do 2 dB zaznaczone, do 3 dB do decyzji). Po „Zastosuj”
  pomarańczowe są tylko kolizje, przy których jest jeszcze co zrobić (np. nowa kolizja, którą usunie ściszenie o kilka
  dB). Niebieskie to rejestr albo brzmienie, czyli aranż, oraz większe ściszenie, które zmienia balans, więc zostaje
  Twoją decyzją.
- Głośny master: „Analizuj” (cały łańcuch) patrzy, ile limiter podglądu mastera ścina w głośnej części przy celu (np.
  -8 LUFS). Gdy więcej niż 3 dB, dokłada clipper na stopie i perkusji, które dają najwyższe szczyty miksu (razem z
  clipperem od szczytów najwyżej 6 dB na kanale), celując w ok. 2,5 dB pracy limitera. Kolejna analiza uwzględnia to,
  co clipper już ścina, więc nie dokłada w kółko i oddaje clipping, gdy cel jest cichszy. Gdy clippery są na limicie,
  raport mówi wprost, że reszta to gęstość dropu albo niższy cel.
- Werdykt, Raport i Propozycja nie wracają już same na górę przy przewijaniu.

### Proporcje bez referencji (faza 29)

- „Naucz poziom” ustawia kanał względem celu według roli: stopa na celu, bas 2 dB niżej, wokal 4, lead i perkusja 6, pad
  8, efekty i powroty 10 dB niżej (rola „Inny” i bus zostają na celu). Stopa ma niskie RMS jak na to, jak głośno brzmi
  (krótkie uderzenia, głównie sub), a pad czy lead grają bez przerwy w paśmie, które ucho słyszy najlepiej, więc ten
  sam poziom RMS stawiał pad nad stopą. Przełącznik „Poziom według roli” obok przycisku nauki to wyłącza.
- Bez referencji ze stemów „Analizuj” (cały łańcuch) wyrównuje grupy (perkusja, bas, wokal, reszta) do typowych proporcji
  house'u (korytarz ±1,5 dB, uszny udział w całości). Perkusja zostaje tam, gdzie postawiła ją nauka, reszta przesuwa
  się względem niej, najwyżej 3 dB na analizę. Głośność całości dopasuje cel mastera. Po wgraniu referencji ze stemów
  proporcje biorą się z nich.

### Ustępowanie bez kabla i dopracowanie (faza 30)

- Kolizję, której EQ ani mały krok poziomu nie usuną (np. jasny bas nad leadem w 1-4 kHz), Mózg rozwiązuje dynamicznie:
  kanał zasłaniający przycisza dzwonem zakres zasłanianego tylko wtedy, gdy ten gra (do 6 dB, głębokość dobrana z
  przewidywania maskowania). Nie trzeba kabla sidechain w FL: Agent zasłanianego kanału mierzy swoje wyjście w ośmiu
  pasmach oktawowych i podaje obwiednię przez wspólną pamięć, a Agent zasłaniający czyta ją co blok. Każdy kanał może tak
  ustępować dwóm innym; widać to w Agencie na karcie „EQ i ustępowanie” („Ustępowanie bez kabla”, przycisk „Wyłącz”).
- Ustępowanie na stopę (bas, pady pod stopą) też działa bez kabla, gdy na wejściu sidechain nie ma sygnału. Podłączony
  kabel ma pierwszeństwo, bo jest co do próbki dokładny; bez niego dip spóźnia się najwyżej o jeden blok bufora.
- Gdy Mózg planuje proporcje grup, kolizje nie dostają już osobnych kroków poziomu kanału (następna analiza i tak
  oddawała grupę na miejsce), tylko ustępowanie dynamiczne.
- „Dopracuj sam” (pod przyciskami, domyślnie włączone): po „Zastosuj” Mózg mierzy drop jeszcze raz i, jeśli coś zostało
  (np. clipper pod głośny master, kolizja), sam analizuje i stosuje, najwyżej 2 rundy. Odznaczone pozycje zostają
  odznaczone. „Analizuj”, „Cofnij” albo „Przed / po” przerywają. Jedno „Cofnij” cofa wszystko razem z rundami.
- Nowy projekt bierze cel tonalny używany ostatnio (`last-target.txt` w folderze celów).

### Decyzje Mózgu, przewodnik, harmoniczne (faza 31)

- Każdy Agent ma zakładkę **„Decyzje Mózgu”**: przy każdym z 11 narzędzi (EQ, porządki, kompresor, clipper,
  wyrównanie nut, stereo, saturacja, ustępowanie stopie, harmoniczne, atak, ustępowanie bez kabla) co Mózg postanowił
  przy ostatnim „Analizuj” (w propozycji, zostaje, niepotrzebne, nie sprawdzone, Twoje ustawienie), jak jest teraz i
  dlaczego. Wyłączone narzędzie mówi więc wprost, czy Mózg je rozważył i odrzucił, czy nie miał danych (np. brak nauki).
  Etykieta przy nagłówku narzędzia na każdej zakładce pokazuje to samo („Mózg: niepotrzebne”), a „Barwa” ma powody pod
  harmonicznymi, atakiem i przesterem. Werdykt w Mózgu ma te linie dla EQ, ustępowania, harmonicznych i ataku.
  Decyzje zapisują się z projektem.
- W Mózgu pierwsza zakładka **„Jak zacząć”**: kroki z sekcji „Jak używać” powyżej, odhaczane na podstawie tego, co Mózg
  widzi (Agenty, role, nauka, cel, drop, analiza, zastosowanie, dopracowanie, raport).
- **Harmoniczne w planie całego miksu:** bas, który jest prawie czystym sinusem (treść powyżej 150 Hz ponad 12 dB pod
  celem), dostaje enhancer harmonicznych zamiast lampy: alikwoty robią się tylko z suba, sam sub zostaje czysty. Bas z
  odrobiną środka nadal dostaje lampę; każde jest zapasem dla drugiego, gdy zasłoniłoby ważniejszy kanał. Wcześniej
  harmoniczne pojawiały się tylko wtedy, gdy kanał ginął na telefonie albo trzeba było wypełnić dziurę w balansie.
- Podpowiedzi (tooltipy) w obu wtyczkach wreszcie się pokazują (brakowało okna podpowiedzi).

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
Potem „Analizuj” w Mózgu dobierze pasmo, głębokość, próg i czas powrotu.

### Stopa a tonacja

Przy „Naucz poziom” Agent stopy mierzy, na jakiej nucie ustala się ogon uderzenia (150–300 ms po
ataku). Raport „Stopa a tonacja” w Mózgu porównuje tę nutę z tonacją utworu (rozpoznaną albo ustawioną
ręcznie w widoku „Referencje”) i z typową nutą basu: najlepiej, gdy stopa siedzi na tonice albo kwincie,
źle, gdy jest poza skalą albo o sekundę, trytom lub septymę od basu (wtedy dół dudni na każdym uderzeniu).
Mózg podaje, o ile półtonów i centów przestroić sampel; przestrajasz go sam w FL („Pitch” albo
„Fine tune” w kanale samplera stopy) i uczysz Agenta stopy ponownie.

### Bus: klej

Agent na kanale busa (np. bus perkusji w mikserze FL) ma rolę „Bus”. Ustaw ją ręcznie przed „Naucz poziom”,
bo bus perkusji brzmi jak stopa i rozpoznawanie roli by go tak nazwało. Źródła busa też powinny mieć
własnych Agentów: Mózg ustawia je tam, a busa nie liczy w kolizjach, translacji ani korekcjach EQ
(inaczej zderzałby się z własnymi źródłami). Bus dostaje od „Analizuj” z zakresem „Cały łańcuch” tylko kompresor sklejający,
dobrany do tego, co Agent na nim rozpoznał:

| Treść busa | Ratio | Atak | Powrót | Redukcja w głośnych miejscach |
| --- | --- | --- | --- | --- |
| perkusja | 4:1 | 30 ms (przepuszcza uderzenia) | 1/16 z tempa | ok. 3 dB |
| wokale | 2:1 | 10 ms | 1/8 | ok. 2 dB |
| muzyka (pady, synthy) | 2:1 | 30 ms | 1/4 | ok. 1,5 dB |

Gdy źródła przychodzą już mocno ściśnięte (limiter na kanałach), klej się nie dokłada. Ściszenie całego
miksu przez Mózg zmienia trim źródeł, a bus zostaje: jego próg idzie za tym, co do niego dociera.

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

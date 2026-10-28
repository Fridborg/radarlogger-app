# LED-skylten på ESP:n (firmware 0.4.0)

ESP32-S3:an styr nu de två P10-panelerna direkt, i stället för Pi Zero. Visningen är samma som i `radar_live.py`:

- När ett nytt fordon kommer visas den blå skylten med gränsen (50) i 2 s, men högst var 30:e sekund.
- Därefter visas hastigheten. Den är grön under "röd från" och röd från den gränsen. Från "blinkande röd från" blinkar den röd, 0,4 s tänd och 0,4 s släckt.
- Skylten släcks när fordonet är borta.
- Ljusstyrkan följer dag och natt.

Panelinställningen är exakt den från Pi:n: `rows=16, cols=32, chain_length=2, multiplexing=4, pixel_mapper="U-mapper"`. Samma pixelomräkning görs i `Skylt.h`. Blir bilden ändå fel kan du byta variant på webbsidan utan att flasha om.

## Koppling: panel till ESP32-S3

Kabeln går till ingången (**IN**) på samma panel som Pi:n satt i. Följ namnen som står tryckta vid kontakten på panelen, inte bara stiftnumren.

| HUB75-stift | Signal | ESP32-S3 |
|---|---|---|
| 1 | R1 | GPIO 4 |
| 2 | G1 | GPIO 5 |
| 3 | B1 | GPIO 6 |
| 4 | GND | GND |
| 5 | R2 | GPIO 7 |
| 6 | G2 | GPIO 15 |
| 7 | B2 | GPIO 16 |
| 8 | GND (E) | GND |
| 9 | A | GPIO 8 |
| 10 | B | GPIO 9 |
| 11 | C | GPIO 13 |
| 12 | D | GPIO 1 |
| 13 | CLK | GPIO 12 |
| 14 | LAT / STB | GPIO 10 |
| 15 | OE | GPIO 11 |
| 16 | GND | GND |

Radarn sitter kvar på GPIO 17 och 18. Inga pinnar krockar med radarn, PSRAM (35–37) eller startpinnarna (0, 3, 45, 46).

Tänk på detta:

- Håll kablarna korta, helst under 20 cm. En flatkabel med en 16-polig kontakt och en adapter till stiftlist är enklast.
- Pi:ns Adafruit-hat hade nivåomvandlare till 5 V, men ESP:n ger 3,3 V direkt. Det fungerar på de flesta P10-paneler. Flimrar det eller blir det skräppixlar som inte går bort med varianterna, sätt en 74HCT245 emellan.

## Ström

- Panelerna får 5 V från en egen step-down, till exempel Pololu D36V50F5 (5 A). Ta aldrig panelströmmen från ESP:ns 5V-pinne.
- ESP:n och radarn får ström från den lilla step-downen, som förut.
- **Alla GND ska vara gemensamma**: panelernas nätdel, ESP:n och radarn.
- Sätt 2–4 × 4700 µF (low-ESR) över panelernas 5 V, nära panelerna.

### Valfritt: stänga av panelerna när ingen bil syns (sparar cirka 1 W)

1. Koppla GPIO 21 via 1 kΩ till gaten på en 2N7000. Lägg 100 kΩ från gaten till GND.
2. Koppla 2N7000-drain till gaten på AOD403 (P-MOSFET). Lägg 10 kΩ mellan AOD403-gaten och AOD403-source.
3. AOD403-source ska gå till +5 V från Pololu (efter kondensatorerna). AOD403-drain ska gå till panelernas +5 V.
4. Sätt **Stäng av panelerna efter** till 10 s på webbsidan.

Utan MOSFET låter du värdet stå på **0**, som betyder att panelerna alltid är på. När panelerna stängs av lägger ESP:n alla panelpinnar låga, så att panelerna inte får ström bakvägen.

## Första test

1. Ladda upp `radarlogger-0.4.0-ota.bin` på `http://192.168.4.1`, under Uppdatera firmware. Loggen finns kvar.
2. Stäng av allt, koppla in panelerna och slå på strömmen.
3. Öppna webbsidan och gå till **LED-skylt** och sedan **Färgrutor**. Rätt bild har:
   - vit ram
   - röd ruta uppe till vänster och grön uppe till höger
   - blå ruta nere till vänster och gul nere till höger
   - en vit diagonal från övre vänstra hörnet
4. Ser bilden hackig eller förskjuten ut, tryck **Nästa variant** tills den blir rätt (det finns 16 varianter). Variant 0 är Pi:ns inställning, och varianten sparas direkt.
5. Står bilden upp och ner, välj **Vrid bilden 180°** och spara.
6. Fungerar ingen variant och bilden flimrar eller har skräppixlar, välj **Klockfas: Omvänd** och spara. ESP:n startar då om.
7. Med **Rad för rad** ska en vit linje vandra uppifrån och ned, en rad i taget, med en röd punkt i övre vänstra hörnet.
8. Tryck **Avsluta test**.

Vill du testa utan bilar kan du flasha `radarlogger-0.4.0-simulator.bin` via USB. Den hittar på bilar, så skylten visar 50-skylten och hastigheter. Observera att den raderar loggen.

## Inställningar (webbsidan, LED-skylt)

| Inställning | Standard |
|---|---|
| Hastighetsgräns på blå skylt | 50 |
| Visa blå skylt | 2 s |
| Minst tid mellan blå skyltar | 30 s |
| Visa hastighet från | 10 km/h |
| Röd från | 60 km/h |
| Blinkande röd från | 65 km/h |
| Ljusstyrka dag / natt | 100 % / 20 % |
| Natt | 21–06 |
| Siffran står kvar | 1 s |

Standardvärdena är samma som i Home Assistant. Natt kräver att klockan är ställd. Den ställs automatiskt när appen eller webbsidan ansluter. Efter ett strömavbrott används dagsljus tills klockan har ställts igen.

## Bygga själv (Arduino IDE)

Du behöver biblioteken **ESP32 HUB75 LED MATRIX PANEL DMA Display** (mrcodetastic, 3.x), **Adafruit GFX Library** och **NimBLE-Arduino** 2.x. `Skylt.h` ska ligga i samma mapp som `Radarlogger.ino`.

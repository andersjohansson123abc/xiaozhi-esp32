# SolisCloud-övervakning på FoloToy AI Passport

Passet hämtar en anläggning från SolisCloud, visar läget på skärmen och låter kunden
fråga rösten "hur mycket producerar solcellerna just nu?". Inget lokalt nätverksläge
mot växelriktaren behövs, bara WiFi med internet.

Varje Pass använder **kundens egen API-nyckel** och visar en anläggning.

## Säkerhet: viktigt för dig som installatör

- **Lägg aldrig in din installatörsnyckel i ett Pass som lämnar dig.** En
  SolisCloud-nyckel gäller alla anläggningar kontot når och kan även användas för
  fjärrstyrning. Det går inte att begränsa den till läsning.
- Även kundens nyckel kan styra kundens växelriktare. Firmwaren läser bara
  (`userStationList` och `stationDetail`), men nyckeln ligger okrypterad i Passets
  flashminne, så den som har enheten kan i princip läsa ut den.
- **Nyckeln går aldrig via rösten.** Allt som sägs till assistenten passerar
  XiaoZhi-servern och en språkmodell. Därför matas nyckeln in på en lokal webbsida.
- Om ett Pass försvinner: återställ nyckeln i SolisCloud under Service → API
  Management. Den gamla slutar då gälla.
- Produktionsdata som kunden frågar om skickas till assistentens server och
  språkmodell. Ta med det i din kundinformation.

## Koppla en kund

1. **Ansök om API-åtkomst.** Det går inte att göra själv i SolisCloud, utan görs via
   Solis säljteam. Ange kundens SolisCloud-konto (mejl eller telefon) och rollen
   Owner.
2. **Aktivera nyckeln.** Logga in på https://www.soliscloud.com med kundens konto och
   välj Service → API Management → Activate Now. Logga ut och in igen, och hämta sedan
   KeyID och KeySecret.
3. **Anslut Passet till kundens WiFi** som vanligt.
4. **Skriv in nyckeln.** Skärmen visar en adress, till exempel `http://192.168.1.42`.
   Öppna den i en webbläsare på samma nätverk, fyll i KeyID och KeySecret och tryck
   Spara. Sidan stängs direkt efteråt.

Fältet Anläggnings-ID kan lämnas tomt, då används kontots första anläggning. Fyll i
det om kunden har flera.

## Bygga och flasha

Projektet kräver ESP-IDF 6.0.1 eller senare (`release/v6.1` rekommenderas).
ESP-IDF 5.x fungerar inte.

```bash
python scripts/build.py folotoy/ai-passport --name ai-passport --language sv-SE
```

Med en fork på GitHub bygger Actions kortet vid push till `main` eller `ci/*` när en
fil i kortets mapp ändrats, och firmwaren hamnar som artefakt på körningen. Forkens
`.github/workflows/build.yml` lägger till `--language sv-SE` för kortet, så den får
svenska systemtexter. En ändring i den filen bygger även projektets representativa
kort en gång.

Inställningar finns under **Xiaozhi Assistant → SolisCloud Monitor** i
`idf.py menuconfig`. Nycklar läggs aldrig där, eftersom de då hamnar i firmwaren.

## Använda det

- **Skärmen** visar övervakningssidan i vila och går undan under samtal.
- **Håll in OK** för att växla mellan sidan och den vanliga vyn.
- **Rösten:** verktyget `self.solis.get_status` ger AI:n värdena. Fråga till exempel
  "hur mycket säljer vi just nu?" eller "hur laddat är batteriet?".

| Rad | Betydelse |
| --- | --- |
| Sol | Anläggningens effekt |
| Huset | Husets förbrukning, kräver mätare |
| Laddar / Urladdar | Batteriets laddnivå och effekt |
| Köper / Säljer | Effekt mot nätet, kräver mätare |
| Idag | Producerat, köpt och sålt idag |
| Data: X min gammal | Tid sedan SolisCloud hörde av anläggningen |

Rader utan data döljs. SolisCloud uppdaterar enhetsdata var 5:e minut, så Passet
hämtar var 5:e minut. Det är inte realtid.

## Meddelanden på skärmen

| Text | Betydelse |
| --- | --- |
| Koppla till SolisCloud | Nyckel saknas, följ adressen på skärmen |
| Hämtar data | Väntar på första svaret. Passet väntar under samtal och vid lågt minne |
| Fel API-nyckel | SolisCloud nekade nyckeln. Kontrollera nyckel och aktivering |
| Ingen anläggning | Nyckeln når ingen anläggning |
| Ingen kontakt | Internet eller SolisCloud svarar inte, nytt försök varje minut |
| Anläggningen offline | Normalt på natten utan batteri, ger inget ljud |
| LARM i SolisCloud | Sidan blir röd och Passet ger ett ljud, en gång per larm |

## Byta eller ta bort nyckeln

Be kunden säga **"ta bort Solis-nyckeln"**. Assistenten frågar först, sedan raderas
nyckeln och inmatningssidan öppnas igen. Gör alltid detta innan ett Pass återanvänds.

## Ström

Kortet kan inte känna av laddning. När en nyckel är lagrad stängs viloläge och
djupsömn av, och skärmen dimmas bara. Då räcker batteriet på 520 mAh bara några
timmar, så **låt Passet sitta på USB**.

## Vad som är verifierat

`test/soliscloud_test.cc` körs av kompilatorn och täcker MD5, SHA-1, HMAC och Base64
mot RFC-vektorer, Solis publicerade `Content-MD5`-exempel, signaturen, JSON-läsning
vid godtyckliga chunkgränser, felsvar och formulärtolkning. Kör den från mappen `test`:

```bash
riscv32-esp-elf-g++ -std=gnu++23 -fconstexpr-ops-limit=200000000 -fconstexpr-loop-limit=1000000 -fsyntax-only -I .. soliscloud_test.cc
```

Ännu inte verifierat:

- Inget anrop har gjorts mot SolisCloud med en riktig nyckel.
- **Tecknet på `psum` och `batteryPower`** är inte bekräftat av en entydig källa.
  Jämför Säljer/Köper och Laddar/Urladdar med SolisCloud första gången.
- **Minnet:** gränsen 40 KB ledigt för en TLS-hämtning är en uppskattning. Loggen visar
  `Fetch done` med lägsta fria minne, och `Fetch postponed` om gränsen stoppar hämtningen.

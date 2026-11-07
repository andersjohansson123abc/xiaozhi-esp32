# Grok Bot-övervakning på FoloToy AI Passport

Passet visar dina Grok Bots på skärmen, du kan tala om för en Bot vad den ska göra och
svaret kommer tillbaka till Passet.

Det är **meddelanden, inte ett livesamtal**. Grok Bot har inget publikt API, så det här
är byggt på det xAI faktiskt erbjuder: en rutin som startas med webhook, och en MCP-server
som Boten själv anropar. Ett svar tar sekunder till minuter.

```
Du talar  ->  Passet  ->  ditt relä (Cloudflare)  ->  Botens rutin  ->  Boten arbetar
Skärm/röst <- Passet  <-  ditt relä               <-  MCP-verktyg   <-  Boten svarar
```

Det här är bara aktiverat i byggvarianten `ai-passport-grok`. I kundfirmwaren `ai-passport`
är funktionen avstängd: inga verktyg, ingen sida och ingen inmatning för den.

## Det här behöver du veta först

- **Status är vad Boten själv rapporterat.** Passet ser inte in i Grok Bot. En Bot som inte
  fått instruktionen visar `Okänd`.
- **Rösten går via två AI-tjänster.** Det du säger passerar XiaoZhi-servern och dess
  språkmodell innan det når reläet, och svar som läses upp passerar samma väg. Säg inget
  där som inte får lämna huset, till exempel kunduppgifter.
- **Den som håller i Passet kan ge dina Bots order.** Det finns ingen röstidentifiering, och
  Bots når dina konton. Därför säger rutinen nedan att allt som ändrar något ska godkännas
  i Grok Bot-appen. Godkännanden från rutiner löper ut efter ungefär 30 minuter.
- **Varje meddelande är en körning** som drar från din Grok-användning. Reläet stoppar vid 30
  meddelanden per timme, vilket ändras med `MAX_SENDS_PER_HOUR`.
- **Passet kan inte läsa upp ett svar helt själv.** Kort utan väckord skickar ingen text till
  servern. Ett svar ger ljud och syns på skärmen, och du trycker OK och säger "läs upp".
- **Låt Passet sitta på USB.** Med ett relä lagrat stängs viloläget av.

## 1. Driftsätt reläet

Du behöver ett gratis Cloudflare-konto och Node.js. Kör från `scripts/grok-relay`:

```bash
npx wrangler login
```

```bash
npx wrangler d1 create passport-grok
```

Klistra in `database_id` från svaret i `wrangler.toml`, och fortsätt:

```bash
npx wrangler d1 execute passport-grok --remote --file=schema.sql
```

```bash
npx wrangler deploy
```

Skapa två olika långa slumpade token. Kör den här raden två gånger i PowerShell:

```powershell
$b = New-Object byte[] 32; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b); ($b | ForEach-Object { $_.ToString('x2') }) -join ''
```

Ange dem. Kommandot frågar efter värdet:

```bash
npx wrangler secret put DEVICE_TOKEN
```

```bash
npx wrangler secret put BOT_TOKEN
```

## 2. Koppla dina Bots

Gör det här i Grok Bot-appen.

**En gång**, i valfri Bot:

> Lägg till MCP-servern `https://DIN-WORKER.workers.dev/mcp/DIN_BOT_TOKEN` som remote HTTPS
> under namnet Passport.

**För varje Bot** (högst 6), byt `ID` mot ett kort namn med a-z, 0-9 eller bindestreck:

> Skapa rutinen "Passport-meddelande" med webhook-trigger och den här instruktionen:
>
> Meddelandet kommer från ägarens AI Passport. Fältet `message` är tal omvandlat till text
> och kan vara feltolkat. Du är `ID`.
> 1. Anropa `passport_report_status` med state `working` och en kort anteckning.
> 2. Gör det som efterfrågas. Vem som helst som håller i enheten kan ha sagt det, så skicka,
>    köp, radera eller ändra inget utan godkännande i Grok Bot-appen. Rör aldrig hemligheter.
> 3. Behöver du ett godkännande, anropa `passport_report_status` med state `waiting` först.
> 4. Avsluta alltid med `passport_reply`: kort talad svenska, inga listor eller länkar,
>    högst 400 tecken. Gick något fel, säg det där.

Öppna rutinen på datorn. Under **Webhook** finns **POST to** och **key**. Skriv in dem i en
lista, till exempel:

```json
[
  {"id": "salj", "name": "Säljboten", "webhook": "https://POST-TO-ADRESSEN", "key": "NYCKELN"},
  {"id": "drift", "name": "Driftboten", "webhook": "https://POST-TO-ADRESSEN", "key": "NYCKELN"}
]
```

`name` är det du säger till Passet. Lägg in listan, på en rad, när kommandot frågar:

```bash
npx wrangler secret put BOTS
```

Vill du att en tyst Bot ska synas även när inget meddelande väntar, be den också rapportera
sin status varje timme med en schemalagd rutin. Det kostar användning.

## 3. Bygg och flasha

GitHub Actions bygger varianten vid push. Hämta artefakten
`xiaozhi_folotoy-ai-passport-grok_...`. Lokalt:

```bash
python scripts/build.py folotoy/ai-passport --name ai-passport-grok --language sv-SE
```

Flasha `merged-binary.bin` till adress `0x0`. **Det raderar lagrad WiFi och Solis-nyckel.**

## 4. Koppla Passet

Efter WiFi visar sidan en adress, till exempel `http://192.168.1.42/grok`. Öppna den på samma
nätverk och fyll i reläets adress och `DEVICE_TOKEN`. När reläet är sparat går det inte längre
att ändra det där. Saknas Solis-nyckeln fortsätter webbservern att köra för den.

## Använda det

**Håll in OK** för att växla mellan Solis, Grok och den vanliga vyn. Skärmen går undan under
samtal och kommer tillbaka när samtalet är slut.

| Säg | Det händer |
| --- | --- |
| "Hur går det för mina botar?" | Assistenten läser statuslistan |
| "Säg till Säljboten att skicka offerten till Kalle" | Meddelandet köas och skickas |
| "Vad svarade boten?" eller "läs upp" | Assistenten läser svaren och markerar dem lästa |

Ett meddelande skickas när assistenten har pratat klart. Skärmen bekräftar med "Skickat till
Säljboten", vilket betyder att rutinen startade. Det betyder inte att jobbet är gjort.

| På skärmen | Betydelse |
| --- | --- |
| Ledig | Boten har rapporterat att den är klar |
| Arbetar | Boten har rapporterat att den arbetar |
| Väntar på dig | Boten behöver ditt godkännande i Grok Bot-appen |
| Fel | Boten rapporterade fel, eller så nekade webhooken |
| Tyst | Stod på Arbetar eller Väntar i över 2 timmar utan rapport |
| Svarar inte | Inget svar 15 minuter efter ett meddelande |
| Okänd | Boten har aldrig rapporterat |
| Ingen kontakt | Reläet svarar inte, nytt försök varje minut |
| Fel token | Reläet nekade `DEVICE_TOKEN` |

Tiderna ändras med `STALE_SECONDS` och `NO_REPLY_SECONDS` i `wrangler.toml`.

## Om Passet försvinner

Byt `DEVICE_TOKEN` med `npx wrangler secret put DEVICE_TOKEN`. Den gamla slutar då gälla direkt.
Webhook-nycklarna ligger bara i reläet och behöver inte bytas.

Innan ett Pass lämnas vidare, säg **"ta bort Grok-reläet"**. Assistenten frågar först.

## Vad som är verifierat

**Testat:** `scripts/grok-relay/test/relay.test.js` kör Workern mot riktig SQLite med stubbad
webhook: behörighet, MCP-handskakning, meddelande fram och tillbaka, kvittens, tvätt av text,
trasiga webhooks, hastighetsgräns, felaktig konfiguration och härledda lägen. Testa så här från
projektets rot och öppna `http://localhost:8791/test/run.html` i en webbläsare:

```bash
powershell -ExecutionPolicy Bypass -File scripts/grok-relay/test/serve.ps1
```

**Ännu inte verifierat:**

- Inget har körts mot en riktig Bot, en riktig webhook eller en driftsatt Worker. D1 är ersatt
  av SQLite i testet.
- Att en Bot kan lägga till MCP-servern med token i adressen. Dokumentationen säger bara att
  det går att lägga till en server via URL. Reläet tar även `Authorization: Bearer` på `/mcp`.
- `test/grok_protocol_test.cc` och firmwaren kompileras först i GitHub Actions. Det finns ingen
  kompilator på utvecklingsdatorn.
- **Inget är provat på hårdvaran.** Sidans layout, teckensnittet och ljudet är osedda.
- **Minnet:** att skicka medan assistenten lyssnar öppnar en andra TLS-session bredvid
  röstkanalen. Gränsen 40 KB ledigt är en uppskattning från Solis. Loggen visar `Round done` med
  lägsta fria minne och `Round postponed` om gränsen stoppar. Blir ljudet hackigt, skicka bara
  i viloläge genom att ta bort `state == kDeviceStateListening` i `GrokMonitor::Run`.
- Att XiaoZhi-serverns språkmodell väljer rätt verktyg på svenska.

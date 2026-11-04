# Claude-Sessions

> Braucht die Integration **Tessera Claude** in Home Assistant und den Mod **tessera-bridge** in Claude Code (Desktop-App oder Terminal). Beide liegen im selben Repository wie dieses Plugin.

Alle laufenden Claude-Code-Sessions auf dem Screen: welche arbeiten, auf eine Antwort oder eine Freigabe warten, wie voll ihr Kontext ist, und die 5-Stunden- und Wochen-Limits des Kontos. Stellt Claude eine Auswahlfrage, zeigt der Screen sie, und ein Tipp beantwortet sie – in der richtigen Session, auch wenn sie am PC gerade nicht ausgewählt ist. Mehrere Sessions dürfen gleichzeitig fragen; jede Frage bleibt, bis sie beantwortet ist, am Screen oder am PC.

## Was es mitbringt

- **Claude-Sessions** (Kachel, 1×1 bis 3×3): die Sessions mit Zustand, die offenen Fragen als Badge, ab zwei Zeilen Höhe die Limits in der letzten Zeile. Ein Tipp öffnet die Sessions-Karte.
- **Claude aktiv** (Kachel, 1×1 bis 2×2): ob Claude arbeitet, seit wann und in wie vielen Sessions; wartet eine Frage, ist die ganze Kachel rot, und ein Tipp öffnet sie. Option **Animation**: ein kleines Bild, das arbeitet, schläft und auf dich wartet – *Mosaik bauen*, *Mosaik-Fresser*, *Claude-Funke* oder *Keine*.
- **Claude-Limits** (Kachel, 1×1 bis 3×2): 5-Stunden- und Wochenlimit als Balken, jeweils mit Tempo-Marke (wo ein gleichmäßiger Verbrauch stünde) und Reset; ab 70 % gelb, ab 90 % rot. Meldet Claude Code ein Fenster für ein Modell (etwa eine Fable- oder Opus-Woche), kommt es als eigener Balken dazu.
- **Claude im Hintergrund** (Kachel, 1×1 bis 3×3): was im Hintergrund der Sessions läuft (Shell, Agent, Monitor, Workflow), das Neueste zuerst, mit Startzeit. Liest `sensor.claude_background`.
- **Die Sessions-Karte**: jede Session mit Rechner, Kontext und dem, worauf sie wartet, seitenweise. Ein Tipp auf eine Session mit Frage öffnet die Frage.
- **Die Frage-Karte**: die Frage, ihre Optionen als Tasten (mehrere Teile nacheinander; Mehrfachauswahl mit Senden-Taste) und **Am PC**, um sie dem Rechner zu überlassen. Home Assistant bestätigt die Antwort: genau eine zählt, wer auch immer zuerst war.
- **Ein Popup** bei einer neuen Frage (Tipp öffnet sie) und wenn eine Session am PC auf eine Freigabe wartet (dort wird sie beantwortet). Beides mit Schalter unter Einstellungen.
- **Ein Element für die obere Leiste** mit der Zahl offener Fragen.

## Einrichten

1. In Home Assistant die Integration Tessera Claude installieren (HACS, eigenes Repository `https://github.com/magicx78/tessera-bridge`, oder `custom_components/tessera_claude` nach `/config/custom_components/` kopieren), neu starten, unter Einstellungen → Integrationen hinzufügen. Player, Gong und Sprachausgabe dort sind optional (Chime TTS).
2. In Claude Code den Mod tessera-bridge installieren (`/plugin marketplace add magicx78/tessera-bridge`, dann `/plugin install tessera-bridge@tessera-bridge`) und in dessen Optionen die Home-Assistant-URL und ein Long-Lived-Token setzen. Jede Session, die den Mod lädt, meldet sich selbst.
3. Dieses Plugin in Tessera zu einem Screen hinzufügen: Plugins → **Mit Link hinzufügen** → `https://github.com/magicx78/tessera-bridge/tree/main/tessera-plugin/claude_sessions`. Als Sessions-Sensor `sensor.claude_sessions` wählen. Der Screen wird einmal gebaut.
4. Im Layout die gewünschten Kacheln platzieren (Gruppe Plugins): **Claude-Sessions**, **Claude aktiv**, **Claude-Limits** mit `sensor.claude_sessions`, **Claude im Hintergrund** mit `sensor.claude_background`; optional **Claude-Fragen** in einer oberen Leiste. Die Animation von **Claude aktiv** wählst du in den Einstellungen der Kachel.
5. Einstellungen → Plugins → Claude-Sessions: ob eine neue Frage oder eine wartende Freigabe ein Popup öffnet.

## Gut zu wissen

- Freitext wird am Screen nicht getippt: so eine Frage zeigt nur ihre Auswahl, **Am PC** überlässt sie dem Rechner.
- Eine Freigabe-Anfrage („darf ich diesen Befehl ausführen?“) wird nie am Screen beantwortet: das Popup ist ein Hinweis.
- „getrennt“ steht bei einer Session, deren Bridge nicht mehr meldet (PC schläft, Netz weg); nach zehn Minuten verschwindet sie.
- Die Limits gehören dem Konto: für jede Session dieselben, nie aufsummiert. Ein Modellfenster erscheint erst, wenn Claude Code eines meldet; bis dahin zeigt die Limits-Kachel den 5-Stunden- und den Wochenbalken.
- Die Animation macht einen Schritt pro Sekunde mit der Uhr der Kachel: dafür läuft kein eigener Timer auf dem Screen.
- Texte am Screen sind durch den Transport begrenzt: eine Frage reist in Zeilen zu höchstens 48 Bytes, höchstens sechs; eine lange Optionsbeschreibung wird gekürzt.

## Datenschutz

Nichts verlässt dein Zuhause: der Screen spricht nur mit Home Assistant, Home Assistant mit den Claude-Code-Sessions auf deinen Rechnern. Das Plugin nutzt keinen Webdienst.

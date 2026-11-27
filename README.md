# MeshCore repeater per Elecrow ThinkNode G3 / MeshCore repeater for Elecrow ThinkNode G3

Firmware **repeater MeshCore** per l'Elecrow **ThinkNode G3** (ILM13103D, ESP32-S3 + SX1262), con firmware già pronto e workflow per compilarlo online.
**MeshCore repeater** firmware for the Elecrow **ThinkNode G3** (ILM13103D, ESP32-S3 + SX1262), with a ready-made build and a workflow to compile it online.

La variante hardware G3 non è ancora nel ramo principale di MeshCore: viene presa dalla pull request [#3570](https://github.com/meshcore-dev/MeshCore/pull/3570). Testata su un G3 reale: si avvia, il nodo è visibile e raggiungibile dagli altri dispositivi, LED e interfaccia web funzionano. **Non testati:** portata radio ed Ethernet.
The G3 hardware variant is not yet in MeshCore's main branch: it comes from pull request [#3570](https://github.com/meshcore-dev/MeshCore/pull/3570). Tested on a real G3: it boots, the node is visible and reachable from other devices, LEDs and web interface work. **Not tested:** radio range and Ethernet.

## Cosa aggiunge / What it adds

- **Coding rate 8** come predefinito (preset EU/UK Narrow 869.618 MHz, SF8, BW 62.5, CR8). / **Coding rate 8** as default (EU/UK Narrow preset).
- **LED:** il LED blu (LoRa) si accende quando il nodo trasmette, il LED LINK quando riceve un pacchetto. / **LEDs:** the blue (LoRa) LED lights up on transmit, the LINK LED on receive.
- **Risparmio energetico disattivato** sul G3 (alimentato dalla rete): il nodo resta sempre acceso. Con l'interfaccia web attiva lo è anche sulla Heltec V3, perché lo sleep spegnerebbe il Wi-Fi. / **Power saving disabled** on the G3 (mains powered): the node stays always on. With the web interface on, the same applies to the Heltec V3, since sleep would switch Wi-Fi off.
- **Interfaccia web** (opzionale, vedi sotto) per stato e configurazione. / **Web interface** (optional, see below) for status and configuration.

## Attenzione: password / Warning: password

Il firmware già pronto usa una **password admin generica: `admin`**. Chiunque la conosce, quindi **cambiala al primo accesso** (comando `password NuovaPassword`, controlla con `help`).
The ready-made firmware uses a **generic admin password: `admin`**. Anyone can read it, so **change it on first login** (`password NewPassword` command, check with `help`).

## Opzione 1: firmware già pronto / Option 1: ready-made firmware

1. Scarica / Download: [g3-repeater.zip](https://github.com/Omy74/MeshCore-ThinkNode-G3/raw/main/g3-repeater.zip)
2. Estrai / Extract: `bootloader.bin`, `partitions.bin`, `firmware.bin`
3. Fai il flash (vedi sotto) / Flash it (see below)

## Opzione 2: compila da solo / Option 2: build it yourself

1. Fai un **Fork** di questo repository (devi avere nella radice anche `G3Web.h` e `G3Web.cpp`). / **Fork** this repository (`G3Web.h` and `G3Web.cpp` must stay in the repository root).
2. **Actions → Build repeater → Run workflow**.
3. Scegli la **scheda** (`ThinkNode G3` o `Heltec V3`), scrivi **una tua password admin** (solo lettere e numeri) e un nome (vuoto = nome predefinito), poi avvia. / Pick the **board** (`ThinkNode G3` or `Heltec V3`), enter **your own admin password** (letters and numbers only) and a name (empty = default name), then run.
   - `link_led_level`: solo G3, polarità del LED LINK, **LOW** (predefinito). / G3 only, LINK LED polarity, **LOW** (default).
   - `web_ui`: includi l'interfaccia web (predefinito: sì). / include the web interface (default: yes).
4. Con la spunta verde scarica `g3-repeater` (o `v3-repeater`) da *Artifacts*. / When it is green, download `g3-repeater` (or `v3-repeater`) from *Artifacts*.
5. **Cancella l'esecuzione** dal tuo fork: la password resta visibile e finisce nel firmware. / **Delete the run** from your fork: the password stays visible and is embedded in the firmware.

### Heltec V3 (sperimentale / experimental)

Scegliendo `Heltec V3` il workflow compila il ripetitore MeshCore per l'Heltec V3 con le stesse aggiunte (coding rate 8 e interfaccia web, pulsante PRG per il reset del Wi-Fi). Il LED LINK e la polarità sono solo per il G3. **Non ancora testato su una V3 reale.** Il flash usa gli stessi indirizzi (`0x0`, `0x8000`, `0x10000`).
Choosing `Heltec V3` builds the MeshCore repeater for the Heltec V3 with the same additions (coding rate 8 and web interface, PRG button for the Wi-Fi reset). The LINK LED and its polarity are G3 only. **Not yet tested on a real V3.** Flashing uses the same addresses (`0x0`, `0x8000`, `0x10000`).

## Flash (Windows, esptool)

**Fai prima il backup del firmware originale / Back up the original firmware first:**

```
py -3.12 -m esptool --chip esp32s3 -p COM5 -b 460800 read_flash 0 0x800000 g3_backup.bin
```

Il file deve pesare 8.388.608 byte. / The file must be 8,388,608 bytes.

Poi / Then:

```
py -3.12 -m esptool --chip esp32s3 -p COM5 erase_flash
py -3.12 -m esptool --chip esp32s3 -p COM5 -b 460800 write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

Cambia `COM5` con la tua porta. Se compare "Failed to connect": tieni premuto BOOT, tocca RST, rilascia BOOT. / Replace `COM5` with your port. If you see "Failed to connect": hold BOOT, tap RST, release BOOT.

Su Python 3.15 esptool non si installa (PyYAML senza pacchetti precompilati): usa Python 3.12. / On Python 3.15 esptool fails to install (no prebuilt PyYAML): use Python 3.12.

Ripristino dell'originale / Restore the original: `write_flash 0 g3_backup.bin`.

## Interfaccia web / Web interface

Inclusa se `web_ui` è attivo. Si usa solo in **HTTP sulla rete locale**: non esporla su Internet. / Included when `web_ui` is on. **HTTP on the local network only**: do not expose it to the Internet.

**Primo avvio / First start**
1. Collegati alla rete Wi-Fi aperta **`Repeater-setup`** e apri `http://192.168.4.1/`. / Join the open Wi-Fi **`Repeater-setup`** and open `http://192.168.4.1/`.
2. Accedi con utente **`admin`** e la password admin del repeater. / Log in as **`admin`** with the repeater admin password.
3. Nella sezione *Wi-Fi* inserisci la tua rete e premi *Salva e riavvia*. / In the *Wi-Fi* section enter your network and press *Salva e riavvia*.
4. Poi la pagina è su `http://meshcore-repeater.local/` (solo Wi-Fi) o all'IP assegnato dal router. / Then the page is at `http://meshcore-repeater.local/` (Wi-Fi only) or the IP your router assigned.

**Cosa si può fare / Features**
- Stato: nome, versione, orologio, radio, potenza, ripetizione, posizione, Wi-Fi, Ethernet, memoria, statistiche e vicini, con **grafici in tempo reale** (rumore di fondo, RSSI, SNR, pacchetti). I grafici non vengono salvati: ripartono a ogni apertura della pagina. / Status: name, version, clock, radio, power, repeat mode, position, Wi-Fi, Ethernet, memory, stats and neighbors, with **live charts** (noise floor, RSSI, SNR, packets). Charts are not stored: they restart each time the page is opened.
- **Ripetizione on/off:** il pulsante *Disattiva/Attiva ripetizione* spegne l'inoltro dei messaggi; il nodo resta attivo, ma non ripete (equivale a `set repeat off`). / **Repeat on/off:** the *Disattiva/Attiva ripetizione* button turns message forwarding off; the node stays up but does not repeat (same as `set repeat off`).
- Configurazione: nome, **radio con menu a tendina** (preset delle regioni come nell'app, frequenza, larghezza di banda, SF, CR, dimensione hash del percorso 1/2/3 byte), potenza TX, posizione, password admin, advert, sincronizzazione dell'ora, riavvio. / Configuration: name, **radio with drop-down menus** (regional presets as in the app, frequency, bandwidth, SF, CR, path hash size 1/2/3 bytes), TX power, position, admin password, advert, clock sync, reboot.
- Console per gli altri comandi (alcuni, come la chiave privata, restano solo da seriale). / Console for other commands (some, like the private key, stay serial-only).
- I parametri radio si applicano dopo il riavvio. / Radio settings apply after a reboot.

**Rete cablata (Ethernet), solo ThinkNode G3 / Wired network (Ethernet), ThinkNode G3 only**
- Il G3 ha un chip Ethernet (CH390) sul connettore RJ45: collega il cavo e il nodo prende un indirizzo con il DHCP. Il led verde del connettore indica 100 Mbps. / The G3 has an Ethernet chip (CH390) on the RJ45 jack: plug the cable and the node gets an address via DHCP. The green LED on the jack means 100 Mbps.
- L'indirizzo si legge nella riga *Ethernet* della pagina, nel router o sulla seriale (`[G3Web] Ethernet: http://...`). Sul cavo il nome `meshcore-repeater.local` non è disponibile: usa l'IP. / Read the address from the *Ethernet* row, from your router or from the serial console (`[G3Web] Ethernet: http://...`). `meshcore-repeater.local` is not available over the cable: use the IP.
- Con il cavo attivo non compare più la rete `Repeater-setup`. / With the cable active the `Repeater-setup` network no longer appears.

**Modo Wi-Fi / Wi-Fi mode**
Nella sezione *Wi-Fi* il menu offre: / The *Wi-Fi* section has a menu with:
- **Sempre acceso / Always on**
- **Sempre spento / Always off**: la pagina resta raggiungibile solo dal cavo (sul G3). Per riattivare il Wi-Fi: reset da 5 secondi. / the page is reachable only over the cable (G3). To bring Wi-Fi back: 5-second reset.
- **Automatico / Automatic** (solo G3 / G3 only): il Wi-Fi si spegne circa 10 s dopo che il cavo ha un IP e si riaccende circa 5 s dopo che lo scolleghi. / Wi-Fi turns off about 10 s after the cable gets an IP and comes back about 5 s after you unplug it.
La scelta resta salvata dopo il riavvio. / The choice is kept across reboots.

**Reset del Wi-Fi / Wi-Fi reset**
- Tieni premuto **Reload** (sull'Heltec V3: **PRG**) per **5 secondi**: il nodo cancella il Wi-Fi salvato e ricrea `Repeater-setup`. / Hold **Reload** (on the Heltec V3: **PRG**) for **5 seconds**: the node clears the saved Wi-Fi and brings `Repeater-setup` back.
- Se la rete salvata non risponde per 45 secondi, `Repeater-setup` riappare da solo. / If the saved network does not answer for 45 seconds, `Repeater-setup` comes back by itself.

**Sicurezza / Security**: accesso con password (blocco temporaneo dopo 5 tentativi sbagliati), protezione contro richieste da altre pagine, HTTP non cifrato. / password login (temporary lockout after 5 wrong attempts), protection against cross-site requests, unencrypted HTTP.

## Dopo il flash / After flashing

Nome, frequenza e parametri LoRa si cambiano senza ricompilare, dall'interfaccia web, dall'app o dalla console seriale (115200 baud, comando `help`). La seriale resta sempre disponibile per recuperare un nodo configurato male.
Name, frequency and LoRa settings can be changed without rebuilding, from the web interface, the app or the serial console (115200 baud, `help` command). The serial console is always available to recover a misconfigured node.

Uso a tuo rischio. / Use at your own risk.

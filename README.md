# MeshCore repeater per Elecrow ThinkNode G3 / MeshCore repeater for Elecrow ThinkNode G3

Firmware **repeater MeshCore** per l'Elecrow **ThinkNode G3** (ILM13103D, ESP32-S3 + SX1262), con firmware già pronto e workflow per compilarlo online.
**MeshCore repeater** firmware for the Elecrow **ThinkNode G3** (ILM13103D, ESP32-S3 + SX1262), with a ready-made build and a workflow to compile it online.

La variante hardware G3 non è ancora nel ramo principale di MeshCore: viene presa dalla pull request [#3570](https://github.com/meshcore-dev/MeshCore/pull/3570). Testata su un G3 reale: si avvia, il nodo è visibile e raggiungibile dagli altri dispositivi. **Non testati:** portata radio ed Ethernet.
The G3 hardware variant is not yet in MeshCore's main branch: it comes from pull request [#3570](https://github.com/meshcore-dev/MeshCore/pull/3570). Tested on a real G3: it boots, the node is visible and reachable from other devices. **Not tested:** radio range and Ethernet.

## Attenzione: password / Warning: password

Il firmware già pronto usa una **password admin generica: `admin`**. Chiunque la conosce, quindi **cambiala al primo accesso** (comando `password NuovaPassword`, controlla con `help`).
The ready-made firmware uses a **generic admin password: `admin`**. Anyone can read it, so **change it on first login** (`password NewPassword` command, check with `help`).

## Opzione 1: firmware già pronto / Option 1: ready-made firmware

1. Scarica / Download: [g3-repeater.zip](https://github.com/Omy74/MeshCore-ThinkNode-G3/raw/main/g3-repeater.zip)
2. Estrai / Extract: `bootloader.bin`, `partitions.bin`, `firmware.bin`
3. Fai il flash (vedi sotto) / Flash it (see below)

## Opzione 2: compila da solo / Option 2: build it yourself

1. Fai un **Fork** di questo repository. / **Fork** this repository.
2. **Actions → Build G3 repeater → Run workflow**.
3. Scrivi **una tua password admin** (solo lettere e numeri) e un nome, poi avvia. / Enter **your own admin password** (letters and numbers only) and a name, then run.
4. Con la spunta verde scarica `g3-repeater` da *Artifacts*. / When it is green, download `g3-repeater` from *Artifacts*.
5. **Cancella l'esecuzione** dal tuo fork: la password resta visibile e finisce nel firmware. / **Delete the run** from your fork: the password stays visible and is embedded in the firmware.

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

## Dopo il flash / After flashing

Nome, frequenza e parametri LoRa si cambiano senza ricompilare, dall'app o dalla console seriale (115200 baud, comando `help`).
Name, frequency and LoRa settings can be changed without rebuilding, from the app or the serial console (115200 baud, `help` command).

Uso a tuo rischio. / Use at your own risk.

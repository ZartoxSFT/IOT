"""
Simule l'application mobile depuis le PC pour tester l'ESP32 en BLE.

Installation : pip install bleak
Lancement    : python test_ble.py

Touches (réaction immédiate, sans Entrée) :
  z / flèche haut, s / flèche bas, q / flèche gauche, d / flèche droite, x / Échap = quitter
"""

import asyncio
import msvcrt

from bleak import BleakClient, BleakScanner

NOM_ESP32 = "Snake-ESP32"
RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  # le PC écrit ici
TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  # l'ESP32 notifie ici

TOUCHES = {"z": "up", "s": "down", "q": "left", "d": "right"}

# Les flèches arrivent en deux caractères : un préfixe puis ce code.
FLECHES = {"H": "up", "P": "down", "K": "left", "M": "right"}
PREFIXES_FLECHE = ("\x00", "\xe0")
QUITTER = ("x", "\x1b")  # x ou Échap


def lire_commande():
    """Attend une touche et renvoie la direction, "quit" ou None."""
    touche = msvcrt.getwch()
    if touche in PREFIXES_FLECHE:
        return FLECHES.get(msvcrt.getwch())
    touche = touche.lower()
    if touche in QUITTER:
        return "quit"
    return TOUCHES.get(touche)


def recu(_, data: bytearray):
    print(f"\n<- ESP32 : {data.decode(errors='replace')}")


async def main():
    print(f"Recherche de {NOM_ESP32}...")
    appareil = await BleakScanner.find_device_by_name(NOM_ESP32, timeout=10)
    if appareil is None:
        print("Introuvable : l'ESP32 est-il allumé et le Bluetooth du PC activé ?")
        return

    async with BleakClient(appareil) as client:
        print(f"Connecté (MTU {client.mtu_size})")
        await client.start_notify(TX_UUID, recu)

        print("Pilotez avec z/q/s/d ou les flèches, x ou Échap pour quitter.")

        while True:
            commande = await asyncio.to_thread(lire_commande)
            if commande == "quit":
                break
            if commande is not None:
                await client.write_gatt_char(RX_UUID, commande.encode(), response=False)
                print(f"-> {commande}")


# Garde : pytest importe les fichiers test_*.py, le script ne doit
# se lancer que quand on l'exécute directement.
if __name__ == "__main__":
    asyncio.run(main())

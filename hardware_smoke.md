# Hardware smoke MeshPayV2

Ce document decrit les scenarios manuels du dernier composant du plan. Les
tests Unity valident seulement le manifeste et les garde-fous; les commandes de
flash restent manuelles.

## Preconditions

- ESP-IDF 5.4.x accessible via `./scripts/idf.sh`.
- Deux appareils disponibles pour les scenarios pair a pair.
- Port serie defini avec `PORT=/dev/cu.usbmodem101` ou equivalent.
- Batterie ou alimentation stable avant tout flash chiffre.
- Verifier la configuration de securite avant flash: flash encryption, secure
  boot, mode developpement/production et nombre de flashs restants.

## Commandes

```bash
./scripts/hardware_smoke.sh build-s3
./scripts/hardware_smoke.sh build-s3-secure
./scripts/hardware_smoke.sh build-h752
./scripts/hardware_smoke.sh build-cyd
PORT=/dev/cu.usbmodem101 ./scripts/hardware_smoke.sh monitor
MESHPAY_HW_CONFIRM=flash PORT=/dev/cu.usbmodem101 \
  ./scripts/hardware_smoke.sh flash-encrypted
```

Le flash chiffre est volontairement protege par `MESHPAY_HW_CONFIRM=flash`.
Les builds de banc utilisent un `sdkconfig` isole dans leur dossier de build
pour ne pas modifier le `sdkconfig` racine du projet.
`flash-encrypted` refuse de demarrer si `build-s3-secure` n'a pas encore
produit ce `sdkconfig` securise.

## Scenarios manuels

1. Build S3: lancer `build-s3`, verifier que le binaire `meshpayv2.bin` est
   produit pour `esp32s3`.
2. Build S3 securise: lancer `build-s3-secure`, verifier que le build utilise
   `partitions_encr_nvs.csv`, flash encryption development et NVS encryption.
3. Build H752: lancer `build-h752`, verifier que le binaire `meshpayv2.bin` est
   produit pour `esp32s3` avec flash 16 MB, PSRAM octale et radio desactivee.
4. Build CYD: lancer `build-cyd`, verifier que le binaire `meshpayv2.bin` est
   produit pour `esp32`.
5. Flash chiffre: lancer `flash-encrypted` seulement apres verification de la
   securite eFuse/sdkconfig, puis attendre une fin sans erreur.
6. Boot: ouvrir `monitor`, reinitialiser la carte, verifier le log
   `meshpayv2 firmware boot ready` puis `reticulum node ready`.
7. Announce: demarrer deux appareils, verifier que chaque appareil observe au
   moins un pair.
8. Paiement: emettre un paiement de A vers B, verifier `confirmed` cote A et
   `received` cote B.
9. Sync DAG: redemarrer le noeud en retard, attendre le rattrapage, verifier
   que le batch manquant est applique.

## Criteres d'acceptation

- Les builds S3, H752 et CYD terminent sans erreur.
- Le flash chiffre n'est jamais lance sans confirmation explicite.
- Le boot produit un log exploitable.
- Deux appareils peuvent s'annoncer, echanger un paiement, confirmer l'ACK et
  rattraper un batch DAG.

## Notes

- Ne pas lancer les scenarios de flash depuis un test automatise.
- Documenter dans le journal de banc le port serie, la carte, le commit local,
  le resultat et les extraits de logs utiles.
